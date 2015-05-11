// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/code-generator.h"

#include "src/compiler/code-generator-impl.h"
#include "src/compiler/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/s390/macro-assembler-s390.h"
#include "src/scopes.h"

namespace v8 {
namespace internal {
namespace compiler {

#define __ masm()->


#define kScratchReg r13


// Adds S390-specific methods to convert InstructionOperands.
class S390OperandConverter FINAL : public InstructionOperandConverter {
 public:
  S390OperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}


  bool CompareLogical() const {
    switch (instr_->flags_condition()) {
      case kUnsignedLessThan:
      case kUnsignedGreaterThanOrEqual:
      case kUnsignedLessThanOrEqual:
      case kUnsignedGreaterThan:
        return true;
      default:
        return false;
    }
    UNREACHABLE();
    return false;
  }

  Operand InputImmediate(size_t index) {
    Constant constant = ToConstant(instr_->InputAt(index));
    switch (constant.type()) {
      case Constant::kInt32:
        return Operand(constant.ToInt32());
      case Constant::kFloat32:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat32(), TENURED));
      case Constant::kFloat64:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat64(), TENURED));
      case Constant::kInt64:
#if V8_TARGET_ARCH_S390X
        return Operand(constant.ToInt64());
#endif
      case Constant::kExternalReference:
      case Constant::kHeapObject:
      case Constant::kRpoNumber:
        break;
    }
    UNREACHABLE();
    return Operand::Zero();
  }

  MemOperand MemoryOperand(AddressingMode* mode, size_t* first_index) {
    const size_t index = *first_index;
    *mode = AddressingModeField::decode(instr_->opcode());
    switch (*mode) {
      case kMode_None:
        break;
      case kMode_MRI:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputInt32(index + 1));
      case kMode_MRR:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputRegister(index + 1));
    }
    UNREACHABLE();
    return MemOperand(r0);
  }

  MemOperand MemoryOperand(AddressingMode* mode, size_t first_index = 0) {
    return MemoryOperand(mode, &first_index);
  }

  MemOperand ToMemOperand(InstructionOperand* op) const {
    DCHECK(op != NULL);
    DCHECK(!op->IsRegister());
    DCHECK(!op->IsDoubleRegister());
    DCHECK(op->IsStackSlot() || op->IsDoubleStackSlot());
    // The linkage computes where all spill slots are located.
    FrameOffset offset = linkage()->GetFrameOffset(op->index(), frame(), 0);
    return MemOperand(offset.from_stack_pointer() ? sp : fp, offset.offset());
  }
};


static inline bool HasRegisterInput(Instruction* instr, int index) {
  return instr->InputAt(index)->IsRegister();
}


namespace {

class OutOfLineLoadNAN32 FINAL : public OutOfLineCode {
 public:
  OutOfLineLoadNAN32(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() FINAL {
    __ LoadDoubleLiteral(result_, std::numeric_limits<float>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};


class OutOfLineLoadNAN64 FINAL : public OutOfLineCode {
 public:
  OutOfLineLoadNAN64(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() FINAL {
    __ LoadDoubleLiteral(result_, std::numeric_limits<double>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};


class OutOfLineLoadZero FINAL : public OutOfLineCode {
 public:
  OutOfLineLoadZero(CodeGenerator* gen, Register result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() FINAL { __ LoadImmP(result_, Operand::Zero()); }

 private:
  Register const result_;
};


Condition FlagsConditionToCondition(FlagsCondition condition) {
  switch (condition) {
    case kEqual:
      return eq;
    case kNotEqual:
      return ne;
    case kSignedLessThan:
    case kUnsignedLessThan:
      return lt;
    case kSignedGreaterThanOrEqual:
    case kUnsignedGreaterThanOrEqual:
      return ge;
    case kSignedLessThanOrEqual:
    case kUnsignedLessThanOrEqual:
      return le;
    case kSignedGreaterThan:
    case kUnsignedGreaterThan:
      return gt;
    case kOverflow:
#if V8_TARGET_ARCH_S390X
      return ne;
#else
      return lt;
#endif
    case kNotOverflow:
#if V8_TARGET_ARCH_S390X
      return eq;
#else
      return ge;
#endif
    case kUnorderedEqual:
    case kUnorderedNotEqual:
      break;
  }
  UNREACHABLE();
  return kNoCondition;
}

}  // namespace

#define ASSEMBLE_FLOAT_UNOP_RC(asm_instr)                            \
  do {                                                               \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));\
  } while (0)


#define ASSEMBLE_FLOAT_BINOP_RC(asm_instr)                           \
  do {                                                               \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0), \
                 i.InputDoubleRegister(1));                          \
  } while (0)


#define ASSEMBLE_BINOP(asm_instr_reg, asm_instr_imm)           \
  do {                                                         \
    if (HasRegisterInput(instr, 1)) {                          \
      __ asm_instr_reg(i.OutputRegister(), i.InputRegister(0), \
                       i.InputRegister(1));                    \
    } else {                                                   \
      __ asm_instr_imm(i.OutputRegister(), i.InputRegister(0), \
                       i.InputImmediate(1));                   \
    }                                                          \
  } while (0)


#define ASSEMBLE_BINOP_RC(asm_instr_reg, asm_instr_imm)        \
  do {                                                         \
    if (HasRegisterInput(instr, 1)) {                          \
      __ asm_instr_reg(i.OutputRegister(), i.InputRegister(0), \
                       i.InputRegister(1));                    \
    } else {                                                   \
      __ asm_instr_imm(i.OutputRegister(), i.InputRegister(0), \
                       i.InputImmediate(1));                   \
    }                                                          \
  } while (0)


#define ASSEMBLE_BINOP_INT_RC(asm_instr_reg, asm_instr_imm)    \
  do {                                                         \
    if (HasRegisterInput(instr, 1)) {                          \
      __ asm_instr_reg(i.OutputRegister(), i.InputRegister(0), \
                       i.InputRegister(1));   \
    } else {                                                   \
      __ asm_instr_imm(i.OutputRegister(), i.InputRegister(0), \
                       i.InputInt32(1));      \
    }                                                          \
  } while (0)


#if V8_TARGET_ARCH_S390X
#define ASSEMBLE_ADD_WITH_OVERFLOW()             \
  do {                                           \
    ASSEMBLE_BINOP(AddP, AddP); \
    __ TestIfInt32(i.OutputRegister(), r0); \
  } while (0)
#else
#define ASSEMBLE_ADD_WITH_OVERFLOW()                                    \
  do {                                                                  \
    if (HasRegisterInput(instr, 1)) {                                   \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputRegister(1), kScratchReg, r0);   \
    } else {                                                            \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputInt32(1), kScratchReg, r0);      \
    }                                                                   \
  } while (0)
#endif


#if V8_TARGET_ARCH_S390X
#define ASSEMBLE_SUB_WITH_OVERFLOW()             \
  do {                                           \
    ASSEMBLE_BINOP(SubP, SubP); \
    __ TestIfInt32(i.OutputRegister(), r0); \
  } while (0)
#else
#define ASSEMBLE_SUB_WITH_OVERFLOW()                                    \
  do {                                                                  \
    if (HasRegisterInput(instr, 1)) {                                   \
      __ SubAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputRegister(1), kScratchReg, r0);   \
    } else {                                                            \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                -i.InputInt32(1), kScratchReg, r0);     \
    }                                                                   \
  } while (0)
