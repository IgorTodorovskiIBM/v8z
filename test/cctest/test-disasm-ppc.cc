// Copyright 2011 the V8 project authors. All rights reserved.
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
//

#include <stdlib.h>

#include "src/v8.h"

#include "src/debug.h"
#include "src/disasm.h"
#include "src/disassembler.h"
#include "src/macro-assembler.h"
#include "src/serialize.h"
#include "test/cctest/cctest.h"

using namespace v8::internal;


bool DisassembleAndCompare(byte* pc, const char* compare_string) {
  disasm::NameConverter converter;
  disasm::Disassembler disasm(converter);
  EmbeddedVector<char, 128> disasm_buffer;

  disasm.InstructionDecode(disasm_buffer, pc);

  if (strcmp(compare_string, disasm_buffer.start()) != 0) {
    fprintf(stderr,
            "\x65\x78\x70\x65\x63\x74\x65\x64\x3a\x20\xa"
            "\x6c\xa2\xa"
            "\x64\x69\x73\x61\x73\x73\x65\x6d\x62\x6c\x65\x64\x3a\x20\xa"
            "\x6c\xa2\xa\xa",
            compare_string, disasm_buffer.start());
    return false;
  }
  return true;
}


// Set up V8 to a state where we can at least run the assembler and
// disassembler. Declare the variables and allocate the data structures used
// in the rest of the macros.
#define SET_UP()                                          \
  CcTest::InitializeVM();                                 \
  Isolate* isolate = Isolate::Current();                  \
  HandleScope scope(isolate);                             \
  byte *buffer = reinterpret_cast<byte*>(malloc(4*1024)); \
  Assembler assm(isolate, buffer, 4*1024);                \
  bool failure = false;


// This macro assembles one instruction using the preallocated assembler and
// disassembles the generated instruction, comparing the output to the expected
// value. If the comparison fails an error message is printed, but the test
// continues to run until the end.
#define COMPARE(asm_, compare_string) \
  { \
    int pc_offset = assm.pc_offset(); \
    byte *progcounter = &buffer[pc_offset]; \
    assm.asm_; \
    if (!DisassembleAndCompare(progcounter, compare_string)) failure = true; \
  }

// Force emission of any pending literals into a pool.
#define EMIT_PENDING_LITERALS() \
  assm.CheckConstPool(true, false)


// Verify that all invocations of the COMPARE macro passed successfully.
// Exit with a failure if at least one of the tests failed.
#define VERIFY_RUN() \
if (failure) { \
    V8_Fatal(__FILE__, __LINE__, "\x50\x50\x43\x20\x44\x69\x73\x61\x73\x73\x65\x6d\x62\x6c\x65\x72\x20\x74\x65\x73\x74\x73\x20\x66\x61\x69\x6c\x65\x64\x2e\xa"); \
  }

