// Copyright 2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/bit-vector.h"

#include "src/base/bits.h"

namespace v8 {
namespace internal {

#ifdef DEBUG
void BitVector::Print() {
  bool first = true;
  PrintF(u8"{");
  for (int i = 0; i < length(); i++) {
    if (Contains(i)) {
      if (!first) PrintF(u8",");
      first = false;
      PrintF(u8"%d", i);
    }
  }
  PrintF(u8"}\n");
}
#endif


void BitVector::Iterator::Advance() {
  current_++;
  uintptr_t val = current_value_;
  while (val == 0) {
    current_index_++;
    if (Done()) return;
    val = target_->data_[current_index_];
    current_ = current_index_ << kDataBitShift;
  }
  val = SkipZeroBytes(val);
  val = SkipZeroBits(val);
  current_value_ = val >> 1;
}


int BitVector::Count() const {
  int count = 0;
  for (int i = 0; i < data_length_; i++) {
    uintptr_t data = data_[i];
    if (sizeof(data) == 8) {
      count += base::bits::CountPopulation64(data);
    } else {
      count += base::bits::CountPopulation32(static_cast<uint32_t>(data));
    }
  }
  return count;
}

}  // namespace internal
}  // namespace v8