#endif


#define ASSEMBLE_COMPARE(cmp_instr, cmpl_instr)                        \
  do {                                                                 \
    if (HasRegisterInput(instr, 1)) {                                  \
      if (i.CompareLogical()) {                                        \
        __ cmpl_instr(i.InputRegister(0), i.InputRegister(1));         \
      } else {                                                         \
        __ cmp_instr(i.InputRegister(0), i.InputRegister(1));          \
      }                                                                \
    } else {                                                           \
      if (i.CompareLogical()) {                                        \
        __ cmpl_instr##i(i.InputRegister(0), i.InputImmediate(1));     \
      } else {                                                         \
        __ cmp_instr##i(i.InputRegister(0), i.InputImmediate(1));      \
      }                                                                \
    }                                                                  \
  } while (0)


#define ASSEMBLE_FLOAT_COMPARE(cmp_instr)                                 \
  do {                                                                    \
    __ cmp_instr(i.InputDoubleRegister(0), i.InputDoubleRegister(1);      \
  } while (0)


// Divide instruction dr will implicity use register pair
// r0 & r1 below.
// R0:R1 = R1 / divisor - R0 remainder
// Copy remainder to output reg
#define ASSEMBLE_MODULO(load_instr)      \
  do {                                   \
  DCHECK(!left_reg.is(r1));              \
  DCHECK(!right_reg.is(r1));             \
  DCHECK(!result_reg.is(r1));            \
  __ load_instr(r0, i.InputRegister(0)); \
  __ srda(r0, Operand(32));              \
  __ dr(r0, i.InputRegister(1));         \
  __ ltr(i.OutputRegister(), r0);        \
  } while (0)


#define ASSEMBLE_FLOAT_MODULO()                                             \
  do {                                                                      \
  FrameScope scope(masm(), StackFrame::MANUAL);                             \
  __ PrepareCallCFunction(0, 2, kScratchReg);                               \
  __ MovToFloatParameters(i.InputDoubleRegister(0),                         \
  i.InputDoubleRegister(1));                                                \
  __ CallCFunction(ExternalReference::mod_two_doubles_operation(isolate()), \
  0, 2);                                                                    \
  __ MovFromFloatResult(i.OutputDoubleRegister());                          \
  } while (0)


#define ASSEMBLE_FLOAT_MAX(double_scratch_reg, general_scratch_reg)           \
  do {                                                                        \
    Label ge, done;                                                           \
    __ ldr(double_scratch_reg, i.InputDoubleRegister(0));                     \
    __ sdbr(double_scratch_reg, i.InputDoubleRegister(1));                    \
	__ lgdr(general_scratch_reg, double_scratch_reg);                         \
	__ CmpP(general_scratch_reg, Operand::Zero());                            \
    __ bge(&ge, Label::kNear);                                                \
  	__ LoadRR(i.OutputDoubleRegister(), i.InputDoubleRegister(1));            \
	__ b(&done, Label::kNear);                                                \
	__ bind(&ge);                                                             \
	__ LoadRR(i.OutputDoubleRegister(), i.InputDoubleRegister(0));            \
	__ bind(&done);                                                           \
  } while (0)


#define ASSEMBLE_FLOAT_MIN(double_scratch_reg, general_scratch_reg)           \
  do {                                                                        \
    Label ge, done;                                                           \
    __ ldr(scratch_reg, i.InputDoubleRegister(0));                            \
    __ sdbr(scratch_reg, i.InputDoubleRegister(1));                           \
	__ lgdr(general_scratch_reg, double_scratch_reg);                         \
	__ CmpP(general_scratch_reg, Operand::Zero());                            \
    __ bge(&ge, Label::kNear);                                                \
  	__ LoadRR(i.OutputDoubleRegister(), i.InputDoubleRegister(0));            \
	__ b(&done, Label::kNear);                                                \
	__ bind(&ge);                                                             \
	__ LoadRR(i.OutputDoubleRegister(), i.InputDoubleRegister(1));            \
	__ bind(&done);                                                           \
  } while (0)


//Only MRI mode for these instructions available
#define ASSEMBLE_LOAD_FLOAT(asm_instr)                \
  do {                                                \
    DoubleRegister result = i.OutputDoubleRegister(); \
    MemOperand operand = i.MemoryOperand();           \
    __ asm_instr(result, operand);                    \
  } while (0)


#define ASSEMBLE_LOAD_INTEGER(asm_instr)             \
  do {                                               \
    Register result = i.OutputRegister();            \
    AddressingMode mode = kMode_None;                \
    MemOperand operand = i.MemoryOperand(&mode);     \
    __ asm_instr(result, operand);                 \
  } while (0)


#define ASSEMBLE_STORE_FLOAT(asm_instr)                  \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    DoubleRegister value = i.InputDoubleRegister(index); \
    __ asm_instr(value, operand);                        \
  } while (0)


