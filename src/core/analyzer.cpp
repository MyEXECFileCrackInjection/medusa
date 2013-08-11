#include "medusa/analyzer.hpp"

#include "medusa/function.hpp"
#include "medusa/character.hpp"
#include "medusa/string.hpp"
#include "medusa/label.hpp"
#include "medusa/log.hpp"

#include <list>
#include <stack>

#include <boost/foreach.hpp>

MEDUSA_NAMESPACE_BEGIN

  // bool Analyzer::DisassembleFollowingExecutionPath(Document const& rDoc, Architecture& rArch, Address const& rAddr, std::list<Instruction*>& rBasicBlock)
  void Analyzer::DisassembleFollowingExecutionPath(Document& rDoc, Address const& rEntrypoint, Architecture& rArch) const
{
  boost::lock_guard<boost::mutex> Lock(m_DisasmMutex);

  auto Lbl = rDoc.GetLabelFromAddress(rEntrypoint);
  if (Lbl.GetType() & Label::Imported)
    return;

  std::stack<Address> CallStack;
  Address::List FuncAddr;
  Address CurAddr             = rEntrypoint;
  MemoryArea const* pMemArea  = rDoc.GetMemoryArea(CurAddr);

  if (pMemArea == nullptr)
  {
    Log::Write("core") << "Unable to get memory area for address " << CurAddr.ToString() << LogEnd;
    return;
  }

  // Push entry point
  CallStack.push(CurAddr);

  // Do we still have functions to disassemble?
  while (!CallStack.empty())
  {
    // Retrieve the last function
    CurAddr = CallStack.top();
    CallStack.pop();
    bool FunctionIsFinished = false;

    //Log::Write("debug") << "Analyzing address: " << CurAddr.ToString() << LogEnd;

    // Disassemble a function
    while (rDoc.IsPresent(CurAddr) && rDoc.ContainsCode(CurAddr) == false)
    {
      //Log::Write("debug") << "Disassembling basic block at " << CurAddr.ToString() << LogEnd;

      // Let's try to disassemble a basic block
      std::list<Instruction*> BasicBlock;
      if (!DisassembleBasicBlock(rDoc, rArch, CurAddr, BasicBlock)) break;
      if (BasicBlock.size() == 0)                                  break;

      for (auto itInsn = std::begin(BasicBlock); itInsn != std::end(BasicBlock); ++itInsn)
      {
        if (rDoc.ContainsCode(CurAddr))
        {
          //Log::Write("debug") << "Instruction is already disassembled at " << CurAddr.ToString() << LogEnd;
          FunctionIsFinished = true;
          delete *itInsn;
          *itInsn = nullptr;
          continue;
        }

        if (!rDoc.InsertCell(CurAddr, *itInsn, true))
        {
          //Log::Write("core") << "Error while inserting instruction at " << CurAddr.ToString() << LogEnd;
          FunctionIsFinished = true;
          delete *itInsn;
          *itInsn = nullptr;
          continue;
        }

        for (u8 i = 0; i < OPERAND_NO; ++i)
        {
          Address DstAddr;
          if ((*itInsn)->GetOperandReference(rDoc, i, CurAddr, DstAddr))
            CallStack.push(DstAddr);
        }

        CreateXRefs(rDoc, CurAddr);

        auto InsnType = (*itInsn)->GetOperationType();
        if (InsnType == Instruction::OpUnknown || InsnType == Instruction::OpCond)
          CurAddr += (*itInsn)->GetLength();
      }

      if (FunctionIsFinished == true) break;

      auto pLastInsn = BasicBlock.back();
      //Log::Write("debug") << "Last insn: " << pLastInsn->ToString() << LogEnd;

      switch  (pLastInsn->GetOperationType() & (Instruction::OpCall | Instruction::OpJump | Instruction::OpRet))
      {
        // If the last instruction is a call, we follow it and save the return address
      case Instruction::OpCall:
        {
          Address DstAddr;

          // Save return address
          CallStack.push(CurAddr + pLastInsn->GetLength());

          // Sometimes, we cannot determine the destination address, so we give up
          // We assume destination is hold in the first operand
          if (!pLastInsn->GetOperandReference(rDoc, 0, CurAddr, DstAddr))
          {
            FunctionIsFinished = true;
            break;
          }

          FuncAddr.push_back(DstAddr);
          CurAddr = DstAddr;
          break;
        } // end OpCall

        // If the last instruction is a ret, we emulate its behavior
      case Instruction::OpRet:
        {
          // We ignore conditional ret
          if (pLastInsn->GetOperationType() & Instruction::OpCond)
          {
            CurAddr += pLastInsn->GetLength();
            continue;
          }

          // ret if reached, we try to disassemble an another function (or another part of this function)
          FunctionIsFinished = true;
          break;
        } // end OpRet

        // Jump type could be a bit tedious to handle because of conditional jump
        // Basically we use the same policy as call instruction
      case Instruction::OpJump:
        {
          Address DstAddr;

          // Save untaken branch address
          if (pLastInsn->GetOperationType() & Instruction::OpCond)
            CallStack.push(CurAddr + pLastInsn->GetLength());

          // Sometime, we can't determine the destination address, so we give up
          if (!pLastInsn->GetOperandReference(rDoc, 0, CurAddr, DstAddr))
          {
            FunctionIsFinished = true;
            break;
          }

          CurAddr = DstAddr;
          break;
        } // end OpJump

      default: break; // This case should never happen
      } // switch (pLastInsn->GetOperationType())

      if (FunctionIsFinished == true) break;
    } // end while (m_Document.IsPresent(CurAddr))
  } // while (!CallStack.empty())

  std::for_each(std::begin(FuncAddr), std::end(FuncAddr), [&](Address const& rAddr)
  {
    CreateFunction(rDoc, rAddr);
  });
}

