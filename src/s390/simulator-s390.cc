// Copyright 2012 the V8 project authors. All rights reserved.
//
// Copyright IBM Corp. 2012, 2013. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>
#include <math.h>
#include <cstdarg>
#include "v8.h"

#if defined(V8_TARGET_ARCH_S390)

#include "disasm.h"
#include "assembler.h"
#include "codegen.h"
#include "s390/constants-s390.h"
#include "s390/simulator-s390.h"
#include "s390/frames-s390.h"

#if defined(USE_SIMULATOR)

// Only build the simulator if not compiling for real PPC hardware.
namespace v8 {
namespace internal {

// This macro provides a platform independent use of sscanf. The reason for
// SScanF not being implemented in a platform independent way through
// ::v8::internal::OS in the same way as SNPrintF is that the
// Windows C Run-Time Library does not provide vsscanf.
#define SScanF sscanf  // NOLINT

// The S390Debugger class is used by the simulator while debugging simulated
// PowerPC code.
class S390Debugger {
 public:
  explicit S390Debugger(Simulator* sim) : sim_(sim) { }
  ~S390Debugger();

  void Stop(Instruction* instr);
  void Info(Instruction* instr);
  void Debug();

 private:
#if __BYTE_ORDER == __LITTLE_ENDIAN
  static const Instr kBreakpointInstr = (0x0000FFB2);  // TRAP4 0000
  static const Instr kNopInstr = (0x00160016);  // OR r0, r0 x2
#else
  static const Instr kBreakpointInstr = (0xB2FF0000);  // TRAP4 0000
  static const Instr kNopInstr = (0x16001600);  // OR r0, r0 x2
#endif

  Simulator* sim_;

  intptr_t GetRegisterValue(int regnum);
  double GetRegisterPairDoubleValue(int regnum);
  double GetFPDoubleRegisterValue(int regnum);
  bool GetValue(const char* desc, intptr_t* value);
  bool GetFPDoubleValue(const char* desc, double* value);

  // Set or delete a breakpoint. Returns true if successful.
  bool SetBreakpoint(Instruction* break_pc);
  bool DeleteBreakpoint(Instruction* break_pc);

  // Undo and redo all breakpoints. This is needed to bracket disassembly and
  // execution to skip past breakpoints when run from the debugger.
  void UndoBreakpoints();
  void RedoBreakpoints();
};


S390Debugger::~S390Debugger() {
}



#ifdef GENERATED_CODE_COVERAGE
static FILE* coverage_log = NULL;


static void InitializeCoverage() {
  char* file_name = getenv("V8_GENERATED_CODE_COVERAGE_LOG");
  if (file_name != NULL) {
    coverage_log = fopen(file_name, "aw+");
  }
}


void S390Debugger::Stop(Instruction* instr) {  // roohack need to fix for PPC
  // Get the stop code.
  uint32_t code = instr->SvcValue() & kStopCodeMask;
  // Retrieve the encoded address, which comes just after this stop.
  char** msg_address =
    reinterpret_cast<char**>(sim_->get_pc() + Instruction::kInstrSize);
  char* msg = *msg_address;
  ASSERT(msg != NULL);

  // Update this stop description.
  if (isWatchedStop(code) && !watched_stops[code].desc) {
    watched_stops[code].desc = msg;
  }

  if (strlen(msg) > 0) {
    if (coverage_log != NULL) {
      fprintf(coverage_log, "%s\n", msg);
      fflush(coverage_log);
    }
    // Overwrite the instruction and address with nops.
    instr->SetInstructionBits(kNopInstr);
    reinterpret_cast<Instruction*>(msg_address)->SetInstructionBits(kNopInstr);
  }
  sim_->set_pc(sim_->get_pc() + Instruction::kInstrSize + kPointerSize);
}

#else  // ndef GENERATED_CODE_COVERAGE

static void InitializeCoverage() {
}


void S390Debugger::Stop(Instruction* instr) {
  // Get the stop code.
  // use of kStopCodeMask not right on PowerPC
  uint32_t code = instr->SvcValue() & kStopCodeMask;
  // Retrieve the encoded address, which comes just after this stop.
  char* msg = *reinterpret_cast<char**>(sim_->get_pc()
                                        + Instruction::kInstrSize);
  // Update this stop description.
  if (sim_->isWatchedStop(code) && !sim_->watched_stops[code].desc) {
    sim_->watched_stops[code].desc = msg;
  }
  // Print the stop message and code if it is not the default code.
  if (code != kMaxStopCode) {
    PrintF("Simulator hit stop %u: %s\n", code, msg);
  } else {
    PrintF("Simulator hit %s\n", msg);
  }
  sim_->set_pc(sim_->get_pc() + Instruction::kInstrSize + kPointerSize);
  Debug();
}
#endif


void S390Debugger::Info(Instruction* instr) {
  // Retrieve the encoded address immediately following the Info breakpoint.
  char* msg = *reinterpret_cast<char**>(sim_->get_pc()
                                        + Instruction::kInstrSize);
  PrintF("Simulator info %s\n", msg);
  sim_->set_pc(sim_->get_pc() + Instruction::kInstrSize + kPointerSize);
}


intptr_t S390Debugger::GetRegisterValue(int regnum) {
  return sim_->get_register(regnum);
}


double S390Debugger::GetRegisterPairDoubleValue(int regnum) {
  return sim_->get_double_from_register_pair(regnum);
}


double S390Debugger::GetFPDoubleRegisterValue(int regnum) {
  return sim_->get_double_from_d_register(regnum);
}

bool S390Debugger::GetValue(const char* desc, intptr_t* value) {
  int regnum = Registers::Number(desc);
  if (regnum != kNoRegister) {
    *value = GetRegisterValue(regnum);
    return true;
  } else {
    if (strncmp(desc, "0x", 2) == 0) {
      return SScanF(desc + 2, "%x", reinterpret_cast<uint32_t*>(value)) == 1;
    } else {
      return SScanF(desc, "%u", reinterpret_cast<uint32_t*>(value)) == 1;
    }
  }
  return false;
}

bool S390Debugger::GetFPDoubleValue(const char* desc, double* value) {
  int regnum = FPRegisters::Number(desc);
  if (regnum != kNoRegister) {
    *value = sim_->get_double_from_d_register(regnum);
    return true;
  }
  return false;
}


bool S390Debugger::SetBreakpoint(Instruction* break_pc) {
  // Check if a breakpoint can be set. If not return without any side-effects.
  if (sim_->break_pc_ != NULL) {
    return false;
  }

  // Set the breakpoint.
  sim_->break_pc_ = break_pc;
  sim_->break_instr_ = break_pc->InstructionBits();
  // Not setting the breakpoint instruction in the code itself. It will be set
  // when the debugger shell continues.
  return true;
}


bool S390Debugger::DeleteBreakpoint(Instruction* break_pc) {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }

  sim_->break_pc_ = NULL;
  sim_->break_instr_ = 0;
  return true;
}


void S390Debugger::UndoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }
}


void S390Debugger::RedoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(kBreakpointInstr);
  }
}


void S390Debugger::Debug() {
  intptr_t last_pc = -1;
  bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];
  char* argv[3] = { cmd, arg1, arg2 };

  // make sure to have a proper terminating character if reaching the limit
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  // Undo all set breakpoints while running in the debugger shell. This will
  // make them invisible to all commands.
  UndoBreakpoints();
  // Disable tracing while simulating
  bool trace=::v8::internal::FLAG_trace_sim;
  ::v8::internal::FLAG_trace_sim = false;

  while (!done && !sim_->has_bad_pc()) {
    if (last_pc != sim_->get_pc()) {
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      // use a reasonably large buffer
      v8::internal::EmbeddedVector<char, 256> buffer;
      dasm.InstructionDecode(buffer,
                             reinterpret_cast<byte*>(sim_->get_pc()));
      PrintF("  0x%08" V8PRIxPTR "  %s\n", sim_->get_pc(), buffer.start());
      last_pc = sim_->get_pc();
    }
    char* line = ReadLine("sim> ");
    if (line == NULL) {
      break;
    } else {
      char* last_input = sim_->last_debugger_input();
      if (strcmp(line, "\n") == 0 && last_input != NULL) {
        line = last_input;
      } else {
        // Ownership is transferred to sim_;
        sim_->set_last_debugger_input(line);
      }
      // Use sscanf to parse the individual parts of the command line. At the
      // moment no command expects more than two parameters.
      int argc = SScanF(line,
                        "%" XSTR(COMMAND_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s",
                        cmd, arg1, arg2);
      if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        intptr_t value;

        // If at a breakpoint, proceed past it.
        if ((reinterpret_cast<Instruction*>(sim_->get_pc()))->InstructionBits()
             == 0x7d821008) {
          sim_->set_pc(sim_->get_pc() + Instruction::kInstrSize);
        } else {
          sim_->InstructionDecode(
                  reinterpret_cast<Instruction*>(sim_->get_pc()));
        }

        if (argc == 2 && last_pc != sim_->get_pc() &&
                            GetValue(arg1, &value)) {
          for (int i = 1; (!sim_->has_bad_pc()) &&  i < value; i++) {
            disasm::NameConverter converter;
            disasm::Disassembler dasm(converter);
            // use a reasonably large buffer
            v8::internal::EmbeddedVector<char, 256> buffer;
            dasm.InstructionDecode(buffer,
                                   reinterpret_cast<byte*>(sim_->get_pc()));
            PrintF("  0x%08" V8PRIxPTR "  %s\n", sim_->get_pc(),
                   buffer.start());
            sim_->InstructionDecode(
                    reinterpret_cast<Instruction*>(sim_->get_pc()));
          }
        }
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // If at a breakpoint, proceed past it.
        if ((reinterpret_cast<Instruction*>(sim_->get_pc()))->InstructionBits()
             == 0x7d821008) {
          sim_->set_pc(sim_->get_pc() + Instruction::kInstrSize);
        } else {
          // Execute the one instruction we broke at with breakpoints disabled.
          sim_->InstructionDecode(
                  reinterpret_cast<Instruction*>(sim_->get_pc()));
        }
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (argc == 2 || (argc == 3 && strcmp(arg2, "fp") == 0)) {
          intptr_t value;
          double dvalue;
          if (strcmp(arg1, "all") == 0) {
            for (int i = 0; i < kNumRegisters; i++) {
              value = GetRegisterValue(i);
              PrintF("    %3s: %08" V8PRIxPTR, Registers::Name(i), value);
              if ((argc == 3 && strcmp(arg2, "fp") == 0) &&
                  i < 8 &&
                  (i % 2) == 0) {
                dvalue = GetRegisterPairDoubleValue(i);
                PrintF(" (%f)\n", dvalue);
              } else if (i != 0 && !((i+1) & 3)) {
                PrintF("\n");
              }
            }
            PrintF("  pc: %08" V8PRIxPTR "  cr: %08x\n",
                   sim_->special_reg_pc_, sim_->condition_reg_);
          } else if (strcmp(arg1, "alld") == 0) {
            for (int i = 0; i < kNumRegisters; i++) {
              value = GetRegisterValue(i);
              PrintF("     %3s: %08" V8PRIxPTR
                     " %11" V8PRIdPTR, Registers::Name(i), value, value);
              if ((argc == 3 && strcmp(arg2, "fp") == 0) &&
                  i < 8 &&
                  (i % 2) == 0) {
                dvalue = GetRegisterPairDoubleValue(i);
                PrintF(" (%f)\n", dvalue);
              } else if (!((i+1) % 2)) {
                PrintF("\n");
              }
            }
            PrintF("   pc: %08" V8PRIxPTR "  cr: %08x\n",
                   sim_->special_reg_pc_, sim_->condition_reg_);
          } else if (strcmp(arg1, "allf") == 0) {
            for (int i = 0; i < kNumFPDoubleRegisters; i++) {
              dvalue = GetFPDoubleRegisterValue(i);
              uint64_t as_words = BitCast<uint64_t>(dvalue);
              PrintF("%3s: %f 0x%08x %08x\n",
                     FPRegisters::Name(i),
                     dvalue,
                     static_cast<uint32_t>(as_words >> 32),
                     static_cast<uint32_t>(as_words & 0xffffffff));
            }
          } else if (arg1[0] == 'r' &&
                      (arg1[1] >= '0' && arg1[1] <= '2' &&
                        (arg1[2] == '\0' ||
                          (arg1[2] >= '0' && arg1[2] <= '5'
                            && arg1[3] == '\0')))) {
              int regnum = strtoul(&arg1[1], 0, 10);
              if (regnum != kNoRegister) {
                value = GetRegisterValue(regnum);
                PrintF("%s: 0x%08" V8PRIxPTR " %" V8PRIdPTR "\n",
                       arg1, value, value);
              } else {
                PrintF("%s unrecognized\n", arg1);
              }
          } else {
            if (GetValue(arg1, &value)) {
              PrintF("%s: 0x%08" V8PRIxPTR " %" V8PRIdPTR "\n",
                     arg1, value, value);
            } else if (GetFPDoubleValue(arg1, &dvalue)) {
              uint64_t as_words = BitCast<uint64_t>(dvalue);
              PrintF("%s: %f 0x%08x %08x\n",
                     arg1,
                     dvalue,
                     static_cast<uint32_t>(as_words >> 32),
                     static_cast<uint32_t>(as_words & 0xffffffff));
            } else {
              PrintF("%s unrecognized\n", arg1);
            }
          }
        } else {
          PrintF("print <register>\n");
        }
      } else if ((strcmp(cmd, "po") == 0)
                 || (strcmp(cmd, "printobject") == 0)) {
        if (argc == 2) {
          intptr_t value;
          if (GetValue(arg1, &value)) {
            Object* obj = reinterpret_cast<Object*>(value);
            PrintF("%s: \n", arg1);
#ifdef DEBUG
            obj->PrintLn();
#else
            obj->ShortPrint();
            PrintF("\n");
#endif
          } else {
            PrintF("%s unrecognized\n", arg1);
          }
        } else {
          PrintF("printobject <value>\n");
        }
      } else if (strcmp(cmd, "setpc") == 0) {
        intptr_t value;

        if (!GetValue(arg1, &value)) {
          PrintF("%s unrecognized\n", arg1);
          continue;
        }
        sim_->set_pc(value);
      } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
        intptr_t* cur = NULL;
        intptr_t* end = NULL;
        int next_arg = 1;

        if (strcmp(cmd, "stack") == 0) {
          cur = reinterpret_cast<intptr_t*>(sim_->get_register(Simulator::sp));
        } else {  // "mem"
          intptr_t value;
          if (!GetValue(arg1, &value)) {
            PrintF("%s unrecognized\n", arg1);
            continue;
          }
          cur = reinterpret_cast<intptr_t*>(value);
          next_arg++;
        }

        intptr_t words;  // likely inaccurate variable name for 64bit
        if (argc == next_arg) {
          words = 10;
        } else if (argc == next_arg + 1) {
          if (!GetValue(argv[next_arg], &words)) {
            words = 10;
          }
        }
        end = cur + words;

        while (cur < end) {
          PrintF("  0x%08" V8PRIxPTR ":  0x%08" V8PRIxPTR " %10" V8PRIdPTR,
                 reinterpret_cast<intptr_t>(cur), *cur, *cur);
          HeapObject* obj = reinterpret_cast<HeapObject*>(*cur);
          int value = *cur;
          Heap* current_heap = v8::internal::Isolate::Current()->heap();
          if (current_heap->Contains(obj) || ((value & 1) == 0)) {
            PrintF(" (");
            if ((value & 1) == 0) {
              PrintF("smi %d", value / 2);
            } else {
              obj->ShortPrint();
            }
            PrintF(")");
          }
          PrintF("\n");
          cur++;
        }
      } else if (strcmp(cmd, "disasm") == 0 || strcmp(cmd, "di") == 0) {
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // use a reasonably large buffer
        v8::internal::EmbeddedVector<char, 256> buffer;

        byte* prev = NULL;
        byte* cur = NULL;
        byte* end = NULL;

        if (argc == 1) {
          cur = reinterpret_cast<byte*>(sim_->get_pc());
          end = cur + (10 * Instruction::kInstrSize);
        } else if (argc == 2) {
          int regnum = Registers::Number(arg1);
          if (regnum != kNoRegister || strncmp(arg1, "0x", 2) == 0) {
            // The argument is an address or a register name.
            intptr_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(value);
              // Disassemble 10 instructions at <arg1>.
              end = cur + (10 * Instruction::kInstrSize);
            }
          } else {
            // The argument is the number of instructions.
            intptr_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(sim_->get_pc());
              // Disassemble <arg1> instructions.
              end = cur + (value * Instruction::kInstrSize);
            }
          }
        } else {
          intptr_t value1;
          intptr_t value2;
          if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
            cur = reinterpret_cast<byte*>(value1);
            end = cur + (value2 * Instruction::kInstrSize);
          }
        }

        while (cur < end) {
          prev = cur;
          cur += dasm.InstructionDecode(buffer, cur);
          PrintF("  0x%08" V8PRIxPTR "  %s\n",
                 reinterpret_cast<intptr_t>(prev), buffer.start());
        }
      } else if (strcmp(cmd, "gdb") == 0) {
        PrintF("relinquishing control to gdb\n");
        v8::internal::OS::DebugBreak();
        PrintF("regaining control from gdb\n");
      } else if (strcmp(cmd, "break") == 0) {
        if (argc == 2) {
          intptr_t value;
          if (GetValue(arg1, &value)) {
            if (!SetBreakpoint(reinterpret_cast<Instruction*>(value))) {
              PrintF("setting breakpoint failed\n");
            }
          } else {
            PrintF("%s unrecognized\n", arg1);
          }
        } else {
          PrintF("break <address>\n");
        }
      } else if (strcmp(cmd, "del") == 0) {
        if (!DeleteBreakpoint(NULL)) {
          PrintF("deleting breakpoint failed\n");
        }
      } else if (strcmp(cmd, "cr") == 0) {
        PrintF("Condition reg: %08x\n", sim_->condition_reg_);
      } else if (strcmp(cmd, "stop") == 0) {
        intptr_t value;
        intptr_t stop_pc = sim_->get_pc() -
                             (Instruction::kInstrSize + kPointerSize);
        Instruction* stop_instr = reinterpret_cast<Instruction*>(stop_pc);
        Instruction* msg_address =
          reinterpret_cast<Instruction*>(stop_pc + Instruction::kInstrSize);
        if ((argc == 2) && (strcmp(arg1, "unstop") == 0)) {
          // Remove the current stop.
          if (sim_->isStopInstruction(stop_instr)) {
            stop_instr->SetInstructionBits(kNopInstr);
            msg_address->SetInstructionBits(kNopInstr);
          } else {
            PrintF("Not at debugger stop.\n");
          }
        } else if (argc == 3) {
          // Print information about all/the specified breakpoint(s).
          if (strcmp(arg1, "info") == 0) {
            if (strcmp(arg2, "all") == 0) {
              PrintF("Stop information:\n");
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->PrintStopInfo(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->PrintStopInfo(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "enable") == 0) {
            // Enable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->EnableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->EnableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "disable") == 0) {
            // Disable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                sim_->DisableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->DisableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          }
        } else {
          PrintF("Wrong usage. Use help command for more information.\n");
        }
      } else if ((strcmp(cmd, "t") == 0) || strcmp(cmd, "trace") == 0) {
        ::v8::internal::FLAG_trace_sim = !::v8::internal::FLAG_trace_sim;
        PrintF("Trace of executed instructions is %s\n",
               ::v8::internal::FLAG_trace_sim ? "on" : "off");
      } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        PrintF("cont\n");
        PrintF("  continue execution (alias 'c')\n");
        PrintF("stepi [num instructions]\n");
        PrintF("  step one/num instruction(s) (alias 'si')\n");
        PrintF("print <register>\n");
        PrintF("  print register content (alias 'p')\n");
        PrintF("  use register name 'all' to display all integer registers\n");
        PrintF("  use register name 'alld' to display integer registers "\
               "with decimal values\n");
        PrintF("  use register name 'rN' to display register number 'N'\n");
        PrintF("  add argument 'fp' to print register pair double values\n");
        PrintF("  use register name 'allf' to display floating-point "\
               "registers\n");
        PrintF("printobject <register>\n");
        PrintF("  print an object from a register (alias 'po')\n");
        PrintF("cr\n");
        PrintF("  print condition register\n");
        PrintF("stack [<num words>]\n");
        PrintF("  dump stack content, default dump 10 words)\n");
        PrintF("mem <address> [<num words>]\n");
        PrintF("  dump memory content, default dump 10 words)\n");
        PrintF("disasm [<instructions>]\n");
        PrintF("disasm [<address/register>]\n");
        PrintF("disasm [[<address/register>] <instructions>]\n");
        PrintF("  disassemble code, default is 10 instructions\n");
        PrintF("  from pc (alias 'di')\n");
        PrintF("gdb\n");
        PrintF("  enter gdb\n");
        PrintF("break <address>\n");
        PrintF("  set a break point on the address\n");
        PrintF("del\n");
        PrintF("  delete the breakpoint\n");
        PrintF("trace (alias 't')\n");
        PrintF("  toogle the tracing of all executed statements\n");
        PrintF("stop feature:\n");
        PrintF("  Description:\n");
        PrintF("    Stops are debug instructions inserted by\n");
        PrintF("    the Assembler::stop() function.\n");
        PrintF("    When hitting a stop, the Simulator will\n");
        PrintF("    stop and and give control to the S390Debugger.\n");
        PrintF("    The first %d stop codes are watched:\n",
               Simulator::kNumOfWatchedStops);
        PrintF("    - They can be enabled / disabled: the Simulator\n");
        PrintF("      will / won't stop when hitting them.\n");
        PrintF("    - The Simulator keeps track of how many times they \n");
        PrintF("      are met. (See the info command.) Going over a\n");
        PrintF("      disabled stop still increases its counter. \n");
        PrintF("  Commands:\n");
        PrintF("    stop info all/<code> : print infos about number <code>\n");
        PrintF("      or all stop(s).\n");
        PrintF("    stop enable/disable all/<code> : enables / disables\n");
        PrintF("      all or number <code> stop(s)\n");
        PrintF("    stop unstop\n");
        PrintF("      ignore the stop instruction at the current location\n");
        PrintF("      from now on\n");
      } else {
        PrintF("Unknown command: %s\n", cmd);
      }
    }
  }

  // Add all the breakpoints back to stop execution and enter the debugger
  // shell when hit.
  RedoBreakpoints();
  // Restore tracing
  ::v8::internal::FLAG_trace_sim = trace;

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}


