/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include "codegen/CodeGenerator.hpp"
#include "env/FrontEnd.hpp"
#include "codegen/InstOpCode.hpp"
#include "codegen/Instruction.hpp"
#include "codegen/Machine.hpp"
#include "codegen/MemoryReference.hpp"
#include "codegen/RealRegister.hpp"
#include "codegen/Relocation.hpp"
#include "compile/Compilation.hpp"
#include "compile/ResolvedMethod.hpp"
#include "env/CompilerEnv.hpp"
#ifdef J9_PROJECT_SPECIFIC
#include "env/CHTable.hpp"
#endif
#include "env/Processors.hpp"
#include "env/TRMemory.hpp"
#include "env/jittypes.h"
#include "il/DataTypes.hpp"
#include "il/LabelSymbol.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "infra/Assert.hpp"
#include "infra/Bit.hpp"
#include "infra/List.hpp"
#include "p/codegen/GenerateInstructions.hpp"
#include "p/codegen/PPCInstruction.hpp"
#include "p/codegen/PPCOpsDefines.hpp"
#include "runtime/Runtime.hpp"

class TR_OpaqueMethodBlock;
namespace TR { class Register; }

static bool reversedConditionalBranchOpCode(TR::InstOpCode::Mnemonic op, TR::InstOpCode::Mnemonic *rop);

static bool isValidInSignExtendedField(uint32_t value, uint32_t mask)
   {
   uint32_t signMask = ~(mask >> 1);

   return (value & signMask) == 0 || (value & signMask) == signMask;
   }

static bool canUseAsVsxRegister(TR::RealRegister *reg)
   {
   switch (reg->getKind())
      {
      case TR_FPR:
      case TR_VRF:
      case TR_VSX_SCALAR:
      case TR_VSX_VECTOR:
         return true;

      default:
         return false;
      }
   }

static void fillFieldRT(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill RT field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_GPR, "Attempt to fill RT field with %s, which is not a GPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRT(cursor);
   }

static void fillFieldFRT(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill FRT field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_FPR, "Attempt to fill FRT field with %s, which is not an FPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRT(cursor);
   }

static void fillFieldVRT(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill VRT field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_VRF, "Attempt to fill VRT field with %s, which is not a VR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRT(cursor);
   }

static void fillFieldXT(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill XT field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, canUseAsVsxRegister(reg), "Attempt to fill XT field with %s, which is not a VSR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldXT(cursor);
   }

static void fillFieldRS(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill RS field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_GPR, "Attempt to fill RS field with %s, which is not a GPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRS(cursor);
   }

static void fillFieldXS(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill XS field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, canUseAsVsxRegister(reg), "Attempt to fill XS field with %s, which is not a VSR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldXS(cursor);
   }

static void fillFieldRA(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill RA field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_GPR, "Attempt to fill RA field with %s, which is not a GPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRA(cursor);
   }

static void fillFieldFRA(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill FRA field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_FPR, "Attempt to fill FRA field with %s, which is not an FPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldFRA(cursor);
   }

static void fillFieldVRA(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill VRA field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_VRF, "Attempt to fill VRA field with %s, which is not a VR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRA(cursor);
   }

static void fillFieldXA(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill XA field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, canUseAsVsxRegister(reg), "Attempt to fill XA field with %s, which is not a VSR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldXA(cursor);
   }

static void fillFieldRB(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill RB field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_GPR, "Attempt to fill RB field with %s, which is not a GPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRB(cursor);
   }

static void fillFieldFRB(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill FRB field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_FPR, "Attempt to fill FRB field with %s, which is not an FPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldFRB(cursor);
   }

static void fillFieldVRB(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill VRB field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_VRF, "Attempt to fill VRB field with %s, which is not a VR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRB(cursor);
   }

static void fillFieldXB(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill XB field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, canUseAsVsxRegister(reg), "Attempt to fill XB field with %s, which is not a VSR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldXB(cursor);
   }

static void fillFieldRC(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill RC field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_GPR, "Attempt to fill RC field with %s, which is not a GPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRC(cursor);
   }

static void fillFieldFRC(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill FRC field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_FPR, "Attempt to fill FRC field with %s, which is not an FPR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldFRC(cursor);
   }

static void fillFieldVRC(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill VRC field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_VRF, "Attempt to fill VRC field with %s, which is not a VR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRC(cursor);
   }

static void fillFieldXC(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill XC field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, canUseAsVsxRegister(reg), "Attempt to fill XC field with %s, which is not a VSR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldXC(cursor);
   }

static void fillFieldBI(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill BI field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_CCR, "Attempt to fill BI field with %s, which is not a CCR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldBI(cursor);
   }

static void fillFieldBF(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill BF field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_CCR, "Attempt to fill BF field with %s, which is not a CCR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRT(cursor);
   }

static void fillFieldBFA(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill BFA field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_CCR, "Attempt to fill BFA field with %s, which is not a CCR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRA(cursor);
   }

static void fillFieldBFA(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x7) == val, "0x%x is out-of-range for BFA field", val);
   *cursor |= val << 18;
   }

static void fillFieldBFB(TR::Instruction *instr, uint32_t *cursor, TR::RealRegister *reg)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg, "Attempt to fill BFB field with null register");
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, reg->getKind() == TR_CCR, "Attempt to fill BFB field with %s, which is not a CCR", reg->getRegisterName(instr->cg()->comp()));
   reg->setRegisterFieldRB(cursor);
   }

static void fillFieldU(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xfu) == val, "0x%x is out-of-range for U field", val);
   *cursor |= val << 12;
   }

static void fillFieldBFW(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xfu) == val, "0x%x is out-of-range for BF/W field", val);
   *cursor |= ((val ^ 0x8) & 0x8) << 13;
   *cursor |= (val & 0x7) << 23;
   }