void Analyzer::CreateXRefs(Document& rDoc, Address const& rAddr) const
{
  auto pInsn = dynamic_cast<Instruction const *>(GetCell(rDoc, rAddr));
  if (pInsn == nullptr)
    return;

  for (u8 CurOp = 0; CurOp < OPERAND_NO; ++CurOp)
  {
    Address DstAddr;
    if (!pInsn->GetOperandReference(rDoc, CurOp, rAddr, DstAddr))
      continue;

    rDoc.ChangeValueSize(DstAddr, pInsn->GetOperandReferenceLength(CurOp), false);

    // Check if the destination is valid and is an instruction
    Cell* pDstCell = rDoc.RetrieveCell(DstAddr);
    if (pDstCell == nullptr)
      continue;

    // Add XRef
    Address OpAddr;
    if (!pInsn->GetOperandAddress(CurOp, rAddr, OpAddr))
      OpAddr = rAddr;
    rDoc.GetXRefs().AddXRef(DstAddr, OpAddr);

    // If the destination has already a label, we skip it
    if (!rDoc.GetLabelFromAddress(DstAddr).GetName().empty())
      continue;

    std::string SuffixName = DstAddr.ToString();
    std::replace(SuffixName.begin(), SuffixName.end(), ':', '_');

    switch (pInsn->GetOperationType() & (Instruction::OpCall | Instruction::OpJump))
    {
    case Instruction::OpJump:
      rDoc.AddLabel(DstAddr, Label(m_LabelPrefix + SuffixName, Label::Code | Label::Local), false);
      break;

    case Instruction::OpUnknown:
      if (rDoc.GetMemoryArea(DstAddr)->GetAccess() & MA_EXEC)
        rDoc.AddLabel(DstAddr, Label(m_LabelPrefix + SuffixName, Label::Code | Label::Local), false);
      else
        rDoc.AddLabel(DstAddr, Label(m_DataPrefix + SuffixName, Label::Data | Label::Global), false);

    default: break;
    } // switch (pInsn->GetOperationType() & (Instruction::OpCall | Instruction::OpJump))
  } // for (u8 CurOp = 0; CurOp < OPERAND_NO; ++CurOp)
}

