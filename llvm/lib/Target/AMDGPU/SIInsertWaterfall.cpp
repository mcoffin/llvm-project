//===- SIInsertWaterfall.cpp - insert waterall loops at intrinsic markers -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Replace 3 intrinsics used to mark waterfall regions with actual waterfall
/// loops. This is done at MachineIR level rather than LLVM-IR due to the use of
/// exec mask in this operation.
///
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "SIInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

#include <set>

using namespace llvm;

#define DEBUG_TYPE "si-insert-waterfall"

namespace {

static unsigned getWFBeginSize(const unsigned Opcode) {
  switch (Opcode) {
  case AMDGPU::SI_WATERFALL_BEGIN_V1:
    return 1;
  case AMDGPU::SI_WATERFALL_BEGIN_V2:
    return 2;
  case AMDGPU::SI_WATERFALL_BEGIN_V4:
    return 4;
  case AMDGPU::SI_WATERFALL_BEGIN_V8:
    return 8;
  default:
    break;
  }

  return 0; // Not SI_WATERFALL_BEGIN_*
}

static unsigned getWFRFLSize(const unsigned Opcode) {
  switch (Opcode) {
  case AMDGPU::SI_WATERFALL_READFIRSTLANE_V1:
    return 1;
  case AMDGPU::SI_WATERFALL_READFIRSTLANE_V2:
    return 2;
  case AMDGPU::SI_WATERFALL_READFIRSTLANE_V4:
    return 4;
  case AMDGPU::SI_WATERFALL_READFIRSTLANE_V8:
    return 8;
  default:
    break;
  }

  return 0; // Not SI_WATERFALL_READFIRSTLANE_*
}

static unsigned getWFEndSize(const unsigned Opcode) {
  switch (Opcode) {
  case AMDGPU::SI_WATERFALL_END_V1:
    return 1;
  case AMDGPU::SI_WATERFALL_END_V2:
    return 2;
  case AMDGPU::SI_WATERFALL_END_V4:
    return 4;
  case AMDGPU::SI_WATERFALL_END_V8:
    return 8;
  default:
    break;
  }

  return 0; // Not SI_WATERFALL_END_*
}

static unsigned getWFLastUseSize(const unsigned Opcode) {
  switch (Opcode) {
  case AMDGPU::SI_WATERFALL_LAST_USE_V1:
    return 1;
  case AMDGPU::SI_WATERFALL_LAST_USE_V2:
    return 2;
  case AMDGPU::SI_WATERFALL_LAST_USE_V4:
    return 4;
  case AMDGPU::SI_WATERFALL_LAST_USE_V8:
    return 8;
  default:
    break;
  }

  return 0; // Not SI_WATERFALL_LAST_USE_*
}

static void initReg(MachineBasicBlock &MBB, MachineRegisterInfo *MRI,
                    const SIRegisterInfo *RI, const SIInstrInfo *TII,
                    MachineBasicBlock::iterator &I, const DebugLoc &DL,
                    unsigned Reg, unsigned ImmVal) {

  auto EndDstRC = MRI->getRegClass(Reg);
  uint32_t RegSize = RI->getRegSizeInBits(*EndDstRC) / 32;

  if (RegSize == 1)
    BuildMI(MBB, I, DL, TII->get(AMDGPU::V_MOV_B32_e32), Reg).addImm(0);
  else {
    SmallVector<unsigned, 8> TRegs;
    for (unsigned i = 0; i < RegSize; i++) {
      unsigned TReg = MRI->createVirtualRegister(&AMDGPU::VGPR_32RegClass);
      BuildMI(MBB, I, DL, TII->get(AMDGPU::V_MOV_B32_e32), TReg).addImm(ImmVal);
      TRegs.push_back(TReg);
    }
    MachineInstrBuilder MIB =
        BuildMI(MBB, I, DL, TII->get(AMDGPU::REG_SEQUENCE), Reg);
    for (unsigned i = 0; i < RegSize; ++i) {
      MIB.addReg(TRegs[i]);
      MIB.addImm(RI->getSubRegFromChannel(i));
    }
  }
}

static void orReg(MachineBasicBlock &MBB, MachineRegisterInfo *MRI,
                  const SIRegisterInfo *RI, const SIInstrInfo *TII,
                  MachineBasicBlock::iterator &I, const DebugLoc &DL,
                  unsigned EndDst, unsigned PhiReg, unsigned EndSrc) {
  auto EndDstRC = MRI->getRegClass(EndDst);
  uint32_t RegSize = RI->getRegSizeInBits(*EndDstRC) / 32;

  if (RegSize == 1)
    BuildMI(MBB, I, DL, TII->get(AMDGPU::V_OR_B32_e64), EndDst)
        .addReg(PhiReg)
        .addReg(EndSrc);
  else {
    SmallVector<unsigned, 8> TRegs;
    for (unsigned i = 0; i < RegSize; ++i) {
      unsigned TReg = MRI->createVirtualRegister(&AMDGPU::VGPR_32RegClass);
      BuildMI(MBB, I, DL, TII->get(AMDGPU::V_OR_B32_e64), TReg)
          .addReg(PhiReg, 0, AMDGPU::sub0 + i)
          .addReg(EndSrc, 0, AMDGPU::sub0 + i);
      TRegs.push_back(TReg);
    }
    MachineInstrBuilder MIB =
        BuildMI(MBB, I, DL, TII->get(AMDGPU::REG_SEQUENCE), EndDst);
    for (unsigned i = 0; i < RegSize; i++) {
      MIB.addReg(TRegs[i]);
      MIB.addImm(RI->getSubRegFromChannel(i));
    }
  }
}

static void readFirstLaneReg(MachineBasicBlock &MBB, MachineRegisterInfo *MRI,
                             const SIRegisterInfo *RI, const SIInstrInfo *TII,
                             MachineBasicBlock::iterator &I, const DebugLoc &DL,
                             unsigned RFLReg, unsigned RFLSrcReg,
                             MachineOperand &RFLSrcOp) {
  auto RFLRegRC = MRI->getRegClass(RFLReg);
  uint32_t RegSize = RI->getRegSizeInBits(*RFLRegRC) / 32;
  assert(RI->hasVGPRs(MRI->getRegClass(RFLSrcReg)) && "unexpected uniform operand for readfirstlane");

  if (RegSize == 1)
    BuildMI(MBB, I, DL, TII->get(AMDGPU::V_READFIRSTLANE_B32), RFLReg)
        .addReg(RFLSrcReg, getUndefRegState(RFLSrcOp.isUndef()));
  else {
    SmallVector<unsigned, 8> TRegs;
    for (unsigned i = 0; i < RegSize; ++i) {
      unsigned TReg = MRI->createVirtualRegister(&AMDGPU::SGPR_32RegClass);
      BuildMI(MBB, I, DL, TII->get(AMDGPU::V_READFIRSTLANE_B32), TReg)
          .addReg(RFLSrcReg, 0, AMDGPU::sub0 + i);
      TRegs.push_back(TReg);
    }
    MachineInstrBuilder MIB =
        BuildMI(MBB, I, DL, TII->get(AMDGPU::REG_SEQUENCE), RFLReg);
    for (unsigned i = 0; i < RegSize; ++i) {
      MIB.addReg(TRegs[i]);
      MIB.addImm(RI->getSubRegFromChannel(i));
    }
  }
}

// Check if operand is uniform by checking:
// 1. Trivially detectable as operand in SGPR
// 2. Direct def is from an SGPR->VGPR copy (which may happen if assumed non-uniform value
//    turns out to be uniform)
static unsigned getUniformOperandReplacementReg(MachineRegisterInfo *MRI,
                                                const SIRegisterInfo *RI,
                                                unsigned Reg) {
  auto RegRC = MRI->getRegClass(Reg);
  if (!RI->hasVGPRs(RegRC)) {
    return Reg;
  }

  // Check for operand def being a copy from SGPR
  MachineInstr *DefMI = MRI->getVRegDef(Reg);
  if (DefMI->isFullCopy()) {
    auto const &DefSrcOp = DefMI->getOperand(1);
    if (DefSrcOp.isReg() &&
        TargetRegisterInfo::isVirtualRegister(DefSrcOp.getReg())) {
      unsigned ReplaceReg = DefSrcOp.getReg();
      if (!RI->hasVGPRs(MRI->getRegClass(ReplaceReg)))
        return ReplaceReg;
    }
  }
  return AMDGPU::NoRegister;
}

static unsigned compareIdx(MachineBasicBlock &MBB, MachineRegisterInfo *MRI,
                           const SIRegisterInfo *RI, const SIInstrInfo *TII,
                           MachineBasicBlock::iterator &I, const DebugLoc &DL,
                           unsigned CurrentIdxReg, MachineOperand &IndexOp,
                           bool IsWave32) {
  // Iterate over the index in dword chunks and'ing the result with the
  // CondReg
  unsigned IndexReg = IndexOp.getReg();
  auto IndexRC = MRI->getRegClass(IndexReg);
  unsigned AndOpc =
      IsWave32 ? AMDGPU::S_AND_B32 : AMDGPU::S_AND_B64;
  const auto *BoolXExecRC = RI->getRegClass(AMDGPU::SReg_1_XEXECRegClassID);

  uint32_t RegSize = RI->getRegSizeInBits(*IndexRC) / 32;
  unsigned CondReg;

  if (RegSize == 1) {
    CondReg = MRI->createVirtualRegister(BoolXExecRC);
    BuildMI(MBB, I, DL, TII->get(AMDGPU::V_CMP_EQ_U32_e64), CondReg)
        .addReg(CurrentIdxReg)
        .addReg(IndexReg, 0, IndexOp.getSubReg());
  } else {
    unsigned TReg = MRI->createVirtualRegister(BoolXExecRC);
    BuildMI(MBB, I, DL, TII->get(AMDGPU::V_CMP_EQ_U32_e64), TReg)
        .addReg(CurrentIdxReg, 0, AMDGPU::sub0)
        .addReg(IndexReg, 0, AMDGPU::sub0);

    for (unsigned i = 1; i < RegSize; ++i) {
      unsigned TReg2 =
          MRI->createVirtualRegister(BoolXExecRC);
      BuildMI(MBB, I, DL, TII->get(AMDGPU::V_CMP_EQ_U32_e64), TReg2)
          .addReg(CurrentIdxReg, 0, AMDGPU::sub0 + i)
          .addReg(IndexReg, 0, AMDGPU::sub0 + i);
      unsigned TReg3 =
          MRI->createVirtualRegister(BoolXExecRC);
      BuildMI(MBB, I, DL, TII->get(AndOpc), TReg3)
          .addReg(TReg)
          .addReg(TReg2);
      TReg = TReg3;
    }
    CondReg = TReg;
  }
  return CondReg;
}

class SIInsertWaterfall : public MachineFunctionPass {
private:
  struct WaterfallWorkitem {
    MachineInstr *Begin;
    const SIInstrInfo *TII;
    unsigned TokReg;
    MachineInstr *Final;