#define ASSEMBLE_STORE_INTEGER(asm_instr)                \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    Register value = i.InputRegister(index);             \
    __ asm_instr(value, operand);                        \
  } while (0)


// TODO(Tara): Check this implementation and those of all the ASSEMBLE_CHECKED_* methods
// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_LOAD_FLOAT(asm_instr, width)              \
  do {                                                             \
    DoubleRegister result = i.OutputDoubleRegister();              \
    size_t index = 0;                                              \
    MemOperand operand = i.MemoryOperand(index);                   \
    Register offset = operand.rb();                                \
    __ lgfr(offset, offset);                                       \
    if (HasRegisterInput(instr, 2)) {                              \
      __ CmpLogical32(offset, i.InputRegister(2));                 \
    } else {                                                       \
      __ CmpLogical32(offset, i.InputImmediate(2));                \
    }                                                              \
    auto ool = new (zone()) OutOfLineLoadNAN##width(this, result); \
    __ bge(ool->entry());                                          \
    __ asm_instr(result, operand);                                 \
    __ bind(ool->exit());                                          \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_LOAD_INTEGER(asm_instr)             \
  do {                                                       \
    Register result = i.OutputRegister();                    \
    size_t index = 0;                                        \
    MemOperand operand = i.MemoryOperand(index);             \
    Register offset = operand.rb();                          \
    __ lgfr(offset, offset);                                 \
    if (HasRegisterInput(instr, 2)) {                        \
      __ CmpLogical32(offset, i.InputRegister(2));           \
    } else {                                                 \
      __ CmpLogical32(offset, i.InputImmediate(2));          \
    }                                                        \
    auto ool = new (zone()) OutOfLineLoadZero(this, result); \
    __ bge(ool->entry());                                    \
    __ asm_instr(result, operand);                           \
    __ bind(ool->exit());                                    \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_STORE_FLOAT(asm_instr)             \
  do {                                                      \
    Label done;                                             \
    size_t index = 0;                                       \
    AddressingMode mode = kMode_None;                       \
    MemOperand operand = i.MemoryOperand(&mode, index);     \
    Register offset = operand.rb();                         \
    __ lgfr(offset, offset);                                \
    if (HasRegisterInput(instr, 2)) {                       \
      __ CmpLogical32(offset, i.InputRegister(2));          \
    } else {                                                \
      __ CmpLogical32(offset, i.InputImmediate(2));         \
    }                                                       \
    __ bge(&done);                                          \
    DoubleRegister value = i.InputDoubleRegister(3);        \
    __ asm_instr(value, operand);                           \
    __ bind(&done);                                         \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_STORE_INTEGER(asm_instr)             \
  do {                                                        \
    Label done;                                               \
    size_t index = 0;                                         \
    AddressingMode mode = kMode_None;                         \
    MemOperand operand = i.MemoryOperand(&mode, index);       \
    Register offset = operand.rb();                           \
    __ lgfr(offset, offset);                                  \
    if (HasRegisterInput(instr, 2)) {                         \
      __ CmpLogical32(offset, i.InputRegister(2));            \
    } else {                                                  \
      __ CmpLogical32(offset, i.InputImmediate(2));           \
    }                                                         \
    __ bge(&done);                                            \
    Register value = i.InputRegister(3);                      \
    __ asm_instr(value, operand);                             \
    __ bind(&done);                                           \
  } while (0)


#define ASSEMBLE_STORE_WRITE_BARRIER()                                   \
  do {                                                                   \
  Register object = i.InputRegister(0);                                  \
  Register index = i.InputRegister(1);                                   \
  Register value = i.InputRegister(2);                                   \
  __ AddP(index, object);                                                \
  __ StoreP(value, MemOperand(index));                                   \
  SaveFPRegsMode mode =                                                  \
  frame()->DidAllocateDoubleRegisters() ? kSaveFPRegs : kDontSaveFPRegs; \
  LinkRegisterStatus lr_status = kLRHasNotBeenSaved;                     \
  __ RecordWrite(object, index, value, lr_status, mode);                 \
  } while (0)


// Assembles an instruction after register allocation, producing machine code.
void CodeGenerator::AssembleArchInstruction(Instruction* instr) {
  S390OperandConverter i(this, instr);
  ArchOpcode opcode = ArchOpcodeField::decode(instr->opcode());

  switch (opcode) {
    case kArchCallCodeObject: {
      EnsureSpaceForLazyDeopt();
      if (HasRegisterInput(instr, 0)) {
        __ AddP(ip, i.InputRegister(0),
                Operand(Code::kHeaderSize - kHeapObjectTag));
        __ Call(ip);
      } else {
        __ Call(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      }
      RecordCallPosition(instr);
      break;
    }
    case kArchCallJSFunction: {
      EnsureSpaceForLazyDeopt();
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ LoadP(kScratchReg,
                 FieldMemOperand(func, JSFunction::kContextOffset));
        __ CmpP(cp, kScratchReg);
        __ Assert(eq, kWrongFunctionContext);
      }
      __ LoadP(ip, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Call(ip);
      RecordCallPosition(instr);
      break;
    }
    case kArchJmp:
      __ b(code_->GetLabel(i.InputBlock(0)));
      break;
    case kArchLookupSwitch:
      AssembleArchLookupSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchNop:
      // don't emit code for nops.
      break;
    case kArchDeoptimize: {
      int deopt_state_id =
          BuildTranslation(instr, -1, 0, OutputFrameStateCombine::Ignore());
      AssembleDeoptimizerCall(deopt_state_id, Deoptimizer::EAGER);
      break;
    }
    case kArchRet:
      AssembleReturn();
      break;
    case kArchStackPointer:
      __ LoadRR(i.OutputRegister(), sp);
      break;
    case kArchTruncateDoubleToI:
      // TODO(mbrandy): move slow call to stub out of line.
      __ TruncateDoubleToI(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_And32:
      ASSEMBLE_BINOP(And, And);
      break;
    case kS390_And64:
      ASSEMBLE_BINOP(AndP, AndP);
      break;
    case kS390_AndComplement32:
    case kS390_AndComplement64:
   	  __ NotP(i.InputRegister(1));
      __ AndP(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
    case kS390_Or32:
      ASSEMBLE_BINOP(Or, Or);
      break;
    case kS390_Or64:
      ASSEMBLE_BINOP(OrP, OrP);
      break;
    case kS390_OrComplement32:
    case kS390_OrComplement64:
	  __ NotP(i.InputRegister(1));
      __ OrP(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
    case kS390_Xor32:
       ASSEMBLE_BINOP(Xor, Xor);
      break;
    case kS390_Xor64:
      ASSEMBLE_BINOP(XorP, XorP);
      break;
    case kS390_ShiftLeft32:
      ASSEMBLE_BINOP(ShiftLeft, ShiftLeft);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftLeft64:
      ASSEMBLE_BINOP(sllg, sllg);
      break;
#endif
    case kS390_ShiftRight32:
      ASSEMBLE_BINOP(ShiftRight, ShiftRight);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftRight64:
      ASSEMBLE_BINOP(srlg, srlg);
      break;
#endif
    case kS390_ShiftRightAlg32:
      ASSEMBLE_BINOP(ShiftRightArith, ShiftRightArith);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftRightAlg64:
      ASSEMBLE_BINOP(srag, srag);
      break;
#endif
    case kS390_RotRight32:
      if (HasRegisterInput(instr, 1)) {
        __ LoadComplementRR(kScratchReg, i.InputRegister(1));
        __ rll(i.OutputRegister(), i.InputRegister(0), kScratchReg);
      } else {
        DCHECK(0); //Not implemented for now
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_RotRight64:
      if (HasRegisterInput(instr, 1)) {
        __ LoadComplementRR(kScratchReg, i.InputRegister(1));
        __ rll(i.OutputRegister(), i.InputRegister(0), kScratchReg,
               Operand(32));
	    __ lgfr(i.OutputRegister(), i.OutputRegister());
      } else {
        UNIMPLEMENTED(); //Not implemented for now
      }
      break;
#endif
    case kS390_Not32:
    case kS390_Not64:
      __ LoadRR(i.OutputRegister(), i.InputRegister(0));
      __ NotP(i.OutputRegister());
      break;
    case kS390_RotLeftAndMask32:
//      __ rlwinm(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
//                31 - i.InputInt32(2), 31 - i.InputInt32(3), i.OutputRCBit());
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_RotLeftAndClear64:
//      __ rldic(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
//               63 - i.InputInt32(2), i.OutputRCBit());
        UNIMPLEMENTED(); //Find correct instruction
      break;
    case kS390_RotLeftAndClearLeft64:
//      __ rldicl(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
//                63 - i.InputInt32(2), i.OutputRCBit());
        UNIMPLEMENTED(); //Find correct instruction
      break;
    case kS390_RotLeftAndClearRight64:
 //     __ rldicr(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
 //               63 - i.InputInt32(2), i.OutputRCBit());  == sldi
      UNIMPLEMENTED();  //Confirm this sllg is correct
      __ sllg(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
              63 - i.InputInt32(2));
      break;
#endif
    case kS390_Add32:
    case kS390_Add64:
      ASSEMBLE_BINOP(AddP, AddP);
      break;
    case kS390_AddWithOverflow32:
      ASSEMBLE_ADD_WITH_OVERFLOW();
      break;
    case kS390_AddFloat64:
    // Ensure we don't clobber right/InputReg(1)
    if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        ASSEMBLE_FLOAT_UNOP(adbr);
    } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ adbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
    }
      break;
    case kS390_Sub32:
      ASSEMBLE_BINOP(Sub32, Sub32);
      break;
    case kS390_Sub64:
      ASSEMBLE_BINOP(SubP, SubP);
      break;
    case kS390_SubWithOverflow32:
      ASSEMBLE_SUB_WITH_OVERFLOW();
      break;
    case kS390_SubFloat64:
    // InputDoubleReg(1) = i.InputDoubleRegister(0) - i.InputDoubleRegister(1)
    if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        __ ldr(kScratchDoubleReg, i.InputDoubleRegister(1));
        __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
        __ sdbr(i.OutputDoubleRegister(), kScratchDoubleReg);
      } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
          __ sdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
      }
      break;
    case kS390_Mul32:
#if V8_TARGET_ARCH_S390X
    case kS390_Mul64:
      __ Mul(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
#endif
    case kS390_MulHigh32:
      UNIMPLEMENTED();
      break;
    case kS390_MulHighU32:
      UNIMPLEMENTED();
      break;
    case kS390_MulFloat64:
      // Ensure we don't clobber right
      if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        ASSEMBLE_FLOAT_UNOP(mdbr);
      } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
        __ mdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Div64:
    case kS390_DivU64:
#endif
    case kS390_Div32:
    case kS390_DivU32:
      __ LoadRR(r0, i.InputRegister(0));
      __ srda(r0, Operand(32));
      __ dr(r0, i.InputRegister(1));   // R0:R1 = R1 / divisor - R0 remainder
      __ ltr(i.OutputRegister(), r0);  // Copy remainder to output reg
      break;

    case kS390_DivFloat64:
      // InputDoubleRegister(1)=InputDoubleRegister(0)/InputDoubleRegister(1)
      if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
      __ ldr(kScratchDoubleReg, i.InputDoubleRegister(1));
      __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ ddbr(i.OutputDoubleRegister(), kScratchDoubleReg);
      } else {
      if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
      __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ ddbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
}
      break;
    case kS390_Mod32:
    case kS390_ModU32:
      ASSEMBLE_MODULO(lr);
      break;
#if V8_TARGET_ARCH_S390x
    case kS390_Mod64:
    case kS390_ModU64:
      ASSEMBLE_MODULO(lgr);
      break;
#endif
    case kS390_ModFloat64:
      // TODO(bmeurer): We should really get rid of this special instruction,
      // and generate a CallAddress instruction instead.
      ASSEMBLE_FLOAT_MODULO();
      break;
    case kS390_Neg32:
      __ lcr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Neg64:
      __ lcgr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_MaxFloat64:
      ASSEMBLE_FLOAT_MAX(kScratchDoubleReg, kScratchReg);
      break;
    case kS390_MinFloat64:
      ASSEMBLE_FLOAT_MIN(kScratchDoubleReg, kScratchReg);
      break;
    case kS390_SqrtFloat64:
      ASSEMBLE_FLOAT_UNOP(sqdbr);
      break;
    case kS390_FloorFloat64:
//      ASSEMBLE_FLOAT_UNOP_RC(frim);
      UNIMPLEMENTED();
      break;
    case kS390_CeilFloat64:
//      ASSEMBLE_FLOAT_UNOP_RC(frip);
      UNIMPLEMENTED();
      break;
    case kS390_TruncateFloat64:
	  UNIMPLEMENTED();
//      ASSEMBLE_FLOAT_UNOP_RC(friz);
      break;
    case kS390_RoundFloat64:
     	UNIMPLEMENTED();
//      ASSEMBLE_FLOAT_UNOP_RC(frin);
      break;
    case kS390_NegFloat64:
      ASSEMBLE_FLOAT_UNOP_RC(lcdbr);
      break;
    case kS390_Cntlz32:
//     __ cntlzw_(i.OutputRegister(), i.InputRegister(0));
       __ llgfr(i.OutputRegister(), i.InputRegister(0));
       __ flogr(kScratchReg, i.OutputRegister());
       __ LoadRR(i.OutputRegister(), kScratchReg);
       __ SubP(i.OutputRegister(), Operand(32));
      DCHECK(0); //Check if this is correct
      break;
    case kS390_Cmp32:
      ASSEMBLE_COMPARE(Cmp32, CmpLogical32);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Cmp64:
      ASSEMBLE_COMPARE(CmpP, CmpLogicalP);
      break;
#endif
    case kS390_CmpFloat64:
      __ cdbr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      break;
    case kS390_Tst32:
      if (HasRegisterInput(instr, 1)) {
        __ AndP(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        __ AndP(r0, i.InputRegister(0), i.InputImmediate(1));
      }
#if V8_TARGET_ARCH_S390X
      __ lgfr(r0, r0);
#endif
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Tst64:
      if (HasRegisterInput(instr, 1)) {
        __ AndP(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        __ AndP(r0, i.InputRegister(0), i.InputImmediate(1));
      }
      break;
#endif
    case kS390_Push:
      __ Push(i.InputRegister(0));
      break;
    case kS390_ExtendSignWord8:
	  #if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.InputRegister(0));
	  #else
	  __ lbr(i.OutputRegister(), i.InputRegister(0));
	  #endif
      break;
    case kS390_ExtendSignWord16:
      #if V8_TARGET_ARCH_S390X
      __ lghr(i.OutputRegister(), i.InputRegister(0));
	  #else
	  __ lhr(i.OutputRegister(), i.InputRegister(0));
	  #endif
      break;
#if V8_TARGET_ARCH_S390X
    case k390_ExtendSignWord32:
      __ lgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Uint32ToUint64:
      // Zero extend
	  __ llgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Int64ToInt32:
      // TODO(mbrandy): sign extend?
      __ Move(i.OutputRegister(), i.InputRegister(0));
      break;
#endif
    case kS390_Int32ToFloat64:
      __ ConvertIntToDouble(i.InputRegister(0), i.OutputDoubleRegister());
      break;
    case kS390_Uint32ToFloat64:
      __ ConvertUnsignedIntToDouble(i.InputRegister(0),
                                    i.OutputDoubleRegister());
      break;
    case kS390_Float64ToInt32:
    case kS390_Float64ToUint32:
      __ ConvertDoubleToInt64(i.InputDoubleRegister(0),
#if !V8_TARGET_ARCH_S390X
                              kScratchReg,
#endif
                              i.OutputRegister(), kScratchDoubleReg);
      break;
    case kS390_Float64ToFloat32:
      UNIMPLEMENTED();
// floating point single precision rounding
      break;
    case kS390_Float32ToFloat64:
      // Nothing to do.
      __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_Float64ExtractLowWord32:
      __ MovDoubleLowToInt(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_Float64ExtractHighWord32:
      __ MovDoubleHighToInt(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_Float64InsertLowWord32:
      __ InsertDoubleLow(i.OutputDoubleRegister(), i.InputRegister(1), r0);
      break;
    case kS390_Float64InsertHighWord32:
      __ InsertDoubleHigh(i.OutputDoubleRegister(), i.InputRegister(1), r0);
      break;
    case kS390_Float64Construct:
#if V8_TARGET_ARCH_S390X
      __ MovInt64ComponentsToDouble(i.OutputDoubleRegister(),
                                    i.InputRegister(0), i.InputRegister(1), r0);
#else
      __ MovInt64ToDouble(i.OutputDoubleRegister(), i.InputRegister(0),
                          i.InputRegister(1));
#endif
      break;
    case kS390_LoadWordU8:
      ASSEMBLE_LOAD_INTEGER(LoadlB);
	  //__ LoadlB(i.OutputRegister(), i.MemoryOperand());
      break;
    case kS390_LoadWordS8:
      ASSEMBLE_LOAD_INTEGER(LoadlB);
#if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.OutputRegister());
#else
      __ lbr(i.OutputRegister(), i.OutputRegister());
#endif
      break;
    case kS390_LoadWordU16:
      ASSEMBLE_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kS390_LoadWordS16:
      ASSEMBLE_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kS390_LoadWordS32:
      ASSEMBLE_LOAD_INTEGER(LoadW);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_LoadWord64:
      ASSEMBLE_LOAD_INTEGER(lg);
      break;
#endif
    case kS390_LoadFloat32:
      ASSEMBLE_LOAD_FLOAT(ldeb);
      break;
    case kS390_LoadFloat64:
      ASSEMBLE_LOAD_FLOAT(ld);
      break;
    case kS390_StoreWord8:
      ASSEMBLE_STORE_INTEGER(StoreByte);
      break;
    case kS390_StoreWord16:
      ASSEMBLE_STORE_INTEGER(StoreHalfWord);
      break;
    case kS390_StoreWord32:
      ASSEMBLE_STORE_INTEGER(StoreW);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_StoreWord64:
      ASSEMBLE_STORE_INTEGER(StoreP);
      break;
#endif
    case kS390_StoreFloat32:
      ASSEMBLE_STORE_FLOAT(StoreShortF);
      break;
    case kS390_StoreFloat64:
      ASSEMBLE_STORE_FLOAT(StoreF);
      break;
    case kS390_StoreWriteBarrier:
      ASSEMBLE_STORE_WRITE_BARRIER();
      break;
    case kCheckedLoadInt8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlB);
#if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.OutputRegister());
#else
	    __ lbr(i.OutputRegister(), i.OutputRegister());
#endif
      break;
    case kCheckedLoadUint8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlB);
      break;
    case kCheckedLoadInt16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kCheckedLoadUint16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kCheckedLoadWord32:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadW);
      break;
    case kCheckedLoadFloat32:
      ASSEMBLE_CHECKED_LOAD_FLOAT(ldeb, 32);
      break;
    case kCheckedLoadFloat64:
      ASSEMBLE_CHECKED_LOAD_FLOAT(ld, 64);
      break;
    case kCheckedStoreWord8:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreByte);
      break;
    case kCheckedStoreWord16:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreHalfWord);
      break;
    case kCheckedStoreWord32:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreW);
      break;
    case kCheckedStoreFloat32:
      ASSEMBLE_CHECKED_STORE_FLOAT(StoreShortF);
      break;
    case kCheckedStoreFloat64:
      ASSEMBLE_CHECKED_STORE_FLOAT(StoreF);
      break;
    default:
      UNREACHABLE();
      break;
  }
}


// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  S390OperandConverter i(this, instr);
  Label* tlabel = branch->true_label;
  Label* flabel = branch->false_label;
  ArchOpcode op = instr->arch_opcode();
  FlagsCondition condition = branch->condition;

  // Overflow checked for add/sub only.
  DCHECK((condition != kOverflow && condition != kNotOverflow) ||
         (op == kS390_AddWithOverflow32 || op == kS390_SubWithOverflow32));

  Condition cond = FlagsConditionToCondition(condition);
  if (op == kS390_CmpFloat64) {
    // check for unordered if necessary
    if (cond == le) {
      __ bunordered(flabel);
      // Unnecessary for eq/lt since only FU bit will be set.
    } else if (cond == gt) {
      __ bunordered(tlabel);
      // Unnecessary for ne/ge since only FU bit will be set.
    }
  }
  __ b(cond, tlabel);
  if (!branch->fallthru) __ b(flabel);  // no fallthru to flabel.
}


