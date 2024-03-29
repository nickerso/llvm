//===-- FunctionLoweringInfo.cpp ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This implements routines for translating functions from LLVM IR into
// Machine IR.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/WinEHFuncInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <algorithm>
using namespace llvm;

#define DEBUG_TYPE "function-lowering-info"

/// isUsedOutsideOfDefiningBlock - Return true if this instruction is used by
/// PHI nodes or outside of the basic block that defines it, or used by a
/// switch or atomic instruction, which may expand to multiple basic blocks.
static bool isUsedOutsideOfDefiningBlock(const Instruction *I) {
  if (I->use_empty()) return false;
  if (isa<PHINode>(I)) return true;
  const BasicBlock *BB = I->getParent();
  for (const User *U : I->users())
    if (cast<Instruction>(U)->getParent() != BB || isa<PHINode>(U))
      return true;

  return false;
}

static ISD::NodeType getPreferredExtendForValue(const Value *V) {
  // For the users of the source value being used for compare instruction, if
  // the number of signed predicate is greater than unsigned predicate, we
  // prefer to use SIGN_EXTEND.
  //
  // With this optimization, we would be able to reduce some redundant sign or
  // zero extension instruction, and eventually more machine CSE opportunities
  // can be exposed.
  ISD::NodeType ExtendKind = ISD::ANY_EXTEND;
  unsigned NumOfSigned = 0, NumOfUnsigned = 0;
  for (const User *U : V->users()) {
    if (const auto *CI = dyn_cast<CmpInst>(U)) {
      NumOfSigned += CI->isSigned();
      NumOfUnsigned += CI->isUnsigned();
    }
  }
  if (NumOfSigned > NumOfUnsigned)
    ExtendKind = ISD::SIGN_EXTEND;

  return ExtendKind;
}

namespace {
struct WinEHNumbering {
  WinEHNumbering(WinEHFuncInfo &FuncInfo) : FuncInfo(FuncInfo), NextState(0) {}

  WinEHFuncInfo &FuncInfo;
  int NextState;

  SmallVector<ActionHandler *, 4> HandlerStack;
  SmallPtrSet<const Function *, 4> VisitedHandlers;

  int currentEHNumber() const {
    return HandlerStack.empty() ? -1 : HandlerStack.back()->getEHState();
  }

  void createUnwindMapEntry(int ToState, ActionHandler *AH);
  void createTryBlockMapEntry(int TryLow, int TryHigh,
                              ArrayRef<CatchHandler *> Handlers);
  void processCallSite(ArrayRef<ActionHandler *> Actions, ImmutableCallSite CS);
  void calculateStateNumbers(const Function &F);
};
}