bool Analyzer::ComputeFunctionLength(
  Document const& rDoc,
  Address const& rFunctionAddress,
  Address& EndAddress,
  u16& rFunctionLength,
  u16& rInstructionCounter,
  u32 LengthThreshold) const
{
  std::stack<Address> CallStack;
  std::map<Address, bool> VisitedInstruction;
  bool RetReached = false;

  u32 FuncLen                = 0x0;
  Address CurAddr            = rFunctionAddress;
  Address EndAddr            = rFunctionAddress;
  rFunctionLength            = 0x0;
  rInstructionCounter        = 0x0;
  MemoryArea const* pMemArea = rDoc.GetMemoryArea(CurAddr);

  auto Lbl = rDoc.GetLabelFromAddress(rFunctionAddress);
  if (Lbl.GetType() & Label::Imported)
    return false;

  if (pMemArea == nullptr)
    return false;

  CallStack.push(CurAddr);

  while (!CallStack.empty())
  {
    CurAddr = CallStack.top();
    CallStack.pop();

    while (rDoc.ContainsCode(CurAddr))
    {
      Instruction const& rInsn = *static_cast<Instruction const*>(rDoc.RetrieveCell(CurAddr));

      if (VisitedInstruction[CurAddr])
      {
        CurAddr += rInsn.GetLength();
        continue;
      }

      FuncLen += static_cast<u32>(rInsn.GetLength());

      VisitedInstruction[CurAddr] = true;

      rFunctionLength += static_cast<u32>(rInsn.GetLength());
      rInstructionCounter++;

      if (rInsn.GetOperationType() & Instruction::OpJump)
      {
        Address DstAddr;

        if (rInsn.GetOperationType() & Instruction::OpCond)
          CallStack.push(CurAddr + rInsn.GetLength());

        if (rInsn.Operand(0)->GetType() & O_MEM)
          break;

        if (!rInsn.GetOperandReference(rDoc, 0, CurAddr, DstAddr))
          break;

        CurAddr = DstAddr;
        continue;
      }

      else if (rInsn.GetOperationType() & Instruction::OpRet && !(rInsn.GetOperationType() & Instruction::OpCond))
      {
        RetReached = true;
        if (EndAddr < CurAddr)
          EndAddr = CurAddr;
        break;
      }

      CurAddr += rInsn.GetLength();

      if (LengthThreshold && FuncLen > LengthThreshold)
        return false;
    } // end while (m_Document.IsPresent(CurAddr))
  } // while (!CallStack.empty())

  return RetReached;
}

void Analyzer::FindStrings(Document& rDoc, Architecture& rArch) const
{
  Document::LabelBimapType const& rLabels = rDoc.GetLabels();
  for (Document::LabelBimapType::const_iterator It = rLabels.begin();
    It != rLabels.end(); ++It)
  {
    if (It->right.GetType() != Label::Data)
      continue;

    MemoryArea const* pMemArea   = rDoc.GetMemoryArea(It->left);
    if (pMemArea == nullptr)
      continue;

    std::string CurString        = "";
    BinaryStream const& rBinStrm = pMemArea->GetBinaryStream();
    TOffset PhysicalOffset;

    if (pMemArea->Convert(It->left.GetOffset(), PhysicalOffset) == false)
      continue;

    /* UTF-16 */
    WinString WinStr;
    WinString::CharType WinChar;
    CurString = "";

    try
    {
      while (true)
      {
        rBinStrm.Read(PhysicalOffset, WinChar);
        if (!WinStr.IsValidCharacter(WinChar))
          break;
        CurString += WinStr.ConvertToUf8(WinChar);
        PhysicalOffset += sizeof(WinChar);
      }
    }
    catch (Exception&) { CurString = ""; }

    if (WinStr.IsFinalCharacter(WinChar) && !CurString.empty())
    {
      Log::Write("core") << "Found string: " << CurString << LogEnd;
      String *pString = new String(String::Utf16Type, CurString);
      rDoc.InsertCell(It->left, pString, true, true);
      rDoc.SetLabelToAddress(It->left, Label(CurString, m_StringPrefix, Label::String));
      continue;
    }

    // LATER: Redo
    /* ASCII */
    AsciiString AsciiStr;
    AsciiString::CharType AsciiChar;

    try
    {
      while (true)
      {
        rBinStrm.Read(PhysicalOffset, AsciiChar);
        if (!AsciiStr.IsValidCharacter(AsciiChar))
          break;
        CurString += AsciiStr.ConvertToUf8(AsciiChar);
        PhysicalOffset += sizeof(AsciiChar);
      }
    }
    catch (Exception&) { CurString = ""; }

    if (AsciiStr.IsFinalCharacter(AsciiChar) && !CurString.empty())
    {
      Log::Write("core") << "Found string: " << CurString << LogEnd;
      String *pString = new String(String::AsciiType, CurString);
      rDoc.InsertCell(It->left, pString, true, true);
      rDoc.SetLabelToAddress(It->left, Label(CurString, m_StringPrefix, Label::String));
    }
  }
}