TEST(DisasmPPC) {
  SET_UP();

  COMPARE(addc(r9, r7, r9),
          "\x37\x64\x32\x37\x34\x38\x31\x34\x20\x20\x20\x20\x20\x20\x20\x61\x64\x64\x63\x20\x20\x20\x20\x72\x39\x2c\x20\x72\x37\x2c\x20\x72\x39");
  COMPARE(addic(r3, r5, Operand(20)),
          "\x33\x30\x36\x35\x30\x30\x31\x34\x20\x20\x20\x20\x20\x20\x20\x61\x64\x64\x69\x63\x20\x20\x20\x72\x33\x2c\x20\x72\x35\x2c\x20\x32\x30");
  COMPARE(addi(r0, ip, Operand(63)),
          "\x33\x38\x30\x63\x30\x30\x33\x66\x20\x20\x20\x20\x20\x20\x20\x61\x64\x64\x69\x20\x20\x20\x20\x72\x30\x2c\x20\x72\x31\x32\x2c\x20\x36\x33");
  COMPARE(add(r5, r7, r0),
          "\x37\x63\x61\x37\x30\x32\x31\x34\x20\x20\x20\x20\x20\x20\x20\x61\x64\x64\x20\x20\x20\x20\x20\x72\x35\x2c\x20\x72\x37\x2c\x20\x72\x30");
  COMPARE(addze(r0, r0, LeaveOE, SetRC),
          "\x37\x63\x30\x30\x30\x31\x39\x35\x20\x20\x20\x20\x20\x20\x20\x61\x64\x64\x7a\x65\x2e\x20\x20\x20\x72\x30\x2c\x20\x72\x30");
  COMPARE(andi(r0, r3, Operand(4)),
          "\x37\x30\x36\x30\x30\x30\x30\x34\x20\x20\x20\x20\x20\x20\x20\x61\x6e\x64\x69\x2e\x20\x20\x20\x72\x30\x2c\x20\x72\x33\x2c\x20\x34");
  COMPARE(and_(r3, r6, r5),
          "\x37\x63\x63\x33\x32\x38\x33\x38\x20\x20\x20\x20\x20\x20\x20\x61\x6e\x64\x20\x20\x20\x20\x20\x72\x33\x2c\x20\x72\x36\x2c\x20\x72\x35");
  COMPARE(and_(r6, r0, r6, SetRC),
          "\x37\x63\x30\x36\x33\x30\x33\x39\x20\x20\x20\x20\x20\x20\x20\x61\x6e\x64\x2e\x20\x20\x20\x20\x72\x36\x2c\x20\x72\x30\x2c\x20\x72\x36");
  // skipping branches (for now?)
  COMPARE(bctr(),
          "\x34\x65\x38\x30\x30\x34\x32\x30\x20\x20\x20\x20\x20\x20\x20\x62\x63\x74\x72");
  COMPARE(blr(),
          "\x34\x65\x38\x30\x30\x30\x32\x30\x20\x20\x20\x20\x20\x20\x20\x62\x6c\x72");
  COMPARE(bclr(BA, SetLK),
          "\x34\x65\x38\x30\x30\x30\x32\x31\x20\x20\x20\x20\x20\x20\x20\x62\x6c\x72\x6c");
  // skipping call - only used in simulator
#if V8_TARGET_ARCH_PPC64
  COMPARE(cmpi(r0, Operand(5)),
          "\x32\x66\x61\x30\x30\x30\x30\x35\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x69\x20\x20\x20\x20\x72\x30\x2c\x20\x35");
#else
  COMPARE(cmpi(r0, Operand(5)),
          "\x32\x66\x38\x30\x30\x30\x30\x35\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x69\x20\x20\x20\x20\x72\x30\x2c\x20\x35");
#endif
#if V8_TARGET_ARCH_PPC64
  COMPARE(cmpl(r6, r7),
          "\x37\x66\x61\x36\x33\x38\x34\x30\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x6c\x20\x20\x20\x20\x72\x36\x2c\x20\x72\x37");
#else
  COMPARE(cmpl(r6, r7),
          "\x37\x66\x38\x36\x33\x38\x34\x30\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x6c\x20\x20\x20\x20\x72\x36\x2c\x20\x72\x37");
#endif
#if V8_TARGET_ARCH_PPC64
  COMPARE(cmp(r5, r11),
          "\x37\x66\x61\x35\x35\x38\x30\x30\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x20\x20\x20\x20\x20\x72\x35\x2c\x20\x72\x31\x31");
#else
  COMPARE(cmp(r5, r11),
          "\x37\x66\x38\x35\x35\x38\x30\x30\x20\x20\x20\x20\x20\x20\x20\x63\x6d\x70\x20\x20\x20\x20\x20\x72\x35\x2c\x20\x72\x31\x31");
#endif
  // skipping crxor - incomplete disassembly
  COMPARE(lbz(r4, MemOperand(r4, 7)),
          "\x38\x38\x38\x34\x30\x30\x30\x37\x20\x20\x20\x20\x20\x20\x20\x6c\x62\x7a\x20\x20\x20\x20\x20\x72\x34\x2c\x20\x37\x28\x72\x34\x29");
  COMPARE(lfd(d0, MemOperand(sp, 128)),
          "\x63\x38\x30\x31\x30\x30\x38\x30\x20\x20\x20\x20\x20\x20\x20\x6c\x66\x64\x20\x20\x20\x20\x20\x64\x30\x2c\x20\x31\x32\x38\x28\x73\x70\x29");
  COMPARE(li(r0, Operand(16)),
          "\x33\x38\x30\x30\x30\x30\x31\x30\x20\x20\x20\x20\x20\x20\x20\x6c\x69\x20\x20\x20\x20\x20\x20\x72\x30\x2c\x20\x31\x36");
  COMPARE(lis(r8, Operand(22560)),
          "\x33\x64\x30\x30\x35\x38\x32\x30\x20\x20\x20\x20\x20\x20\x20\x6c\x69\x73\x20\x20\x20\x20\x20\x72\x38\x2c\x20\x32\x32\x35\x36\x30");
  COMPARE(lwz(ip, MemOperand(r19, 44)),
          "\x38\x31\x39\x33\x30\x30\x32\x63\x20\x20\x20\x20\x20\x20\x20\x6c\x77\x7a\x20\x20\x20\x20\x20\x72\x31\x32\x2c\x20\x34\x34\x28\x72\x31\x39\x29");
  COMPARE(lwzx(r0, MemOperand(r5, ip)),
          "\x37\x63\x30\x35\x36\x30\x32\x65\x20\x20\x20\x20\x20\x20\x20\x6c\x77\x7a\x78\x20\x20\x20\x20\x72\x30\x2c\x20\x72\x35\x2c\x20\x72\x31\x32");
  COMPARE(mflr(r0),
          "\x37\x63\x30\x38\x30\x32\x61\x36\x20\x20\x20\x20\x20\x20\x20\x6d\x66\x6c\x72\x20\x20\x20\x20\x72\x30");
  COMPARE(mr(r15, r4),
          "\x37\x63\x38\x66\x32\x33\x37\x38\x20\x20\x20\x20\x20\x20\x20\x6d\x72\x20\x20\x20\x20\x20\x20\x72\x31\x35\x2c\x20\x72\x34");
  COMPARE(mtctr(r0),
          "\x37\x63\x30\x39\x30\x33\x61\x36\x20\x20\x20\x20\x20\x20\x20\x6d\x74\x63\x74\x72\x20\x20\x20\x72\x30");
  COMPARE(mtlr(r15),
          "\x37\x64\x65\x38\x30\x33\x61\x36\x20\x20\x20\x20\x20\x20\x20\x6d\x74\x6c\x72\x20\x20\x20\x20\x72\x31\x35");
  COMPARE(ori(r8, r8, Operand(42849)),
          "\x36\x31\x30\x38\x61\x37\x36\x31\x20\x20\x20\x20\x20\x20\x20\x6f\x72\x69\x20\x20\x20\x20\x20\x72\x38\x2c\x20\x72\x38\x2c\x20\x34\x32\x38\x34\x39");
  COMPARE(orx(r5, r3, r4),
          "\x37\x63\x36\x35\x32\x33\x37\x38\x20\x20\x20\x20\x20\x20\x20\x6f\x72\x20\x20\x20\x20\x20\x20\x72\x35\x2c\x20\x72\x33\x2c\x20\x72\x34");
  COMPARE(rlwinm(r4, r3, 2, 0, 29),
          "\x35\x34\x36\x34\x31\x30\x33\x61\x20\x20\x20\x20\x20\x20\x20\x72\x6c\x77\x69\x6e\x6d\x20\x20\x72\x34\x2c\x20\x72\x33\x2c\x20\x32\x2c\x20\x30\x2c\x20\x32\x39");
  COMPARE(rlwinm(r0, r3, 0, 31, 31, SetRC),
          "\x35\x34\x36\x30\x30\x37\x66\x66\x20\x20\x20\x20\x20\x20\x20\x72\x6c\x77\x69\x6e\x6d\x2e\x20\x72\x30\x2c\x20\x72\x33\x2c\x20\x30\x2c\x20\x33\x31\x2c\x20\x33\x31");
  COMPARE(srawi(r3, r6, 1),
          "\x37\x63\x63\x33\x30\x65\x37\x30\x20\x20\x20\x20\x20\x20\x20\x73\x72\x61\x77\x69\x20\x20\x20\x72\x33\x2c\x72\x36\x2c\x31");
  COMPARE(stb(r5, MemOperand(r11, 11)),
          "\x39\x38\x61\x62\x30\x30\x30\x62\x20\x20\x20\x20\x20\x20\x20\x73\x74\x62\x20\x20\x20\x20\x20\x72\x35\x2c\x20\x31\x31\x28\x72\x31\x31\x29");
  COMPARE(stfd(d2, MemOperand(sp, 8)),
         "\x64\x38\x34\x31\x30\x30\x30\x38\x20\x20\x20\x20\x20\x20\x20\x73\x74\x66\x64\x20\x20\x20\x20\x64\x32\x2c\x20\x38\x28\x73\x70\x29");
  COMPARE(stw(r16, MemOperand(sp, 64)),
         "\x39\x32\x30\x31\x30\x30\x34\x30\x20\x20\x20\x20\x20\x20\x20\x73\x74\x77\x20\x20\x20\x20\x20\x72\x31\x36\x2c\x20\x36\x34\x28\x73\x70\x29");
  COMPARE(stwu(r3, MemOperand(sp, -4)),
         "\x39\x34\x36\x31\x66\x66\x66\x63\x20\x20\x20\x20\x20\x20\x20\x73\x74\x77\x75\x20\x20\x20\x20\x72\x33\x2c\x20\x2d\x34\x28\x73\x70\x29");
  COMPARE(sub(r3, r3, r4),
         "\x37\x63\x36\x34\x31\x38\x35\x30\x20\x20\x20\x20\x20\x20\x20\x73\x75\x62\x66\x20\x20\x20\x20\x72\x33\x2c\x20\x72\x34\x2c\x20\x72\x33");
  COMPARE(sub(r0, r9, r8, LeaveOE, SetRC),
         "\x37\x63\x30\x38\x34\x38\x35\x31\x20\x20\x20\x20\x20\x20\x20\x73\x75\x62\x66\x2e\x20\x20\x20\x72\x30\x2c\x20\x72\x38\x2c\x20\x72\x39");
  COMPARE(xor_(r6, r5, r4),
         "\x37\x63\x61\x36\x32\x32\x37\x38\x20\x20\x20\x20\x20\x20\x20\x78\x6f\x72\x20\x20\x20\x20\x20\x72\x36\x2c\x20\x72\x35\x2c\x20\x72\x34");

  VERIFY_RUN();
}