void FunctionLoweringInfo::set(const Function &fn, MachineFunction &mf,
                               SelectionDAG *DAG) {
  Fn = &fn;
  MF = &mf;
  TLI = MF->getSubtarget().getTargetLowering();
  RegInfo = &MF->getRegInfo();
  MachineModuleInfo &MMI = MF->getMMI();

  // Check whether the function can return without sret-demotion.
  SmallVector<ISD::OutputArg, 4> Outs;
  GetReturnInfo(Fn->getReturnType(), Fn->getAttributes(), Outs, *TLI);
  CanLowerReturn = TLI->CanLowerReturn(Fn->getCallingConv(), *MF,
                                       Fn->isVarArg(), Outs, Fn->getContext());

  // Initialize the mapping of values to registers.  This is only set up for
  // instruction values that are used outside of the block that defines
  // them.
  Function::const_iterator BB = Fn->begin(), EB = Fn->end();
  for (; BB != EB; ++BB)
    for (BasicBlock::const_iterator I = BB->begin(), E = BB->end();
         I != E; ++I) {
      if (const AllocaInst *AI = dyn_cast<AllocaInst>(I)) {
        // Static allocas can be folded into the initial stack frame adjustment.
        if (AI->isStaticAlloca()) {
          const ConstantInt *CUI = cast<ConstantInt>(AI->getArraySize());
          Type *Ty = AI->getAllocatedType();
          uint64_t TySize = TLI->getDataLayout()->getTypeAllocSize(Ty);
          unsigned Align =
              std::max((unsigned)TLI->getDataLayout()->getPrefTypeAlignment(Ty),
                       AI->getAlignment());

          TySize *= CUI->getZExtValue();   // Get total allocated size.
          if (TySize == 0) TySize = 1; // Don't create zero-sized stack objects.

          StaticAllocaMap[AI] =
            MF->getFrameInfo()->CreateStackObject(TySize, Align, false, AI);

        } else {
          unsigned Align = std::max(
              (unsigned)TLI->getDataLayout()->getPrefTypeAlignment(
                AI->getAllocatedType()),
              AI->getAlignment());
          unsigned StackAlign =
              MF->getSubtarget().getFrameLowering()->getStackAlignment();
          if (Align <= StackAlign)
            Align = 0;
          // Inform the Frame Information that we have variable-sized objects.
          MF->getFrameInfo()->CreateVariableSizedObject(Align ? Align : 1, AI);
        }
      }

      // Look for inline asm that clobbers the SP register.
      if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
        ImmutableCallSite CS(I);
        if (isa<InlineAsm>(CS.getCalledValue())) {
          unsigned SP = TLI->getStackPointerRegisterToSaveRestore();
          const TargetRegisterInfo *TRI = MF->getSubtarget().getRegisterInfo();
          std::vector<TargetLowering::AsmOperandInfo> Ops =
              TLI->ParseConstraints(TRI, CS);
          for (size_t I = 0, E = Ops.size(); I != E; ++I) {
            TargetLowering::AsmOperandInfo &Op = Ops[I];
            if (Op.Type == InlineAsm::isClobber) {
              // Clobbers don't have SDValue operands, hence SDValue().
              TLI->ComputeConstraintToUse(Op, SDValue(), DAG);
              std::pair<unsigned, const TargetRegisterClass *> PhysReg =
                  TLI->getRegForInlineAsmConstraint(TRI, Op.ConstraintCode,
                                                    Op.ConstraintVT);
              if (PhysReg.first == SP)
                MF->getFrameInfo()->setHasInlineAsmWithSPAdjust(true);
            }
          }
        }
      }

      // Look for calls to the @llvm.va_start intrinsic. We can omit some
      // prologue boilerplate for variadic functions that don't examine their
      // arguments.
      if (const auto *II = dyn_cast<IntrinsicInst>(I)) {
        if (II->getIntrinsicID() == Intrinsic::vastart)
          MF->getFrameInfo()->setHasVAStart(true);
      }

      // If we have a musttail call in a variadic funciton, we need to ensure we
      // forward implicit register parameters.
      if (const auto *CI = dyn_cast<CallInst>(I)) {
        if (CI->isMustTailCall() && Fn->isVarArg())
          MF->getFrameInfo()->setHasMustTailInVarArgFunc(true);
      }

      // Mark values used outside their block as exported, by allocating
      // a virtual register for them.
      if (isUsedOutsideOfDefiningBlock(I))
        if (!isa<AllocaInst>(I) ||
            !StaticAllocaMap.count(cast<AllocaInst>(I)))
          InitializeRegForValue(I);

      // Collect llvm.dbg.declare information. This is done now instead of
      // during the initial isel pass through the IR so that it is done
      // in a predictable order.
      if (const DbgDeclareInst *DI = dyn_cast<DbgDeclareInst>(I)) {
        DIVariable DIVar = DI->getVariable();
        if (MMI.hasDebugInfo() && DIVar && DI->getDebugLoc()) {
          // Don't handle byval struct arguments or VLAs, for example.
          // Non-byval arguments are handled here (they refer to the stack
          // temporary alloca at this point).
          const Value *Address = DI->getAddress();
          if (Address) {
            if (const BitCastInst *BCI = dyn_cast<BitCastInst>(Address))
              Address = BCI->getOperand(0);
            if (const AllocaInst *AI = dyn_cast<AllocaInst>(Address)) {
              DenseMap<const AllocaInst *, int>::iterator SI =
                StaticAllocaMap.find(AI);
              if (SI != StaticAllocaMap.end()) { // Check for VLAs.
                int FI = SI->second;
                MMI.setVariableDbgInfo(DI->getVariable(), DI->getExpression(),
                                       FI, DI->getDebugLoc());
              }
            }
          }
        }
      }

      // Decide the preferred extend type for a value.
      PreferredExtendType[I] = getPreferredExtendForValue(I);
    }

  // Create an initial MachineBasicBlock for each LLVM BasicBlock in F.  This
  // also creates the initial PHI MachineInstrs, though none of the input
  // operands are populated.
  for (BB = Fn->begin(); BB != EB; ++BB) {
    MachineBasicBlock *MBB = mf.CreateMachineBasicBlock(BB);
    MBBMap[BB] = MBB;
    MF->push_back(MBB);

    // Transfer the address-taken flag. This is necessary because there could
    // be multiple MachineBasicBlocks corresponding to one BasicBlock, and only
    // the first one should be marked.
    if (BB->hasAddressTaken())
      MBB->setHasAddressTaken();

    // Create Machine PHI nodes for LLVM PHI nodes, lowering them as
    // appropriate.
    for (BasicBlock::const_iterator I = BB->begin();
         const PHINode *PN = dyn_cast<PHINode>(I); ++I) {
      if (PN->use_empty()) continue;

      // Skip empty types
      if (PN->getType()->isEmptyTy())
        continue;

      DebugLoc DL = PN->getDebugLoc();
      unsigned PHIReg = ValueMap[PN];
      assert(PHIReg && "PHI node does not have an assigned virtual register!");

      SmallVector<EVT, 4> ValueVTs;
      ComputeValueVTs(*TLI, PN->getType(), ValueVTs);
      for (unsigned vti = 0, vte = ValueVTs.size(); vti != vte; ++vti) {
        EVT VT = ValueVTs[vti];
        unsigned NumRegisters = TLI->getNumRegisters(Fn->getContext(), VT);
        const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
        for (unsigned i = 0; i != NumRegisters; ++i)
          BuildMI(MBB, DL, TII->get(TargetOpcode::PHI), PHIReg + i);
        PHIReg += NumRegisters;
      }
    }
  }

  // Mark landing pad blocks.
  for (BB = Fn->begin(); BB != EB; ++BB)
    if (const auto *Invoke = dyn_cast<InvokeInst>(BB->getTerminator()))
      MBBMap[Invoke->getSuccessor(1)]->setIsLandingPad();

  // Calculate EH numbers for WinEH.
  if (fn.getFnAttribute("wineh-parent").getValueAsString() == fn.getName()) {
    WinEHNumbering Num(MMI.getWinEHFuncInfo(&fn));
    Num.calculateStateNumbers(fn);
    // Pop everything on the handler stack.
    Num.processCallSite(None, ImmutableCallSite());
  }
}

