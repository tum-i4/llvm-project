//===- RISCVCleanupVSETVLI.cpp - Cleanup unneeded VSETVLI instructions ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a function pass that removes duplicate vsetvli
// instructions within a basic block.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
using namespace llvm;

#define DEBUG_TYPE "riscv-cleanup-vsetvli"
#define RISCV_CLEANUP_VSETVLI_NAME "RISCV Cleanup VSETVLI pass"

namespace {

class RISCVCleanupVSETVLI : public MachineFunctionPass {
public:
  static char ID;

  RISCVCleanupVSETVLI() : MachineFunctionPass(ID) {
    initializeRISCVCleanupVSETVLIPass(*PassRegistry::getPassRegistry());
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
  bool runOnMachineBasicBlock(MachineBasicBlock &MBB);

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

  // This pass modifies the program, but does not modify the CFG
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return RISCV_CLEANUP_VSETVLI_NAME; }
};

} // end anonymous namespace

char RISCVCleanupVSETVLI::ID = 0;

INITIALIZE_PASS(RISCVCleanupVSETVLI, DEBUG_TYPE,
                RISCV_CLEANUP_VSETVLI_NAME, false, false)

static bool isRedundantVSETVLI(MachineInstr &MI, MachineInstr *PrevVSETVLI) {
  // If we don't have a previous VSET{I}VLI or the VL output isn't dead, we
  // can't remove this VSETVLI.
  if (!PrevVSETVLI || !MI.getOperand(0).isDead())
    return false;

  // Does this VSET{I}VLI use the same VTYPE immediate.
  int64_t PrevVTYPEImm = PrevVSETVLI->getOperand(2).getImm();
  int64_t VTYPEImm = MI.getOperand(2).getImm();
  if (PrevVTYPEImm != VTYPEImm)
    return false;

  if (MI.getOpcode() == RISCV::PseudoVSETIVLI) {
    // If the previous opcode wasn't vsetivli we can't compare them.
    if (PrevVSETVLI->getOpcode() != RISCV::PseudoVSETIVLI)
      return false;

    // For VSETIVLI, we can just compare the immediates.
    return PrevVSETVLI->getOperand(1).getImm() == MI.getOperand(1).getImm();
  }

  assert(MI.getOpcode() == RISCV::PseudoVSETVLI);
  Register AVLReg = MI.getOperand(1).getReg();

  // If this VSETVLI isn't changing VL, it is redundant.
  if (AVLReg == RISCV::X0 && MI.getOperand(0).getReg() == RISCV::X0)
    return true;

  // If the previous opcode isn't vsetvli we can't do any more comparison.
  if (PrevVSETVLI->getOpcode() != RISCV::PseudoVSETVLI)
    return false;

  // Does this VSETVLI use the same AVL register?
  if (AVLReg != PrevVSETVLI->getOperand(1).getReg())
    return false;

  // If the AVLReg is X0 we must be setting VL to VLMAX. Keeping VL unchanged
  // was handled above.
  if (AVLReg == RISCV::X0) {
    // This instruction is setting VL to VLMAX, this is redundant if the
    // previous VSETVLI was also setting VL to VLMAX. But it is not redundant
    // if they were setting it to any other value or leaving VL unchanged.
    Register PrevOutVL = PrevVSETVLI->getOperand(0).getReg();
    return PrevOutVL != RISCV::X0;
  }

  // This vsetvli is redundant.
  return true;
}

bool RISCVCleanupVSETVLI::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Changed = false;
  MachineInstr *PrevVSETVLI = nullptr;

  for (auto MII = MBB.begin(), MIE = MBB.end(); MII != MIE;) {
    MachineInstr &MI = *MII++;

    if (MI.getOpcode() != RISCV::PseudoVSETVLI &&
        MI.getOpcode() != RISCV::PseudoVSETIVLI) {
      if (PrevVSETVLI &&
          (MI.isCall() || MI.modifiesRegister(RISCV::VL) ||
           MI.modifiesRegister(RISCV::VTYPE))) {
        // Old VL/VTYPE is overwritten.
        PrevVSETVLI = nullptr;
      }
      continue;
    }

    // If we don't have a previous VSET{I}VLI or the VL output isn't dead, we
    // can't remove this VSETVLI.
    if (!PrevVSETVLI || !MI.getOperand(0).isDead()) {
      PrevVSETVLI = &MI;
      continue;
    }

    // If a previous "set vl" instruction opcode is different from this one, we
    // can't differentiate the AVL values.
    if (PrevVSETVLI->getOpcode() != MI.getOpcode()) {
      PrevVSETVLI = &MI;
      continue;
    }

    // The remaining two cases are
    // 1. PrevVSETVLI = PseudoVSETVLI
    //    MI = PseudoVSETVLI
    //
    // 2. PrevVSETVLI = PseudoVSETIVLI
    //    MI = PseudoVSETIVLI
    Register AVLReg;
    bool SameAVL = false;
    if (MI.getOpcode() == RISCV::PseudoVSETVLI) {
      AVLReg = MI.getOperand(1).getReg();
      SameAVL = PrevVSETVLI->getOperand(1).getReg() == AVLReg;
    } else { // RISCV::PseudoVSETIVLI
      SameAVL =
          PrevVSETVLI->getOperand(1).getImm() == MI.getOperand(1).getImm();
    }
    int64_t PrevVTYPEImm = PrevVSETVLI->getOperand(2).getImm();
    int64_t VTYPEImm = MI.getOperand(2).getImm();

    // Does this VSET{I}VLI use the same AVL register/value and VTYPE immediate?
    if (!SameAVL || PrevVTYPEImm != VTYPEImm) {
      PrevVSETVLI = &MI;
      continue;
    }

    // If the AVLReg is X0 we need to look at the output VL of both VSETVLIs.
    if ((MI.getOpcode() == RISCV::PseudoVSETVLI) && (AVLReg == RISCV::X0)) {
      assert((PrevVSETVLI->getOpcode() == RISCV::PseudoVSETVLI) &&
             "Unexpected vsetvli opcode.");
      Register PrevOutVL = PrevVSETVLI->getOperand(0).getReg();
      Register OutVL = MI.getOperand(0).getReg();
      // We can't remove if the previous VSETVLI left VL unchanged and the
      // current instruction is setting it to VLMAX. Without knowing the VL
      // before the previous instruction we don't know if this is a change.
      if (PrevOutVL == RISCV::X0 && OutVL != RISCV::X0) {
        PrevVSETVLI = &MI;
        continue;
      }
    }
  }

  return Changed;
}

bool RISCVCleanupVSETVLI::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  // Skip if the vector extension is not enabled.
  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  if (!ST.hasStdExtV())
    return false;

  bool Changed = false;

  for (MachineBasicBlock &MBB : MF)
    Changed |= runOnMachineBasicBlock(MBB);

  return Changed;
}

/// Returns an instance of the Cleanup VSETVLI pass.
FunctionPass *llvm::createRISCVCleanupVSETVLIPass() {
  return new RISCVCleanupVSETVLI();
}