static void fillFieldFLM(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xffu) == val, "0x%x is out-of-range for FLM field", val);
   *cursor |= val << 17;
   }

static void fillFieldFXM(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xffu) == val, "0x%x is out-of-range for FXM field", val);
   *cursor |= val << 12;
   }

static void fillFieldFXM1(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, populationCount(val) == 1, "0x%x is invalid for FXM field, expecting exactly 1 bit set", val);
   fillFieldFXM(instr, cursor, val);
   }

static void fillFieldDCM(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3fu) == val, "0x%x is out-of-range for DCM/DQM field", val);
   *cursor |= val << 10;
   }

static void fillFieldSH5(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x1fu) == val, "0x%x is out-of-range for SH(5) field", val);
   *cursor |= val << 11;
   }

static void fillFieldSH6(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3fu) == val, "0x%x is out-of-range for SH(6) field", val);
   *cursor |= (val & 0x1fu) << 11;
   *cursor |= (val & 0x20u) >> 4;
   }

static void fillFieldSIM(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, isValidInSignExtendedField(val, 0x1fu), "0x%x is out-of-range for SIM field", val);
   *cursor |= (val & 0x1f) << 16;
   }

static void fillFieldSI(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, isValidInSignExtendedField(val, 0xffffu), "0x%x is out-of-range for SI field", val);
   *cursor |= val & 0xffff;
   }

static void fillFieldSI5(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, isValidInSignExtendedField(val, 0x1fu), "0x%x is out-of-range for SI(5) field", val);
   *cursor |= (val & 0x1f) << 11;
   }

static void fillFieldUIM(TR::Instruction *instr, uint32_t *cursor, int32_t numBits, uint32_t val)
   {
   uint32_t fieldMask = (1u << numBits) - 1;
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & fieldMask) == val, "0x%x is out-of-range for UIM(%d) field", val, numBits);
   *cursor |= val << 16;
   }

static void fillFieldUI(TR::Instruction *instr, uint32_t *cursor, uint32_t val)
   {
   // TODO: This is a hack until PIC sites are reworked. Currently, the PIC site handling code
   // assumes that the entire address is in the immediate of the instruction, so we can't chop it to
   // 16 bits like we should.
   if (!instr->cg()->comp()->isPICSite(instr))
      TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xffffu) == val, "0x%x is out-of-range for UI field", val);
   *cursor |= val & 0xffff;
   }

static void fillFieldM6(TR::Instruction *instr, uint32_t *cursor, int32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3f) == val, "0x%x is out-of-range for ME(6)/MB(6) field", val);
   *cursor |= (val & 0x1f) << 6;
   *cursor |= (val & 0x20);
   }

static void fillFieldMB5(TR::Instruction *instr, uint32_t *cursor, int32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x1f) == val, "0x%x is out-of-range for MB(5) field", val);
   *cursor |= val << 6;
   }

static void fillFieldME5(TR::Instruction *instr, uint32_t *cursor, int32_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x1f) == val, "0x%x is out-of-range for ME(5) field", val);
   *cursor |= val << 1;
   }

static void fillFieldRMC(TR::Instruction *instr, uint32_t *cursor, uint64_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3) == val, "0x%llx is out-of-range for RMC field", val);
   *cursor |= static_cast<uint32_t>(val << 9);
   }

static void fillFieldSHB(TR::Instruction *instr, uint32_t *cursor, uint64_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0xf) == val, "0x%llx is out-of-range for SHB field", val);
   *cursor |= static_cast<uint32_t>(val << 6);
   }

static void fillFieldDM(TR::Instruction *instr, uint32_t *cursor, uint64_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3) == val, "0x%llx is out-of-range for DM field", val);
   *cursor |= static_cast<uint32_t>(val << 8);
   }

static void fillFieldSHW(TR::Instruction *instr, uint32_t *cursor, uint64_t val)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (val & 0x3) == val, "0x%llx is out-of-range for SHW field", val);
   *cursor |= static_cast<uint32_t>(val << 8);
   }

uint8_t *
OMR::Power::Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = self()->cg()->getBinaryBufferCursor();
   uint8_t *cursor = instructionStart;
   TR::InstOpCode& opCode = self()->getOpCode();

   cursor = opCode.copyBinaryToBuffer(cursor);
   self()->fillBinaryEncodingFields(reinterpret_cast<uint32_t*>(cursor));

   cursor += opCode.getBinaryLength();

   TR_ASSERT_FATAL_WITH_INSTRUCTION(
      self(),
      (cursor - instructionStart) <= getEstimatedBinaryLength(),
      "Estimated binary length was %u bytes, but actual length was %u bytes",
      getEstimatedBinaryLength(),
      static_cast<uint32_t>(cursor - instructionStart)
   );

   self()->setBinaryLength(cursor - instructionStart);
   self()->setBinaryEncoding(instructionStart);

   return cursor;
   }

void
OMR::Power::Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   switch (self()->getOpCode().getFormat())
      {
      case FORMAT_NONE:
         break;

      case FORMAT_DIRECT:
         // TODO: Split genop into two instructions depending on version of Power in use
         if (self()->getOpCodeValue() == TR::InstOpCode::genop)
            {
            TR::RealRegister *r = self()->cg()->machine()->getRealRegister(TR::Compiler->target.cpu.id() > TR_PPCp6 ? TR::RealRegister::gr2 : TR::RealRegister::gr1);
            fillFieldRA(self(), cursor, r);
            fillFieldRS(self(), cursor, r);
            }
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by Instruction", self()->getOpCode().getFormat());
      }
   }

int32_t
OMR::Power::Instruction::estimateBinaryLength(int32_t currentEstimate)
   {
   int8_t maxLength = self()->getOpCode().getMaxBinaryLength();

   self()->setEstimatedBinaryLength(maxLength);
   return currentEstimate + maxLength;
   }