void WinEHNumbering::createUnwindMapEntry(int ToState, ActionHandler *AH) {
  WinEHUnwindMapEntry UME;
  UME.ToState = ToState;
  if (auto *CH = dyn_cast_or_null<CleanupHandler>(AH))
    UME.Cleanup = cast<Function>(CH->getHandlerBlockOrFunc());
  else
    UME.Cleanup = nullptr;
  FuncInfo.UnwindMap.push_back(UME);
}

void WinEHNumbering::createTryBlockMapEntry(int TryLow, int TryHigh,
                                            ArrayRef<CatchHandler *> Handlers) {
  WinEHTryBlockMapEntry TBME;
  TBME.TryLow = TryLow;
  TBME.TryHigh = TryHigh;
  assert(TBME.TryLow <= TBME.TryHigh);
  for (CatchHandler *CH : Handlers) {
    WinEHHandlerType HT;
    if (CH->getSelector()->isNullValue()) {
      HT.Adjectives = 0x40;
      HT.TypeDescriptor = nullptr;
    } else {
      auto *GV = cast<GlobalVariable>(CH->getSelector()->stripPointerCasts());
      // Selectors are always pointers to GlobalVariables with 'struct' type.
      // The struct has two fields, adjectives and a type descriptor.
      auto *CS = cast<ConstantStruct>(GV->getInitializer());
      HT.Adjectives =
          cast<ConstantInt>(CS->getAggregateElement(0U))->getZExtValue();
      HT.TypeDescriptor =
          cast<GlobalVariable>(CS->getAggregateElement(1)->stripPointerCasts());
    }
    HT.Handler = cast<Function>(CH->getHandlerBlockOrFunc());
    HT.CatchObjRecoverIdx = CH->getExceptionVarIndex();
    TBME.HandlerArray.push_back(HT);
  }
  FuncInfo.TryBlockMap.push_back(TBME);
}

