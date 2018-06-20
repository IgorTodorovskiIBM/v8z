// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_BASIC_BLOCK_PROFILER_H_
#define V8_BASIC_BLOCK_PROFILER_H_

#include <iosfwd>
#include <list>
#include <string>
#include <vector>

#include "src/base/macros.h"
#include "src/globals.h"

namespace v8 {
namespace internal {

class BasicBlockProfiler {
 public:
  class Data {
   public:
    size_t n_blocks() const { return n_blocks_; }
    const uint32_t* counts() const { return &counts_[0]; }

    void SetCode(v8::base::OStringStream* os);
    void SetFunctionName(v8::base::OStringStream* os);
    void SetSchedule(v8::base::OStringStream* os);
    void SetBlockId(size_t offset, size_t block_id);
    uint32_t* GetCounterAddress(size_t offset);

   private:
    friend class BasicBlockProfiler;
    friend v8::base::OStream& operator<<(v8::base::OStream& os,
                                    const BasicBlockProfiler::Data& s);

    explicit Data(size_t n_blocks);
    ~Data();

    void ResetCounts();

    const size_t n_blocks_;
    std::vector<size_t> block_ids_;
    std::vector<uint32_t> counts_;
    std::string function_name_;
    std::string schedule_;
    std::string code_;
    DISALLOW_COPY_AND_ASSIGN(Data);
  };

  typedef std::list<Data*> DataList;

  BasicBlockProfiler();
  ~BasicBlockProfiler();

  Data* NewData(size_t n_blocks);
  void ResetCounts();

  const DataList* data_list() { return &data_list_; }

 private:
  friend V8_EXPORT_PRIVATE v8::base::OStream& operator<<(
      v8::base::OStream& os, const BasicBlockProfiler& s);

  DataList data_list_;

  DISALLOW_COPY_AND_ASSIGN(BasicBlockProfiler);
};

V8_EXPORT_PRIVATE v8::base::OStream& operator<<(v8::base::OStream& os,
                                           const BasicBlockProfiler& s);
v8::base::OStream& operator<<(v8::base::OStream& os, const BasicBlockProfiler::Data& s);

}  // namespace internal
}  // namespace v8

#endif  // V8_BASIC_BLOCK_PROFILER_H_