uint8_t *TR::PPCAlignmentNopInstruction::generateBinaryEncoding()
   {
   bool trace = cg()->comp()->getOption(TR_TraceCG);
   uint32_t currentMisalign = reinterpret_cast<uintptr_t>(cg()->getBinaryBufferCursor()) % _alignment;

   if (currentMisalign)
      {
      uint32_t nopsToAdd = (_alignment - currentMisalign) / PPC_INSTRUCTION_LENGTH;

      // For performance reasons, the last nop added might be different than the others, e.g. on P6
      // and above a group-ending nop is typically used. Since we add nops in reverse order, we add
      // this special nop first. All other padding instructions will be regular nops.
      TR::Instruction *lastNop = generateInstruction(cg(), getOpCodeValue(), getNode(), self());
      lastNop->setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH);

      if (trace)
         traceMsg(cg()->comp(), "Expanding alignment nop %p into %u instructions: [ %p ", self(), nopsToAdd, lastNop);

      for (uint32_t i = 1; i < nopsToAdd; i++)
         {
         TR::Instruction *nop = generateInstruction(cg(), TR::InstOpCode::nop, getNode(), self());
         nop->setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH);

         if (trace)
            traceMsg(cg()->comp(), "%p ", nop);
         }

      if (trace)
         traceMsg(cg()->comp(), "]\n");
      }
   else
      {
      if (trace)
         traceMsg(cg()->comp(), "Eliminating alignment nop %p, since the next instruction is already aligned\n", self());
      }

   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - currentMisalign);

   // When the trace log prints the list of instructions after binary encoding, we don't want this
   // instruction to show up any more. Removing it from the linked list of instructions does this
   // without affecting this instruction's next pointer, so the binary encoding loop can continue
   // and encode the actual nops we emitted as if nothing happened.
   self()->remove();

   return cg()->getBinaryBufferCursor();
   }

int32_t TR::PPCAlignmentNopInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   self()->setEstimatedBinaryLength(_alignment - PPC_INSTRUCTION_LENGTH);
   return currentEstimate + self()->getEstimatedBinaryLength();
   }

uint8_t TR::PPCAlignmentNopInstruction::getBinaryLengthLowerBound()
   {
   return 0;
   }

void TR::PPCLabelInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::LabelSymbol *label = getLabelSymbol();

   switch (getOpCode().getFormat())
      {
      case FORMAT_NONE:
         if (getOpCodeValue() == TR::InstOpCode::label)
            label->setCodeLocation(reinterpret_cast<uint8_t*>(cursor));
         break;

      case FORMAT_I_FORM:
         if (label->getCodeLocation())
            cg()->apply24BitLabelRelativeRelocation(reinterpret_cast<int32_t*>(cursor), label);
         else
            cg()->addRelocation(new (cg()->trHeapMemory()) TR::LabelRelative24BitRelocation(reinterpret_cast<uint8_t*>(cursor), label));
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCLabelInstruction", getOpCode().getFormat());
      }
   }

int32_t TR::PPCLabelInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   if (getOpCodeValue() == TR::InstOpCode::label)
      getLabelSymbol()->setEstimatedCodeLocation(currentEstimate);

   return self()->TR::Instruction::estimateBinaryLength(currentEstimate);
   }

// TODO This should probably be refactored and moved onto OMR::Power::InstOpCode
static bool reversedConditionalBranchOpCode(TR::InstOpCode::Mnemonic op, TR::InstOpCode::Mnemonic *rop)
   {
   switch (op)
      {
      case TR::InstOpCode::bdnz:
         *rop = TR::InstOpCode::bdz;
         return(false);
      case TR::InstOpCode::bdz:
         *rop = TR::InstOpCode::bdnz;
         return(false);
      case TR::InstOpCode::beq:
         *rop = TR::InstOpCode::bne;
         return(false);
      case TR::InstOpCode::beql:
         *rop = TR::InstOpCode::bne;
         return(true);
      case TR::InstOpCode::bge:
         *rop = TR::InstOpCode::blt;
         return(false);
      case TR::InstOpCode::bgel:
         *rop = TR::InstOpCode::blt;
         return(true);
      case TR::InstOpCode::bgt:
         *rop = TR::InstOpCode::ble;
         return(false);
      case TR::InstOpCode::bgtl:
         *rop = TR::InstOpCode::ble;
         return(true);
      case TR::InstOpCode::ble:
         *rop = TR::InstOpCode::bgt;
         return(false);
      case TR::InstOpCode::blel:
         *rop = TR::InstOpCode::bgt;
         return(true);
      case TR::InstOpCode::blt:
         *rop = TR::InstOpCode::bge;
         return(false);
      case TR::InstOpCode::bltl:
         *rop = TR::InstOpCode::bge;
         return(true);
      case TR::InstOpCode::bne:
         *rop = TR::InstOpCode::beq;
         return(false);
      case TR::InstOpCode::bnel:
         *rop = TR::InstOpCode::beq;
         return(true);
      case TR::InstOpCode::bnun:
         *rop = TR::InstOpCode::bun;
         return(false);
      case TR::InstOpCode::bun:
         *rop = TR::InstOpCode::bnun;
         return(false);
      default:
         TR_ASSERT(0, "New PPC conditional branch opcodes have to have corresponding reversed opcode: %d\n", (int32_t)op);
         *rop = TR::InstOpCode::bad;
         return(false);
      }
   }