static void print_name(const Value *V) {
#ifndef NDEBUG
  if (!V) {
    DEBUG(dbgs() << "null");
    return;
  }

  if (const auto *F = dyn_cast<Function>(V))
    DEBUG(dbgs() << F->getName());
  else
    DEBUG(V->dump());
#endif
}

void WinEHNumbering::processCallSite(ArrayRef<ActionHandler *> Actions,
                                     ImmutableCallSite CS) {
  int FirstMismatch = 0;
  for (int E = std::min(HandlerStack.size(), Actions.size()); FirstMismatch < E;
       ++FirstMismatch) {
    if (HandlerStack[FirstMismatch]->getHandlerBlockOrFunc() !=
        Actions[FirstMismatch]->getHandlerBlockOrFunc())
      break;
    delete Actions[FirstMismatch];
  }

  bool EnteringScope = (int)Actions.size() > FirstMismatch;

  // Don't recurse while we are looping over the handler stack.  Instead, defer
  // the numbering of the catch handlers until we are done popping.
  SmallVector<CatchHandler *, 4> PoppedCatches;
  for (int I = HandlerStack.size() - 1; I >= FirstMismatch; --I) {
    if (auto *CH = dyn_cast<CatchHandler>(HandlerStack.back())) {
      PoppedCatches.push_back(CH);
    } else {
      // Delete cleanup handlers
      delete HandlerStack.back();
    }
    HandlerStack.pop_back();
  }

  // We need to create a new state number if we are exiting a try scope and we
  // will not push any more actions.
  int TryHigh = NextState - 1;
  if (!EnteringScope && !PoppedCatches.empty()) {
    createUnwindMapEntry(currentEHNumber(), nullptr);
    ++NextState;
  }

  int LastTryLowIdx = 0;
  for (int I = 0, E = PoppedCatches.size(); I != E; ++I) {
    CatchHandler *CH = PoppedCatches[I];
    if (I + 1 == E || CH->getEHState() != PoppedCatches[I + 1]->getEHState()) {
      int TryLow = CH->getEHState();
      auto Handlers =
          makeArrayRef(&PoppedCatches[LastTryLowIdx], I - LastTryLowIdx + 1);
      createTryBlockMapEntry(TryLow, TryHigh, Handlers);
      LastTryLowIdx = I + 1;
    }
  }

  for (CatchHandler *CH : PoppedCatches) {
    if (auto *F = dyn_cast<Function>(CH->getHandlerBlockOrFunc()))
      calculateStateNumbers(*F);
    delete CH;
  }

  bool LastActionWasCatch = false;
  for (size_t I = FirstMismatch; I != Actions.size(); ++I) {
    // We can reuse eh states when pushing two catches for the same invoke.
    bool CurrActionIsCatch = isa<CatchHandler>(Actions[I]);
    // FIXME: Reenable this optimization!
    if (CurrActionIsCatch && LastActionWasCatch && false) {
      Actions[I]->setEHState(currentEHNumber());
    } else {
      createUnwindMapEntry(currentEHNumber(), Actions[I]);
      Actions[I]->setEHState(NextState);
      NextState++;
      DEBUG(dbgs() << "Creating unwind map entry for: (");
      print_name(Actions[I]->getHandlerBlockOrFunc());
      DEBUG(dbgs() << ", " << currentEHNumber() << ")\n");
    }
    HandlerStack.push_back(Actions[I]);
    LastActionWasCatch = CurrActionIsCatch;
  }

  DEBUG(dbgs() << "In EHState " << currentEHNumber() << " for CallSite: ");
  print_name(CS ? CS.getCalledValue() : nullptr);
  DEBUG(dbgs() << '\n');
}