bool Analyzer::MakeAsciiString(Document& rDoc, Address const& rAddr) const
{
  try
  {
    s8 CurChar;
    TOffset StrOff;
    std::string StrData = "";
    auto pMemArea       = rDoc.GetMemoryArea(rAddr);
    auto rCurBinStrm    = pMemArea->GetBinaryStream();

    if (pMemArea->Convert(rAddr.GetOffset(), StrOff) == false)
      return false;

    for (;;)
    {
      rCurBinStrm.Read(StrOff, CurChar);
      if (CurChar == '\0') break;

      StrData += CurChar;
      ++StrOff;
    }

    if (StrData.length() == 0) return false;

    auto pStr = new String(String::AsciiType, StrData);
    rDoc.InsertCell(rAddr, pStr, true);
    rDoc.AddLabel(rAddr, Label(m_StringPrefix + pStr->GetCharacters(), Label::String | Label::Global));
  }
  catch (Exception const&)
  {
    return false;
  }

  return true;
}

bool Analyzer::MakeWindowsString(Document& rDoc, Address const& rAddr) const
{
  try
  {
    TOffset StrStartOff, StrOff;
    std::string StrData = "";
    auto pMemArea       = rDoc.GetMemoryArea(rAddr);
    auto rCurBinStrm    = pMemArea->GetBinaryStream();
    WinString WinStr;
    WinString::CharType CurChar;

    if (pMemArea->Convert(rAddr.GetOffset(), StrOff) == false)
      return false;

    StrStartOff = StrOff;

    bool EndReached = false;
    do
    {
      rCurBinStrm.Read(StrOff, CurChar);
      if (WinStr.IsFinalCharacter(CurChar)) EndReached = true;

      if (EndReached == false)
        StrData += WinStr.ConvertToUf8(CurChar);
      StrOff += sizeof(CurChar);
    } while (EndReached == false);

    if (StrData.length() == 0) return false;

    auto pStr = new String(String::Utf16Type, StrData, static_cast<u16>(StrOff - StrStartOff));
    rDoc.InsertCell(rAddr, pStr, true);
    rDoc.AddLabel(rAddr, Label(m_StringPrefix + pStr->GetCharacters(), Label::String | Label::Global));
  }
  catch (Exception const&)
  {
    return false;
  }

  return true;
}

bool Analyzer::CreateFunction(Document& rDoc, Address const& rAddr) const
{
  std::string SuffixName = rAddr.ToString();
  std::replace(SuffixName.begin(), SuffixName.end(), ':', '_');
  Address FuncEnd;
  u16 FuncLen;
  u16 InsnCnt;
  std::string FuncName = m_FunctionPrefix + SuffixName;

  if (ComputeFunctionLength(rDoc, rAddr, FuncEnd, FuncLen, InsnCnt, 0x1000) == true)
  {
    Log::Write("core")
      << "Function found"
      << ": address="               << rAddr.ToString()
      << ", length="                << FuncLen
      << ", instruction counter: "  << InsnCnt
      << LogEnd;

    ControlFlowGraph Cfg;
    if (BuildControlFlowGraph(rDoc, rAddr, Cfg) == false)
    {
      Log::Write("core")
        << "Unable to build control flow graph for " << rAddr.ToString() << LogEnd;
      return false;
    }

    Function* pFunction = new Function(FuncLen, InsnCnt, Cfg);
    rDoc.InsertMultiCell(rAddr, pFunction, false);
  }
  else
  {
    auto pMemArea = rDoc.GetMemoryArea(rAddr);
    if (pMemArea == nullptr)
      return false;
    auto rBinStrm = pMemArea->GetBinaryStream();
    auto pInsn = GetCell(rDoc, rAddr);
    if (pInsn == nullptr)
      return false;
    auto spArch = GetArchitecture(pInsn->GetArchitectureTag());
    auto pFuncInsn = static_cast<Instruction const*>(GetCell(rDoc, rAddr));
    if (pFuncInsn->GetOperationType() != Instruction::OpJump)
      return false;
    Address OpRefAddr;
    if (pFuncInsn->GetOperandReference(rDoc, 0, rAddr, OpRefAddr) == false)
      return false;
    auto OpLbl = rDoc.GetLabelFromAddress(OpRefAddr);
    if (OpLbl.GetType() == Label::Unknown)
      return false;
    FuncName = std::string(pFuncInsn->GetName()) + std::string("_") + OpLbl.GetLabel();
  }

  rDoc.AddLabel(rAddr, Label(FuncName, Label::Code | Label::Global), false);
  return true;
}