void TR::PPCConditionalBranchInstruction::expandIntoFarBranch()
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), getLabelSymbol(), "Cannot expand conditional branch without a label");

   if (comp()->getOption(TR_TraceCG))
      traceMsg(comp(), "Expanding conditional branch instruction %p into a far branch\n", self());

   TR::InstOpCode::Mnemonic newOpCode;
   bool wasLinkForm = reversedConditionalBranchOpCode(getOpCodeValue(), &newOpCode);

   setOpCodeValue(newOpCode);

   TR::LabelSymbol *skipBranchLabel = generateLabelSymbol(cg());
   skipBranchLabel->setEstimatedCodeLocation(getEstimatedBinaryLocation() + 4);

   TR::Instruction *branchInstr = generateLabelInstruction(cg(), wasLinkForm ? TR::InstOpCode::bl : TR::InstOpCode::b, getNode(), getLabelSymbol(), self());
   branchInstr->setEstimatedBinaryLength(4);

   TR::Instruction *labelInstr = generateLabelInstruction(cg(), TR::InstOpCode::label, getNode(), skipBranchLabel, branchInstr);
   labelInstr->setEstimatedBinaryLength(0);

   setLabelSymbol(skipBranchLabel);
   setEstimatedBinaryLength(4);
   reverseLikeliness();
   _farRelocation = true;
   }

void TR::PPCConditionalBranchInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   switch (getOpCode().getFormat())
      {
      case FORMAT_B_FORM:
         {
         TR::LabelSymbol *label = getLabelSymbol();
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), label, "B-form conditional branch has no label");

         if (label->getCodeLocation())
            cg()->apply16BitLabelRelativeRelocation(reinterpret_cast<int32_t*>(cursor), label);
         else
            cg()->addRelocation(new (cg()->trHeapMemory()) TR::LabelRelative16BitRelocation(reinterpret_cast<uint8_t*>(cursor), label));
         break;
         }

      case FORMAT_XL_FORM_BRANCH:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), !getLabelSymbol(), "XL-form conditional branch has a label");
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCConditionalBranchInstruction", getOpCode().getFormat());
      }

   fillFieldBI(self(), cursor, toRealRegister(_conditionRegister));
   if (haveHint())
      {
      if (getOpCode().setsCTR())
         *cursor |= (getLikeliness() ? PPCOpProp_BranchLikelyMaskCtr : PPCOpProp_BranchUnlikelyMaskCtr);
      else
         *cursor |= (getLikeliness() ? PPCOpProp_BranchLikelyMask : PPCOpProp_BranchUnlikelyMask);
      }
   }

int32_t TR::PPCConditionalBranchInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), getOpCode().getMaxBinaryLength() == PPC_INSTRUCTION_LENGTH, "Format %d cannot be binary encoded by PPCConditionalBranchInstruction", getOpCode().getFormat());

   // Conditional branches can be expanded into a conditional branch around an unconditional branch if the target label
   // is out of range for a simple bc instruction. This is done by expandFarConditionalBranches, which runs after binary
   // length estimation but before binary encoding and will call PPCConditionalBranchInstruction::expandIntoFarBranch to
   // expand the branch into two instructions. For this reason, we conservatively assume that any conditional branch
   // could be expanded to ensure that the binary length estimates are correct.
   setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 2);
   setEstimatedBinaryLocation(currentEstimate);

   return currentEstimate + getEstimatedBinaryLength();
   }

void TR::PPCAdminInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), getOpCode().getFormat() == FORMAT_NONE, "Format %d cannot be binary encoded by PPCAdminInstruction", getOpCode().getFormat());

   if (getOpCodeValue() == TR::InstOpCode::fence)
      {
      TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), _fenceNode, "Fence instruction is missing a fence node");
      TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), _fenceNode->getRelocationType() == TR_EntryRelative32Bit, "Unhandled relocation type %u", _fenceNode->getRelocationType());

      for (int i = 0; i < _fenceNode->getNumRelocations(); i++)
         *static_cast<uint32_t*>(_fenceNode->getRelocationDestination(i)) = cg()->getCodeLength();
      }
   else
      {
      TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), !_fenceNode, "Non-fence instruction has a fence node %p", _fenceNode);
      }
   }

void
TR::PPCImmInstruction::addMetaDataForCodeAddress(uint8_t *cursor)
   {

   if (needsAOTRelocation())
      {
         switch(getReloKind())
            {
            case TR_AbsoluteHelperAddress:
               cg()->addExternalRelocation(new (cg()->trHeapMemory()) TR::ExternalRelocation(cursor, (uint8_t *)getSymbolReference(), TR_AbsoluteHelperAddress, cg()), __FILE__, __LINE__, getNode());
               break;
            case TR_RamMethod:
               if (comp()->getOption(TR_UseSymbolValidationManager))
                  {
                  cg()->addExternalRelocation(
                     new (comp()->trHeapMemory()) TR::ExternalRelocation(
                        cursor,
                        (uint8_t *)comp()->getJittedMethodSymbol()->getResolvedMethod()->resolvedMethodAddress(),
                        (uint8_t *)TR::SymbolType::typeMethod,
                        TR_SymbolFromManager,
                        cg()),
                     __FILE__,
                     __LINE__,
                     getNode());
                  }
               else
                  {
                  cg()->addExternalRelocation(new (cg()->trHeapMemory()) TR::ExternalRelocation(cursor, NULL, TR_RamMethod, cg()), __FILE__, __LINE__, getNode());
                  }
               break;
            case TR_BodyInfoAddress:
               cg()->addExternalRelocation(new (cg()->trHeapMemory()) TR::ExternalRelocation(cursor, 0, TR_BodyInfoAddress, cg()), __FILE__, __LINE__, getNode());
               break;
            default:
               TR_ASSERT(false, "Unsupported AOT relocation type specified.");
            }
      }

   TR::Compilation *comp = cg()->comp();
   if (std::find(comp->getStaticPICSites()->begin(), comp->getStaticPICSites()->end(), this) != comp->getStaticPICSites()->end())
      {
      // none-HCR: low-tag to invalidate -- BE or LE is relevant
      //
      void *valueToHash = *(void**)(cursor - (comp->target().is64Bit()?4:0));
      void *addressToPatch = comp->target().is64Bit()?
         (comp->target().cpu.isBigEndian()?cursor:(cursor-4)) : cursor;
      cg()->jitAddPicToPatchOnClassUnload(valueToHash, addressToPatch);
      }

   if (std::find(comp->getStaticHCRPICSites()->begin(), comp->getStaticHCRPICSites()->end(), this) != comp->getStaticHCRPICSites()->end())
      {
      // HCR: whole pointer replacement.
      //
      void **locationToPatch = (void**)(cursor - (comp->target().is64Bit()?4:0));
      cg()->jitAddPicToPatchOnClassRedefinition(*locationToPatch, locationToPatch);
      cg()->addExternalRelocation(new (cg()->trHeapMemory()) TR::ExternalRelocation((uint8_t *)locationToPatch, (uint8_t *)*locationToPatch, TR_HCR, cg()), __FILE__,__LINE__, getNode());
      }

   }