void CodeGenerator::AssembleArchJump(RpoNumber target) {
  if (!IsNextInAssemblyOrder(target)) __ b(GetLabel(target));
}


// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  S390OperandConverter i(this, instr);
  Label done;
  ArchOpcode op = instr->arch_opcode();
  bool check_unordered = (op == kS390_CmpFloat64);
  CRegister cr = cr0;

  // Overflow checked for add/sub only.
  DCHECK((condition != kOverflow && condition != kNotOverflow) ||
         (op == kS390_AddWithOverflow32 || op == kS390_SubWithOverflow32));

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  DCHECK_NE(0u, instr->OutputCount());
  Register reg = i.OutputRegister(instr->OutputCount() - 1);

  Condition cond = FlagsConditionToCondition(condition);
  switch (cond) {
    case eq:
    case lt:
      __ LoadImmP(reg, Operand::Zero());
      __ LoadImmP(kScratchReg, Operand(1));
      //__ isel(cond, reg, kScratchReg, reg, cr);
	  Label cond_false;
	  __ b(NegateCondition(cond), &cond_false, Label::kNear);
	  __ LoadRR(reg, kScratchReg);
	  __ bind(&cond_false);
	  
      break;
    case ne:
    case ge:
      __ LoadImmP(reg, Operand(1));
     // __ isel(NegateCondition(cond), reg, r0, reg, cr);
	 Label cond_true;
	 __ b(cond, &cond_true, Label::kNear);
	 __ LoadRR(reg, r0);
	 __ bind(&cond_true);
      break;
    case gt:
      if (check_unordered) {
        __ LoadImmP(reg, Operand(1));
        __ LoadImmP(kScratchReg, Operand::Zero());
        __ bunordered(&done);
        //__ isel(cond, reg, reg, kScratchReg, cr);
		Label cond_true;
		__ b(cond, &cond_true, Label::kNear);
		__ LoadRR(reg, kScratchReg);
		__ bind(&cond_true);
      } else {
        __ LoadImmP(reg, Operand::Zero());
        __ LoadImmP(kScratchReg, Operand(1));
        //__ isel(cond, reg, kScratchReg, reg, cr);
        Label cond_false;
		__ b(NegateCondition(cond), cond_false, Label::kNear);
		__ LoadRR(reg, kScratchReg);
		__ bind(&cond_false);
      }
      break;
    case le:
      if (check_unordered) {
        __ LoadImmP(reg, Operand::Zero());
        __ LoadImmP(kScratchReg, Operand(1));
        __ bunordered(&done);
        //__ isel(NegateCondition(cond), reg, r0, kScratchReg, cr);
		Label cond_true, load_done;
		__ b(cond, &cond_true, Label::kNear);
		__ LoadRR(reg, r0);
		__ b(&load_done, Label::kNear);
		__ bind(&cond_true);
		__ LoadRR(reg, kScratchReg);
		__ bind(&load_done);
      } else {
        __ LoadImmP(reg, Operand(1));
        //__ isel(NegateCondition(cond), reg, r0, reg, cr);
        Label cond_true;
		__ b(cond, &cond_true, Label::kNear);
		__ LoadRR(reg, r0);
		__ bind(&cond_true);
      }
      break;
    default:
      UNREACHABLE();
      break;
  }
  __ bind(&done);
}


