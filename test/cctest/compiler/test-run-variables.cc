// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#include "test/cctest/compiler/function-tester.h"

using namespace v8::internal;
using namespace v8::internal::compiler;

static const char* throws = NULL;

static const char* load_tests[] = {
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",                       "\x31\x32\x33",       "\x30",
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x28\x72\x20\x3d\x20\x78\x29",                        "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64", "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64",
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x28\x61\x3f\x31\x3a\x32\x29\x3b\x20\x72\x20\x3d\x20\x78",                 "\x31",         "\x32",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",                     "\x31\x32\x33",       "\x30",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x72\x20\x3d\x20\x78\x29",                      "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64", "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x33\x3a\x34\x29\x3b\x20\x72\x20\x3d\x20\x78",               "\x33",         "\x34",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",       "\x31\x32\x33",       "\x30",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x72\x20\x3d\x20\x78\x29",        throws,      throws,
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x35\x3a\x36\x29\x3b\x20\x72\x20\x3d\x20\x78", "\x35",         "\x36",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",         "\x31\x32\x33",       "\x30",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x28\x72\x20\x3d\x20\x78\x29",          throws,      throws,
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x37\x3a\x38\x29\x3b\x20\x72\x20\x3d\x20\x78",   "\x37",         "\x38",
    NULL};

static const char* store_tests[] = {
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x31\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",                     "\x31\x32\x33",  "\x30",
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x28\x61\x3f\x28\x78\x3d\x34\x2c\x32\x29\x3a\x33\x29\x3b\x20\x72\x20\x3d\x20\x78",                "\x32",    "\x33",
    "\x76\x61\x72\x20\x78\x20\x3d\x20\x28\x61\x3f\x34\x3a\x35\x29\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",               "\x31\x32\x33",  "\x30",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x31\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",                   "\x31",    "\x31",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x28\x78\x3d\x34\x2c\x32\x29\x3a\x33\x29\x3b\x20\x72\x20\x3d\x20\x78",              "\x32",    "\x33",
    "\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x34\x3a\x35\x29\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",             "\x34",    "\x35",
    // Assignments to 'const' are SyntaxErrors, handled by the parser,
    // hence we cannot test them here because they are early errors.
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x31\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78",       "\x31\x32\x33",  "\x30",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x28\x78\x3d\x34\x2c\x32\x29\x3a\x33\x29\x3b\x20\x72\x20\x3d\x20\x78",  throws, "\x33",
    "\x27\x75\x73\x65\x20\x73\x74\x72\x69\x63\x74\x27\x3b\x20\x6c\x65\x74\x20\x78\x20\x3d\x20\x28\x61\x3f\x34\x3a\x35\x29\x3b\x20\x78\x20\x3d\x20\x61\x3b\x20\x72\x20\x3d\x20\x78", "\x31\x32\x33",  "\x30",
    NULL};

static const char* bind_tests[] = {
    "\x69\x66\x20\x28\x61\x29\x20\x7b\x20\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x61\x20\x7d\x3b\x20\x72\x20\x3d\x20\x78\x3b",            "\x31\x32\x33", "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64",
    "\x66\x6f\x72\x20\x28\x3b\x20\x61\x20\x3e\x20\x30\x3b\x20\x61\x2d\x2d\x29\x20\x7b\x20\x63\x6f\x6e\x73\x74\x20\x78\x20\x3d\x20\x61\x20\x7d\x3b\x20\x72\x20\x3d\x20\x78", "\x31\x32\x33", "\x75\x6e\x64\x65\x66\x69\x6e\x65\x64",
    // Re-initialization of variables other than legacy 'const' is not
    // possible due to sane variable scoping, hence no tests here.
    NULL};


static void RunVariableTests(const char* source, const char* tests[]) {
  FLAG_harmony_scoping = true;
  EmbeddedVector<char, 512> buffer;

  for (int i = 0; tests[i] != NULL; i += 3) {
    SNPrintF(buffer, source, tests[i]);
    PrintF("\x23\x6c\x84\x3a\x20\x6c\xa2\xa", i / 3, buffer.start());
    FunctionTester T(buffer.start());

    // Check function with non-falsey parameter.
    if (tests[i + 1] != throws) {
      Handle<Object> r = v8::Utils::OpenHandle(*CompileRun(tests[i + 1]));
      T.CheckCall(r, T.Val(123), T.Val("\x72\x65\x73\x75\x6c\x74"));
    } else {
      T.CheckThrows(T.Val(123), T.Val("\x72\x65\x73\x75\x6c\x74"));
    }

    // Check function with falsey parameter.
    if (tests[i + 2] != throws) {
      Handle<Object> r = v8::Utils::OpenHandle(*CompileRun(tests[i + 2]));
      T.CheckCall(r, T.Val(0.0), T.Val("\x72\x65\x73\x75\x6c\x74"));
    } else {
      T.CheckThrows(T.Val(0.0), T.Val("\x72\x65\x73\x75\x6c\x74"));
    }
  }
}


TEST(StackLoadVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, load_tests);
}


TEST(ContextLoadVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x66\x75\x6e\x63\x74\x69\x6f\x6e\x20\x66\x28\x29\x20\x7b\x78\x7d\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, load_tests);
}


TEST(StackStoreVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, store_tests);
}


TEST(ContextStoreVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x66\x75\x6e\x63\x74\x69\x6f\x6e\x20\x66\x28\x29\x20\x7b\x78\x7d\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, store_tests);
}


TEST(StackInitializeVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, bind_tests);
}


TEST(ContextInitializeVariables) {
  const char* source = "\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x28\x61\x2c\x72\x29\x20\x7b\x20\x6c\xa2\x3b\x20\x66\x75\x6e\x63\x74\x69\x6f\x6e\x20\x66\x28\x29\x20\x7b\x78\x7d\x20\x72\x65\x74\x75\x72\x6e\x20\x72\x3b\x20\x7d\x29";
  RunVariableTests(source, bind_tests);
}


TEST(SelfReferenceVariable) {
  FunctionTester T("\x28\x66\x75\x6e\x63\x74\x69\x6f\x6e\x20\x73\x65\x6c\x66\x28\x29\x20\x7b\x20\x72\x65\x74\x75\x72\x6e\x20\x73\x65\x6c\x66\x3b\x20\x7d\x29");

  T.CheckCall(T.function);
  CompileRun("\x76\x61\x72\x20\x73\x65\x6c\x66\x20\x3d\x20\x27\x6e\x6f\x74\x20\x61\x20\x66\x75\x6e\x63\x74\x69\x6f\x6e\x27");
  T.CheckCall(T.function);
}