static bool ICacheMatch(void* one, void* two) {
  ASSERT((reinterpret_cast<intptr_t>(one) & CachePage::kPageMask) == 0);
  ASSERT((reinterpret_cast<intptr_t>(two) & CachePage::kPageMask) == 0);
  return one == two;
}


static uint32_t ICacheHash(void* key) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)) >> 2;
}


static bool AllOnOnePage(uintptr_t start, int size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}


void Simulator::set_last_debugger_input(char* input) {
  DeleteArray(last_debugger_input_);
  last_debugger_input_ = input;
}


void Simulator::FlushICache(v8::internal::HashMap* i_cache,
                            void* start_addr,
                            size_t size) {
  intptr_t start = reinterpret_cast<intptr_t>(start_addr);
  int intra_line = (start & CachePage::kLineMask);
  start -= intra_line;
  size += intra_line;
  size = ((size - 1) | CachePage::kLineMask) + 1;
  int offset = (start & CachePage::kPageMask);
  while (!AllOnOnePage(start, size - 1)) {
    int bytes_to_flush = CachePage::kPageSize - offset;
    FlushOnePage(i_cache, start, bytes_to_flush);
    start += bytes_to_flush;
    size -= bytes_to_flush;
    ASSERT_EQ(0, static_cast<int>(start & CachePage::kPageMask));
    offset = 0;
  }
  if (size != 0) {
    FlushOnePage(i_cache, start, size);
  }
}


CachePage* Simulator::GetCachePage(v8::internal::HashMap* i_cache, void* page) {
  v8::internal::HashMap::Entry* entry = i_cache->Lookup(page,
                                                        ICacheHash(page),
                                                        true);
  if (entry->value == NULL) {
    CachePage* new_page = new CachePage();
    entry->value = new_page;
  }
  return reinterpret_cast<CachePage*>(entry->value);
}


// Flush from start up to and not including start + size.
void Simulator::FlushOnePage(v8::internal::HashMap* i_cache,
                             intptr_t start,
                             int size) {
  ASSERT(size <= CachePage::kPageSize);
  ASSERT(AllOnOnePage(start, size - 1));
  ASSERT((start & CachePage::kLineMask) == 0);
  ASSERT((size & CachePage::kLineMask) == 0);
  void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
  int offset = (start & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* valid_bytemap = cache_page->ValidityByte(offset);
  memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}


void Simulator::CheckICache(v8::internal::HashMap* i_cache,
                            Instruction* instr) {
  intptr_t address = reinterpret_cast<intptr_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* cache_valid_byte = cache_page->ValidityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->CachedData(offset & ~CachePage::kLineMask);
  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    CHECK(memcmp(reinterpret_cast<void*>(instr),
                 cache_page->CachedData(offset),
                 Instruction::kInstrSize) == 0);
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}


void Simulator::Initialize(Isolate* isolate) {
  if (isolate->simulator_initialized()) return;
  isolate->set_simulator_initialized(true);
  ::v8::internal::ExternalReference::set_redirector(isolate,
                                                    &RedirectExternalReference);
}


Simulator::Simulator(Isolate* isolate) : isolate_(isolate) {
  i_cache_ = isolate_->simulator_i_cache();
  if (i_cache_ == NULL) {
    i_cache_ = new v8::internal::HashMap(&ICacheMatch);
    isolate_->set_simulator_i_cache(i_cache_);
  }
  Initialize(isolate);
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.
  size_t stack_size = 1 * 1024*1024;  // allocate 1MB for stack
  stack_ = reinterpret_cast<char*>(malloc(stack_size));
  pc_modified_ = false;
  icount_ = 0;
  break_pc_ = NULL;
  break_instr_ = 0;

  // make sure our register type can hold exactly 4/8 bytes
#ifdef V8_TARGET_ARCH_S390X
  ASSERT(sizeof(intptr_t) == 8);
#else
  ASSERT(sizeof(intptr_t) == 4);
#endif
  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < kNumGPRs; i++) {
    registers_[i] = 0;
  }
  condition_reg_ = 0;  // PowerPC
  fp_condition_reg_ = 0;  // PowerPC
  special_reg_pc_ = 0;  // PowerPC
  special_reg_lr_ = 0;  // PowerPC
  special_reg_ctr_ = 0;  // PowerPC

  // Initializing FP registers.
  for (int i = 0; i < kNumFPRs; i++) {
    fp_register[i] = 0.0;
  }

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<intptr_t>(stack_) + stack_size - 64;
  InitializeCoverage();

  last_debugger_input_ = NULL;
}


// When the generated code calls an external reference we need to catch that in
// the simulator.  The external reference will be a function compiled for the
// host architecture.  We need to call that function instead of trying to
// execute it with the simulator.  We do that by redirecting the external
// reference to a svc (Supervisor Call) instruction that is handled by
// the simulator.  We write the original destination of the jump just at a known
// offset from the svc instruction so the simulator knows what to call.
class Redirection {
 public:
  Redirection(void* external_function, ExternalReference::Type type)
      : external_function_(external_function),
      // we use TRAP4 here (0xBF22)
#if __BYTE_ORDER == __LITTLE_ENDIAN
      // quick and dirty way to hack the swi instrution
        swi_instruction_(0x1000FFB2),
#else
        swi_instruction_(0xB2FF0000 | kCallRtRedirected),
#endif
        type_(type),
        next_(NULL) {
    Isolate* isolate = Isolate::Current();
    next_ = isolate->simulator_redirection();
    Simulator::current(isolate)->
        FlushICache(isolate->simulator_i_cache(),
                    reinterpret_cast<void*>(&swi_instruction_),
                    Instruction::kInstrSize);
    isolate->set_simulator_redirection(this);
  }

  void* address_of_swi_instruction() {
    return reinterpret_cast<void*>(&swi_instruction_);
  }

  void* external_function() { return external_function_; }
  ExternalReference::Type type() { return type_; }

  static Redirection* Get(void* external_function,
                          ExternalReference::Type type) {
    Isolate* isolate = Isolate::Current();
    Redirection* current = isolate->simulator_redirection();
    for (; current != NULL; current = current->next_) {
      if (current->external_function_ == external_function) return current;
    }
    return new Redirection(external_function, type);
  }

  static Redirection* FromSwiInstruction(Instruction* swi_instruction) {
    char* addr_of_swi = reinterpret_cast<char*>(swi_instruction);
    char* addr_of_redirection =
        addr_of_swi - OFFSET_OF(Redirection, swi_instruction_);
    return reinterpret_cast<Redirection*>(addr_of_redirection);
  }

 private:
  void* external_function_;
  uint32_t swi_instruction_;
  ExternalReference::Type type_;
  Redirection* next_;
};


void* Simulator::RedirectExternalReference(void* external_function,
                                           ExternalReference::Type type) {
  Redirection* redirection = Redirection::Get(external_function, type);
  return redirection->address_of_swi_instruction();
}


// Get the active Simulator for the current thread.
Simulator* Simulator::current(Isolate* isolate) {
  v8::internal::Isolate::PerIsolateThreadData* isolate_data =
      isolate->FindOrAllocatePerThreadDataForThisThread();
  ASSERT(isolate_data != NULL);

  Simulator* sim = isolate_data->simulator();
  if (sim == NULL) {
    // TODO(146): delete the simulator object when a thread/isolate goes away.
    sim = new Simulator(isolate);
    isolate_data->set_simulator(sim);
  }
  return sim;
}


// Sets the register in the architecture state.
void Simulator::set_register(int reg, intptr_t value) {
  ASSERT((reg >= 0) && (reg < kNumGPRs));
  registers_[reg] = value;
}


// Get the register from the architecture state.
intptr_t Simulator::get_register(int reg) const {
  ASSERT((reg >= 0) && (reg < kNumGPRs));
  // Stupid code added to avoid bug in GCC.
  // See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43949
  if (reg >= kNumGPRs) return 0;
  // End stupid code.
  return registers_[reg];
}

template<typename T>
T Simulator::get_low_register(int reg) const {
  ASSERT((reg >= 0) && (reg < kNumGPRs));
  // Stupid code added to avoid bug in GCC.
  // See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43949
  if (reg >= kNumGPRs) return 0;
  // End stupid code.
#ifdef V8_TARGET_ARCH_S390X
  return static_cast<T>(registers_[reg] & 0xFFFFFFFF);
#else
  return static_cast<T>(registers_[reg]);
#endif
}

template<typename T>
T Simulator::get_high_register(int reg) const {
  ASSERT((reg >= 0) && (reg < kNumGPRs));
  // Stupid code added to avoid bug in GCC.
  // See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43949
  if (reg >= kNumGPRs) return 0;
  // End stupid code.
#ifdef V8_TARGET_ARCH_S390X
  ASSERT(sizeof(intptr_t) >= 8)
  return reinterpret_cast<T>(&registers_[reg]);
#else
  ASSERT(false);  // forbid 31bit to use high word
  return -1;
#endif
}