void CodeGenerator::AssembleArchLookupSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  for (size_t index = 2; index < instr->InputCount(); index += 2) {
    __ CmpP(input, Operand(i.InputInt32(index + 0)));
    __ beq(GetLabel(i.InputRpo(index + 1)));
  }
  AssembleArchJump(i.InputRpo(1));
}


void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  int32_t const case_count = static_cast<int32_t>(instr->InputCount() - 2);
  Label** cases = zone()->NewArray<Label*>(case_count);
  for (int32_t index = 0; index < case_count; ++index) {
    cases[index] = GetLabel(i.InputRpo(index + 2));
  }
  Label* const table = AddJumpTable(cases, case_count);
  __ CmpLogicalP(input, Operand(case_count));
  __ bge(GetLabel(i.InputRpo(1)));
  __ mov_label_addr(kScratchReg, table);
  __ ShiftLeftP(r0, input, Operand(kPointerSizeLog2));
  __ LoadP(kScratchReg, MemOperand(kScratchReg, r0));
  __ Jump(kScratchReg);
}


void CodeGenerator::AssembleDeoptimizerCall(
    int deoptimization_id, Deoptimizer::BailoutType bailout_type) {
  Address deopt_entry = Deoptimizer::GetDeoptimizationEntry(
      isolate(), deoptimization_id, bailout_type);
  __ Call(deopt_entry, RelocInfo::RUNTIME_ENTRY);
}