bool Analyzer::BuildControlFlowGraph(Document& rDoc, std::string const& rLblName, ControlFlowGraph& rCfg) const
{
  Address const& rLblAddr = rDoc.GetAddressFromLabelName(rLblName);
  if (rLblAddr.GetAddressingType() == Address::UnknownType) return false;

  return BuildControlFlowGraph(rDoc, rLblAddr, rCfg);
}

bool Analyzer::BuildControlFlowGraph(Document& rDoc, Address const& rAddr, ControlFlowGraph& rCfg) const
{
  std::stack<Address> CallStack;
  Address::List Addresses;
  typedef std::tuple<Address, Address, BasicBlockEdgeProperties::Type> TupleEdge;
  std::list<TupleEdge> Edges;
  std::map<Address, bool> VisitedInstruction;
  bool RetReached = false;

  Address CurAddr = rAddr;

  MemoryArea const* pMemArea = rDoc.GetMemoryArea(CurAddr);

  if (pMemArea == nullptr)
    return false;

  CallStack.push(CurAddr);

  while (!CallStack.empty())
  {
    CurAddr = CallStack.top();
    CallStack.pop();

    while (rDoc.ContainsCode(CurAddr))
    {
      Instruction const& rInsn = *static_cast<Instruction const*>(rDoc.RetrieveCell(CurAddr));

      // If the current address is already visited
      if (VisitedInstruction[CurAddr])
      {
        // ... and if the current instruction is the end of the function, we take another address from the callstack
        if (rInsn.GetOperationType() & Instruction::OpRet && !(rInsn.GetOperationType() & Instruction::OpCond))
          break;

        // if not, we try with the next address.
        CurAddr += rInsn.GetLength();
        continue;
      }

      Addresses.push_back(CurAddr);
      VisitedInstruction[CurAddr] = true;

      if (rInsn.GetOperationType() & Instruction::OpJump)
      {
        Address DstAddr;

        if (rInsn.Operand(0)->GetType() & O_MEM)
          break;

         if (!rInsn.GetOperandReference(rDoc, 0, CurAddr, DstAddr))
          break;

        if (rInsn.GetOperationType() & Instruction::OpCond)
        {
          Address NextAddr = CurAddr + rInsn.GetLength();
          Edges.push_back(TupleEdge(DstAddr, CurAddr,  BasicBlockEdgeProperties::True ));
          Edges.push_back(TupleEdge(NextAddr, CurAddr, BasicBlockEdgeProperties::False));
          CallStack.push(NextAddr);
        }
        else
        {
          Edges.push_back(TupleEdge(DstAddr, CurAddr, BasicBlockEdgeProperties::Unconditional));
        }

        CurAddr = DstAddr;
        continue;
      }

      else if (rInsn.GetOperationType() & Instruction::OpRet && !(rInsn.GetOperationType() & Instruction::OpCond))
      {
        RetReached = true;
        break;
      }

      CurAddr += rInsn.GetLength();
    } // end while (m_Document.IsPresent(CurAddr))
  } // while (!CallStack.empty())

  BasicBlockVertexProperties FirstBasicBlock(Addresses);
  rCfg.AddBasicBlockVertex(FirstBasicBlock);

  for (auto itEdge = std::begin(Edges); itEdge != std::end(Edges); ++itEdge)
  {
    static const char *TypeStr[] =
    {
      "Unknown",
      "Unconditional",
      "True",
      "False"
    };
    bool Res = rCfg.SplitBasicBlock(std::get<0>(*itEdge), std::get<1>(*itEdge), std::get<2>(*itEdge));
    Log::Write("core") << "dst: " << std::get<0>(*itEdge) << ", src: " << std::get<1>(*itEdge) << ", type: " << TypeStr[std::get<2>(*itEdge)] << (Res ? ", succeed" : ", failed") << LogEnd;
  }

  for (auto itEdge = std::begin(Edges); itEdge != std::end(Edges); ++itEdge)
    rCfg.AddBasicBlockEdge(BasicBlockEdgeProperties(std::get<2>(*itEdge)), std::get<1>(*itEdge), std::get<0>(*itEdge));

  rCfg.Finalize(rDoc);

  return RetReached;
}