template<typename T>
void Simulator::set_low_register(int reg, T value) {
  ASSERT(sizeof(T) == 4);
#ifdef V8_TARGET_ARCH_S390X
  T* reg_addr = reinterpret_cast<T*>(&registers_[reg]) + 1;
#else
  T* reg_addr = reinterpret_cast<T*>(&registers_[reg]);
#endif
  *reg_addr = value;
}

template<typename T>
void Simulator::set_high_register(int reg, T value) {
  ASSERT(sizeof(T) == 4);
#ifdef V8_TARGET_ARCH_S390X
  T* reg_addr = reinterpret_cast<T*>(&registers_[reg]);
  *reg_addr = value;
#else
  ASSERT(false);  // forbid 31bit to use high word
#endif
}

double Simulator::get_double_from_register_pair(int reg) {
  ASSERT((reg >= 0) && (reg < kNumGPRs) && ((reg % 2) == 0));

  double dm_val = 0.0;
#ifndef V8_TARGET_ARCH_S390X  // doesn't make sense in 64bit mode
  // Read the bits from the unsigned integer register_[] array
  // into the double precision floating point value and return it.
  char buffer[sizeof(fp_register[0])];
  memcpy(buffer, &registers_[reg], sizeof(registers_[0]));
  memcpy(&dm_val, buffer, sizeof(registers_[0]));
#endif
  return(dm_val);
}

// Raw access to the PC register.
void Simulator::set_pc(intptr_t value) {
  pc_modified_ = true;
  special_reg_pc_ = value;
}


bool Simulator::has_bad_pc() const {
  return ((special_reg_pc_ == bad_lr) || (special_reg_pc_ == end_sim_pc));
}


// Raw access to the PC register without the special adjustment when reading.
intptr_t Simulator::get_pc() const {
  return special_reg_pc_;
}

// For use in calls that take two double values which are currently
// in d1 and d2
void Simulator::GetFpArgs(double* x, double* y) {
  *x = get_double_from_d_register(0);
  *y = get_double_from_d_register(2);
}

// For use in calls that take one double value (d1)
void Simulator::GetFpArgs(double* x) {
  *x = get_double_from_d_register(0);
}


// For use in calls that take one double value (d1) and one integer
// value (r3).
void Simulator::GetFpArgs(double* x, intptr_t* y) {
  *x = get_double_from_d_register(0);
  *y = registers_[2];
}


// The return value is in d1.
void Simulator::SetFpResult(const double& result) {
  set_d_register_from_double(0, result);
}


void Simulator::TrashCallerSaveRegisters() {
  // We don't trash the registers with the return value.
#if 0  // A good idea to trash volatile registers, needs to be done
  registers_[2] = 0x50Bad4U;
  registers_[3] = 0x50Bad4U;
  registers_[12] = 0x50Bad4U;
#endif
}


uint32_t Simulator::ReadWU(intptr_t addr, Instruction* instr) {
  uint32_t* ptr = reinterpret_cast<uint32_t*>(addr);
  return *ptr;
}

int32_t Simulator::ReadW(intptr_t addr, Instruction* instr) {
  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  return *ptr;
}


void Simulator::WriteW(intptr_t addr, uint32_t value, Instruction* instr) {
  uint32_t* ptr = reinterpret_cast<uint32_t*>(addr);
  *ptr = value;
  return;
}

void Simulator::WriteW(intptr_t addr, int32_t value, Instruction* instr) {
  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  *ptr = value;
  return;
}


uint16_t Simulator::ReadHU(intptr_t addr, Instruction* instr) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  // need to switch each byte
  uint8_t* highptr = reinterpret_cast<uint8_t*>(addr);
  uint8_t* lowptr = highptr+1;
  return ((*highptr) << 8) + *lowptr;
#else
  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
  return *ptr;
#endif
}


int16_t Simulator::ReadH(intptr_t addr, Instruction* instr) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  // @TODO: must check correctness here for little endian.
  UNIMPLEMENTED();
#endif
  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
  return *ptr;
}


void Simulator::WriteH(intptr_t addr, uint16_t value, Instruction* instr) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint8_t* newaddr = reinterpret_cast<byte*>(addr) + 2;
  uint16_t* ptr = reinterpret_cast<uint16_t*>(newaddr);
#else
  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
#endif
  *ptr = value;
  return;
}


void Simulator::WriteH(intptr_t addr, int16_t value, Instruction* instr) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint8_t* newaddr = reinterpret_cast<byte*>(addr) + 2;
  int16_t* ptr = reinterpret_cast<int16_t*>(newaddr);
#else
  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
#endif
  *ptr = value;
  return;
}


uint8_t Simulator::ReadBU(intptr_t addr) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  return *ptr;
}


int8_t Simulator::ReadB(intptr_t addr) {
  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  return *ptr;
}


void Simulator::WriteB(intptr_t addr, uint8_t value) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  *ptr = value;
}


void Simulator::WriteB(intptr_t addr, int8_t value) {
  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  *ptr = value;
}


intptr_t* Simulator::ReadDW(intptr_t addr) {
  intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
  return ptr;
}


void Simulator::WriteDW(intptr_t addr, int64_t value) {
  int64_t* ptr = reinterpret_cast<int64_t*>(addr);
  *ptr = value;
  return;
}


// Returns the limit of the stack area to enable checking for stack overflows.
uintptr_t Simulator::StackLimit() const {
  // Leave a safety margin of 1024 bytes to prevent overrunning the stack when
  // pushing values.
  return reinterpret_cast<uintptr_t>(stack_) + 1024;
}


// Unsupported instructions use Format to print an error and stop execution.
void Simulator::Format(Instruction* instr, const char* format) {
  PrintF("Simulator found unsupported instruction:\n 0x%08" V8PRIxPTR ": %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  UNIMPLEMENTED();
}


// Calculate C flag value for additions.
bool Simulator::CarryFrom(int32_t left, int32_t right, int32_t carry) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);
  uint32_t urest  = 0xffffffffU - uleft;

  return (uright > urest) ||
         (carry && (((uright + 1) > urest) || (uright > (urest - 1))));
}


// Calculate C flag value for subtractions.
bool Simulator::BorrowFrom(int32_t left, int32_t right) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);

  return (uright > uleft);
}


// Calculate V flag value for additions and subtractions.
bool Simulator::OverflowFrom(int32_t alu_out,
                             int32_t left, int32_t right, bool addition) {
  bool overflow;
  if (addition) {
               // operands have the same sign
    overflow = ((left >= 0 && right >= 0) || (left < 0 && right < 0))
               // and operands and result have different sign
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  } else {
               // operands have different signs
    overflow = ((left < 0 && right >= 0) || (left >= 0 && right < 0))
               // and first operand and result have different signs
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  }
  return overflow;
}


// Calls into the V8 runtime are based on this very simple interface.
// Note: To be able to return two values from some calls the code in runtime.cc
// uses the ObjectPair which is essentially two 32-bit values stuffed into a
// 64-bit value. With the code below we assume that all runtime calls return
// 64 bits of result. If they don't, the r4 result register contains a bogus
// value, which is fine because it is caller-saved.
typedef int64_t (*SimulatorRuntimeCall)(intptr_t arg0,
                                        intptr_t arg1,
                                        intptr_t arg2,
                                        intptr_t arg3,
                                        intptr_t arg4,
                                        intptr_t arg5);
typedef double (*SimulatorRuntimeFPCall)(double arg0,
                                         double arg1);
typedef int64_t (*SimulatorRuntimeFPCallX)(double arg0,
                                         double arg1);
typedef double (*SimulatorRuntimeFPCallY)(double arg0,
                                         intptr_t arg1);

// This signature supports direct call in to API function native callback
// (refer to InvocationCallback in v8.h).
typedef v8::Handle<v8::Value> (*SimulatorRuntimeDirectApiCall)(intptr_t arg0);

// This signature supports direct call to accessor getter callback.
typedef v8::Handle<v8::Value> (*SimulatorRuntimeDirectGetterCall)(
  intptr_t arg0,
  intptr_t arg1);

// Software interrupt instructions are used by the simulator to call into the
// C-based V8 runtime.
void Simulator::SoftwareInterrupt(Instruction* instr) {
  int svc = instr->SvcValue();
  switch (svc) {
    case kCallRtRedirected: {
      // Check if stack is aligned. Error if not aligned is reported below to
      // include information on the function called.
      bool stack_aligned =
          (get_register(sp)
           & (::v8::internal::FLAG_sim_stack_alignment - 1)) == 0;
      Redirection* redirection = Redirection::FromSwiInstruction(instr);
      intptr_t arg0 = get_register(r2);
      intptr_t arg1 = get_register(r3);
      intptr_t arg2 = get_register(r4);
      intptr_t arg3 = get_register(r5);
      intptr_t arg4 = get_register(r6);
      intptr_t* stack_pointer = reinterpret_cast<intptr_t*>(get_register(sp));
      intptr_t arg5 = stack_pointer[0];
      bool fp_call =
         (redirection->type() == ExternalReference::BUILTIN_FP_FP_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_COMPARE_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_FP_CALL) ||
         (redirection->type() == ExternalReference::BUILTIN_FP_INT_CALL);
      // This is dodgy but it works because the C entry stubs are never moved.
      // See comment in codegen-arm.cc and bug 1242173.
      intptr_t saved_lr = get_register(r14);
#ifndef V8_TARGET_ARCH_S390X
      // On zLinux-31, the saved_lr might be tagged with a high bit of 1.
      // Cleanse it before proceeding with simulation.
      saved_lr &= 0x7FFFFFFF;
#endif
      intptr_t external =
          reinterpret_cast<intptr_t>(redirection->external_function());
      if (fp_call) {
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          SimulatorRuntimeFPCall target =
              reinterpret_cast<SimulatorRuntimeFPCall>(external);
          double dval0, dval1;
          intptr_t ival;
          switch (redirection->type()) {
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_COMPARE_CALL:
            GetFpArgs(&dval0, &dval1);
            PrintF("Call to host function at %p with args %f, %f",
                FUNCTION_ADDR(target), dval0, dval1);
            break;
          case ExternalReference::BUILTIN_FP_CALL:
            GetFpArgs(&dval0);
            PrintF("Call to host function at %p with arg %f",
                FUNCTION_ADDR(target), dval0);
            break;
          case ExternalReference::BUILTIN_FP_INT_CALL:
            GetFpArgs(&dval0, &ival);
            PrintF("Call to host function at %p with args %f, %" V8PRIdPTR,
                FUNCTION_ADDR(target), dval0, ival);
            break;
          default:
            UNREACHABLE();
            break;
          }
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08" V8PRIxPTR
                   "\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
        SimulatorRuntimeFPCall target =
            reinterpret_cast<SimulatorRuntimeFPCall>(external);
        SimulatorRuntimeFPCallX targetx =
            reinterpret_cast<SimulatorRuntimeFPCallX>(external);
        SimulatorRuntimeFPCallY targety =
            reinterpret_cast<SimulatorRuntimeFPCallY>(external);
        double dval0, dval1, result;
        intptr_t ival;
        int32_t lo_res, hi_res;
        int64_t iresult;
        switch (redirection->type()) {
          case ExternalReference::BUILTIN_FP_FP_CALL:
            GetFpArgs(&dval0, &dval1);
            result = target(dval0, dval1);
            if (::v8::internal::FLAG_trace_sim) {
                PrintF("Returned %f\n", result);
            }
            SetFpResult(result);
            break;
          case ExternalReference::BUILTIN_COMPARE_CALL:
            GetFpArgs(&dval0, &dval1);
            iresult = targetx(dval0, dval1);
            lo_res = static_cast<int32_t>(iresult);
            hi_res = static_cast<int32_t>(iresult >> 32);
            if (::v8::internal::FLAG_trace_sim) {
                PrintF("Returned %08x\n", lo_res);
            }
#if __BYTE_ORDER == __BIG_ENDIAN
            set_register(r2, hi_res);
            set_register(r3, lo_res);
#else
            set_register(r2, lo_res);
            set_register(r3, hi_res);
#endif
            break;
          case ExternalReference::BUILTIN_FP_CALL:
              GetFpArgs(&dval0, &dval1);
            result = target(dval0, dval1);  // 2nd parm ignored
            if (::v8::internal::FLAG_trace_sim) {
                PrintF("Returned %f\n", result);
            }
            SetFpResult(result);
            break;
          case ExternalReference::BUILTIN_FP_INT_CALL:
            GetFpArgs(&dval0, &ival);
            result = targety(dval0, ival);
            if (::v8::internal::FLAG_trace_sim) {
                PrintF("Returned %f\n", result);
            }
            SetFpResult(result);
            break;
          default:
            UNREACHABLE();
            break;
        }
      } else if (redirection->type() == ExternalReference::DIRECT_API_CALL) {
        // See callers of MacroAssembler::CallApiFunctionAndReturn for
        // explanation of register usage.
        SimulatorRuntimeDirectApiCall target =
            reinterpret_cast<SimulatorRuntimeDirectApiCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF("Call to host function at %p args %08" V8PRIxPTR,
              FUNCTION_ADDR(target), arg0);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08" V8PRIxPTR
                   "\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
#if ABI_RETURNS_HANDLES_IN_REGS
        intptr_t p0 = arg0;
#else
        intptr_t p0 = arg1;
#endif
        v8::Handle<v8::Value> result = target(p0);
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %p\n", reinterpret_cast<void *>(*result));
        }
#if ABI_RETURNS_HANDLES_IN_REGS
        arg0 = (intptr_t)*result;
#else
        *(reinterpret_cast<intptr_t*>(arg0)) = (intptr_t) *result;
#endif
        set_register(r2, arg0);
      } else if (redirection->type() == ExternalReference::DIRECT_GETTER_CALL) {
        // See callers of MacroAssembler::CallApiFunctionAndReturn for
        // explanation of register usage.
        SimulatorRuntimeDirectGetterCall target =
            reinterpret_cast<SimulatorRuntimeDirectGetterCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF("Call to host function at %p args %08" V8PRIxPTR " %08"
                 V8PRIxPTR,
              FUNCTION_ADDR(target), arg0, arg1);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08" V8PRIxPTR
                   "\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
#if ABI_RETURNS_HANDLES_IN_REGS
        intptr_t p0 = arg0;
        intptr_t p1 = arg1;
#else
        intptr_t p0 = arg1;
        intptr_t p1 = arg2;
#endif
#if !ABI_PASSES_HANDLES_IN_REGS
        p0 = *(reinterpret_cast<intptr_t *>(p0));
#endif
        v8::Handle<v8::Value> result = target(p0, p1);
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %p\n", reinterpret_cast<void *>(*result));
        }
#if ABI_RETURNS_HANDLES_IN_REGS
        arg0 = (intptr_t)*result;
#else
        *(reinterpret_cast<intptr_t*>(arg0)) = (intptr_t) *result;
#endif
        set_register(r2, arg0);
      } else {
        // builtin call.
        ASSERT(redirection->type() == ExternalReference::BUILTIN_CALL);
        SimulatorRuntimeCall target =
            reinterpret_cast<SimulatorRuntimeCall>(external);
        if (::v8::internal::FLAG_trace_sim || !stack_aligned) {
          PrintF(
              "Call to host function at %p,\n"
              "\t\t\t\targs %08" V8PRIxPTR ", %08" V8PRIxPTR ", %08" V8PRIxPTR
              ", %08" V8PRIxPTR ", %08" V8PRIxPTR ", %08" V8PRIxPTR,
              FUNCTION_ADDR(target),
              arg0,
              arg1,
              arg2,
              arg3,
              arg4,
              arg5);
          if (!stack_aligned) {
            PrintF(" with unaligned stack %08" V8PRIxPTR
                   "\n", get_register(sp));
          }
          PrintF("\n");
        }
        CHECK(stack_aligned);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5);
#if V8_TARGET_ARCH_S390X
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %08" V8PRIxPTR "\n", result);
        }
        set_register(r2, result);