    std::vector<MachineInstr *> RFLList;
    std::vector<MachineInstr *> EndList;
    std::vector<MachineInstr *> LastUseList;

    // List of corresponding init, newdst and phi registers used in loop for
    // end pseudos
    std::vector<std::tuple<unsigned, unsigned, unsigned, unsigned>> EndRegs;
    std::vector<unsigned> RFLRegs;

    WaterfallWorkitem() = default;
    WaterfallWorkitem(MachineInstr *_Begin, const SIInstrInfo *_TII)
        : Begin(_Begin), TII(_TII), Final(nullptr) {

      auto TokMO = TII->getNamedOperand(*Begin, AMDGPU::OpName::tok);
      assert(TokMO &&
             "Unable to extract tok operand from SI_WATERFALL_BEGIN pseudo op");
      TokReg = TokMO->getReg();
    };

    void processCandidate(MachineInstr *Cand) {
      unsigned Opcode = Cand->getOpcode();
      // Trivially end any waterfall intrinsic instructions
      if (getWFBeginSize(Opcode) || getWFRFLSize(Opcode) ||
          getWFEndSize(Opcode) || getWFLastUseSize(Opcode)) {
        // TODO: A new waterfall clause shouldn't overlap with any uses
        // tagged by a last_use intrinsic
        return;
      }

      // Iterate over the LastUseList to determine if this instruction has a
      // later use of a tagged last_use
      for (auto Use : LastUseList) {
        auto UseMO = TII->getNamedOperand(*Use, AMDGPU::OpName::dst);
        unsigned UseReg = UseMO->getReg();

        if (Cand->findRegisterUseOperand(UseReg))
          Final = Cand;
      }
    }