void CodeGenerator::AssemblePrologue() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  int stack_slots = frame()->GetSpillSlotCount();
  if (descriptor->kind() == CallDescriptor::kCallAddress) {
//    __ function_descriptor();
    int register_save_area_size = 0;
    RegList frame_saves = fp.bit();
    __ Push(r14, fp);
    __ LoadRR(fp, sp);
    // Save callee-saved registers.
    const RegList saves = descriptor->CalleeSavedRegisters() & ~frame_saves;
    for (int i = Register::kNumRegisters - 1; i >= 0; i--) {
      if (!((1 << i) & saves)) continue;
      register_save_area_size += kPointerSize;
    }
    frame()->SetRegisterSaveAreaSize(register_save_area_size);
    __ MultiPush(saves);
  } else if (descriptor->IsJSFunctionCall()) {
    CompilationInfo* info = this->info();
    __ Prologue(info->IsCodePreAgingActive());
    frame()->SetRegisterSaveAreaSize(
        StandardFrameConstants::kFixedFrameSizeFromFp);
  } else if (stack_slots > 0) {
    __ StubPrologue();
    frame()->SetRegisterSaveAreaSize(
        StandardFrameConstants::kFixedFrameSizeFromFp);
  }

  if (info()->is_osr()) {
    // TurboFan OSR-compiled functions cannot be entered directly.
    __ Abort(kShouldNotDirectlyEnterOsrFunction);

    // Unoptimized code jumps directly to this entrypoint while the unoptimized
    // frame is still on the stack. Optimized code uses OSR values directly from
    // the unoptimized frame. Thus, all that needs to be done is to allocate the
    // remaining stack slots.
    if (FLAG_code_comments) __ RecordComment("-- OSR entrypoint --");
    osr_pc_offset_ = __ pc_offset();
    // TODO(titzer): cannot address target function == local #-1
    __ LoadP(r3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
    DCHECK(stack_slots >= frame()->GetOsrStackSlotCount());
    stack_slots -= frame()->GetOsrStackSlotCount();
  }

  if (stack_slots > 0) {
    __ lay(sp, MemOperand(sp, -stack_slots * kPointerSize));
  }
}