#else
        int32_t lo_res = static_cast<int32_t>(result);
        int32_t hi_res = static_cast<int32_t>(result >> 32);
        if (::v8::internal::FLAG_trace_sim) {
          PrintF("Returned %08x\n", lo_res);
        }
#if __BYTE_ORDER == __BIG_ENDIAN
        set_register(r2, hi_res);
        set_register(r3, lo_res);
#else
        set_register(r2, lo_res);
        set_register(r3, hi_res);
#endif
#endif
      }
      set_pc(saved_lr);
      break;
    }
    case kBreakpoint: {
      S390Debugger dbg(this);
      dbg.Debug();
      break;
    }
    case kInfo: {
       S390Debugger dbg(this);
       dbg.Info(instr);
       break;
     }
    // stop uses all codes greater than 1 << 23.
    default: {
      if (svc >= (1 << 23)) {
        uint32_t code = svc & kStopCodeMask;
        if (isWatchedStop(code)) {
          IncreaseStopCounter(code);
        }
        // Stop if it is enabled, otherwise go on jumping over the stop
        // and the message address.
        if (isEnabledStop(code)) {
          S390Debugger dbg(this);
          dbg.Stop(instr);
        } else {
          set_pc(get_pc() + Instruction::kInstrSize + kPointerSize);
        }
      } else {
        // This is not a valid svc code.
        UNREACHABLE();
        break;
      }
    }
  }
}


// Stop helper functions.
bool Simulator::isStopInstruction(Instruction* instr) {
  return (instr->Bits(27, 24) == 0xF) && (instr->SvcValue() >= kStopCode);
}


bool Simulator::isWatchedStop(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  return code < kNumOfWatchedStops;
}


bool Simulator::isEnabledStop(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  // Unwatched stops are always enabled.
  return !isWatchedStop(code) ||
    !(watched_stops[code].count & kStopDisabledBit);
}


void Simulator::EnableStop(uint32_t code) {
  ASSERT(isWatchedStop(code));
  if (!isEnabledStop(code)) {
    watched_stops[code].count &= ~kStopDisabledBit;
  }
}


void Simulator::DisableStop(uint32_t code) {
  ASSERT(isWatchedStop(code));
  if (isEnabledStop(code)) {
    watched_stops[code].count |= kStopDisabledBit;
  }
}


void Simulator::IncreaseStopCounter(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  ASSERT(isWatchedStop(code));
  if ((watched_stops[code].count & ~(1 << 31)) == 0x7fffffff) {
    PrintF("Stop counter for code %i has overflowed.\n"
           "Enabling this code and reseting the counter to 0.\n", code);
    watched_stops[code].count = 0;
    EnableStop(code);
  } else {
    watched_stops[code].count++;
  }
}


// Print a stop status.
void Simulator::PrintStopInfo(uint32_t code) {
  ASSERT(code <= kMaxStopCode);
  if (!isWatchedStop(code)) {
    PrintF("Stop not watched.");
  } else {
    const char* state = isEnabledStop(code) ? "Enabled" : "Disabled";
    int32_t count = watched_stops[code].count & ~kStopDisabledBit;
    // Don't print the state of unused breakpoints.
    if (count != 0) {
      if (watched_stops[code].desc) {
        PrintF("stop %i - 0x%x: \t%s, \tcounter = %i, \t%s\n",
               code, code, state, count, watched_stops[code].desc);
      } else {
        PrintF("stop %i - 0x%x: \t%s, \tcounter = %i\n",
               code, code, state, count);
      }
    }
  }
}


void Simulator::SetCR0(intptr_t result, bool setSO) {
  int bf = 0;
  if (result <  0) { bf |= 0x80000000; }
  if (result >  0) { bf |= 0x40000000; }
  if (result == 0) { bf |= 0x20000000; }
  if (setSO) { bf |= 0x10000000; }
  condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
}

void Simulator::DecodeBranchConditional(Instruction* instr) {
  int bo = instr->Bits(25, 21) << 21;
  int offset = (instr->Bits(15, 2) << 18) >> 16;
  int condition_bit = instr->Bits(20, 16);
  int condition_mask = 0x80000000 >> condition_bit;
  switch (bo) {
    case DCBNZF:  // Decrement CTR; branch if CTR != 0 and condition false
    case DCBEZF:  // Decrement CTR; branch if CTR == 0 and condition false
      UNIMPLEMENTED();
    case BF: {   // Branch if condition false
      if (!(condition_reg_ & condition_mask)) {
        if (instr->Bit(0) == 1) {  // LK flag set
          special_reg_lr_ = get_pc() + 4;
        }
        set_pc(get_pc() + offset);
      }
      break;
    }
    case DCBNZT:  // Decrement CTR; branch if CTR != 0 and condition true
    case DCBEZT:  // Decrement CTR; branch if CTR == 0 and condition true
      UNIMPLEMENTED();
    case BT: {   // Branch if condition true
      if (condition_reg_ & condition_mask) {
        if (instr->Bit(0) == 1) {  // LK flag set
          special_reg_lr_ = get_pc() + 4;
        }
        set_pc(get_pc() + offset);
      }
      break;
    }
    case DCBNZ:  // Decrement CTR; branch if CTR != 0
    case DCBEZ:  // Decrement CTR; branch if CTR == 0
      special_reg_ctr_ -= 1;
      if ((special_reg_ctr_ == 0) == (bo == DCBEZ)) {
        if (instr->Bit(0) == 1) {  // LK flag set
          special_reg_lr_ = get_pc() + 4;
        }
        set_pc(get_pc() + offset);
      }
      break;
    case BA: {   // Branch always
      if (instr->Bit(0) == 1) {  // LK flag set
        special_reg_lr_ = get_pc() + 4;
      }
      set_pc(get_pc() + offset);
      break;
    }
    default:
      UNIMPLEMENTED();  // Invalid encoding
  }
}

