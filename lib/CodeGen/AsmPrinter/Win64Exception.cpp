//===-- CodeGen/AsmPrinter/Win64Exception.cpp - Dwarf Exception Impl ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing Win64 exception info into asm files.
//
//===----------------------------------------------------------------------===//

#include "Win64Exception.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/WinEHFuncInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegisterInfo.h"
using namespace llvm;

Win64Exception::Win64Exception(AsmPrinter *A)
  : EHStreamer(A), shouldEmitPersonality(false), shouldEmitLSDA(false),
    shouldEmitMoves(false) {}

Win64Exception::~Win64Exception() {}

/// endModule - Emit all exception information that should come after the
/// content.
void Win64Exception::endModule() {
}

void Win64Exception::beginFunction(const MachineFunction *MF) {
  shouldEmitMoves = shouldEmitPersonality = shouldEmitLSDA = false;

  // If any landing pads survive, we need an EH table.
  bool hasLandingPads = !MMI->getLandingPads().empty();

  shouldEmitMoves = Asm->needsSEHMoves();

  const TargetLoweringObjectFile &TLOF = Asm->getObjFileLowering();
  unsigned PerEncoding = TLOF.getPersonalityEncoding();
  const Function *Per = MF->getMMI().getPersonality();

  shouldEmitPersonality = hasLandingPads &&
    PerEncoding != dwarf::DW_EH_PE_omit && Per;

  unsigned LSDAEncoding = TLOF.getLSDAEncoding();
  shouldEmitLSDA = shouldEmitPersonality &&
    LSDAEncoding != dwarf::DW_EH_PE_omit;


  // If this was an outlined handler, we need to define the label corresponding
  // to the offset of the parent frame relative to the stack pointer after the
  // prologue.
  const Function *F = MF->getFunction();
  const Function *ParentF = MMI->getWinEHParent(F);
  if (F != ParentF) {
    WinEHFuncInfo &FuncInfo = MMI->getWinEHFuncInfo(ParentF);
    auto I = FuncInfo.CatchHandlerParentFrameObjOffset.find(F);
    if (I != FuncInfo.CatchHandlerParentFrameObjOffset.end()) {
      MCSymbol *HandlerTypeParentFrameOffset =
          Asm->OutContext.getOrCreateParentFrameOffsetSymbol(
              GlobalValue::getRealLinkageName(F->getName()));

      // Emit a symbol assignment.
      Asm->OutStreamer.EmitAssignment(
          HandlerTypeParentFrameOffset,
          MCConstantExpr::Create(I->second, Asm->OutContext));
    }
  }

  if (!shouldEmitPersonality && !shouldEmitMoves)
    return;

  Asm->OutStreamer.EmitWinCFIStartProc(Asm->CurrentFnSym);

  if (!shouldEmitPersonality)
    return;

  const MCSymbol *PersHandlerSym =
      TLOF.getCFIPersonalitySymbol(Per, *Asm->Mang, Asm->TM, MMI);
  Asm->OutStreamer.EmitWinEHHandler(PersHandlerSym, true, true);
}

/// endFunction - Gather and emit post-function exception information.
///
void Win64Exception::endFunction(const MachineFunction *MF) {
  if (!shouldEmitPersonality && !shouldEmitMoves)
    return;

  // Map all labels and get rid of any dead landing pads.
  MMI->TidyLandingPads();

  if (shouldEmitPersonality) {
    Asm->OutStreamer.PushSection();

    // Emit an UNWIND_INFO struct describing the prologue.
    Asm->OutStreamer.EmitWinEHHandlerData();

    // Emit the tables appropriate to the personality function in use. If we
    // don't recognize the personality, assume it uses an Itanium-style LSDA.
    EHPersonality Per = MMI->getPersonalityType();
    if (Per == EHPersonality::MSVC_Win64SEH)
      emitCSpecificHandlerTable();
    else if (Per == EHPersonality::MSVC_CXX)
      emitCXXFrameHandler3Table(MF);
    else
      emitExceptionTable();

    Asm->OutStreamer.PopSection();
  }
  Asm->OutStreamer.EmitWinCFIEndProc();
}