void TR::PPCImmInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   addMetaDataForCodeAddress(reinterpret_cast<uint8_t*>(cursor));

   switch (getOpCode().getFormat())
      {
      case FORMAT_DD:
         *cursor = getSourceImmediate();
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCImmInstruction", getOpCode().getFormat());
      }
   }

void TR::PPCImm2Instruction::fillBinaryEncodingFields(uint32_t* cursor)
   {
   uint32_t imm1 = getSourceImmediate();
   uint32_t imm2 = getSourceImmediate2();

   switch (getOpCode().getFormat())
      {
      case FORMAT_MTFSFI:
         fillFieldU(self(), cursor, imm1);
         fillFieldBFW(self(), cursor, imm2);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCImm2Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCSrc1Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *src = toRealRegister(getSource1Register());
   uint32_t imm = getSourceImmediate();

   switch (getOpCode().getFormat())
      {
      case FORMAT_MTFSF:
         fillFieldFRB(self(), cursor, src);
         fillFieldFLM(self(), cursor, imm);
         break;

      case FORMAT_RS:
         fillFieldRS(self(), cursor, src);
         break;

      case FORMAT_RA_SI:
         fillFieldRA(self(), cursor, src);
         fillFieldSI(self(), cursor, imm);
         break;

      case FORMAT_RA_SI5:
         fillFieldRA(self(), cursor, src);
         fillFieldSI5(self(), cursor, imm);
         break;

      case FORMAT_RS_FXM:
         fillFieldRS(self(), cursor, src);
         fillFieldFXM(self(), cursor, imm);
         break;

      case FORMAT_RS_FXM1:
         fillFieldRS(self(), cursor, src);
         fillFieldFXM1(self(), cursor, imm);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCSrc1Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCTrg1Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());

   switch (getOpCode().getFormat())
      {
      case FORMAT_RT:
         fillFieldRT(self(), cursor, trg);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCTrg1Src1Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src = toRealRegister(getSource1Register());

   switch (getOpCode().getFormat())
      {
      case FORMAT_RA_RS:
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         break;

      case FORMAT_RT_RA:
         fillFieldRT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src);
         break;

      case FORMAT_FRT_FRB:
         fillFieldFRT(self(), cursor, trg);
         fillFieldFRB(self(), cursor, src);
         break;

      case FORMAT_BF_BFA:
         fillFieldBF(self(), cursor, trg);
         fillFieldBFA(self(), cursor, src);
         break;

      case FORMAT_RA_XS:
         fillFieldRA(self(), cursor, trg);
         fillFieldXS(self(), cursor, src);
         break;

      case FORMAT_XT_RA:
         fillFieldXT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src);
         break;

      case FORMAT_RT_BFA:
         fillFieldRT(self(), cursor, trg);
         fillFieldBFA(self(), cursor, src);
         break;

      case FORMAT_VRT_VRB:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRB(self(), cursor, src);
         break;

      case FORMAT_XT_XB:
         fillFieldXT(self(), cursor, trg);
         fillFieldXB(self(), cursor, src);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src1Instruction", getOpCode().getFormat());
      }
   }

void
TR::PPCTrg1ImmInstruction::addMetaDataForCodeAddress(uint8_t *cursor)
   {
   TR::Compilation *comp = cg()->comp();

   if (std::find(comp->getStaticPICSites()->begin(), comp->getStaticPICSites()->end(), this) != comp->getStaticPICSites()->end())
      {
      TR::Node *node = getNode();
      cg()->jitAddPicToPatchOnClassUnload((void *)(comp->target().is64Bit()?node->getLongInt():node->getInt()), (void *)cursor);
      }

   if (std::find(comp->getStaticMethodPICSites()->begin(), comp->getStaticMethodPICSites()->end(), this) != comp->getStaticMethodPICSites()->end())
      {
      TR::Node *node = getNode();
      cg()->jitAddPicToPatchOnClassUnload((void *) (cg()->fe()->createResolvedMethod(cg()->trMemory(), (TR_OpaqueMethodBlock *) (comp->target().is64Bit()?node->getLongInt():node->getInt()), comp->getCurrentMethod())->classOfMethod()), (void *)cursor);
      }
   }

void
TR::PPCTrg1ImmInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   uint32_t imm = getSourceImmediate();

   addMetaDataForCodeAddress(reinterpret_cast<uint8_t*>(cursor));

   switch (getOpCode().getFormat())
      {
      case FORMAT_RT_SI:
         fillFieldRT(self(), cursor, trg);
         fillFieldSI(self(), cursor, imm);
         break;

      case FORMAT_BF_BFAI:
         fillFieldBF(self(), cursor, trg);
         fillFieldBFA(self(), cursor, imm);
         break;

      case FORMAT_RT_FXM:
         fillFieldRT(self(), cursor, trg);
         fillFieldFXM(self(), cursor, imm);
         break;

      case FORMAT_RT_FXM1:
         fillFieldRT(self(), cursor, trg);
         fillFieldFXM1(self(), cursor, imm);
         break;

      case FORMAT_VRT_SIM:
         fillFieldVRT(self(), cursor, trg);
         fillFieldSIM(self(), cursor, imm);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1ImmInstruction", getOpCode().getFormat());
      }
   }