void WinEHNumbering::calculateStateNumbers(const Function &F) {
  auto I = VisitedHandlers.insert(&F);
  if (!I.second)
    return; // We've already visited this handler, don't renumber it.

  DEBUG(dbgs() << "Calculating state numbers for: " << F.getName() << '\n');
  SmallVector<ActionHandler *, 4> ActionList;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      const auto *CI = dyn_cast<CallInst>(&I);
      if (!CI || CI->doesNotThrow())
        continue;
      processCallSite(None, CI);
    }
    const auto *II = dyn_cast<InvokeInst>(BB.getTerminator());
    if (!II)
      continue;
    const LandingPadInst *LPI = II->getLandingPadInst();
    auto *ActionsCall = dyn_cast<IntrinsicInst>(LPI->getNextNode());
    if (!ActionsCall)
      continue;
    assert(ActionsCall->getIntrinsicID() == Intrinsic::eh_actions);
    parseEHActions(ActionsCall, ActionList);
    processCallSite(ActionList, II);
    ActionList.clear();
    FuncInfo.LandingPadStateMap[LPI] = currentEHNumber();
  }

  FuncInfo.CatchHandlerMaxState[&F] = NextState - 1;
}

/// clear - Clear out all the function-specific state. This returns this
/// FunctionLoweringInfo to an empty state, ready to be used for a
/// different function.
void FunctionLoweringInfo::clear() {
  assert(CatchInfoFound.size() == CatchInfoLost.size() &&
         "Not all catch info was assigned to a landing pad!");

  MBBMap.clear();
  ValueMap.clear();
  StaticAllocaMap.clear();
#ifndef NDEBUG
  CatchInfoLost.clear();
  CatchInfoFound.clear();
#endif
  LiveOutRegInfo.clear();
  VisitedBBs.clear();
  ArgDbgValues.clear();
  ByValArgFrameIndexMap.clear();
  RegFixups.clear();
  StatepointStackSlots.clear();
  PreferredExtendType.clear();
}

/// CreateReg - Allocate a single virtual register for the given type.
unsigned FunctionLoweringInfo::CreateReg(MVT VT) {
  return RegInfo->createVirtualRegister(
      MF->getSubtarget().getTargetLowering()->getRegClassFor(VT));
}

/// CreateRegs - Allocate the appropriate number of virtual registers of
/// the correctly promoted or expanded types.  Assign these registers
/// consecutive vreg numbers and return the first assigned number.
///
/// In the case that the given value has struct or array type, this function
/// will assign registers for each member or element.
///
unsigned FunctionLoweringInfo::CreateRegs(Type *Ty) {
  const TargetLowering *TLI = MF->getSubtarget().getTargetLowering();

  SmallVector<EVT, 4> ValueVTs;
  ComputeValueVTs(*TLI, Ty, ValueVTs);

  unsigned FirstReg = 0;
  for (unsigned Value = 0, e = ValueVTs.size(); Value != e; ++Value) {
    EVT ValueVT = ValueVTs[Value];
    MVT RegisterVT = TLI->getRegisterType(Ty->getContext(), ValueVT);

    unsigned NumRegs = TLI->getNumRegisters(Ty->getContext(), ValueVT);
    for (unsigned i = 0; i != NumRegs; ++i) {
      unsigned R = CreateReg(RegisterVT);
      if (!FirstReg) FirstReg = R;
    }
  }
  return FirstReg;
}