// Handle execution based on instruction types.
void Simulator::DecodeExt1(Instruction* instr) {
  switch (instr->Bits(10, 1) << 1) {
    case MCRF:
      UNIMPLEMENTED();  // Not used by V8.
    case BCLRX: {
        // need to check BO flag
        int old_pc = get_pc();
        set_pc(special_reg_lr_);
        if (instr->Bit(0) == 1) {  // LK flag set
          special_reg_lr_ = old_pc + 4;
        }
      break;
    }
    case BCCTRX: {
        // need to check BO flag
        int old_pc = get_pc();
        set_pc(special_reg_ctr_);
        if (instr->Bit(0) == 1) {  // LK flag set
          special_reg_lr_ = old_pc + 4;
        }
      break;
    }
    case CRNOR:
    case RFI:
    case CRANDC:
      UNIMPLEMENTED();
    case ISYNC: {
      // todo - simulate isync
      break;
    }
    case CRXOR: {
      int bt = instr->Bits(25, 21);
      int ba = instr->Bits(20, 16);
      int bb = instr->Bits(15, 11);
      int ba_val = ((0x80000000 >> ba) & condition_reg_) == 0 ? 0 : 1;
      int bb_val = ((0x80000000 >> bb) & condition_reg_) == 0 ? 0 : 1;
      int bt_val = ba_val ^ bb_val;
      bt_val = bt_val << (31-bt);  // shift bit to correct destination
      condition_reg_ &= ~(0x80000000 >> bt);
      condition_reg_ |= bt_val;
      break;
    }
    case CRNAND:
    case CRAND:
    case CREQV:
    case CRORC:
    case CROR:
    default: {
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}

bool Simulator::DecodeExt2_10bit(Instruction *instr) {
  bool found = true;

  int opcode = instr->Bits(10, 1) << 1;
  switch (opcode) {
    case SRWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uint32_t rs_val = get_register(rs);
      uintptr_t rb_val = get_register(rb);
      intptr_t  result = rs_val >> (rb_val & 0x3f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case SRDX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uintptr_t rs_val = get_register(rs);
      uintptr_t rb_val = get_register(rb);
      intptr_t  result = rs_val >> (rb_val & 0x7f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#endif
    case SRAW: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t result = rs_val >> (rb_val & 0x3f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case SRAD: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t result = rs_val >> (rb_val & 0x7f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#endif
    case SRAWIX: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int sh = instr->Bits(15, 11);
      int32_t rs_val = get_register(rs);
      intptr_t result = rs_val >> sh;
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case EXTSW: {
      const int shift = kBitsPerPointer - 32;
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      intptr_t rs_val = get_register(rs);
      intptr_t ra_val = (rs_val << shift) >> shift;
      set_register(ra, ra_val);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(ra_val);
      }
      break;
    }
#endif
    case EXTSH: {
      const int shift = kBitsPerPointer - 16;
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      intptr_t rs_val = get_register(rs);
      intptr_t ra_val = (rs_val << shift) >> shift;
      set_register(ra, ra_val);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(ra_val);
      }
      break;
    }
    case EXTSB: {
      const int shift = kBitsPerPointer - 8;
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      intptr_t rs_val = get_register(rs);
      intptr_t ra_val = (rs_val << shift) >> shift;
      set_register(ra, ra_val);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(ra_val);
      }
      break;
    }
    case LFSUX:
    case LFSX: {
      int frt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      int32_t val = ReadW(ra_val + rb_val, instr);
      float *fptr = reinterpret_cast<float*>(&val);
      set_d_register_from_double(frt, static_cast<double>(*fptr));
      if (opcode == LFSUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case LFDUX:
    case LFDX: {
      int frt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      double *dptr = reinterpret_cast<double*>(ReadDW(ra_val + rb_val));
      set_d_register_from_double(frt, *dptr);
      if (opcode == LFDUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case STFSUX: {
    case STFSX:
      int frs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      float frs_val = static_cast<float>(get_double_from_d_register(frs));
      int32_t *p=  reinterpret_cast<int32_t*>(&frs_val);
      WriteW(ra_val + rb_val, *p, instr);
      if (opcode == STFSUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case STFDUX: {
    case STFDX:
      int frs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      double frs_val = get_double_from_d_register(frs);
      int64_t *p = reinterpret_cast<int64_t *>(&frs_val);
      WriteDW(ra_val + rb_val, *p);
      if (opcode == STFDUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case SYNC: {
      // todo - simulate sync
      break;
    }
    case ICBI: {
      // todo - simulate icbi
      break;
    }
    default: {
      found = false;
      break;
    }
  }

  if (found)
    return found;

  found = true;
  opcode = instr->Bits(10, 2) << 2;
  switch (opcode) {
    case SRADIX: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      int sh = (instr->Bits(15, 11) | (instr->Bit(1) << 5));
      intptr_t rs_val = get_register(rs);
      intptr_t result = rs_val >> sh;
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
    default: {
      found = false;
      break;
    }
  }

  return found;
}

void Simulator::DecodeExt2_9bit(Instruction* instr) {
  int opcode = instr->Bits(9, 1) << 1;
  switch (opcode) {
    case TW: {
      // used for call redirection in simulation mode
      SoftwareInterrupt(instr);
      break;
    }
    case CMP: {
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = get_register(ra);
      intptr_t rb_val = get_register(rb);
      int cr = instr->Bits(25, 23);
      int bf = 0;
      if (ra_val < rb_val) { bf |= 0x80000000; }
      if (ra_val > rb_val) { bf |= 0x40000000; }
      if (ra_val == rb_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case SUBFCX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      uintptr_t ra_val = get_register(ra);
      uintptr_t rb_val = get_register(rb);
      uintptr_t alu_out = ~ra_val + rb_val + 1;
      set_register(rt, alu_out);
      // If the sign of rb and alu_out don't match, carry = 0
      if ((alu_out ^ rb_val) & 0x80000000) {
        special_reg_xer_ &= ~0xF0000000;
      } else {
        special_reg_xer_ = (special_reg_xer_ & ~0xF0000000) | 0x20000000;
      }
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
    case ADDCX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      uintptr_t ra_val = get_register(ra);
      uintptr_t rb_val = get_register(rb);
      uintptr_t alu_out = ra_val + rb_val;
      // Check overflow
      if (~ra_val < rb_val) {
        special_reg_xer_ = (special_reg_xer_ & ~0xF0000000) | 0x20000000;
      } else {
        special_reg_xer_ &= ~0xF0000000;
      }
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(static_cast<intptr_t>(alu_out));
      }
      // todo - handle OE bit
      break;
    }
    case MULHWX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t ra_val = (get_register(ra) & 0xFFFFFFFF);
      int32_t rb_val = (get_register(rb) & 0xFFFFFFFF);
      int64_t alu_out = (int64_t)ra_val * (int64_t)rb_val;
      alu_out >>= 32;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(static_cast<intptr_t>(alu_out));
      }
      // todo - handle OE bit
      break;
    }
    case NEGX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      intptr_t ra_val = get_register(ra);
      intptr_t alu_out = 1 + ~ra_val;
      set_register(rt, alu_out);
      if (instr->Bit(10)) {  // OE bit set
        if (ra_val == kMinInt) {
            special_reg_xer_ |= 0xC0000000;  // set SO,OV
        } else {
            special_reg_xer_ &= ~0x40000000;  // clear OV
        }
      }
      if (instr->Bit(0)) {  // RC bit set
        bool setSO = (special_reg_xer_ & 0x80000000);
        SetCR0(alu_out, setSO);
      }
      break;
    }
    case SLWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uint32_t rs_val = get_register(rs);
      uintptr_t rb_val = get_register(rb);
      uint32_t result = rs_val << (rb_val & 0x3f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case SLDX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uintptr_t rs_val = get_register(rs);
      uintptr_t rb_val = get_register(rb);
      uintptr_t result = rs_val << (rb_val & 0x7f);
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      break;
    }
#endif
    case CNTLZWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      uintptr_t rs_val = get_register(rs);
      uintptr_t count  = 0;
      int      n      = 0;
      uintptr_t bit    = 0x80000000;
      for (; n < 32; n++) {
          if (bit & rs_val)
              break;
          count++;
          bit >>= 1;
      }
      set_register(ra, count);
      if (instr->Bit(0)) {  // RC Bit set
        int bf = 0;
        if (count > 0)  { bf |= 0x40000000; }
        if (count == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case CNTLZDX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      uintptr_t rs_val = get_register(rs);
      uintptr_t count  = 0;
      int      n      = 0;
      uintptr_t bit    = 0x8000000000000000UL;
      for (; n < 64; n++) {
          if (bit & rs_val)
              break;
          count++;
          bit >>= 1;
      }
      set_register(ra, count);
      if (instr->Bit(0)) {  // RC Bit set
        int bf = 0;
        if (count > 0)  { bf |= 0x40000000; }
        if (count == 0) { bf |= 0x20000000; }
        condition_reg_ = (condition_reg_ & ~0xF0000000) | bf;
      }
      break;
    }
#endif
    case ANDX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = rs_val & rb_val;
      set_register(ra, alu_out);
      if (instr->Bit(0)) {  // RC Bit set
        SetCR0(alu_out);
      }
      break;
    }
    case ANDCX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = rs_val & ~rb_val;
      set_register(ra, alu_out);
      if (instr->Bit(0)) {  // RC Bit set
        SetCR0(alu_out);
      }
      break;
    }
    case CMPL: {
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      uintptr_t ra_val = get_register(ra);
      uintptr_t rb_val = get_register(rb);
      int cr = instr->Bits(25, 23);
      int bf = 0;
      if (ra_val < rb_val) { bf |= 0x80000000; }
      if (ra_val > rb_val) { bf |= 0x40000000; }
      if (ra_val == rb_val) { bf |= 0x20000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      break;
    }
    case SUBFX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      intptr_t ra_val = get_register(ra);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = rb_val - ra_val;
      // todo - figure out underflow
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC Bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
    case ADDZEX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      intptr_t ra_val = get_register(ra);
      if (special_reg_xer_ & 0x20000000) {
        ra_val += 1;
      }
      set_register(rt, ra_val);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(ra_val);
      }
      // todo - handle OE bit
      break;
    }
    case NORX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = ~(rs_val | rb_val);
      set_register(ra, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      break;
    }
    case MULLW: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t ra_val = (get_register(ra) & 0xFFFFFFFF);
      int32_t rb_val = (get_register(rb) & 0xFFFFFFFF);
      int32_t alu_out = ra_val * rb_val;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
#if V8_TARGET_ARCH_S390X
    case MULLD: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int64_t ra_val = get_register(ra);
      int64_t rb_val = get_register(rb);
      int64_t alu_out = ra_val * rb_val;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
#endif
    case DIVW: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int32_t ra_val = get_register(ra);
      int32_t rb_val = get_register(rb);
      // result is undefined if divisor is zero.
      int32_t alu_out = rb_val ? ra_val / rb_val : -1;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
#if V8_TARGET_ARCH_S390X
    case DIVD: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      int64_t ra_val = get_register(ra);
      int64_t rb_val = get_register(rb);
      // result is undefined if divisor is zero.
      int64_t alu_out = rb_val ? ra_val / rb_val : -1;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
#endif
    case ADDX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      // int oe = instr->Bit(10);
      intptr_t ra_val = get_register(ra);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = ra_val + rb_val;
      set_register(rt, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      // todo - handle OE bit
      break;
    }
    case XORX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = rs_val ^ rb_val;
      set_register(ra, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      break;
    }
    case ORX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      intptr_t alu_out = rs_val | rb_val;
      set_register(ra, alu_out);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(alu_out);
      }
      break;
    }
    case MFSPR: {
      int rt = instr->RTValue();
      int spr = instr->Bits(20, 11);
      if (spr != 256) {
        UNIMPLEMENTED();  // Only LRLR supported
      }
      set_register(rt, special_reg_lr_);
      break;
    }
    case MTSPR: {
      int rt = instr->RTValue();
      intptr_t rt_val = get_register(rt);
      int spr = instr->Bits(20, 11);
      if (spr == 256) {
        special_reg_lr_ = rt_val;
      } else if (spr == 288) {
        special_reg_ctr_ = rt_val;
      } else if (spr == 32) {
        special_reg_xer_ = rt_val;
      } else {
        UNIMPLEMENTED();  // Only LR supported
      }
      break;
    }
    case MFCR: {
      int rt = instr->RTValue();
      set_register(rt, condition_reg_);
      break;
    }
    case STWUX:
    case STWX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      int32_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      WriteW(ra_val+rb_val, rs_val, instr);
      if (opcode == STWUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case STBUX:
    case STBX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      int8_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      WriteB(ra_val+rb_val, rs_val);
      if (opcode == STBUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case STHUX:
    case STHX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      int16_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      WriteH(ra_val+rb_val, rs_val, instr);
      if (opcode == STHUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case LWZX:
    case LWZUX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      set_register(rt, ReadWU(ra_val+rb_val, instr));
      if (opcode == LWZUX) {
        ASSERT(ra != 0 && ra != rt);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
#if V8_TARGET_ARCH_S390X
    case LDX:
    case LDUX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      intptr_t *result = ReadDW(ra_val+rb_val);
      set_register(rt, *result);
      if (opcode == LDUX) {
        ASSERT(ra != 0 && ra != rt);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case STDX:
    case STDUX: {
      int rs = instr->RSValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rs_val = get_register(rs);
      intptr_t rb_val = get_register(rb);
      WriteDW(ra_val+rb_val, rs_val);
      if (opcode == STDUX) {
        ASSERT(ra != 0);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
#endif
    case LBZX:
    case LBZUX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      set_register(rt, ReadBU(ra_val+rb_val) & 0xFF);
      if (opcode == LBZUX) {
        ASSERT(ra != 0 && ra != rt);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case LHZX:
    case LHZUX: {
      int rt = instr->RTValue();
      int ra = instr->RAValue();
      int rb = instr->RBValue();
      intptr_t ra_val = ra == 0 ? 0 : get_register(ra);
      intptr_t rb_val = get_register(rb);
      set_register(rt, ReadHU(ra_val+rb_val, instr) & 0xFFFF);
      if (opcode == LHZUX) {
        ASSERT(ra != 0 && ra != rt);
        set_register(ra, ra_val+rb_val);
      }
      break;
    }
    case DCBF: {
      // todo - simulate dcbf
      break;
    }
    default: {
      PrintF("Unimplemented: %08x\n", instr->InstructionBits());
      UNIMPLEMENTED();  // Not used by V8.
    }
  }
}

void Simulator::DecodeExt2(Instruction* instr) {
    // Check first the 10-1 bit versions
    if (DecodeExt2_10bit(instr))
        return;
    // Now look at the lesser encodings
    DecodeExt2_9bit(instr);
}

void Simulator::DecodeExt4(Instruction* instr) {
  switch (instr->Bits(5, 1) << 1) {
    case FDIV: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val / frb_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FSUB: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val - frb_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FADD: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frt_val = fra_val + frb_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FSQRT: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      double frt_val = sqrt(frb_val);
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FSEL: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      int frc = instr->RCValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      double frc_val = get_double_from_d_register(frc);
      double frt_val = ((fra_val >= 0.0) ? frc_val : frb_val);
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FMUL: {
      int frt = instr->RTValue();
      int fra = instr->RAValue();
      int frc = instr->RCValue();
      double fra_val = get_double_from_d_register(fra);
      double frc_val = get_double_from_d_register(frc);
      double frt_val = fra_val * frc_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
  }
  int opcode = instr->Bits(10, 1) << 1;
  switch (opcode) {
    case FCMPU: {
      int fra = instr->RAValue();
      int frb = instr->RBValue();
      double fra_val = get_double_from_d_register(fra);
      double frb_val = get_double_from_d_register(frb);
      int cr = instr->Bits(25, 23);
      int bf = 0;
      if (fra_val < frb_val) { bf |= 0x80000000; }
      if (fra_val > frb_val) { bf |= 0x40000000; }
      if (fra_val == frb_val) { bf |= 0x20000000; }
      if (isunordered(fra_val, frb_val)) { bf |= 0x10000000; }
      int condition_mask = 0xF0000000 >> (cr*4);
      int condition =  bf >> (cr*4);
      condition_reg_ = (condition_reg_ & ~condition_mask) | condition;
      return;
    }
    case FRSP: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      // frsp round 8-byte double-precision value to 8-byte
      // single-precision value, ignore the round here
      set_d_register_from_double(frt, frb_val);
      if (instr->Bit(0)) {  // RC bit set
        //  UNIMPLEMENTED();
      }
      return;
    }
    case FCFID: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double t_val = get_double_from_d_register(frb);
      int64_t* frb_val_p = reinterpret_cast<int64_t*>(&t_val);
      double frt_val = static_cast<double>(*frb_val_p);
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FCTID: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      int64_t frt_val;
      int64_t one = 1;  // work-around gcc
      int64_t kMinLongLong = (one << 63);
      int64_t kMaxLongLong = kMinLongLong - 1;

      if (frb_val > kMaxLongLong) {
        frt_val = kMaxLongLong;
      } else if (frb_val < kMinLongLong) {
        frt_val = kMinLongLong;
      } else {
        switch (fp_condition_reg_ & kVFPRoundingModeMask) {
          case kRoundToZero:
            frt_val = (int64_t)frb_val;
            break;
          case kRoundToPlusInf:
            frt_val = (int64_t)ceil(frb_val);
            break;
          case kRoundToMinusInf:
            frt_val = (int64_t)floor(frb_val);
            break;
          default:
            frt_val = (int64_t)frb_val;
            UNIMPLEMENTED();  // Not used by V8.
            break;
        }
      }
      double *p = reinterpret_cast<double*>(&frt_val);
      set_d_register_from_double(frt, *p);
      return;
    }
    case FCTIDZ: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      int64_t frt_val;
      int64_t one = 1;  // work-around gcc
      int64_t kMinLongLong = (one << 63);
      int64_t kMaxLongLong = kMinLongLong - 1;

      if (frb_val > kMaxLongLong) {
        frt_val = kMaxLongLong;
      } else if (frb_val < kMinLongLong) {
        frt_val = kMinLongLong;
      } else {
        frt_val = (int64_t)frb_val;
      }
      double *p = reinterpret_cast<double*>(&frt_val);
      set_d_register_from_double(frt, *p);
      return;
    }
    case FCTIW:
    case FCTIWZ: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      int64_t frt_val;
      if (frb_val > kMaxInt) {
        frt_val = kMaxInt;
      } else if (frb_val < kMinInt) {
        frt_val = kMinInt;
      } else {
        if (opcode == FCTIWZ) {
          frt_val = (int64_t)frb_val;
        } else {
          switch (fp_condition_reg_ & kVFPRoundingModeMask) {
          case kRoundToZero:
            frt_val = (int64_t)frb_val;
            break;
          case kRoundToPlusInf:
            frt_val = (int64_t)ceil(frb_val);
            break;
          case kRoundToMinusInf:
            frt_val = (int64_t)floor(frb_val);
            break;
          case kRoundToNearest:
            frt_val = (int64_t)lround(frb_val);

            // Round to even if exactly halfway.  (lround rounds up)
            if (fabs(static_cast<double>(frt_val) - frb_val) == 0.5 &&
                (frt_val % 2)) {
                frt_val += ((frt_val > 0) ? -1 : 1);
            }

            break;
          default:
            ASSERT(false);
            frt_val = (int64_t)frb_val;
            break;
          }
        }
      }
      double *p = reinterpret_cast<double*>(&frt_val);
      set_d_register_from_double(frt, *p);
      return;
    }
    case FNEG: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      double frt_val = -frb_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FMR: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      double frt_val = frb_val;
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case MTFSFI: {
      int bf = instr->Bits(25, 23);
      int imm = instr->Bits(15, 12);
      int fp_condition_mask = 0xF0000000 >> (bf*4);
      fp_condition_reg_ &= ~fp_condition_mask;
      fp_condition_reg_ |= (imm << (28 - (bf*4)));
      if (instr->Bit(0)) {  // RC bit set
        condition_reg_ &= 0xF0FFFFFF;
        condition_reg_ |= (imm << 23);
      }
      return;
    }
    case MTFSF: {
      int frb = instr->RBValue();
      double frb_dval = get_double_from_d_register(frb);
      int64_t *p =  reinterpret_cast<int64_t*>(&frb_dval);
      int32_t frb_ival = static_cast<int32_t>((*p) & 0xffffffff);
      int l = instr->Bits(25, 25);
      if (l == 1) {
        fp_condition_reg_ = frb_ival;
      } else {
        UNIMPLEMENTED();
      }
      if (instr->Bit(0)) {  // RC bit set
        UNIMPLEMENTED();
        // int w = instr->Bits(16, 16);
        // int flm = instr->Bits(24, 17);
      }
      return;
    }
    case MFFS: {
      int frt = instr->RTValue();
      int64_t lval = static_cast<int64_t>(fp_condition_reg_);
      double* p = reinterpret_cast<double*>(&lval);
      set_d_register_from_double(frt, *p);
      return;
    }
    case FABS: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      double frt_val = abs(frb_val);
      set_d_register_from_double(frt, frt_val);
      return;
    }
    case FRIM: {
      int frt = instr->RTValue();
      int frb = instr->RBValue();
      double frb_val = get_double_from_d_register(frb);
      int64_t floor_val = (int64_t)frb_val;
      if (floor_val > frb_val)
        floor_val--;
      double frt_val = static_cast<double>(floor_val);
      set_d_register_from_double(frt, frt_val);
      return;
    }
  }
  UNIMPLEMENTED();  // Not used by V8.
}

#if V8_TARGET_ARCH_S390X
void Simulator::DecodeExt5(Instruction* instr) {
  switch (instr->Bits(4, 2) << 2) {
    case RLDICL: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      uintptr_t rs_val = get_register(rs);
      int sh = (instr->Bits(15, 11) | (instr->Bit(1) << 5));
      int mb = (instr->Bits(10, 6) | (instr->Bit(5) << 5));
      ASSERT(sh >=0 && sh <= 63);
      ASSERT(mb >=0 && mb <= 63);
      // rotate left
      uintptr_t result = (rs_val << sh) | (rs_val >> (64-sh));
      uintptr_t mask = 0xffffffffffffffff >> mb;
      result &= mask;
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      return;
    }
    case RLDICR: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      uintptr_t rs_val = get_register(rs);
      int sh = (instr->Bits(15, 11) | (instr->Bit(1) << 5));
      int me = (instr->Bits(10, 6) | (instr->Bit(5) << 5));
      ASSERT(sh >=0 && sh <= 63);
      ASSERT(me >=0 && me <= 63);
      // rotate left
      uintptr_t result = (rs_val << sh) | (rs_val >> (64-sh));
      uintptr_t mask = 0xffffffffffffffff << (63-me);
      result &= mask;
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      return;
    }
    case RLDIC: {
      int ra = instr->RAValue();
      int rs = instr->RSValue();
      uintptr_t rs_val = get_register(rs);
      int sh = (instr->Bits(15, 11) | (instr->Bit(1) << 5));
      int mb = (instr->Bits(10, 6) | (instr->Bit(5) << 5));
      ASSERT(sh >=0 && sh <= 63);
      ASSERT(mb >=0 && mb <= 63);
      // rotate left
      uintptr_t result = (rs_val << sh) | (rs_val >> (64-sh));
      uintptr_t mask = (0xffffffffffffffff >> mb) & (0xffffffffffffffff << sh);
      result &= mask;
      set_register(ra, result);
      if (instr->Bit(0)) {  // RC bit set
        SetCR0(result);
      }
      return;
    }
  }
  UNIMPLEMENTED();  // Not used by V8.
}
#endif

// Method for checking overflow on signed addition:
//   Test src1 and src2 have opposite sign,
//   (1) No overflow if they have opposite sign
//   (2) Test the result and one of the operands have opposite sign
//      (a) No overflow if they don't have opposite sign
//      (b) Overflow if opposite
#define CheckOverflowForIntAdd(src1, src2) \
  (((src1) ^ (src2)) < 0 ? \
    false : ((((src1) + (src2)) ^ (src1)) < 0))

// Method for checking overflow on signed subtraction:
#define CheckOverflowForIntSub(src1, src2) \
  CheckOverflowForIntAdd((src1), -(src2))

// Method for checking overflow on unsigned addtion
#define CheckOverflowForUIntAdd(src1, src2) \
  ((src1) + (src2) < (src1) || (src1) + (src2) < (src2))

// Method for checking overflow on unsigned subtraction
#define CheckOverflowForUIntSub(src1, src2) \
  ((src1) - (src2) > (src1))

// Method for checking overflow on multiplication
#define CheckOverflowForMul(src1, src2) \
  (((src1) * (src2)) / (src2) != (src1))

// Method for checking overflow on shift right
#define CheckOverflowForShiftRight(src1, src2) \
  (((src1) >> (src2)) << (src2) != (src1))

// Method for checking overflow on shift left
#define CheckOverflowForShiftLeft(src1, src2) \
  (((src1) << (src2)) >> (src2) != (src1))

// S390 Decode and simulate helpers
bool Simulator::DecodeTwoByte(Instruction* instr) {
  Opcode op = instr->S390OpcodeValue();

  switch (op) {
    // RR format instructions
    case SVC: {
      UNIMPLEMENTED();
      return true;
    }
    case AR:
    case SR:
    case MR:
    case DR:
    case OR:
    case NR:
    case XR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      int32_t r2_val = get_low_register<int32_t>(r2);
      bool isOF = false;
      switch (op) {
        case AR:
          isOF = CheckOverflowForIntAdd(r1_val, r2_val);
          r1_val += r2_val;
          SetS390ConditionCode<int32_t>(r1_val, 0);
          SetS390OverflowCode(isOF);
          break;
        case SR:
          isOF = CheckOverflowForIntSub(r1_val, r2_val);
          r1_val -= r2_val;
          SetS390ConditionCode<int32_t>(r1_val, 0);
          SetS390OverflowCode(isOF);
          break;
        case OR:
          r1_val |= r2_val;
          SetS390BitWiseConditionCode<uint32_t>(r1_val);
          break;
        case NR:
          r1_val &= r2_val;
          SetS390BitWiseConditionCode<uint32_t>(r1_val);
          break;
        case XR:
          r1_val ^= r2_val;
          SetS390BitWiseConditionCode<uint32_t>(r1_val);
          break;
        case MR:
        case DR:
          UNIMPLEMENTED();
          break;  // reg pair
        default: UNREACHABLE(); break;
      }
      set_low_register<int32_t>(r1, r1_val);
      break;
    }
    case LR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      set_low_register<int32_t>(r1,
                                get_low_register<int32_t>(r2));
      break;
    }
    case LDR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      double r2_val = get_double_from_d_register(r2);
      set_d_register_from_double(r1, r2_val);
      break;
    }
    case CR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      int32_t r2_val = get_low_register<int32_t>(r2);
      SetS390ConditionCode<int32_t>(r1_val, r2_val);
      break;
    }
    case CLR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      uint32_t r1_val = get_low_register<uint32_t>(r1);
      uint32_t r2_val = get_low_register<uint32_t>(r2);
      SetS390ConditionCode<uint32_t>(r1_val, r2_val);
      break;
    }
    case BCR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      if (TestConditionCode(Condition(r1))) {
        intptr_t r2_val = get_register(r2);
#ifndef V8_TARGET_ARCH_S390X
        // On 31-bit, the top most bit may be 0 or 1, but is ignored by the
        // hardware.  Cleanse the top bit before jumping to it, unless it's one
        // of the special PCs
        if (r2_val != bad_lr && r2_val != end_sim_pc)
          r2_val &= 0x7FFFFFFF;
#endif
        set_pc(r2_val);
      }
      break;
    }
    case LTR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      int32_t r2_val = get_low_register<int32_t>(r2);
      SetS390ConditionCode<int32_t>(r2_val, 0);
      set_low_register<int32_t>(r1, r2_val);
      break;
    }
    case ALR:
    case SLR: {
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      uint32_t r1_val = get_low_register<uint32_t>(r1);
      uint32_t r2_val = get_low_register<uint32_t>(r2);
      uint32_t alu_out;
      bool isOF = false;
      if (op == ALR) {
        alu_out = r1_val + r2_val;
        isOF = CheckOverflowForUIntAdd(r1_val, r2_val);
      } else if (op == SLR) {
        alu_out = r1_val - r2_val;
        isOF = CheckOverflowForUIntSub(r1_val, r2_val);
      }
      set_low_register<uint32_t>(r1, alu_out);
      SetS390ConditionCode<uint32_t>(alu_out, 0);
      SetS390OverflowCode(isOF);
      break;
    }
    case LBR: { UNIMPLEMENTED(); break; }
    case LNR: {
      // Load Negative (32)
      RRInstruction* rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      int32_t r2_val = get_low_register<int32_t>(r2);
      r2_val = r2_val < 0 ? -r2_val : r2_val;
      set_low_register<int32_t>(r1, -r2_val);
      break;
    }
    case BASR: {
      RRInstruction * rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      intptr_t r2_val = get_register(r2);
      intptr_t link_addr = get_pc() + 2;
#ifndef V8_TARGET_ARCH_S390X
      // On 31-bit, the top most bit may be 0 or 1, which can cause issues
      // for stackwalker.  The top bit should either be cleanse before being
      // pushed onto the stack, or during stack walking when dereferenced.
      // For simulator, we'll take the worst case scenario and always tag
      // the high bit, to flush out more problems.
      link_addr |= 0x80000000;
#endif
      set_register(r1, link_addr);
      set_pc(r2_val);
      break;
    }
    case LCR: {
      RRInstruction * rrinst = reinterpret_cast<RRInstruction*>(instr);
      int r1 = rrinst->R1Value();
      int r2 = rrinst->R2Value();
      int32_t r2_val = get_low_register<int32_t>(r2);
      r2_val = ~r2_val;
      r2_val = r2_val+1;
      set_low_register<int32_t>(r1, r2_val);
      SetS390ConditionCode<int32_t>(r2_val, 0);
      break;
    }
    default:
      UNREACHABLE();
      return false;
      break;
  }
  return true;
}