void
TR::PPCTrg1Src1ImmInstruction::addMetaDataForCodeAddress(uint8_t *cursor)
   {
   TR::Compilation *comp = cg()->comp();

   if (std::find(comp->getStaticPICSites()->begin(), comp->getStaticPICSites()->end(), this) != comp->getStaticPICSites()->end())
      {
      cg()->jitAddPicToPatchOnClassUnload((void *)(getSourceImmPtr()), (void *)cursor);
      }
   if (std::find(comp->getStaticMethodPICSites()->begin(), comp->getStaticMethodPICSites()->end(), this) != comp->getStaticMethodPICSites()->end())
      {
      cg()->jitAddPicToPatchOnClassUnload((void *) (cg()->fe()->createResolvedMethod(cg()->trMemory(), (TR_OpaqueMethodBlock *) (getSourceImmPtr()), comp->getCurrentMethod())->classOfMethod()), (void *)cursor);
      }
   }

void TR::PPCTrg1Src1ImmInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src = toRealRegister(getSource1Register());
   uint32_t imm = getSourceImmediate();

   addMetaDataForCodeAddress(reinterpret_cast<uint8_t*>(cursor));

   switch (getOpCode().getFormat())
      {
      case FORMAT_RT_RA_SI:
         fillFieldRT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src);
         fillFieldSI(self(), cursor, imm);
         break;

      case FORMAT_RA_RS_UI:
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldUI(self(), cursor, imm);
         break;

      case FORMAT_BF_RA_SI:
         fillFieldBF(self(), cursor, trg);
         fillFieldRA(self(), cursor, src);
         fillFieldSI(self(), cursor, imm);
         break;

      case FORMAT_BF_RA_UI:
         fillFieldBF(self(), cursor, trg);
         fillFieldRA(self(), cursor, src);
         fillFieldUI(self(), cursor, imm);
         break;

      case FORMAT_BF_FRA_DM:
         fillFieldBF(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src);
         fillFieldDCM(self(), cursor, imm);
         break;

      case FORMAT_RA_RS_SH5:
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH5(self(), cursor, imm);
         break;

      case FORMAT_RA_RS_SH6:
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH6(self(), cursor, imm);
         break;

      case FORMAT_VRT_VRB_UIM4:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRB(self(), cursor, src);
         fillFieldUIM(self(), cursor, 4, imm);
         break;

      case FORMAT_VRT_VRB_UIM3:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRB(self(), cursor, src);
         fillFieldUIM(self(), cursor, 3, imm);
         break;

      case FORMAT_VRT_VRB_UIM2:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRB(self(), cursor, src);
         fillFieldUIM(self(), cursor, 2, imm);
         break;

      case FORMAT_XT_XB_UIM2:
         fillFieldXT(self(), cursor, trg);
         fillFieldXB(self(), cursor, src);
         fillFieldUIM(self(), cursor, 2, imm);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src1ImmInstruction", getOpCode().getFormat());
      }
   }

static std::pair<int32_t, int32_t> getMaskEnds64(TR::Instruction *instr, uint64_t mask)
   {
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, mask != 0, "Cannot encode a mask of 0");

   int32_t lead = leadingZeroes(mask);
   int32_t trail = trailingZeroes(mask);

   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, mask == ((0xffffffffffffffffuLL >> lead) & (0xffffffffffffffffuLL << trail)), "Mask of 0x%llx has more than one group of 1 bits", mask);

   return std::make_pair(lead, 63 - trail);
   }

static std::pair<int32_t, int32_t> getMaskEnds32(TR::Instruction *instr, uint64_t mask)
   {
   // TODO While it would be nice to enable this assert, there are numerous false positives at the
   //      moment due to use of int32_t for masks. When converted to int64_t to generate an rlwinm
   //      instruction, this causes the mask to be improperly sign-extended. We *should* be using
   //      unsigned integers instead, but this assert needs to remain disabled until that can be
   //      fixed.
   // TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, (mask & 0xffffffff) == mask, "Invalid 32-bit mask 0x%llx", mask);
   mask &= 0xffffffff;
   TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, mask != 0, "Cannot encode a mask of 0");

   uint32_t mask32 = static_cast<uint32_t>(mask);

   if (mask32 != 0xffffffffu && (mask32 & 0x80000001u) == 0x80000001u)
      {
      int32_t lead = leadingZeroes(~mask32);
      int32_t trail = trailingZeroes(~mask32);

      TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, mask32 == ~((0xffffffffu >> lead) & (0xffffffffu << trail)), "Mask of 0x%x has more than one group of 1 bits", mask32);

      return std::make_pair(32 - trail, lead - 1);
      }
   else
      {
      int32_t lead = leadingZeroes(mask32);
      int32_t trail = trailingZeroes(mask32);

      TR_ASSERT_FATAL_WITH_INSTRUCTION(instr, mask32 == ((0xffffffffu >> lead) & (0xffffffffu << trail)), "Mask of 0x%x has more than one group of 1 bits", mask32);

      return std::make_pair(lead, 31 - trail);
      }
   }