void CodeGenerator::AssembleReturn() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  int stack_slots = frame()->GetSpillSlotCount();
  if (descriptor->kind() == CallDescriptor::kCallAddress) {
    if (frame()->GetRegisterSaveAreaSize() > 0) {
      // Remove this frame's spill slots first.
      if (stack_slots > 0) {
        __ lay(sp, MemOperand(sp, stack_slots * kPointerSize));
      }
      // Restore registers.
      RegList frame_saves = fp.bit();
      const RegList saves = descriptor->CalleeSavedRegisters() & ~frame_saves;
      if (saves != 0) {
        __ MultiPop(saves);
      }
    }
    __ LeaveFrame(StackFrame::MANUAL);
    __ Ret();
  } else if (descriptor->IsJSFunctionCall() || stack_slots > 0) {
    int pop_count = descriptor->IsJSFunctionCall()
                        ? static_cast<int>(descriptor->JSParameterCount())
                        : 0;
    __ LeaveFrame(StackFrame::MANUAL, pop_count * kPointerSize);
    __ Ret();
  } else {
    __ Ret();
  }
}


void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, NULL);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ Move(g.ToRegister(destination), src);
    } else {
      __ StoreP(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsRegister()) {
      __ LoadP(g.ToRegister(destination), src);
    } else {
      Register temp = kScratchReg;
      __ LoadP(temp, src, r0);
      __ StoreP(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant()) {
    Constant src = g.ToConstant(source);
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst =
          destination->IsRegister() ? g.ToRegister(destination) : kScratchReg;
      switch (src.type()) {
        case Constant::kInt32:
          __ mov(dst, Operand(src.ToInt32()));
          break;
        case Constant::kInt64:
          __ mov(dst, Operand(src.ToInt64()));
          break;
        case Constant::kFloat32:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat32(), TENURED));
          break;
        case Constant::kFloat64:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat64(), TENURED));
          break;
        case Constant::kExternalReference:
          __ mov(dst, Operand(src.ToExternalReference()));
          break;
        case Constant::kHeapObject:
          __ Move(dst, src.ToHeapObject());
          break;
        case Constant::kRpoNumber:
          UNREACHABLE();  // TODO(dcarney): loading RPO constants on PPC.
          break;
      }
      if (destination->IsStackSlot()) {
        __ StoreP(dst, g.ToMemOperand(destination), r0);
      }
    } else {
      DoubleRegister dst = destination->IsDoubleRegister()
                               ? g.ToDoubleRegister(destination)
                               : kScratchDoubleReg;
      double value = (src.type() == Constant::kFloat32) ? src.ToFloat32()
                                                        : src.ToFloat64();
      __ LoadDoubleLiteral(dst, value, kScratchReg);
      if (destination->IsDoubleStackSlot()) {
        __ StoreF(dst, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsDoubleRegister()) {
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ Move(dst, src);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      __ StoreF(src, g.ToMemOperand(destination));
    }
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleRegister() || destination->IsDoubleStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsDoubleRegister()) {
      __ LoadF(g.ToDoubleRegister(destination), src);
    } else {
      DoubleRegister temp = kScratchDoubleReg;
      __ LoadF(temp, src);
      __ StoreF(temp, g.ToMemOperand(destination));
    }
  } else {
    UNREACHABLE();
  }
}