const MCExpr *Win64Exception::createImageRel32(const MCSymbol *Value) {
  if (!Value)
    return MCConstantExpr::Create(0, Asm->OutContext);
  return MCSymbolRefExpr::Create(Value, MCSymbolRefExpr::VK_COFF_IMGREL32,
                                 Asm->OutContext);
}

const MCExpr *Win64Exception::createImageRel32(const GlobalValue *GV) {
  if (!GV)
    return MCConstantExpr::Create(0, Asm->OutContext);
  return createImageRel32(Asm->getSymbol(GV));
}

/// Emit the language-specific data that __C_specific_handler expects.  This
/// handler lives in the x64 Microsoft C runtime and allows catching or cleaning
/// up after faults with __try, __except, and __finally.  The typeinfo values
/// are not really RTTI data, but pointers to filter functions that return an
/// integer (1, 0, or -1) indicating how to handle the exception. For __finally
/// blocks and other cleanups, the landing pad label is zero, and the filter
/// function is actually a cleanup handler with the same prototype.  A catch-all
/// entry is modeled with a null filter function field and a non-zero landing
/// pad label.
///
/// Possible filter function return values:
///   EXCEPTION_EXECUTE_HANDLER (1):
///     Jump to the landing pad label after cleanups.
///   EXCEPTION_CONTINUE_SEARCH (0):
///     Continue searching this table or continue unwinding.
///   EXCEPTION_CONTINUE_EXECUTION (-1):
///     Resume execution at the trapping PC.
///
/// Inferred table structure:
///   struct Table {
///     int NumEntries;
///     struct Entry {
///       imagerel32 LabelStart;
///       imagerel32 LabelEnd;
///       imagerel32 FilterOrFinally;  // One means catch-all.
///       imagerel32 LabelLPad;        // Zero means __finally.
///     } Entries[NumEntries];
///   };
void Win64Exception::emitCSpecificHandlerTable() {
  const std::vector<LandingPadInfo> &PadInfos = MMI->getLandingPads();

  // Simplifying assumptions for first implementation:
  // - Cleanups are not implemented.
  // - Filters are not implemented.

  // The Itanium LSDA table sorts similar landing pads together to simplify the
  // actions table, but we don't need that.
  SmallVector<const LandingPadInfo *, 64> LandingPads;
  LandingPads.reserve(PadInfos.size());
  for (const auto &LP : PadInfos)
    LandingPads.push_back(&LP);

  // Compute label ranges for call sites as we would for the Itanium LSDA, but
  // use an all zero action table because we aren't using these actions.
  SmallVector<unsigned, 64> FirstActions;
  FirstActions.resize(LandingPads.size());
  SmallVector<CallSiteEntry, 64> CallSites;
  computeCallSiteTable(CallSites, LandingPads, FirstActions);

  MCSymbol *EHFuncBeginSym = Asm->getFunctionBegin();
  MCSymbol *EHFuncEndSym = Asm->getFunctionEnd();

  // Emit the number of table entries.
  unsigned NumEntries = 0;
  for (const CallSiteEntry &CSE : CallSites) {
    if (!CSE.LPad)
      continue; // Ignore gaps.
    for (int Selector : CSE.LPad->TypeIds) {
      // Ignore C++ filter clauses in SEH.
      // FIXME: Implement cleanup clauses.
      if (isCatchEHSelector(Selector))
        ++NumEntries;
    }
  }
  Asm->OutStreamer.EmitIntValue(NumEntries, 4);

  // Emit the four-label records for each call site entry. The table has to be
  // sorted in layout order, and the call sites should already be sorted.
  for (const CallSiteEntry &CSE : CallSites) {
    // Ignore gaps. Unlike the Itanium model, unwinding through a frame without
    // an EH table entry will propagate the exception rather than terminating
    // the program.
    if (!CSE.LPad)
      continue;
    const LandingPadInfo *LPad = CSE.LPad;

    // Compute the label range. We may reuse the function begin and end labels
    // rather than forming new ones.
    const MCExpr *Begin =
        createImageRel32(CSE.BeginLabel ? CSE.BeginLabel : EHFuncBeginSym);
    const MCExpr *End;
    if (CSE.EndLabel) {
      // The interval is half-open, so we have to add one to include the return
      // address of the last invoke in the range.
      End = MCBinaryExpr::CreateAdd(createImageRel32(CSE.EndLabel),
                                    MCConstantExpr::Create(1, Asm->OutContext),
                                    Asm->OutContext);
    } else {
      End = createImageRel32(EHFuncEndSym);
    }

    // These aren't really type info globals, they are actually pointers to
    // filter functions ordered by selector. The zero selector is used for
    // cleanups, so slot zero corresponds to selector 1.
    const std::vector<const GlobalValue *> &SelectorToFilter = MMI->getTypeInfos();

    // Do a parallel iteration across typeids and clause labels, skipping filter
    // clauses.
    size_t NextClauseLabel = 0;
    for (size_t I = 0, E = LPad->TypeIds.size(); I < E; ++I) {
      // AddLandingPadInfo stores the clauses in reverse, but there is a FIXME
      // to change that.
      int Selector = LPad->TypeIds[E - I - 1];

      // Ignore C++ filter clauses in SEH.
      // FIXME: Implement cleanup clauses.
      if (!isCatchEHSelector(Selector))
        continue;

      Asm->OutStreamer.EmitValue(Begin, 4);
      Asm->OutStreamer.EmitValue(End, 4);
      if (isCatchEHSelector(Selector)) {
        assert(unsigned(Selector - 1) < SelectorToFilter.size());
        const GlobalValue *TI = SelectorToFilter[Selector - 1];
        if (TI) // Emit the filter function pointer.
          Asm->OutStreamer.EmitValue(createImageRel32(Asm->getSymbol(TI)), 4);
        else  // Otherwise, this is a "catch i8* null", or catch all.
          Asm->OutStreamer.EmitIntValue(1, 4);
      }
      MCSymbol *ClauseLabel = LPad->ClauseLabels[NextClauseLabel++];
      Asm->OutStreamer.EmitValue(createImageRel32(ClauseLabel), 4);
    }
  }
}