// Decode routine for four-byte instructions
bool Simulator::DecodeFourByte(Instruction* instr) {
  Opcode op = instr->S390OpcodeValue();

  switch (op) {
    case AHI:  // @TODO (AlanLi): set condition code for overflow
    case MHI: {
      RIInstruction* riinst = reinterpret_cast<RIInstruction*>(instr);
      int r1 = riinst->R1Value();
      int i  = riinst->I2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      bool isOF = false;
      switch (op) {
        case AHI:
          isOF = CheckOverflowForIntAdd(r1_val, i);
          r1_val += i;
          break;
        case MHI:
          isOF = CheckOverflowForMul(r1_val, i);
          r1_val *= i;
          break;  // no overflow indication is given
        default: break;
      }
      set_low_register<int32_t>(r1, r1_val);
      SetS390ConditionCode<int32_t>(r1_val, 0);
      SetS390OverflowCode(isOF);
      break;
    }
    case LHI: {
      RIInstruction* riinst = reinterpret_cast<RIInstruction*>(instr);
      int r1 = riinst->R1Value();
      int i  = riinst->I2Value();
      set_low_register<int32_t>(r1, i);
      break;
    }
    case CHI: {
      RIInstruction* riinst = reinterpret_cast<RIInstruction*>(instr);
      int r1 = riinst->R1Value();
      int16_t i = riinst->I2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      SetS390ConditionCode<int32_t>(r1_val, i);
      break;
    }
    case BRAS: {
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      intptr_t d2 = rilInstr->I2Value();
      intptr_t pc = get_pc();
      set_register(r1, pc + 4);  // save next instruction to register
      set_pc(pc + d2 * 2);    // update register
      break;
    }
    case BRC: {
      RIInstruction* riinst = reinterpret_cast<RIInstruction*>(instr);
      int m1 = riinst->M1Value();
      if (TestConditionCode((Condition)m1)) {
        intptr_t offset = riinst->I2Value() * 2;
        set_pc(get_pc() + offset);
      }
      break;
    }
    case BRCT:
    case BRCTG: {
       // Branch On Count (32/64).
       RIInstruction* riinst = reinterpret_cast<RIInstruction*>(instr);
       int r1 = riinst->R1Value();
       int64_t value = (op == BRCT)?get_low_register<int32_t>(r1):
                                    get_register(r1);
       if (BRCT == op)
         set_low_register<int32_t>(r1, value--);
       else
         set_register(r1, value--);
       // Branch if value != 0
       if (value != 0) {
        intptr_t offset = riinst->I2Value() * 2;
        set_pc(get_pc() + offset);
      }
      break;
    }
    case IIHH: case IIHL: case IILH: case IILL: {
      UNIMPLEMENTED();
      break;
    }
    case STM:
    case LM: {
      // Store Multiple 32-bits.
      RSInstruction* rsinstr = reinterpret_cast<RSInstruction*>(instr);
      int r1 = rsinstr->R1Value();
      int r3 = rsinstr->R3Value();
      int rb = rsinstr->B2Value();
      int offset = rsinstr->D2Value();

      // Regs roll around if r3 is less than r1.
      // Artifically increase r3 by 16 so we can calculate
      // the number of regs stored properly.
      if (r3 < r1)
        r3 += 16;

      int32_t rb_val = (rb == 0) ? 0 : get_low_register<int32_t>(rb);

      // Store each register in ascending order.
      for (int i = 0; i <= r3 - r1; i++) {
        if (op == STM) {
          int32_t value = get_low_register<int32_t>((r1 + i) % 16);
          WriteW(rb_val + offset + 4 * i, value, instr);
        } else if (op == LM) {
          int32_t value = ReadW(rb_val + offset + 4 * i, instr);
          set_low_register<int32_t>((r1 + i) % 16, value);
        }
      }
      break;
    }
    case SLL:
    case SRL: {
      RSInstruction* rsInstr = reinterpret_cast<RSInstruction*>(instr);
      int r1 = rsInstr->R1Value();
      int b2 = rsInstr->B2Value();
      intptr_t d2 = rsInstr->D2Value();
      // only takes rightmost 6bits
      intptr_t b2_val = b2 == 0 ? 0 : get_register(b2);
      int shiftBits = (b2_val + d2) & 0x3F;
      uint32_t r1_val = get_low_register<uint32_t>(r1);
      uint32_t alu_out;
      if (op == SLL) {
        alu_out = r1_val << shiftBits;
      } else if (op == SRL) {
        alu_out = r1_val >> shiftBits;
      }
      set_low_register<uint32_t>(r1, alu_out);
      SetS390ConditionCode<uint32_t>(alu_out, 0);
      break;
    }
    case SLA:
    case SRA: {
      RSInstruction* rsInstr = reinterpret_cast<RSInstruction*>(instr);
      int r1 = rsInstr->R1Value();
      int b2 = rsInstr->B2Value();
      intptr_t d2 = rsInstr->D2Value();
      // only takes rightmost 6bits
      intptr_t b2_val = b2 == 0 ? 0 : get_register(b2);
      int shiftBits = (b2_val + d2) & 0x3F;
      int32_t r1_val = get_low_register<int32_t>(r1);
      int32_t alu_out = 0;
      bool isOF = false;
      if (op == SLA) {
        isOF = CheckOverflowForShiftLeft(r1_val, shiftBits);
        alu_out = r1_val << shiftBits;
      } else if (op == SRA) {
        isOF = CheckOverflowForShiftRight(r1_val, shiftBits);
        alu_out = r1_val >> shiftBits;
      }
      set_low_register<int32_t>(r1, alu_out);
      SetS390ConditionCode<int32_t>(alu_out, 0);
      SetS390OverflowCode(isOF);
      break;
    }
    case MLR: { UNIMPLEMENTED(); break; }
    case LLHR: { UNIMPLEMENTED(); break; }
    case LLGHR: { UNIMPLEMENTED(); break; }
    case A: case S: case M: case D:
    case O: case N: case X: {
      // 32-bit Reg-Mem instructions
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int32_t  r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      int32_t mem_val = ReadW(b2_val + x2_val + d2_val, instr);
      int32_t alu_out = 0;
      bool isOF = false;
      switch (op) {
        case A:
          isOF = CheckOverflowForIntAdd(r1_val, mem_val);
          alu_out = r1_val + mem_val;
          SetS390ConditionCode<int32_t>(alu_out, 0);
          SetS390OverflowCode(isOF);
          break;
        case S:
          isOF = CheckOverflowForIntSub(r1_val, mem_val);
          alu_out = r1_val - mem_val;
          SetS390ConditionCode<int32_t>(alu_out, 0);
          SetS390OverflowCode(isOF);
          break;
        case M:
        case D:
          UNIMPLEMENTED();
          break;
        case O:
          alu_out = r1_val | mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        case N:
          alu_out = r1_val & mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        case X:
          alu_out = r1_val ^ mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        default:
          UNREACHABLE();
          break;
      }
      set_low_register<int32_t>(r1, alu_out);
      break;
    }
    case NILL:
    case NILH: {
      RIInstruction* riInst = reinterpret_cast<RIInstruction*>(instr);
      int r1 = riInst->R1Value();
      int i  = riInst->I2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      if (NILL == op) {
        i |= 0xFFFF0000;
      } else if (NILH == op) {
        i = (i << 16) | 0x0000FFFF;
      } else {
        UNIMPLEMENTED();
      }
      set_low_register<int32_t>(r1, r1_val & i);
      SetS390BitWiseConditionCode<uint32_t>(r1_val & i);
      break;
    }
    case L:
    case LA:
    case LB:
    case LD: {
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int32_t  r1 = rxinst->R1Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t addr = b2_val + x2_val + d2_val;
      if (op == L) {
        int32_t mem_val = ReadW(addr, instr);
        set_low_register<int32_t>(r1, mem_val);
      } else if (op == LA) {
        set_register(r1, addr);
      } else if (op == LB) {
        int32_t mem_val = ReadB(addr);
        set_low_register<int32_t>(r1, mem_val);
      } else if (op == LD) {
        double *dptr = reinterpret_cast<double*>(ReadDW(addr));
        set_d_register_from_double(r1, *dptr);
      }
      break;
    }
    case C: {
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int32_t  r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t addr = b2_val + x2_val + d2_val;
      int32_t mem_val = ReadW(addr, instr);
      SetS390ConditionCode<int32_t>(r1_val, mem_val);
      break;
    }
    case ST:
    case STD: {
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int32_t  r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t addr = b2_val + x2_val + d2_val;
      if (op == ST) {
        WriteW(addr, r1_val, instr);
      } else if (op == STD) {
        double frs_val = get_double_from_d_register(r1);
        int64_t *p = reinterpret_cast<int64_t *>(&frs_val);
        WriteDW(addr, *p);
      }
      break;
    }
    case AH:
    case SH:
    case MH: {
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int32_t r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t addr = b2_val + x2_val + d2_val;
      int16_t mem_val = ReadH(addr, instr);
      int32_t alu_out;
      bool isOF = false;
      if (op == AH) {
        isOF = CheckOverflowForIntAdd(r1_val, mem_val);
        alu_out = r1_val + mem_val;
      } else if (op == SH) {
        isOF = CheckOverflowForIntSub(r1_val, mem_val);
        alu_out = r1_val - mem_val;
      } else if (op == MH) {
        alu_out = r1_val * mem_val;
      }
      set_low_register<int32_t>(r1, alu_out);
      if (op != MH) {  // MH does not change op code
        SetS390ConditionCode<int32_t>(alu_out, 0);
        SetS390OverflowCode(isOF);
      }
      break;
    }
    case AHY:
    case SHY: {
      RXYInstruction* rxyinst = reinterpret_cast<RXYInstruction*>(instr);
      int32_t r1_val = get_low_register<int32_t>(rxyinst->R1Value());
      int b2 = rxyinst->B2Value();
      int x2 = rxyinst->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxyinst->D2Value();
      int16_t mem_val = ReadH(b2_val + d2_val + x2_val, instr);
      int32_t alu_out = 0;
      bool isOF = false;
      switch (op) {
        case AHY:
          alu_out = r1_val + mem_val;
          isOF = CheckOverflowForIntAdd(r1_val, mem_val);
          break;
        case SHY:
          alu_out = r1_val - mem_val;
          isOF = CheckOverflowForIntSub(r1_val, mem_val);
          break;
        default:
          UNREACHABLE();
          break;
      }
      set_low_register<int32_t>(r1, alu_out);
      SetS390ConditionCode<int32_t>(alu_out, 0);
      SetS390OverflowCode(isOF);
      break;
    }
    case LGFR: {
      RREInstruction* rreInstr = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreInstr->R1Value();
      int r2 = rreInstr->R2Value();
      int32_t r2_val = get_low_register<int32_t>(r2);
      set_register(r1, r2_val);
      break;
    }
    case LNGR: {
      RREInstruction* rreinst = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreinst->R1Value();
      int r2 = rreinst->R2Value();
      int64_t r2_val = get_register(r2);
      r2_val = r2_val < 0 ? -r2_val : r2_val;
      set_register(r1, -r2_val);
      if (get_register(r2) == 0) {
        condition_reg_ = CC_EQ;
      } else {  // less than zero
        condition_reg_ = CC_LT;
      }
      break;
    }
    case TRAP4: {
      SoftwareInterrupt(instr);
      break;
    }
    case STC: {
      // Store Character/Byte
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      uint8_t  r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t mem_addr = b2_val + x2_val + d2_val;
      WriteB(mem_addr, r1_val);
      break;
    }
    case STH: {
      RXInstruction* rxinst = reinterpret_cast<RXInstruction*>(instr);
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      int16_t  r1_val = get_low_register<int32_t>(rxinst->R1Value());
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxinst->D2Value();
      intptr_t mem_addr = b2_val + x2_val + d2_val;
      WriteH(mem_addr, r1_val, instr);
      break;
    }
    case LCGR: {
      RREInstruction * rreinst = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreinst->R1Value();
      int r2 = rreinst->R2Value();
      int64_t r2_val = get_register(r2);
      r2_val = ~r2_val;
      r2_val = r2_val+1;
      set_register(r1, r2_val);
      SetS390ConditionCode<int64_t>(r2_val, 0);
      // if the input is INT_MIN, loading its compliment would be overflowing
      if (r2_val == -2147483647 - 1) {  // bypass gcc complain
        SetS390OverflowCode(true);
      }
      break;
    }
    case MSR:
    case MSGR: {  // they do not set overflow code
      RREInstruction * rreinst = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreinst->R1Value();
      int r2 = rreinst->R2Value();
      if (op == MSR) {
        int32_t r1_val = get_low_register<int32_t>(r1);
        int32_t r2_val = get_low_register<int32_t>(r2);
        set_low_register<int32_t>(r1, r1_val * r2_val);
      } else if (op == MSGR) {
        intptr_t r1_val = get_register(r1);
        intptr_t r2_val = get_register(r2);
        set_register(r1, r1_val * r2_val);
      } else {
        UNREACHABLE();
      }
    }
    case MS: {
      RXInstruction * rxinst = reinterpret_cast<RXInstruction*>(instr);
      int r1 = rxinst->R1Value();
      int b2 = rxinst->B2Value();
      int x2 = rxinst->X2Value();
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t d2_val = rxinst->D2Value();
      int32_t mem_val = ReadW(b2_val + x2_val + d2_val, instr);
      int32_t r1_val = get_low_register<int32_t>(r1);
      set_low_register<int32_t>(r1, r1_val * mem_val);
    }
    default: {
      return DecodeFourByteFloatingPoint(instr);
    }
  }
  return true;
}