void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, NULL);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    // Register-register.
    Register temp = kScratchReg;
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      Register dst = g.ToRegister(destination);
      __ LoadRR(temp, src);
      __ LoadRR(src, dst);
      __ LoadRR(dst, temp);
    } else {
      DCHECK(destination->IsStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ LoadRR(temp, src);
      __ LoadP(src, dst);
      __ StoreP(temp, dst);
    }
#if V8_TARGET_ARCH_S390X
  } else if (source->IsStackSlot() || source->IsDoubleStackSlot()) {
#else
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsStackSlot());
#endif
    Register temp_0 = kScratchReg;
    Register temp_1 = r0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ LoadP(temp_0, src);
    __ LoadP(temp_1, dst);
    __ StoreP(temp_0, dst);
    __ StoreP(temp_1, src);
  } else if (source->IsDoubleRegister()) {
    DoubleRegister temp = kScratchDoubleReg;
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ ldr(temp, src);
      __ ldr(src, dst);
      __ ldr(dst, temp);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ ldr(temp, src);
      __ LoadF(src, dst);
      __ StoreF(temp, dst);
    }
#if !V8_TARGET_ARCH_S390X
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleStackSlot());
    DoubleRegister temp_0 = kScratchDoubleReg;
    DoubleRegister temp_1 = d0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ LoadF(temp_0, src);
    __ LoadF(temp_1, dst);
    __ StoreF(temp_0, dst);
    __ StoreF(temp_1, src);
#endif
  } else {
    // No other combinations are possible.
    UNREACHABLE();
  }
}

//emit_label_addr used only with function_descriptors

void CodeGenerator::AssembleJumpTable(Label** targets, size_t target_count) {
  for (size_t index = 0; index < target_count; ++index) {
    __ emit_label_addr(targets[index]);
  }
}


void CodeGenerator::AddNopForSmiCodeInlining() {
  // We do not insert nops for inlined Smi code.
}


void CodeGenerator::EnsureSpaceForLazyDeopt() {
  int space_needed = Deoptimizer::patch_size();
  if (!info()->IsStub()) {
    // Ensure that we have enough space after the previous lazy-bailout
    // instruction for patching the code here.
    int current_pc = masm()->pc_offset();
    DCHECK(0); //Fix this to have variable instr size for s390 into consideration
    if (current_pc < last_lazy_deopt_pc_ + space_needed) {
      int padding_size = last_lazy_deopt_pc_ + space_needed - current_pc;
      DCHECK_EQ(0, padding_size % v8::internal::Assembler::kInstrSize);
      while (padding_size > 0) {
        __ nop();
        padding_size -= v8::internal::Assembler::kInstrSize;
      }
    }
  }
  MarkLazyDeoptSite();
}

#undef __

}  // namespace compiler
}  // namespace internal
}  // namespace v8