void Win64Exception::emitCXXFrameHandler3Table(const MachineFunction *MF) {
  const Function *F = MF->getFunction();
  const Function *ParentF = MMI->getWinEHParent(F);
  auto &OS = Asm->OutStreamer;
  WinEHFuncInfo &FuncInfo = MMI->getWinEHFuncInfo(ParentF);

  StringRef ParentLinkageName =
      GlobalValue::getRealLinkageName(ParentF->getName());

  MCSymbol *FuncInfoXData =
      Asm->OutContext.GetOrCreateSymbol(Twine("$cppxdata$", ParentLinkageName));
  OS.EmitValue(createImageRel32(FuncInfoXData), 4);

  // The Itanium LSDA table sorts similar landing pads together to simplify the
  // actions table, but we don't need that.
  SmallVector<const LandingPadInfo *, 64> LandingPads;
  const std::vector<LandingPadInfo> &PadInfos = MMI->getLandingPads();
  LandingPads.reserve(PadInfos.size());
  for (const auto &LP : PadInfos)
    LandingPads.push_back(&LP);

  RangeMapType PadMap;
  computePadMap(LandingPads, PadMap);

  // The end label of the previous invoke or nounwind try-range.
  MCSymbol *LastLabel = Asm->getFunctionBegin();

  // Whether there is a potentially throwing instruction (currently this means
  // an ordinary call) between the end of the previous try-range and now.
  bool SawPotentiallyThrowing = false;

  int LastEHState = -2;

  // The parent function and the catch handlers contribute to the 'ip2state'
  // table.
  for (const auto &MBB : *MF) {
    for (const auto &MI : MBB) {
      if (!MI.isEHLabel()) {
        if (MI.isCall())
          SawPotentiallyThrowing |= !callToNoUnwindFunction(&MI);
        continue;
      }

      // End of the previous try-range?
      MCSymbol *BeginLabel = MI.getOperand(0).getMCSymbol();
      if (BeginLabel == LastLabel)
        SawPotentiallyThrowing = false;

      // Beginning of a new try-range?
      RangeMapType::const_iterator L = PadMap.find(BeginLabel);
      if (L == PadMap.end())
        // Nope, it was just some random label.
        continue;

      const PadRange &P = L->second;
      const LandingPadInfo *LandingPad = LandingPads[P.PadIndex];
      assert(BeginLabel == LandingPad->BeginLabels[P.RangeIndex] &&
             "Inconsistent landing pad map!");

      if (SawPotentiallyThrowing) {
        FuncInfo.IPToStateList.push_back(std::make_pair(LastLabel, -1));
        SawPotentiallyThrowing = false;
        LastEHState = -1;
      }

      if (LandingPad->WinEHState != LastEHState)
        FuncInfo.IPToStateList.push_back(
            std::make_pair(BeginLabel, LandingPad->WinEHState));
      LastEHState = LandingPad->WinEHState;
      LastLabel = LandingPad->EndLabels[P.RangeIndex];
    }
  }

  if (ParentF != F)
    return;

  MCSymbol *UnwindMapXData = nullptr;
  MCSymbol *TryBlockMapXData = nullptr;
  MCSymbol *IPToStateXData = nullptr;
  if (!FuncInfo.UnwindMap.empty())
    UnwindMapXData = Asm->OutContext.GetOrCreateSymbol(
        Twine("$stateUnwindMap$", ParentLinkageName));
  if (!FuncInfo.TryBlockMap.empty())
    TryBlockMapXData = Asm->OutContext.GetOrCreateSymbol(
        Twine("$tryMap$", ParentLinkageName));
  if (!FuncInfo.IPToStateList.empty())
    IPToStateXData = Asm->OutContext.GetOrCreateSymbol(
        Twine("$ip2state$", ParentLinkageName));

  // FuncInfo {
  //   uint32_t           MagicNumber
  //   int32_t            MaxState;
  //   UnwindMapEntry    *UnwindMap;
  //   uint32_t           NumTryBlocks;
  //   TryBlockMapEntry  *TryBlockMap;
  //   uint32_t           IPMapEntries;
  //   IPToStateMapEntry *IPToStateMap;
  //   uint32_t           UnwindHelp; // (x64/ARM only)
  //   ESTypeList        *ESTypeList;
  //   int32_t            EHFlags;
  // }
  // EHFlags & 1 -> Synchronous exceptions only, no async exceptions.
  // EHFlags & 2 -> ???
  // EHFlags & 4 -> The function is noexcept(true), unwinding can't continue.
  OS.EmitLabel(FuncInfoXData);
  OS.EmitIntValue(0x19930522, 4);                      // MagicNumber
  OS.EmitIntValue(FuncInfo.UnwindMap.size(), 4);       // MaxState
  OS.EmitValue(createImageRel32(UnwindMapXData), 4);   // UnwindMap
  OS.EmitIntValue(FuncInfo.TryBlockMap.size(), 4);     // NumTryBlocks
  OS.EmitValue(createImageRel32(TryBlockMapXData), 4); // TryBlockMap
  OS.EmitIntValue(FuncInfo.IPToStateList.size(), 4);   // IPMapEntries
  OS.EmitValue(createImageRel32(IPToStateXData), 4);   // IPToStateMap
  OS.EmitIntValue(FuncInfo.UnwindHelpFrameOffset, 4);  // UnwindHelp
  OS.EmitIntValue(0, 4);                               // ESTypeList
  OS.EmitIntValue(1, 4);                               // EHFlags

  // UnwindMapEntry {
  //   int32_t ToState;
  //   void  (*Action)();
  // };
  if (UnwindMapXData) {
    OS.EmitLabel(UnwindMapXData);
    for (const WinEHUnwindMapEntry &UME : FuncInfo.UnwindMap) {
      OS.EmitIntValue(UME.ToState, 4);                // ToState
      OS.EmitValue(createImageRel32(UME.Cleanup), 4); // Action
    }
  }

  // TryBlockMap {
  //   int32_t      TryLow;
  //   int32_t      TryHigh;
  //   int32_t      CatchHigh;
  //   int32_t      NumCatches;
  //   HandlerType *HandlerArray;
  // };
  if (TryBlockMapXData) {
    OS.EmitLabel(TryBlockMapXData);
    SmallVector<MCSymbol *, 1> HandlerMaps;
    for (size_t I = 0, E = FuncInfo.TryBlockMap.size(); I != E; ++I) {
      WinEHTryBlockMapEntry &TBME = FuncInfo.TryBlockMap[I];
      MCSymbol *HandlerMapXData = nullptr;

      if (!TBME.HandlerArray.empty())
        HandlerMapXData =
            Asm->OutContext.GetOrCreateSymbol(Twine("$handlerMap$")
                                                  .concat(Twine(I))
                                                  .concat("$")
                                                  .concat(ParentLinkageName));

      HandlerMaps.push_back(HandlerMapXData);

      int CatchHigh = -1;
      for (WinEHHandlerType &HT : TBME.HandlerArray)
        CatchHigh =
            std::max(CatchHigh, FuncInfo.CatchHandlerMaxState[HT.Handler]);

      assert(TBME.TryLow <= TBME.TryHigh);
      assert(CatchHigh > TBME.TryHigh);
      OS.EmitIntValue(TBME.TryLow, 4);                    // TryLow
      OS.EmitIntValue(TBME.TryHigh, 4);                   // TryHigh
      OS.EmitIntValue(CatchHigh, 4);                      // CatchHigh
      OS.EmitIntValue(TBME.HandlerArray.size(), 4);       // NumCatches
      OS.EmitValue(createImageRel32(HandlerMapXData), 4); // HandlerArray
    }

    for (size_t I = 0, E = FuncInfo.TryBlockMap.size(); I != E; ++I) {
      WinEHTryBlockMapEntry &TBME = FuncInfo.TryBlockMap[I];
      MCSymbol *HandlerMapXData = HandlerMaps[I];
      if (!HandlerMapXData)
        continue;
      // HandlerType {
      //   int32_t         Adjectives;
      //   TypeDescriptor *Type;
      //   int32_t         CatchObjOffset;
      //   void          (*Handler)();
      //   int32_t         ParentFrameOffset; // x64 only
      // };
      OS.EmitLabel(HandlerMapXData);
      for (const WinEHHandlerType &HT : TBME.HandlerArray) {
        MCSymbol *ParentFrameOffset =
            Asm->OutContext.getOrCreateParentFrameOffsetSymbol(
                GlobalValue::getRealLinkageName(HT.Handler->getName()));
        const MCSymbolRefExpr *ParentFrameOffsetRef = MCSymbolRefExpr::Create(
            ParentFrameOffset, MCSymbolRefExpr::VK_None, Asm->OutContext);

        // Get the frame escape label with the offset of the catch object. If
        // the index is -1, then there is no catch object, and we should emit an
        // offset of zero, indicating that no copy will occur.
        const MCExpr *FrameAllocOffsetRef = nullptr;
        if (HT.CatchObjRecoverIdx >= 0) {
          MCSymbol *FrameAllocOffset =
              Asm->OutContext.getOrCreateFrameAllocSymbol(
                  GlobalValue::getRealLinkageName(F->getName()),
                  HT.CatchObjRecoverIdx);
          FrameAllocOffsetRef = MCSymbolRefExpr::Create(
              FrameAllocOffset, MCSymbolRefExpr::VK_None, Asm->OutContext);
        } else {
          FrameAllocOffsetRef = MCConstantExpr::Create(0, Asm->OutContext);
        }

        OS.EmitIntValue(HT.Adjectives, 4);                    // Adjectives
        OS.EmitValue(createImageRel32(HT.TypeDescriptor), 4); // Type
        OS.EmitValue(FrameAllocOffsetRef, 4);                 // CatchObjOffset
        OS.EmitValue(createImageRel32(HT.Handler), 4);        // Handler
        OS.EmitValue(ParentFrameOffsetRef, 4);                // ParentFrameOffset
      }
    }
  }

  // IPToStateMapEntry {
  //   void   *IP;
  //   int32_t State;
  // };
  if (IPToStateXData) {
    OS.EmitLabel(IPToStateXData);
    for (auto &IPStatePair : FuncInfo.IPToStateList) {
      OS.EmitValue(createImageRel32(IPStatePair.first), 4); // IP
      OS.EmitIntValue(IPStatePair.second, 4);               // State
    }
  }
}