bool Simulator::DecodeFourByteFloatingPoint(Instruction* instr) {
  Opcode op = instr->S390OpcodeValue();

  switch (op) {
    case ADBR:
    case SDBR:
    case MDBR:
    case DDBR:
    case CDBR:
    case CDFBR:
    case CDGBR:
    case CFDBR:
    case CGDBR:
    case SQDBR:
    case CFEBR:
    case CEFBR:
    case LPDBR: {
      RREInstruction* rreInstr = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreInstr->R1Value();
      int r2 = rreInstr->R2Value();
      double r1_val = get_double_from_d_register(r1);
      double r2_val = get_double_from_d_register(r2);
        if (op == ADBR) {
          r1_val += r2_val;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
        } else if (op == SDBR) {
          r1_val -= r2_val;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
        } else if (op == MDBR) {
          r1_val *= r2_val;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
        } else if (op == DDBR) {
          r1_val /= r2_val;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
        } else if (op == CDBR) {
          SetS390ConditionCode<double>(r1_val, r2_val);
        } else if (op == CDGBR) {
          intptr_t r2_val = get_register(r2);
          double r1_val = static_cast<double>(r2_val);
          set_d_register_from_double(r1, r1_val);
        } else if (op == CDFBR) {
          int32_t r2_val = get_low_register<int32_t>(r2);
          double r1_val = static_cast<double>(r2_val);
          set_d_register_from_double(r1, r1_val);
        } else if (op == CFDBR) {
          int32_t r1_val = static_cast<int32_t>(r2_val);
          set_low_register<int32_t>(r1, r1_val);
        } else if (op == CGDBR) {
          int64_t r1_val = static_cast<int64_t>(r2_val);
          set_register(r1, r1_val);
        } else if (op == SQDBR) {
          r1_val = sqrt(r2_val);
          set_d_register_from_double(r1, r1_val);
        } else if (op == CFEBR) {
          UNIMPLEMENTED();
        } else if (op == CEFBR) {
          UNIMPLEMENTED();
        } else if (op == LPDBR) {
          r1_val = fabs(r2_val);
          set_d_register_from_double(r1, r1_val);
          if (r2_val != r2_val) {  // input is NaN
            condition_reg_ = CC_OF;
          } else if (r2_val == 0) {
            condition_reg_ = CC_EQ;
          } else {
            condition_reg_ = CC_GT;
          }
        } else {
          UNREACHABLE();
        }
        break;
    }
    case CDLFBR:
    case CDLGBR:
    case CLFDBR:
    case CLGDBR: {
      // TODO(AlanLi): create different behavior for different masks.
      // need to instantiate RRFInstruciton actually.
      RREInstruction *rreInstr = reinterpret_cast<RREInstruction*>(instr);
      int r1 = rreInstr->R1Value();
      int r2 = rreInstr->R2Value();
      if (op == CDLFBR) {
        uint32_t r2_val = get_low_register<uint32_t>(r2);
        double r1_val = static_cast<double>(r2_val);
        set_d_register_from_double(r1, r1_val);
      } else if (op == CDLGBR) {
        uint64_t r2_val = get_low_register<uint64_t>(r2);
        double r1_val = static_cast<double>(r2_val);
        set_d_register_from_double(r1, r1_val);
      } else if (op == CLFDBR) {
        double r2_val = get_double_from_d_register(r2);
        uint32_t r1_val = static_cast<uint32_t>(r1);
        set_low_register<uint32_t>(r1, r1_val);
        SetS390ConditionCode<double>(r2_val, 0);
      } else if (op == CLGDBR) {
        double r2_val = get_double_from_d_register(r2);
        uint64_t r1_val = static_cast<uint64_t>(r1);
        set_register(r1, r1_val);
        SetS390ConditionCode<double>(r2_val, 0);
      }
      break;
    }
    default: {
      UNREACHABLE();
      return false;
    }
  }
  return true;
}

// Decode routine for six-byte instructions
bool Simulator::DecodeSixByte(Instruction* instr) {
  Opcode op = instr->S390OpcodeValue();

  switch (op) {
    case LAY: {
      // Load Address
      RXYInstruction *rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int rb = rxyInstr->B2Value();
      int rx = rxyInstr->X2Value();
      int offset = rxyInstr->D2Value();
      intptr_t rb_val = (rb == 0) ? 0 : get_register(rb);
      intptr_t rx_val = (rx == 0) ? 0 : get_register(rx);
      set_register(r1, rx_val + rb_val + offset);
      break;
    }
    case LARL: {
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      intptr_t offset = rilInstr->I2Value() * 2;
      set_register(r1, get_pc() + offset);
      break;
    }
    case LLILF: {  // length is architecture dependent
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint64_t imm = static_cast<uint64_t>(rilInstr->I2UnsignedValue());
      set_register(r1, imm);
      break;
    }
    case OILF:
    case NILF:
    case IILF: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t imm = rilInstr->I2UnsignedValue();
      uint32_t alu_out = get_low_register<uint32_t>(r1);
      if (NILF == op) {
        alu_out &= imm;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (OILF == op) {
        alu_out |= imm;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == IILF) {
        alu_out = imm;
      } else { ASSERT(false); }
      set_low_register<uint32_t>(r1, alu_out);
      break;
    }
    case OIHF:
    case NIHF:
    case IIHF: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t imm = rilInstr->I2Value();
      uint32_t alu_out = get_high_register<uint32_t>(r1);
      if (op == NIHF) {
        alu_out &= imm;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == OIHF) {
        alu_out |= imm;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == IIHF) {
        alu_out = imm;
      } else { ASSERT(false); }
      set_high_register<uint32_t>(r1, alu_out);
      break;
    }
    case CLFI: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t imm = rilInstr->I2UnsignedValue();
      SetS390ConditionCode<uint32_t>(get_low_register<uint32_t>(r1), imm);
      break;
    }
    case CFI: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      int32_t imm = rilInstr->I2Value();
      SetS390ConditionCode<int32_t>(get_low_register<int32_t>(r1), imm);
      break;
    }
    case BRASL: {  // save the next instruction address
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      intptr_t d2 = rilInstr->I2Value();
      intptr_t pc = get_pc();
      set_register(r1, pc + 6);  // save next instruction to register
      set_pc(pc + d2 *2);        // update register
      break;
    }
    case BRCL: {
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      Condition m1 = (Condition)rilInstr->R1Value();
      if (TestConditionCode((Condition)m1)) {
        intptr_t offset = rilInstr->I2Value() * 2;
        set_pc(get_pc() + offset);
      }
      break;
    }
    case LMG:
    case STMG: {
      // Store Multiple 64-bits.
      RSYInstruction* rsyinstr = reinterpret_cast<RSYInstruction*>(instr);
      int r1 = rsyinstr->R1Value();
      int r3 = rsyinstr->R3Value();
      int rb = rsyinstr->B2Value();
      int offset = rsyinstr->D2Value();

      // Regs roll around if r3 is less than r1.
      // Artifically increase r3 by 16 so we can calculate
      // the number of regs stored properly.
      if (r3 < r1)
        r3 += 16;

      intptr_t rb_val = (rb == 0) ? 0 : get_register(rb);

      // Store each register in ascending order.
      for (int i = 0; i <= r3 - r1; i++) {
        if (op == LMG) {
          int64_t value =
            *reinterpret_cast<int64_t*>(ReadDW(rb_val + offset + 4 * i));
          set_register((r1 + i) % 16, value);
        } else if (op == STMG) {
          int64_t value = get_register((r1 + i) % 16);
          WriteDW(rb_val + offset + 4 * i, value);
        } else { ASSERT(false); }
      }
      break;
      break;
    }
    case SLLG:
    case SRLG:
    case SLAG:
    case SRAG: {
      UNIMPLEMENTED();
      break;
    }
    case LMY:
    case STMY: {
      RSYInstruction* rsyInstr = reinterpret_cast<RSYInstruction*>(instr);
      int r1 = rsyInstr->R1Value();
      int r3 = rsyInstr->R3Value();
      int b2 = rsyInstr->B2Value();
      int offset = rsyInstr->D2Value();

      // Regs roll around if r3 is less than r1.
      // Artifically increase r3 by 16 so we can calculate
      // the number of regs stored properly.
      if (r3 < r1)
        r3 += 16;

      int32_t b2_val = (b2 == 0) ? 0 : get_low_register<int32_t>(b2);

      // Store each register in ascending order.
      for (int i = 0; i < r3 - r1; i++) {
        if (op == LMY) {
          int32_t value = ReadW(b2_val + offset + 4*i, instr);
          set_low_register<int32_t>((r1 + i) % 16, value);
        } else {
          int32_t value = get_low_register<int32_t>((r1 + i) % 16);
          WriteW(b2_val + offset + 4*i, value, instr);
        }
      }
      break;
    }
    case LT:
    case LTG: {
      RXYInstruction* rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int x2 = rxyInstr->X2Value();
      int b2 = rxyInstr->B2Value();
      int d2 = rxyInstr->D2Value();

      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t addr = x2_val + b2_val + d2;

      if (op == LT) {
        int32_t value = ReadW(addr, instr);
        set_low_register<int32_t>(r1, value);
        SetS390ConditionCode<int32_t>(value, 0);
      } else if (op == LTG) {
        int64_t value = *reinterpret_cast<int64_t*>(ReadDW(addr));
        set_register(r1, value);
        SetS390ConditionCode<int64_t>(value, 0);
      }
      break;
    }
    case LY:
    case STY:
    case STCY:
    case STHY:
    case LDY:
    case STDY: {
      RXYInstruction* rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int x2 = rxyInstr->X2Value();
      int b2 = rxyInstr->B2Value();
      int d2 = rxyInstr->D2Value();
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t addr = x2_val + b2_val + d2;
      if (op == LY) {
        uint32_t mem_val = ReadWU(addr, instr);
        set_low_register<uint32_t>(r1, mem_val);
      } else if (op == LDY) {
        double *dptr = reinterpret_cast<double*>(ReadDW(addr));
        set_d_register_from_double(r1, *dptr);
      } else if (op == STY) {
        uint32_t value = get_low_register<uint32_t>(r1);
        WriteW(addr, value, instr);
      } else if (op == STDY) {
        double frs_val = get_double_from_d_register(r1);
        int64_t *p = reinterpret_cast<int64_t *>(&frs_val);
        WriteDW(addr, *p);
      } else if (op == STCY) {
        uint8_t value = get_low_register<uint32_t>(r1);
        WriteB(addr, value);
      } else if (op == STHY) {
        uint16_t value = get_low_register<uint32_t>(r1);
        WriteH(addr, value, instr);
      }
      break;
    }
    case MVC: {
      SSInstruction* ssInstr = reinterpret_cast<SSInstruction*>(instr);
      int b1 = ssInstr->B1Value();
      intptr_t d1 = ssInstr->D1Value();
      int b2 = ssInstr->B2Value();
      intptr_t d2 = ssInstr->D2Value();
      int length = ssInstr->Length();
      intptr_t b1_val = (b1 == 0) ? 0 : get_register(b1);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t src_addr = b2_val + d2;
      intptr_t dst_addr = b1_val + d1;
      // remember that the length is the actual length - 1
      for (int i = 0; i < length + 1; ++i) {
        WriteB(dst_addr++, ReadB(src_addr++));
      }
      break;
    }
    case LLH:
    case LLGH: {
      RXYInstruction* rxyinst = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyinst->R1Value();
      int b2 = rxyinst->B2Value();
      int x2 = rxyinst->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxyinst->D2Value();
      uint16_t mem_val = ReadHU(b2_val + d2_val + x2_val, instr);
      if (op == LLH) {
        set_low_register<uint32_t>(r1, mem_val);
      } else if (op == LLGH) {
        set_register(r1, mem_val);
      } else {
        UNREACHABLE();
      }
      break;
    }
    case LLC:
    case LLGC: {
      // Load Logical Character - loads a byte and zeor extends.
      RXYInstruction* rxyinst = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyinst->R1Value();
      int b2 = rxyinst->B2Value();
      int x2 = rxyinst->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxyinst->D2Value();
      uint16_t mem_val = ReadBU(b2_val + d2_val + x2_val);
      if (op == LLC) {
        set_low_register<uint32_t>(r1, static_cast<uint32_t>(mem_val));
      } else if (op == LLGC) {
        set_register(r1, static_cast<uint64_t>(mem_val));
      } else {
        UNREACHABLE();
      }
      break;
    }
    case CDB:
    case ADB:
    case SDB:
    case MDB:
    case DDB:
    case SQDB: {
      RXEInstruction* rxeInstr = reinterpret_cast<RXEInstruction*>(instr);
      int b2 = rxeInstr->B2Value();
      int x2 = rxeInstr->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxeInstr->D2Value();
      double r1_val = get_double_from_d_register(rxeInstr->R1Value());
      double *dptr = reinterpret_cast<double*>(ReadDW(b2_val+x2_val+d2_val));
      switch (op) {
        case CDB:
          SetS390ConditionCode<double>(r1_val, *dptr);
          break;
        case ADB:
          r1_val += *dptr;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
          break;
        case SDB:
          r1_val -= *dptr;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
          break;
        case MDB:
          r1_val *= *dptr;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
          break;
        case DDB:
          r1_val /= *dptr;
          set_d_register_from_double(r1, r1_val);
          SetS390ConditionCode<double>(r1_val, 0);
          break;
       case SQDB:
          r1_val = sqrt(*dptr);
          set_d_register_from_double(r1, r1_val);
       default:
          UNREACHABLE();
          break;
      }
      break;
    }
    case XIHF:
    case XILF: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t imm = rilInstr->I2UnsignedValue();
      uint32_t alu_out = 0;
      if (op == XILF) {
        alu_out = get_low_register<uint32_t>(r1);
        alu_out = alu_out ^ imm;
        set_low_register<uint32_t>(r1, alu_out);
      } else if (op == XIHF) {
        alu_out = get_high_register<uint32_t>(r1);
        alu_out = alu_out ^ imm;
        set_high_register<uint32_t>(r1, alu_out);
      } else {
        UNREACHABLE();
      }
      SetS390BitWiseConditionCode<uint32_t>(alu_out);
      break;
    }
    default:
      return DecodeSixByteArithInstruction(instr);
  }
  return true;
}