void TR::PPCTrg1Src1Imm2Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src = toRealRegister(getSource1Register());
   uint32_t imm1 = getSourceImmediate();
   uint64_t imm2 = getLongMask();

   switch (getOpCode().getFormat())
      {
      case FORMAT_RLDIC:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH6(self(), cursor, imm1);

         auto maskEnds = getMaskEnds64(self(), imm2);
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), maskEnds.second == 63 - imm1 && maskEnds.first <= maskEnds.second, "Mask of 0x%llx does not match rldic-form for shift by %u", imm2, imm1);
         fillFieldM6(self(), cursor, maskEnds.first);
         break;
         }

      case FORMAT_RLDICL:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH6(self(), cursor, imm1);

         auto maskEnds = getMaskEnds64(self(), imm2);
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), maskEnds.second == 63 && maskEnds.first <= maskEnds.second, "Mask of 0x%llx does not match rldicl-form", imm2);
         fillFieldM6(self(), cursor, maskEnds.first);
         break;
         }

      case FORMAT_RLDICR:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH6(self(), cursor, imm1);

         auto maskEnds = getMaskEnds64(self(), imm2);
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), maskEnds.first == 0 && maskEnds.first <= maskEnds.second, "Mask of 0x%llx does not match rldicr-form", imm2);
         fillFieldM6(self(), cursor, maskEnds.second);
         break;
         }

      case FORMAT_RLWINM:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src);
         fillFieldSH5(self(), cursor, imm1);

         auto maskEnds = getMaskEnds32(self(), imm2);
         fillFieldMB5(self(), cursor, maskEnds.first);
         fillFieldME5(self(), cursor, maskEnds.second);
         break;
         }

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src1Imm2Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCTrg1Src2Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src1 = toRealRegister(getSource1Register());
   TR::RealRegister *src2 = toRealRegister(getSource2Register());

   switch (getOpCode().getFormat())
      {
      case FORMAT_RT_RA_RB:
         fillFieldRT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         break;

      case FORMAT_RA_RS_RB:
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         break;

      case FORMAT_BF_RA_RB:
         fillFieldBF(self(), cursor, trg);
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         break;

      case FORMAT_BF_FRA_FRB:
         fillFieldBF(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src1);
         fillFieldFRB(self(), cursor, src2);
         break;

      case FORMAT_FRT_FRA_FRB:
         fillFieldFRT(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src1);
         fillFieldFRB(self(), cursor, src2);
         break;

      case FORMAT_VRT_RA_RB:
         fillFieldVRT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         break;

      case FORMAT_VRT_VRA_VRB:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRA(self(), cursor, src1);
         fillFieldVRB(self(), cursor, src2);
         break;

      case FORMAT_XT_XA_XB:
         fillFieldXT(self(), cursor, trg);
         fillFieldXA(self(), cursor, src1);
         fillFieldXB(self(), cursor, src2);
         break;

      case FORMAT_FRT_FRA_FRC:
         fillFieldFRT(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src1);
         fillFieldFRC(self(), cursor, src2);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src2Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCTrg1Src2ImmInstruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src1 = toRealRegister(getSource1Register());
   TR::RealRegister *src2 = toRealRegister(getSource2Register());
   uint64_t imm = getLongMask();

   switch (getOpCode().getFormat())
      {
      case FORMAT_BF_RA_RB_L:
         fillFieldBF(self(), cursor, trg);
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);

         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), (imm & 1) == imm, "0x%llx is out-of-range for L field");
         *cursor |= imm << 21;

         break;

      case FORMAT_BT_BA_BB:
         fillFieldBF(self(), cursor, trg);
         fillFieldBFA(self(), cursor, src1);
         fillFieldBFB(self(), cursor, src2);

         // TODO The API for specifying which CCR fields to use should be improved
         *cursor |= imm;

         break;

      case FORMAT_FRT_FRA_FRB_RMC:
         fillFieldFRT(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src1);
         fillFieldFRB(self(), cursor, src2);
         fillFieldRMC(self(), cursor, imm);

         break;

      case FORMAT_RLDCL:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);

         auto maskEnds = getMaskEnds64(self(), imm);
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), maskEnds.second == 63 && maskEnds.first <= maskEnds.second, "Mask of 0x%llx does not match rldcl-form", imm);

         fillFieldM6(self(), cursor, maskEnds.first);

         break;
         }

      case FORMAT_RLWNM:
         {
         fillFieldRA(self(), cursor, trg);
         fillFieldRS(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);

         auto maskEnds = getMaskEnds32(self(), imm);
         fillFieldMB5(self(), cursor, maskEnds.first);
         fillFieldME5(self(), cursor, maskEnds.second);

         break;
         }

      case FORMAT_VRT_VRA_VRB_SHB:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRA(self(), cursor, src1);
         fillFieldVRB(self(), cursor, src2);
         fillFieldSHB(self(), cursor, imm);
         break;

      case FORMAT_XT_XA_XB_DM:
         fillFieldXT(self(), cursor, trg);
         fillFieldXA(self(), cursor, src1);
         fillFieldXB(self(), cursor, src2);
         fillFieldDM(self(), cursor, imm);
         break;

      case FORMAT_XT_XA_XB_SHW:
         fillFieldXT(self(), cursor, trg);
         fillFieldXA(self(), cursor, src1);
         fillFieldXB(self(), cursor, src2);
         fillFieldSHW(self(), cursor, imm);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src2ImmInstruction", getOpCode().getFormat());
      }
   }

void TR::PPCTrg1Src3Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *trg = toRealRegister(getTargetRegister());
   TR::RealRegister *src1 = toRealRegister(getSource1Register());
   TR::RealRegister *src2 = toRealRegister(getSource2Register());
   TR::RealRegister *src3 = toRealRegister(getSource3Register());

   switch (getOpCode().getFormat())
      {
      case FORMAT_RT_RA_RB_RC:
         fillFieldRT(self(), cursor, trg);
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         fillFieldRC(self(), cursor, src3);
         break;

      case FORMAT_FRT_FRA_FRC_FRB:
         fillFieldFRT(self(), cursor, trg);
         fillFieldFRA(self(), cursor, src1);
         fillFieldFRC(self(), cursor, src2);
         fillFieldFRB(self(), cursor, src3);
         break;

      case FORMAT_VRT_VRA_VRB_VRC:
         fillFieldVRT(self(), cursor, trg);
         fillFieldVRA(self(), cursor, src1);
         fillFieldVRB(self(), cursor, src2);
         fillFieldVRC(self(), cursor, src3);
         break;

      case FORMAT_XT_XA_XB_XC:
         fillFieldXT(self(), cursor, trg);
         fillFieldXA(self(), cursor, src1);
         fillFieldXB(self(), cursor, src2);
         fillFieldXC(self(), cursor, src3);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCTrg1Src3Instruction", getOpCode().getFormat());
      }
   }