bool Analyzer::DisassembleBasicBlock(Document const& rDoc, Architecture& rArch, Address const& rAddr, std::list<Instruction*>& rBasicBlock)
{
  Address CurAddr = rAddr;
  MemoryArea const* pMemArea = rDoc.GetMemoryArea(CurAddr);
  bool Res = rArch.DisassembleBasicBlockOnly() == false ? true : false;

  auto Lbl = rDoc.GetLabelFromAddress(rAddr);
  if (Lbl.GetType() & Label::Imported)
    return false;

  if (pMemArea == nullptr)
    goto exit;

  while (rDoc.IsPresent(CurAddr))
  {
    // If we changed the current memory area, we must update it
    if (!pMemArea->IsPresent(CurAddr))
      if ((pMemArea = rDoc.GetMemoryArea(CurAddr)) == nullptr)
        goto exit;

    // If the current memory area is not executable, we skip this execution flow
    if (!(pMemArea->GetAccess() & MA_EXEC))
      goto exit;

    auto pCurCell = rDoc.RetrieveCell(CurAddr);

    if (pCurCell == nullptr)
      goto exit;

    if (pCurCell->GetType() != CellData::ValueType || pCurCell->GetLength() != 1)
      goto exit;

    // We create a new entry and disassemble it
    Instruction* pInsn = new Instruction;

    try
    {
      TOffset PhysicalOffset;

      PhysicalOffset = CurAddr.GetOffset() - pMemArea->GetVirtualBase().GetOffset();

      // If something bad happens, we skip this instruction and go to the next function
      if (!rArch.Disassemble(pMemArea->GetBinaryStream(), PhysicalOffset, *pInsn))
        throw Exception(L"Unable to disassemble this instruction");
    }
    catch (Exception const& e)
    {
      Log::Write("core")
        << "Exception while disassemble instruction at " << CurAddr.ToString()
        << ", reason: " << e.What()
        << LogEnd;
      delete pInsn;
      goto exit;
    }

    // We try to retrieve the current instruction, if it's true we go to the next function
    for (size_t InsnLen = 0; InsnLen < pInsn->GetLength(); ++InsnLen)
      if (rDoc.ContainsCode(CurAddr + InsnLen))
      {
        Res = true;
        goto exit;
      }

      rBasicBlock.push_back(pInsn);

      auto OpType = pInsn->GetOperationType();
      if (
        OpType & Instruction::OpJump
        || OpType & Instruction::OpCall
        || OpType & Instruction::OpRet)
      {
        Res = true;
        goto exit;
      }

      CurAddr += pInsn->GetLength();
  } // !while (rDoc.IsPresent(CurAddr))

exit:
  if (Res == false)
  {
    for (auto itInsn = std::begin(rBasicBlock); itInsn != std::end(rBasicBlock); ++itInsn)
      delete *itInsn;
    rBasicBlock.erase(std::begin(rBasicBlock), std::end(rBasicBlock));
  }
  return Res;
}

