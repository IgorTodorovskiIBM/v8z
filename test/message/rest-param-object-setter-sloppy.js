// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Flags: --harmony-rest-parameters

var _bad = "this should fail!";
({
  get bad() { return _bad; },
  set bad(...args) { _bad = args[0]; }
});