    bool addCandidate(MachineInstr *Cand) {
      unsigned Opcode = Cand->getOpcode();

      if (getWFRFLSize(Opcode) || getWFEndSize(Opcode) ||
          getWFLastUseSize(Opcode)) {
        auto CandTokMO = TII->getNamedOperand(*Cand, AMDGPU::OpName::tok);
        unsigned CandTokReg = CandTokMO->getReg();
        if (CandTokReg == TokReg) {
          if (getWFRFLSize(Opcode)) {
            RFLList.push_back(Cand);
            return true;
          } else if (getWFEndSize(Opcode)) {
            EndList.push_back(Cand);
            Final = Cand;
            return true;
          } else {
            LastUseList.push_back(Cand);
            return true;
          }
        }
      } else {
        llvm_unreachable(
            "Candidate is not an SI_WATERFALL_* pseudo instruction");
      }
      return false;
    }
  };

  std::vector<WaterfallWorkitem> Worklist;

  const GCNSubtarget *ST;
  const SIInstrInfo *TII;
  MachineRegisterInfo *MRI;
  const SIRegisterInfo *RI;

public:
  static char ID;

  SIInsertWaterfall() : MachineFunctionPass(ID) {
    initializeSIInsertWaterfallPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool removeRedundantWaterfall(WaterfallWorkitem &Item);
  bool processWaterfall(MachineBasicBlock &MBB);

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // End anonymous namespace.

INITIALIZE_PASS(SIInsertWaterfall, DEBUG_TYPE, "SI Insert waterfalls", false,
                false)

char SIInsertWaterfall::ID = 0;

char &llvm::SIInsertWaterfallID = SIInsertWaterfall::ID;

FunctionPass *llvm::createSIInsertWaterfallPass() {
  return new SIInsertWaterfall;
}

bool SIInsertWaterfall::removeRedundantWaterfall(WaterfallWorkitem &Item) {
  // In some cases, the waterfall is actually redundant
  // If all the readfirstlane intrinsics are actually for uniform values and 
  // the token used in the begin/end isn't used in anything else the waterfall
  // can be removed.
  // The readfirstlane intrinsics are replaced with the uniform source value,
  // the loop is removed and the defs in the end intrinsics are just replaced with
  // the input operands

  // First step is to identify any readfirstlane intrinsics that are actually
  // uniform
  bool LoopRemoved = false;
  unsigned Removed = 0;
  std::vector<MachineInstr *> NewRFLList;
  std::vector<MachineInstr *> ToRemoveRFLList;
  for (auto RFLMI : Item.RFLList) {
    auto RFLSrcOp = TII->getNamedOperand(*RFLMI, AMDGPU::OpName::src);
    auto RFLDstOp = TII->getNamedOperand(*RFLMI, AMDGPU::OpName::dst);
    unsigned RFLSrcReg = RFLSrcOp->getReg();
    unsigned RFLDstReg = RFLDstOp->getReg();

    unsigned ReplaceReg = getUniformOperandReplacementReg(MRI, RI, RFLSrcReg);
    if (ReplaceReg != AMDGPU::NoRegister) {
      MRI->replaceRegWith(RFLDstReg, ReplaceReg);
      Removed++;
      ToRemoveRFLList.push_back(RFLMI);
    } else {
      NewRFLList.push_back(RFLMI);
    }
  }

  if (Removed == Item.RFLList.size()) {
    // Removed all of the RFLs
    // We can remove the waterfall loop entirely

    // Protocol is to replace all dst operands for the waterfall_end intrinsics with
    // their src operands.
    // Replace all last_use dst operands with their src operands
    // We don't need to check that the loop index isn't used anywhere as the
    // protocol for waterfall intrinsics is to only use the begin index via a
    // readfirstlane intrinsic anyway (which should also be removed)
    // Any problems due to errors in this pass around loop removal will be
    // picked up later by e.g. use before def errors
    LLVM_DEBUG(dbgs() << "detected case for waterfall loop removal - already all uniform\n");
    for (auto EndMI : Item.EndList) {
      auto EndDstReg = TII->getNamedOperand(*EndMI, AMDGPU::OpName::dst)->getReg();
      auto EndSrcReg = TII->getNamedOperand(*EndMI, AMDGPU::OpName::src)->getReg();
      MRI->replaceRegWith(EndDstReg, EndSrcReg);
    }
    for (auto LUMI : Item.LastUseList) {
      auto LUDstReg = TII->getNamedOperand(*LUMI, AMDGPU::OpName::dst)->getReg();
      auto LUSrcReg = TII->getNamedOperand(*LUMI, AMDGPU::OpName::src)->getReg();
      MRI->replaceRegWith(LUDstReg, LUSrcReg);
    }

    Item.Begin->eraseFromParent();
    for (auto RFLMI : Item.RFLList)
      RFLMI->eraseFromParent();
    for (auto ENDMI : Item.EndList)
      ENDMI->eraseFromParent();
    for (auto LUMI : Item.LastUseList)
      LUMI->eraseFromParent();

    LoopRemoved = true;

  } else if (Removed) {
    LLVM_DEBUG(dbgs() << "Removed " << Removed <<
               " waterfall rfl intrinsics due to being uniform - updating remaining rfl list\n");
    // TODO: there's an opportunity to pull the DAG involving the (removed) rfls
    // out of the waterfall loop
    Item.RFLList.clear();
    std::copy(NewRFLList.begin(), NewRFLList.end(), std::back_inserter(Item.RFLList));
    for (auto RFLMI : ToRemoveRFLList)
      RFLMI->eraseFromParent();
  }

  return LoopRemoved;
}

bool SIInsertWaterfall::processWaterfall(MachineBasicBlock &MBB) {
  bool Changed = false;
  MachineFunction &MF = *MBB.getParent();
  MachineBasicBlock *CurrMBB = &MBB;

  // Firstly we check that there are at least 3 related waterfall instructions
  // for this begin
  // SI_WATERFALL_BEGIN [ SI_WATERFALL_READFIRSTLANE ]+ [ SI_WATERFALL_END ]+
  // If there are multiple waterfall loops they must also be disjoint

  for (WaterfallWorkitem &Item : Worklist) {
    LLVM_DEBUG(dbgs() << "Processing " << *Item.Begin << "\n");

    LLVM_DEBUG(
        for (auto RUse = MRI->use_begin(Item.TokReg), RSE = MRI->use_end();
             RUse != RSE; ++RUse) {
          MachineInstr *RUseMI = RUse->getParent();
          assert((CurrMBB->getNumber() == RUseMI->getParent()->getNumber()) &&
                 "Linked WATERFALL pseudo ops found in different BBs");
        });

    assert(Item.RFLList.size() &&
           (Item.EndList.size() || Item.LastUseList.size()) &&
           "SI_WATERFALL* pseudo instruction group must have at least 1 of "
           "each type");

    if (removeRedundantWaterfall(Item)) {
      Changed = true;
      continue;
    }

    // Insert the waterfall loop code around the identified region of
    // instructions
    // Loop starts at the SI_WATERFALL_BEGIN
    // SI_WATERFALL_READFIRSTLANE is replaced with appropriate readfirstlane
    // instructions OR is removed
    // if the readfirstlane is using the same index as the SI_WATERFALL_BEGIN
    // Loop is ended after the last SI_WATERFALL_END and these instructions are
    // removed with the src replacing all dst uses
    auto Index = TII->getNamedOperand(*(Item.Begin), AMDGPU::OpName::idx);
    auto IndexRC = MRI->getRegClass(Index->getReg());

    if (!RI->hasVGPRs(IndexRC)) {
      // Waterfall loop index is uniform! Loop can be removed
      // TODO:: Implement loop removal
      LLVM_DEBUG(dbgs() << "Uniform loop detected - waterfall loop is redundant\n");
    }

    auto IndexSRC = RI->getEquivalentSGPRClass(IndexRC);

    MachineBasicBlock::iterator I(Item.Begin);
    const DebugLoc &DL = Item.Begin->getDebugLoc();

    // Initialize the register we accumulate the result into, which is the
    // target of any SI_WATERFALL_END instruction
    for (auto EndMI : Item.EndList) {
      auto EndDst = TII->getNamedOperand(*EndMI, AMDGPU::OpName::dst)->getReg();
      auto EndDstRC = MRI->getRegClass(EndDst);
      unsigned EndInit = MRI->createVirtualRegister(EndDstRC);
      unsigned PhiReg = MRI->createVirtualRegister(EndDstRC);
      unsigned EndSrc =
          TII->getNamedOperand(*EndMI, AMDGPU::OpName::src)->getReg();
      initReg(*CurrMBB, MRI, RI, TII, I, DL, EndInit, 0x0);
      Item.EndRegs.push_back(std::make_tuple(EndInit, EndDst, PhiReg, EndSrc));
    }
    for (auto LUMI : Item.LastUseList) {
      unsigned LUSrc =
          TII->getNamedOperand(*LUMI, AMDGPU::OpName::src)->getReg();
      unsigned LUDst =
          TII->getNamedOperand(*LUMI, AMDGPU::OpName::dst)->getReg();
      MRI->replaceRegWith(LUDst, LUSrc);
    }

    // EXEC mask handling
    unsigned Exec = ST->isWave32() ? AMDGPU::EXEC_LO : AMDGPU::EXEC;
    unsigned SaveExecOpc =
      ST->isWave32() ? AMDGPU::S_AND_SAVEEXEC_B32 : AMDGPU::S_AND_SAVEEXEC_B64;
    unsigned XorTermOpc =
      ST->isWave32() ? AMDGPU::S_XOR_B32_term : AMDGPU::S_XOR_B64_term;
    unsigned MovOpc =
      ST->isWave32() ? AMDGPU::S_MOV_B32 : AMDGPU::S_MOV_B64;
    const auto *BoolXExecRC = RI->getRegClass(AMDGPU::SReg_1_XEXECRegClassID);

    unsigned SaveExec =
        MRI->createVirtualRegister(BoolXExecRC);
    unsigned TmpExec =
        MRI->createVirtualRegister(BoolXExecRC);

    BuildMI(*CurrMBB, I, DL, TII->get(TargetOpcode::IMPLICIT_DEF), TmpExec);

    // Save the EXEC mask
    BuildMI(*CurrMBB, I, DL, TII->get(MovOpc), SaveExec)
        .addReg(Exec);

    MachineBasicBlock &LoopBB = *MF.CreateMachineBasicBlock();
    MachineBasicBlock &RemainderBB = *MF.CreateMachineBasicBlock();
    MachineFunction::iterator MBBI(*CurrMBB);
    ++MBBI;

    MF.insert(MBBI, &LoopBB);
    MF.insert(MBBI, &RemainderBB);

    LoopBB.addSuccessor(&LoopBB);
    LoopBB.addSuccessor(&RemainderBB);

    // Move all instructions from the SI_WATERFALL_BEGIN to the last
    // SI_WATERFALL_END or last use tagged from SI_WATERFALL_LAST_USE
    // into the new LoopBB
    MachineBasicBlock::iterator SpliceE(Item.Final);
    ++SpliceE;
    LoopBB.splice(LoopBB.begin(), CurrMBB, I, SpliceE);

    // Iterate over the instructions inserted into the loop
    // Need to unset any kill flag on any uses as now this is a loop that is no
    // longer valid
    // Also replace any use of a waterfall.end dst register with the one that
    // will replace it in the new phi node at the start of the BB
    for (MachineInstr &MI : LoopBB) {
      MI.clearKillInfo();
      for (auto EndReg : Item.EndRegs) {
        MI.substituteRegister(std::get<1>(EndReg), std::get<2>(EndReg), 0, *RI);
      }
    }

    RemainderBB.transferSuccessorsAndUpdatePHIs(CurrMBB);
    RemainderBB.splice(RemainderBB.begin(), CurrMBB, SpliceE, CurrMBB->end());
    MachineBasicBlock::iterator E(Item.Final);
    ++E;

    CurrMBB->addSuccessor(&LoopBB);

    MachineBasicBlock::iterator J = LoopBB.begin();

    unsigned PhiExec =
        MRI->createVirtualRegister(BoolXExecRC);
    unsigned NewExec =
        MRI->createVirtualRegister(BoolXExecRC);
    unsigned CurrentIdxReg = MRI->createVirtualRegister(IndexSRC);

    for (auto EndReg : Item.EndRegs) {
      BuildMI(LoopBB, J, DL, TII->get(TargetOpcode::PHI), std::get<2>(EndReg))
          .addReg(std::get<0>(EndReg))
          .addMBB(CurrMBB)
          .addReg(std::get<1>(EndReg))
          .addMBB(&LoopBB);
    }
    BuildMI(LoopBB, J, DL, TII->get(TargetOpcode::PHI), PhiExec)
        .addReg(TmpExec)
        .addMBB(CurrMBB)
        .addReg(NewExec)
        .addMBB(&LoopBB);

    // Get the next index to use from the first enabled lane
    readFirstLaneReg(LoopBB, MRI, RI, TII, J, DL, CurrentIdxReg,
                     Index->getReg(), *Index);

    // Also process the readlane pseudo ops - if readfirstlane is using the
    // index then just replace with the CurrentIdxReg instead
    for (auto RFLMI : Item.RFLList) {
      auto RFLSrcOp = TII->getNamedOperand(*RFLMI, AMDGPU::OpName::src);
      auto RFLDstOp = TII->getNamedOperand(*RFLMI, AMDGPU::OpName::dst);
      unsigned RFLSrcReg = RFLSrcOp->getReg();
      unsigned RFLDstReg = RFLDstOp->getReg();

      if (RFLSrcReg == Index->getReg()) {
        // Use the CurrentIdxReg for this
        Item.RFLRegs.push_back(CurrentIdxReg);
        MRI->replaceRegWith(RFLDstReg, CurrentIdxReg);
      } else {
        Item.RFLRegs.push_back(RFLDstReg);
        // Insert function to expand to required size here
        readFirstLaneReg(LoopBB, MRI, RI, TII, J, DL, RFLDstReg, RFLSrcReg,
                         *RFLSrcOp);
      }
    }

    // Compare the just read idx value to all possible idx values
    unsigned CondReg =
      compareIdx(LoopBB, MRI, RI, TII, J, DL, CurrentIdxReg, *Index, ST->isWave32());

    // Update EXEC, save the original EXEC value to VCC
    BuildMI(LoopBB, J, DL, TII->get(SaveExecOpc), NewExec)
        .addReg(CondReg, RegState::Kill);

    // TODO: Conditional branch here to loop header as potential optimization?

    // Move the just read value into the destination using OR
    // TODO: In theory a mov would do here - but this is tricky to get to work
    // correctly as it seems to confuse the register allocator and other passes
    for (auto EndReg : Item.EndRegs) {
      MachineBasicBlock::iterator EndInsert(Item.Final);
      orReg(LoopBB, MRI, RI, TII, EndInsert, DL, std::get<1>(EndReg),
            std::get<2>(EndReg), std::get<3>(EndReg));
    }

    MRI->setSimpleHint(NewExec, CondReg);

    // Update EXEC, switch all done bits to 0 and all todo bits to 1.
    BuildMI(LoopBB, E, DL, TII->get(XorTermOpc), Exec)
        .addReg(Exec)
        .addReg(NewExec);

    // XXX - s_xor_b64 sets scc to 1 if the result is nonzero, so can we use
    // s_cbranch_scc0?

    // Loop back if there are still variants to cover
    BuildMI(LoopBB, E, DL, TII->get(AMDGPU::S_CBRANCH_EXECNZ)).addMBB(&LoopBB);

    MachineBasicBlock::iterator First = RemainderBB.begin();
    BuildMI(RemainderBB, First, DL, TII->get(MovOpc), Exec)
        .addReg(SaveExec);

    Item.Begin->eraseFromParent();
    for (auto RFLMI : Item.RFLList)
      RFLMI->eraseFromParent();
    for (auto ENDMI : Item.EndList)
      ENDMI->eraseFromParent();
    for (auto LUMI : Item.LastUseList)
      LUMI->eraseFromParent();

    // To process subsequent waterfall groups, update CurrMBB to the RemainderBB
    CurrMBB = &RemainderBB;

    Changed = true;
  }
  return Changed;
}

bool SIInsertWaterfall::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;

  ST = &MF.getSubtarget<GCNSubtarget>();
  TII = ST->getInstrInfo();
  MRI = &MF.getRegInfo();
  RI = ST->getRegisterInfo();

  for (MachineBasicBlock &MBB : MF) {
    Worklist.clear();

    for (MachineInstr &MI : MBB) {
      unsigned Opcode = MI.getOpcode();

      if (getWFBeginSize(Opcode))
        Worklist.push_back(WaterfallWorkitem(&MI, TII));
      else if (getWFRFLSize(Opcode) || getWFEndSize(Opcode) ||
               getWFLastUseSize(Opcode)) {
        if (!Worklist.back().addCandidate(&MI)) {
          llvm_unreachable("Overlapping SI_WATERFALL_* groups");
        }
      } else {
        if (Worklist.size())
          Worklist.back().processCandidate(&MI);
      }
    }
    Changed |= processWaterfall(MBB);
  }

  return Changed;
}