/// GetLiveOutRegInfo - Gets LiveOutInfo for a register, returning NULL if the
/// register is a PHI destination and the PHI's LiveOutInfo is not valid. If
/// the register's LiveOutInfo is for a smaller bit width, it is extended to
/// the larger bit width by zero extension. The bit width must be no smaller
/// than the LiveOutInfo's existing bit width.
const FunctionLoweringInfo::LiveOutInfo *
FunctionLoweringInfo::GetLiveOutRegInfo(unsigned Reg, unsigned BitWidth) {
  if (!LiveOutRegInfo.inBounds(Reg))
    return nullptr;

  LiveOutInfo *LOI = &LiveOutRegInfo[Reg];
  if (!LOI->IsValid)
    return nullptr;

  if (BitWidth > LOI->KnownZero.getBitWidth()) {
    LOI->NumSignBits = 1;
    LOI->KnownZero = LOI->KnownZero.zextOrTrunc(BitWidth);
    LOI->KnownOne = LOI->KnownOne.zextOrTrunc(BitWidth);
  }

  return LOI;
}

/// ComputePHILiveOutRegInfo - Compute LiveOutInfo for a PHI's destination
/// register based on the LiveOutInfo of its operands.
void FunctionLoweringInfo::ComputePHILiveOutRegInfo(const PHINode *PN) {
  Type *Ty = PN->getType();
  if (!Ty->isIntegerTy() || Ty->isVectorTy())
    return;

  SmallVector<EVT, 1> ValueVTs;
  ComputeValueVTs(*TLI, Ty, ValueVTs);
  assert(ValueVTs.size() == 1 &&
         "PHIs with non-vector integer types should have a single VT.");
  EVT IntVT = ValueVTs[0];

  if (TLI->getNumRegisters(PN->getContext(), IntVT) != 1)
    return;
  IntVT = TLI->getTypeToTransformTo(PN->getContext(), IntVT);
  unsigned BitWidth = IntVT.getSizeInBits();

  unsigned DestReg = ValueMap[PN];
  if (!TargetRegisterInfo::isVirtualRegister(DestReg))
    return;
  LiveOutRegInfo.grow(DestReg);
  LiveOutInfo &DestLOI = LiveOutRegInfo[DestReg];

  Value *V = PN->getIncomingValue(0);
  if (isa<UndefValue>(V) || isa<ConstantExpr>(V)) {
    DestLOI.NumSignBits = 1;
    APInt Zero(BitWidth, 0);
    DestLOI.KnownZero = Zero;
    DestLOI.KnownOne = Zero;
    return;
  }

  if (ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
    APInt Val = CI->getValue().zextOrTrunc(BitWidth);
    DestLOI.NumSignBits = Val.getNumSignBits();
    DestLOI.KnownZero = ~Val;
    DestLOI.KnownOne = Val;
  } else {
    assert(ValueMap.count(V) && "V should have been placed in ValueMap when its"
                                "CopyToReg node was created.");
    unsigned SrcReg = ValueMap[V];
    if (!TargetRegisterInfo::isVirtualRegister(SrcReg)) {
      DestLOI.IsValid = false;
      return;
    }
    const LiveOutInfo *SrcLOI = GetLiveOutRegInfo(SrcReg, BitWidth);
    if (!SrcLOI) {
      DestLOI.IsValid = false;
      return;
    }
    DestLOI = *SrcLOI;
  }

  assert(DestLOI.KnownZero.getBitWidth() == BitWidth &&
         DestLOI.KnownOne.getBitWidth() == BitWidth &&
         "Masks should have the same bit width as the type.");

  for (unsigned i = 1, e = PN->getNumIncomingValues(); i != e; ++i) {
    Value *V = PN->getIncomingValue(i);
    if (isa<UndefValue>(V) || isa<ConstantExpr>(V)) {
      DestLOI.NumSignBits = 1;
      APInt Zero(BitWidth, 0);
      DestLOI.KnownZero = Zero;
      DestLOI.KnownOne = Zero;
      return;
    }

    if (ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
      APInt Val = CI->getValue().zextOrTrunc(BitWidth);
      DestLOI.NumSignBits = std::min(DestLOI.NumSignBits, Val.getNumSignBits());
      DestLOI.KnownZero &= ~Val;
      DestLOI.KnownOne &= Val;
      continue;
    }

    assert(ValueMap.count(V) && "V should have been placed in ValueMap when "
                                "its CopyToReg node was created.");
    unsigned SrcReg = ValueMap[V];
    if (!TargetRegisterInfo::isVirtualRegister(SrcReg)) {
      DestLOI.IsValid = false;
      return;
    }
    const LiveOutInfo *SrcLOI = GetLiveOutRegInfo(SrcReg, BitWidth);
    if (!SrcLOI) {
      DestLOI.IsValid = false;
      return;
    }
    DestLOI.NumSignBits = std::min(DestLOI.NumSignBits, SrcLOI->NumSignBits);
    DestLOI.KnownZero &= SrcLOI->KnownZero;
    DestLOI.KnownOne &= SrcLOI->KnownOne;
  }
}