bool Analyzer::RegisterArchitecture(Architecture::SharedPtr spArch)
{
  u8 Id = 0;
  bool FoundId = false;

  for (u8 i = 0; i < 32; ++i)
    if (!(m_ArchIdPool & (1 << i)))
    {
      m_ArchIdPool |= (1 << i);
      Id = i;
      FoundId = true;
      break;
    }

    if (FoundId == false) return false;

    spArch->UpdateId(Id);

    m_UsedArchitectures[spArch->GetTag()] = spArch;

    if (m_DefaultArchitectureTag == MEDUSA_ARCH_UNK)
      m_DefaultArchitectureTag = spArch->GetTag();

    return true;
}

bool Analyzer::UnregisterArchitecture(Architecture::SharedPtr spArch)
{
  return false; /* Not implemented */
}

void Analyzer::ResetArchitecture(void)
{
  m_UsedArchitectures.erase(std::begin(m_UsedArchitectures), std::end(m_UsedArchitectures));
  m_DefaultArchitectureTag = MEDUSA_ARCH_UNK;
}

Cell* Analyzer::GetCell(Document& rDoc, Address const& rAddr)
{
  //boost::lock_guard<MutexType> Lock(m_Mutex);
  Cell* pCell = rDoc.RetrieveCell(rAddr);
  if (pCell == nullptr)
    return nullptr;

  return pCell;
}

Cell const* Analyzer::GetCell(Document const& rDoc, Address const& rAddr) const
{
  //boost::lock_guard<MutexType> Lock(m_Mutex);
  Cell* pCell = const_cast<Cell*>(rDoc.RetrieveCell(rAddr));
  if (pCell == nullptr)
    return nullptr;

  return pCell;
}

bool Analyzer::FormatCell(
  Document      const& rDoc,
  BinaryStream  const& rBinStrm,
  Address       const& rAddress,
  Cell          const& rCell,
  std::string        & rStrCell,
  Cell::Mark::List   & rMarks) const
{
  auto spArch = GetArchitecture(rCell.GetArchitectureTag());
  if (!spArch)
    return false;
  return spArch->FormatCell(rDoc, rBinStrm, rAddress, rCell, rStrCell, rMarks);
}

MultiCell* Analyzer::GetMultiCell(Document& rDoc, Address const& rAddr)
{
  MultiCell* pMultiCell = rDoc.RetrieveMultiCell(rAddr);
  if (pMultiCell == nullptr)
    return nullptr;

  return pMultiCell;
}

MultiCell const* Analyzer::GetMultiCell(Document const& rDoc, Address const& rAddr) const
{
  MultiCell const* pMultiCell = rDoc.RetrieveMultiCell(rAddr);
  if (pMultiCell == nullptr)
    return nullptr;

  return pMultiCell;
}

bool Analyzer::FormatMultiCell(
  Document      const& rDoc,
  BinaryStream  const& rBinStrm,
  Address       const& rAddress,
  MultiCell     const& rMultiCell,
  std::string        & rStrMultiCell,
  Cell::Mark::List   & rMarks) const
{
  auto spArch = GetArchitecture(m_DefaultArchitectureTag);
  if (!spArch)
    return false;
  return spArch->FormatMultiCell(rDoc, rBinStrm, rAddress, rMultiCell, rStrMultiCell, rMarks);
}

Architecture::SharedPtr Analyzer::GetArchitecture(Tag ArchTag) const
{
  if (ArchTag == MEDUSA_ARCH_UNK)
    ArchTag = m_DefaultArchitectureTag;

  auto itArch = m_UsedArchitectures.find(ArchTag);
  if (itArch == std::end(m_UsedArchitectures))
    return Architecture::SharedPtr();

  return itArch->second;
}