bool Simulator::DecodeSixByteArithInstruction(Instruction *instr) {
  Opcode op = instr->S390OpcodeValue();

  switch (op) {
    case ALFI:
    case SLFI: {
      RILInstruction *rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t imm = rilInstr->I2UnsignedValue();
      uint32_t alu_out = get_low_register<uint32_t>(r1);
      if (op == ALFI) {
        alu_out += imm;
      } else if (op == SLFI) {
        alu_out -= imm;
      }
      SetS390ConditionCode<uint32_t>(alu_out, 0);
      set_low_register<uint32_t>(r1, alu_out);
      break;
    }
    case ML: { UNIMPLEMENTED(); break; }
    case AY:
    case SY:
    case NY:
    case OY:
    case XY:
    case CY: {  // @TODO(Alanli): set overflow
      RXYInstruction* rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int x2 = rxyInstr->X2Value();
      int b2 = rxyInstr->B2Value();
      int d2 = rxyInstr->D2Value();
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      int32_t alu_out = get_low_register<int32_t>(r1);
      int32_t mem_val = ReadW(b2_val + x2_val + d2, instr);
      if (op == AY) {
        alu_out += mem_val;
        SetS390ConditionCode<int32_t>(alu_out, 0);
      } else if (op == SY) {
        alu_out -= mem_val;
        SetS390ConditionCode<int32_t>(alu_out, 0);
      } else if (op == NY) {
        alu_out &= mem_val;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == OY) {
        alu_out |= mem_val;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == XY) {
        alu_out ^= mem_val;
        SetS390BitWiseConditionCode<uint32_t>(alu_out);
      } else if (op == CY) {
        SetS390ConditionCode<int32_t>(alu_out, mem_val);
      }
      if (op != CY) {
        set_low_register<int32_t>(r1, alu_out);
      }
      break;
    }
    case AG:
    case SG:
    case NG:
    case OG:
    case XG:
    case CG: {
      RXYInstruction* rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int x2 = rxyInstr->X2Value();
      int b2 = rxyInstr->B2Value();
      int d2 = rxyInstr->D2Value();
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      int64_t alu_out = get_register(r1);
      int64_t mem_val =
        *reinterpret_cast<int64_t*>(ReadDW(b2_val + x2_val + d2));

      switch (op) {
        case AG: {
          alu_out += mem_val;
          SetS390ConditionCode<int32_t>(alu_out, 0);
          break;
        }
        case SG: {
          alu_out -= mem_val;
          SetS390ConditionCode<int32_t>(alu_out, 0);
          break;
        }
        case NG: {
          alu_out &= mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        }
        case OG: {
          alu_out |= mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        }
        case XG: {
          alu_out ^= mem_val;
          SetS390BitWiseConditionCode<uint32_t>(alu_out);
          break;
        }
        case CG: {
          SetS390ConditionCode<int64_t>(alu_out, mem_val);
          break;
        }
        default: {
          ASSERT(false);
          break;
        }
      }

      if (op != CG) {
        set_register(r1, alu_out);
      }
      break;
    }
    case ALY:
    case SLY:
    case CLY: {  // @TODO(AlanLi): ALY and SLY needs to set condition code.
      RXYInstruction* rxyInstr = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyInstr->R1Value();
      int x2 = rxyInstr->X2Value();
      int b2 = rxyInstr->B2Value();
      int d2 = rxyInstr->D2Value();
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      uint32_t alu_out = get_low_register<uint32_t>(r1);
      uint32_t mem_val = ReadWU(b2_val + x2_val + d2, instr);

      if (op == ALY) {
        alu_out += mem_val;
        set_low_register<uint32_t>(r1, alu_out);
        SetS390ConditionCode<uint32_t>(alu_out, 0);
      } else if (op == SLY) {
        alu_out -= mem_val;
        set_low_register<uint32_t>(r1, alu_out);
        SetS390ConditionCode<uint32_t>(alu_out, 0);
      } else if (op == CLY) {
        SetS390ConditionCode<uint32_t>(alu_out, mem_val);
      }
      break;
    }
    case AFI: {  // TODO(ALANLI): add overflow
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      int i2 = rilInstr->I2Value();
      int32_t r1_val = get_low_register<int32_t>(r1);
      int32_t alu_out = r1_val + i2;
      set_low_register<int32_t>(r1, alu_out);
      SetS390ConditionCode<int32_t>(alu_out, 0);
      break;
    }
    case AGF:
    case SGF:
    case ALG:
    case SLG: {
#ifndef V8_TARGET_ARCH_S390X
      ASSERT(false);
#endif
      RXYInstruction* rxyinst = reinterpret_cast<RXYInstruction*>(instr);
      uint64_t r1_val = get_register(rxyinst->R1Value());
      int b2 = rxyinst->B2Value();
      int x2 = rxyinst->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxyinst->D2Value();
      uint64_t alu_out = r1_val;
      if (op == ALG) {
        uint64_t mem_val =
            *reinterpret_cast<uint64_t*>(ReadDW(b2_val + d2_val + x2_val));
        alu_out += mem_val;
        SetS390ConditionCode<uint64_t>(alu_out, 0);
      } else if (op == SLG) {
        uint64_t mem_val =
            *reinterpret_cast<uint64_t*>(ReadDW(b2_val + d2_val + x2_val));
        alu_out -= mem_val;
        SetS390ConditionCode<uint64_t>(alu_out, 0);
      } else if (op == AGF) {
        uint32_t mem_val = ReadW(b2_val + d2_val + x2_val, instr);
        alu_out += mem_val;
        SetS390ConditionCode<int64_t>(alu_out, 0);
      } else if (op == SGF) {
        uint32_t mem_val = ReadW(b2_val + d2_val + x2_val, instr);
        alu_out -= mem_val;
        SetS390ConditionCode<int64_t>(alu_out, 0);
      } else { ASSERT(false); }
      set_register(r1, alu_out);
      break;
    }
    case ALGFI:
    case SLGFI: {  // TODO(ALANLI): add carry
#ifndef V8_TARGET_ARCH_S390X
    // should only be called on 64bit
      ASSERT(false);
#endif
      RILInstruction* rilInstr = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilInstr->R1Value();
      uint32_t i2 = rilInstr->I2UnsignedValue();
      uint64_t r1_val = (uint64_t)(get_register(r1));
      uint64_t alu_out;
      if (op == ALGFI)
        alu_out = r1_val + i2;
      else
        alu_out = r1_val - i2;
      set_register(r1, (intptr_t)alu_out);
      SetS390ConditionCode<uint64_t>(alu_out, 0);
      break;
    }
    case MSY:
    case MSG: {
      RXYInstruction* rxyinst = reinterpret_cast<RXYInstruction*>(instr);
      int r1 = rxyinst->R1Value();
      int b2 = rxyinst->B2Value();
      int x2 = rxyinst->X2Value();
      intptr_t b2_val = (b2 == 0) ? 0 : get_register(b2);
      intptr_t x2_val = (x2 == 0) ? 0 : get_register(x2);
      intptr_t d2_val = rxyinst->D2Value();
      if (op == MSY) {
        int32_t mem_val = ReadW(b2_val + d2_val + x2_val, instr);
        int32_t r1_val = get_low_register<int32_t>(r1);
        set_low_register<int32_t>(r1, mem_val * r1_val);
      } else if (op == MSG) {
        int64_t mem_val =
          *reinterpret_cast<int64_t*>(ReadDW(b2_val + d2_val + x2_val));
        int64_t r1_val = get_register(r1);
        set_register(r1, mem_val * r1_val);
      } else {
        UNREACHABLE();
      }
      break;
    }
    case MSFI:
    case MSGFI: {
      RILInstruction* rilinst = reinterpret_cast<RILInstruction*>(instr);
      int r1 = rilinst->R1Value();
      int32_t i2 = rilinst->I2Value();
      if (op == MSFI) {
        int32_t alu_out = get_low_register<int32_t>(r1);
        alu_out = alu_out * i2;
        set_low_register<int32_t>(r1, alu_out);
      } else if (op == MSGFI) {
        intptr_t alu_out = get_register(r1);
        alu_out = alu_out *i2;
        set_register(r1, alu_out);
      } else {
        UNREACHABLE();
      }
      break;
    }
    default:
      UNREACHABLE();
      return false;
  }
  return true;
}

// Executes the current instruction.
void Simulator::InstructionDecode(Instruction* instr) {
  if (v8::internal::FLAG_check_icache) {
    CheckICache(isolate_->simulator_i_cache(), instr);
  }
  pc_modified_ = false;
  if (::v8::internal::FLAG_trace_sim) {
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    // use a reasonably large buffer
    v8::internal::EmbeddedVector<char, 256> buffer;
    dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(instr));
    PrintF("%05d  %08" V8PRIxPTR "  %s\n", icount_,
           reinterpret_cast<intptr_t>(instr), buffer.start());
  }

  // Try to simulate as S390 Instruction first.
  bool processed = true;

  int instrLength = instr->InstructionLength();
  if (instrLength == 2)
    processed = DecodeTwoByte(instr);
  else if (instrLength == 4)
    processed = DecodeFourByte(instr);
  else if (instrLength == 6)
    processed = DecodeSixByte(instr);

  if (processed) {
    if (!pc_modified_) {
      set_pc(reinterpret_cast<intptr_t>(instr) + instrLength);
    }
    return;
  }
}


void Simulator::Execute() {
  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  intptr_t program_counter = get_pc();


  if (::v8::internal::FLAG_stop_sim_at == 0) {
    // Fast version of the dispatch loop without checking whether the simulator
    // should be stopping at a particular executed instruction.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      InstructionDecode(instr);
      program_counter = get_pc();
    }
  } else {
    // FLAG_stop_sim_at is at the non-default value. Stop in the debugger when
    // we reach the particular instuction count.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      if (icount_ == ::v8::internal::FLAG_stop_sim_at) {
        S390Debugger dbg(this);
        dbg.Debug();
      } else {
        InstructionDecode(instr);
      }
      program_counter = get_pc();
    }
  }
}


intptr_t Simulator::Call(byte* entry, int argument_count, ...) {
  // Remember the values of non-volatile registers.
  intptr_t r6_val = get_register(r6);
  intptr_t r7_val = get_register(r7);
  intptr_t r8_val = get_register(r8);
  intptr_t r9_val = get_register(r9);
  intptr_t r10_val = get_register(r10);
  intptr_t r11_val = get_register(r11);
  intptr_t r12_val = get_register(r12);
  intptr_t r13_val = get_register(r13);

  va_list parameters;
  va_start(parameters, argument_count);
  // Set up arguments

  // First 5 arguments passed in registers r2-r6.
  int reg_arg_count   = (argument_count > 5) ? 5 : argument_count;
  int stack_arg_count = argument_count - reg_arg_count;
  for (int i = 0; i < reg_arg_count; i++) {
      set_register(i + 2, va_arg(parameters, intptr_t));
  }

  // Remaining arguments passed on stack.
  intptr_t original_stack = get_register(sp);
  // Compute position of stack on entry to generated code.
  intptr_t entry_stack = (original_stack -
                          (kNumRequiredStackFrameSlots + stack_arg_count) *
                          sizeof(intptr_t));
  if (OS::ActivationFrameAlignment() != 0) {
    entry_stack &= -OS::ActivationFrameAlignment();
  }
  // Store remaining arguments on stack, from low to high memory.
  // +2 is a hack for the LR slot + old SP on PPC
  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack) +
    kStackFrameExtraParamSlot;
  for (int i = 0; i < stack_arg_count; i++) {
    stack_argument[i] = va_arg(parameters, intptr_t);
  }
  va_end(parameters);
  set_register(sp, entry_stack);

  // Prepare to execute the code at entry
#if ABI_USES_FUNCTION_DESCRIPTORS
  // entry is the function descriptor
  set_pc(*(reinterpret_cast<intptr_t *>(entry)));
#else
  // entry is the instruction address
  set_pc(reinterpret_cast<intptr_t>(entry));
#endif



  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  registers_[14] = end_sim_pc;


  // Set up the non-volatile registers with a known value. To be able to check
  // that they are preserved properly across JS execution.
  intptr_t callee_saved_value = icount_;
  if (reg_arg_count < 5) {
    set_register(r6, callee_saved_value);
  }
  set_register(r7, callee_saved_value);
  set_register(r8, callee_saved_value);
  set_register(r9, callee_saved_value);
  set_register(r10, callee_saved_value);
  set_register(r11, callee_saved_value);
  set_register(r12, callee_saved_value);
  set_register(r13, callee_saved_value);

  // Start the simulation
  Execute();

  // Check that the non-volatile registers have been preserved.
  if (reg_arg_count < 5) {
    CHECK_EQ(callee_saved_value, get_register(r6));
  }
  CHECK_EQ(callee_saved_value, get_register(r7));
  CHECK_EQ(callee_saved_value, get_register(r8));
  CHECK_EQ(callee_saved_value, get_register(r9));
  CHECK_EQ(callee_saved_value, get_register(r10));
  CHECK_EQ(callee_saved_value, get_register(r11));
  CHECK_EQ(callee_saved_value, get_register(r12));
  CHECK_EQ(callee_saved_value, get_register(r13));

  // Restore non-volatile registers with the original value.
  set_register(r6, r6_val);
  set_register(r7, r7_val);
  set_register(r8, r8_val);
  set_register(r9, r9_val);
  set_register(r10, r10_val);
  set_register(r11, r11_val);
  set_register(r12, r12_val);
  set_register(r13, r13_val);

  // Pop stack passed arguments.
  CHECK_EQ(entry_stack, get_register(sp));
  set_register(sp, original_stack);

  // Return value register
  intptr_t result = get_register(r2);
  return result;
}


uintptr_t Simulator::PushAddress(uintptr_t address) {
  uintptr_t new_sp = get_register(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  set_register(sp, new_sp);
  return new_sp;
}


uintptr_t Simulator::PopAddress() {
  uintptr_t current_sp = get_register(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  set_register(sp, current_sp + sizeof(uintptr_t));
  return address;
}

} }  // namespace v8::internal

#endif  // USE_SIMULATOR
#endif  // V8_TARGET_ARCH_S390