/// setArgumentFrameIndex - Record frame index for the byval
/// argument. This overrides previous frame index entry for this argument,
/// if any.
void FunctionLoweringInfo::setArgumentFrameIndex(const Argument *A,
                                                 int FI) {
  ByValArgFrameIndexMap[A] = FI;
}

/// getArgumentFrameIndex - Get frame index for the byval argument.
/// If the argument does not have any assigned frame index then 0 is
/// returned.
int FunctionLoweringInfo::getArgumentFrameIndex(const Argument *A) {
  DenseMap<const Argument *, int>::iterator I =
    ByValArgFrameIndexMap.find(A);
  if (I != ByValArgFrameIndexMap.end())
    return I->second;
  DEBUG(dbgs() << "Argument does not have assigned frame index!\n");
  return 0;
}

/// ComputeUsesVAFloatArgument - Determine if any floating-point values are
/// being passed to this variadic function, and set the MachineModuleInfo's
/// usesVAFloatArgument flag if so. This flag is used to emit an undefined
/// reference to _fltused on Windows, which will link in MSVCRT's
/// floating-point support.
void llvm::ComputeUsesVAFloatArgument(const CallInst &I,
                                      MachineModuleInfo *MMI)
{
  FunctionType *FT = cast<FunctionType>(
    I.getCalledValue()->getType()->getContainedType(0));
  if (FT->isVarArg() && !MMI->usesVAFloatArgument()) {
    for (unsigned i = 0, e = I.getNumArgOperands(); i != e; ++i) {
      Type* T = I.getArgOperand(i)->getType();
      for (po_iterator<Type*> i = po_begin(T), e = po_end(T);
           i != e; ++i) {
        if (i->isFloatingPointTy()) {
          MMI->setUsesVAFloatArgument(true);
          return;
        }
      }
    }
  }
}

/// AddLandingPadInfo - Extract the exception handling information from the
/// landingpad instruction and add them to the specified machine module info.
void llvm::AddLandingPadInfo(const LandingPadInst &I, MachineModuleInfo &MMI,
                             MachineBasicBlock *MBB) {
  MMI.addPersonality(MBB,
                     cast<Function>(I.getPersonalityFn()->stripPointerCasts()));

  if (I.isCleanup())
    MMI.addCleanup(MBB);

  // FIXME: New EH - Add the clauses in reverse order. This isn't 100% correct,
  //        but we need to do it this way because of how the DWARF EH emitter
  //        processes the clauses.
  for (unsigned i = I.getNumClauses(); i != 0; --i) {
    Value *Val = I.getClause(i - 1);
    if (I.isCatch(i - 1)) {
      MMI.addCatchTypeInfo(MBB,
                           dyn_cast<GlobalValue>(Val->stripPointerCasts()));
    } else {
      // Add filters in a list.
      Constant *CVal = cast<Constant>(Val);
      SmallVector<const GlobalValue*, 4> FilterList;
      for (User::op_iterator
             II = CVal->op_begin(), IE = CVal->op_end(); II != IE; ++II)
        FilterList.push_back(cast<GlobalValue>((*II)->stripPointerCasts()));

      MMI.addFilterTypeInfo(MBB, FilterList);
    }
  }
}