// Workaround from http://stackoverflow.com/questions/9669109/print-a-constified-subgraph-with-write-graphviz
template<typename Graph> struct PropWriter
{
  PropWriter(Graph const& rCfg, Analyzer const& rAnlz, Document const& rDoc, BinaryStream const& rBinStrm)
    : m_rCfg(rCfg), m_rAnlz(rAnlz), m_rDoc(rDoc), m_rBinStrm(rBinStrm) {}
  template<typename Vertex> void operator()(std::ostream & out, Vertex const& v) const
  {
    out << "[shape=box] [label=\"";
    for (auto itAddr = std::begin(m_rCfg[v].GetAddresses()); itAddr != std::end(m_rCfg[v].GetAddresses()); ++itAddr)
    {
      std::string LineString = "Unknown";
      auto pCell = m_rAnlz.GetCell(m_rDoc, *itAddr);
      if (pCell != nullptr)
        return;
      Cell::Mark::List Marks;
      if (m_rAnlz.FormatCell(m_rDoc, m_rBinStrm, *itAddr, *pCell, LineString, Marks) == false)
        continue;
      auto Cmt = pCell->GetComment();
      if (!Cmt.empty())
      {
        LineString += std::string(" ; ");
        LineString += Cmt;
      }

      out << *itAddr << ": " << LineString << "\\n";
    }
    out << "\"]";
  }

private:
  Graph        const& m_rCfg;
  Analyzer     const& m_rAnlz;
  Document     const& m_rDoc;
  BinaryStream const& m_rBinStrm;
};

void Analyzer::DumpControlFlowGraph(std::string const& rFilename, ControlFlowGraph const& rCfg, Document const& rDoc, BinaryStream const& rBinStrm) const
{
  std::ofstream File(rFilename.c_str());
  boost::write_graphviz(File, rCfg.GetGraph(), PropWriter<ControlFlowGraph::Type>(rCfg.GetGraph(), *this, rDoc, rBinStrm));
}

void Analyzer::TrackOperand(Document& rDoc, Address const& rStartAddress, Tracker& rTracker)
{
  std::map<Address, bool> TrackedAddresses;

  Address::List FuncAddrs;
  rDoc.FindFunctionAddressFromAddress(FuncAddrs, rStartAddress);

  if (!FuncAddrs.empty()) std::for_each(std::begin(FuncAddrs), std::end(FuncAddrs), [this, &rDoc, &rTracker, &TrackedAddresses, &rStartAddress](Address const& rFuncAddr)
  {
    auto pFunc = dynamic_cast<Function const*>(GetMultiCell(rDoc, rFuncAddr));
    if (pFunc == nullptr)
      return;

    auto rCfg = pFunc->GetControlFlowGraph();
    Address::List AllAddrs;
    AllAddrs.push_back(rStartAddress);

    while (!AllAddrs.empty())
    {
      auto Addr = AllAddrs.front();
      AllAddrs.pop_front();
      if (TrackedAddresses[Addr])
        continue;
      TrackedAddresses[Addr] = true;
      if (rTracker(*this, rDoc, Addr) && !rCfg.GetNextAddress(Addr, AllAddrs))
        return;
    }
  });

  else
  {
    Address CurAddr = rStartAddress;
    while (rDoc.MoveAddress(CurAddr, CurAddr, 1))
    {
      if (!rTracker(*this, rDoc, CurAddr))
        break;
    }
  }
}

void Analyzer::BacktrackOperand(Document& rDoc, Address const& rStartAddress, Tracker& rTracker)
{
  std::map<Address, bool> TrackedAddresses;

  Address::List FuncAddrs;
  rDoc.FindFunctionAddressFromAddress(FuncAddrs, rStartAddress);

  if (!FuncAddrs.empty()) std::for_each(std::begin(FuncAddrs), std::end(FuncAddrs), [this, &rDoc, &rTracker, &TrackedAddresses, &rStartAddress](Address const& rFuncAddr)
  {
    auto pFunc = dynamic_cast<Function const*>(GetMultiCell(rDoc, rFuncAddr));
    if (pFunc == nullptr)
      return;

    auto rCfg = pFunc->GetControlFlowGraph();
    Address::List AllAddrs;
    AllAddrs.push_back(rStartAddress);

    while (!AllAddrs.empty())
    {
      auto Addr = AllAddrs.front();
      AllAddrs.pop_front();
      if (TrackedAddresses[Addr])
        continue;
      TrackedAddresses[Addr] = true;
      if (rTracker(*this, rDoc, Addr) == false || rCfg.GetPreviousAddress(Addr, AllAddrs) == false)
        return;
    }
  });

  else
  {
    Address CurAddr = rStartAddress;
    while (rDoc.MoveAddress(CurAddr, CurAddr, -1))
    {
      if (!rTracker(*this, rDoc, CurAddr))
        break;
    }
  }
}

MEDUSA_NAMESPACE_END
