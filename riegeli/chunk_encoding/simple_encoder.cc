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

#include "riegeli/chunk_encoding/simple_encoder.h"

#include <stddef.h>
#include <stdint.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/message_lite.h"
#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/memory.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/chain_writer.h"
#include "riegeli/bytes/message_serialize.h"
#include "riegeli/bytes/writer.h"
#include "riegeli/bytes/writer_utils.h"
#include "riegeli/chunk_encoding/compressor_options.h"
#include "riegeli/chunk_encoding/types.h"

namespace riegeli {

SimpleEncoder::SimpleEncoder(CompressorOptions options, uint64_t size_hint)
    : compression_type_(options.compression_type()),
      sizes_compressor_(options),
      values_compressor_(options, size_hint) {}

void SimpleEncoder::Done() {
  if (ABSL_PREDICT_FALSE(!sizes_compressor_.Close())) Fail(sizes_compressor_);
  if (ABSL_PREDICT_FALSE(!values_compressor_.Close())) Fail(values_compressor_);
  ChunkEncoder::Done();
}

void SimpleEncoder::Reset() {
  ChunkEncoder::Reset();
  sizes_compressor_.Reset();
  values_compressor_.Reset();
}

bool SimpleEncoder::AddRecord(const google::protobuf::MessageLite& record) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (ABSL_PREDICT_FALSE(!record.IsInitialized())) {
    return Fail(absl::StrCat("Failed to serialize message of type ",
                             record.GetTypeName(),
                             " because it is missing required fields: ",
                             record.InitializationErrorString()));
  }
  const size_t size = record.ByteSizeLong();
  if (ABSL_PREDICT_FALSE(size > size_t{std::numeric_limits<int>::max()})) {
    return Fail(absl::StrCat(
        "Failed to serialize message of type ", record.GetTypeName(),
        " because it exceeds maximum protobuf size of 2GB: ", size));
  }
  if (ABSL_PREDICT_FALSE(num_records_ ==
                         std::numeric_limits<uint64_t>::max())) {
    return Fail("Too many records");
  }
  ++num_records_;
  if (ABSL_PREDICT_FALSE(!WriteVarint64(sizes_compressor_.writer(),
                                        IntCast<uint64_t>(size)))) {
    return Fail(*sizes_compressor_.writer());
  }
  if (ABSL_PREDICT_FALSE(
          !SerializePartialToWriter(record, values_compressor_.writer()))) {
    return Fail(*values_compressor_.writer());
  }
  return true;
}

bool SimpleEncoder::AddRecord(absl::string_view record) {
  return AddRecordImpl(record);
}

bool SimpleEncoder::AddRecord(std::string&& record) {
  return AddRecordImpl(std::move(record));
}

bool SimpleEncoder::AddRecord(const Chain& record) {
  return AddRecordImpl(record);
}

bool SimpleEncoder::AddRecord(Chain&& record) {
  return AddRecordImpl(std::move(record));
}

template <typename Record>
bool SimpleEncoder::AddRecordImpl(Record&& record) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (ABSL_PREDICT_FALSE(num_records_ ==
                         std::numeric_limits<uint64_t>::max())) {
    return Fail("Too many records");
  }
  ++num_records_;
  if (ABSL_PREDICT_FALSE(!WriteVarint64(sizes_compressor_.writer(),
                                        IntCast<uint64_t>(record.size())))) {
    return Fail(*sizes_compressor_.writer());
  }
  if (ABSL_PREDICT_FALSE(
          !values_compressor_.writer()->Write(std::forward<Record>(record)))) {
    return Fail(*values_compressor_.writer());
  }
  return true;
}

bool SimpleEncoder::AddRecords(Chain records, std::vector<size_t> limits) {
  RIEGELI_ASSERT_EQ(limits.empty() ? size_t{0} : limits.back(), records.size())
      << "Failed precondition of ChunkEncoder::AddRecords(): "
         "record end positions do not match concatenated record values";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (ABSL_PREDICT_FALSE(limits.size() >
                         std::numeric_limits<uint64_t>::max() - num_records_)) {
    return Fail("Too many records");
  }
  num_records_ += IntCast<uint64_t>(limits.size());
  size_t start = 0;
  for (const auto limit : limits) {
    RIEGELI_ASSERT_GE(limit, start)
        << "Failed precondition of ChunkEncoder::AddRecords(): "
           "record end positions not sorted";
    RIEGELI_ASSERT_LE(limit, records.size())
        << "Failed precondition of ChunkEncoder::AddRecords(): "
           "record end positions do not match concatenated record values";
    if (ABSL_PREDICT_FALSE(!WriteVarint64(sizes_compressor_.writer(),
                                          IntCast<uint64_t>(limit - start)))) {
      return Fail(*sizes_compressor_.writer());
    }
    start = limit;
  }
  if (ABSL_PREDICT_FALSE(
          !values_compressor_.writer()->Write(std::move(records)))) {
    return Fail(*values_compressor_.writer());
  }
  return true;
}

bool SimpleEncoder::EncodeAndClose(Writer* dest, uint64_t* num_records,
                                   uint64_t* decoded_data_size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (ABSL_PREDICT_FALSE(values_compressor_.writer()->pos() >
                         std::numeric_limits<uint64_t>::max())) {
    return Fail("Decoded data size too large");
  }
  *num_records = num_records_;
  *decoded_data_size = values_compressor_.writer()->pos();

  if (ABSL_PREDICT_FALSE(
          !WriteByte(dest, static_cast<uint8_t>(compression_type_)))) {
    return Fail(*dest);
  }

  Chain compressed_sizes;
  ChainWriter compressed_sizes_writer(&compressed_sizes);
  if (ABSL_PREDICT_FALSE(
          !sizes_compressor_.EncodeAndClose(&compressed_sizes_writer))) {
    return Fail(sizes_compressor_);
  }
  if (ABSL_PREDICT_FALSE(!compressed_sizes_writer.Close())) {
    return Fail(compressed_sizes_writer);
  }
  if (ABSL_PREDICT_FALSE(
          !WriteVarint64(dest, IntCast<uint64_t>(compressed_sizes.size()))) ||
      ABSL_PREDICT_FALSE(!dest->Write(std::move(compressed_sizes)))) {
    return Fail(*dest);
  }

  if (ABSL_PREDICT_FALSE(!values_compressor_.EncodeAndClose(dest))) {
    return Fail(values_compressor_);
  }
  return Close();
}

ChunkType SimpleEncoder::GetChunkType() const { return ChunkType::kSimple; }

}  // namespace riegeli
