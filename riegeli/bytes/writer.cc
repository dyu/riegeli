// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/bytes/writer.h"

#include <stddef.h>
#include <cstring>
#include <string>

#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"

namespace riegeli {

bool Writer::FailOverflow() { return Fail("Writer position overflow"); }

bool Writer::WriteSlow(absl::string_view src) {
  RIEGELI_ASSERT_GT(src.size(), available())
      << "Failed precondition of Writer::WriteSlow(string_view): "
         "length too small, use Write(string_view) instead";
  if (available() == 0) goto skip_copy;  // memcpy(nullptr, _, 0) is undefined.
  do {
    {
      const size_t available_length = available();
      std::memcpy(cursor_, src.data(), available_length);
      cursor_ = limit_;
      src.remove_prefix(available_length);
    }
  skip_copy:
    if (ABSL_PREDICT_FALSE(!PushSlow())) return false;
  } while (src.size() > available());
  std::memcpy(cursor_, src.data(), src.size());
  cursor_ += src.size();
  return true;
}

bool Writer::WriteSlow(std::string&& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(string&&): "
         "length too small, use Write(string&&) instead";
  // Not std::move(src): forward to Write(string_view).
  return Write(src);
}

bool Writer::WriteSlow(const Chain& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(Chain): "
         "length too small, use Write(Chain) instead";
  for (absl::string_view fragment : src.blocks()) {
    if (ABSL_PREDICT_FALSE(!Write(fragment))) return false;
  }
  return true;
}

bool Writer::WriteSlow(Chain&& src) {
  RIEGELI_ASSERT_GT(src.size(), UnsignedMin(available(), kMaxBytesToCopy()))
      << "Failed precondition of Writer::WriteSlow(Chain&&): "
         "length too small, use Write(Chain&&) instead";
  // Not std::move(src): forward to WriteSlow(const Chain&).
  return WriteSlow(src);
}

}  // namespace riegeli