void TR::PPCSrc2Instruction::fillBinaryEncodingFields(uint32_t *cursor)
   {
   TR::RealRegister *src1 = toRealRegister(getSource1Register());
   TR::RealRegister *src2 = toRealRegister(getSource2Register());

   switch (getOpCode().getFormat())
      {
      case FORMAT_RA_RB:
         fillFieldRA(self(), cursor, src1);
         fillFieldRB(self(), cursor, src2);
         break;

      default:
         TR_ASSERT_FATAL_WITH_INSTRUCTION(self(), false, "Format %d cannot be binary encoded by PPCSrc2Instruction", getOpCode().getFormat());
      }
   }

uint8_t *TR::PPCMemSrc1Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;

   getMemoryReference()->mapOpCode(this);

   cursor = getOpCode().copyBinaryToBuffer(instructionStart);

   insertSourceRegister(toPPCCursor(cursor));
   cursor = getMemoryReference()->generateBinaryEncoding(this, cursor, cg());
   setBinaryLength(cursor-instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

uint8_t *TR::PPCMemInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   getMemoryReference()->mapOpCode(this);
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   cursor = getMemoryReference()->generateBinaryEncoding(this, cursor, cg());
   setBinaryLength(cursor-instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }
int32_t TR::PPCMemSrc1Instruction::estimateBinaryLength(int32_t currentEstimate)
   {
   setEstimatedBinaryLength(getMemoryReference()->estimateBinaryLength(*cg()));
   return(currentEstimate + getEstimatedBinaryLength());
   }

uint8_t *TR::PPCTrg1MemInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;

   getMemoryReference()->mapOpCode(this);

   cursor = getOpCode().copyBinaryToBuffer(instructionStart);

   insertTargetRegister(toPPCCursor(cursor));
   // Set hint bit if there is any
   // The control for the different values is done through asserts in the constructor
   if (haveHint())
   {
      *(int32_t *)instructionStart |=  getHint();
   }

   cursor = getMemoryReference()->generateBinaryEncoding(this, cursor, cg());
   setBinaryLength(cursor-instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

int32_t TR::PPCTrg1MemInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   setEstimatedBinaryLength(getMemoryReference()->estimateBinaryLength(*cg()));
   return currentEstimate + getEstimatedBinaryLength();
   }

uint8_t *TR::PPCControlFlowInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   setBinaryLength(0);
   return cursor;
   }


int32_t TR::PPCControlFlowInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   switch(getOpCodeValue())
      {
      case TR::InstOpCode::iflong:
      case TR::InstOpCode::setbool:
      case TR::InstOpCode::idiv:
      case TR::InstOpCode::ldiv:
      case TR::InstOpCode::iselect:
         if (useRegPairForResult())
            {
            if (!useRegPairForCond())
               setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 6);
            else
               setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 8);
            }
         else
            {
            if (!useRegPairForCond())
               setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 4);
            else
               setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 6);
            }
         break;
      case TR::InstOpCode::setbflt:
         setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 5);
         break;
      case TR::InstOpCode::setblong:
      case TR::InstOpCode::flcmpg:
      case TR::InstOpCode::flcmpl:
      case TR::InstOpCode::irem:
      case TR::InstOpCode::lrem:
      case TR::InstOpCode::d2i:
         setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 6);
         break;
      case TR::InstOpCode::d2l:
         if (cg()->comp()->target().is64Bit())
            setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 6);
       else
            setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 8);
         break;
      case TR::InstOpCode::lcmp:
         setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH * 11);
         break;
      default:
         TR_ASSERT(false,"unknown control flow instruction (estimateBinaryLength)");
      }
   return currentEstimate + getEstimatedBinaryLength();
   }

#ifdef J9_PROJECT_SPECIFIC
uint8_t *TR::PPCVirtualGuardNOPInstruction::generateBinaryEncoding()
   {
   uint8_t    *cursor           = cg()->getBinaryBufferCursor();
   TR::LabelSymbol *label        = getLabelSymbol();
   int32_t     length = 0;

   _site->setLocation(cursor);
   if (label->getCodeLocation() == NULL)
      {
      _site->setDestination(cursor);
      cg()->addRelocation(new (cg()->trHeapMemory()) TR::LabelAbsoluteRelocation((uint8_t *) (&_site->getDestination()), label));

#ifdef DEBUG
   if (debug("traceVGNOP"))
      printf("####> virtual location = %p, label (relocation) = %p\n", cursor, label);
#endif
      }
   else
      {
       _site->setDestination(label->getCodeLocation());
#ifdef DEBUG
   if (debug("traceVGNOP"))
      printf("####> virtual location = %p, label location = %p\n", cursor, label->getCodeLocation());
#endif
      }

   setBinaryEncoding(cursor);
   if (cg()->sizeOfInstructionToBePatched(this) == 0 ||
       // AOT needs an explicit nop, even if there are patchable instructions at this site because
       // 1) Those instructions might have AOT data relocations (and therefore will be incorrectly patched again)
       // 2) We might want to re-enable the code path and unpatch, in which case we would have to know what the old instruction was
         cg()->comp()->compileRelocatableCode())
      {
      TR::InstOpCode opCode(TR::InstOpCode::nop);
      opCode.copyBinaryToBuffer(cursor);
      length = PPC_INSTRUCTION_LENGTH;
      }

   setBinaryLength(length);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor+length;
   }

int32_t TR::PPCVirtualGuardNOPInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   // This is a conservative estimation for reserving NOP space.
   setEstimatedBinaryLength(PPC_INSTRUCTION_LENGTH);
   return currentEstimate+PPC_INSTRUCTION_LENGTH;
   }
#endif
