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

#ifndef RIEGELI_CHUNK_ENCODING_TRANSPOSE_DECODER_H_
#define RIEGELI_CHUNK_ENCODING_TRANSPOSE_DECODER_H_

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "riegeli/base/object.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/reader_utils.h"
#include "riegeli/chunk_encoding/field_filter.h"
#include "riegeli/chunk_encoding/transpose_internal.h"

namespace riegeli {

namespace internal {
enum class CallbackType : uint8_t;
}  // namespace internal

class TransposeDecoder final : public Object {
 public:
  TransposeDecoder() noexcept : Object(State::kClosed) {}

  TransposeDecoder(const TransposeDecoder&) = delete;
  TransposeDecoder& operator=(const TransposeDecoder&) = delete;

  // Resets the TransposeDecoder and parses the chunk.
  //
  // Writes concatenated record values to *dest. Sets *limits to sorted record
  // end positions.
  //
  // Precondition: dest->pos() == 0
  //
  // Return values:
  //  * true  - success (healthy())
  //  * false - failure (!healthy());
  //            if !dest->healthy() then the problem was at dest
  bool Reset(Reader* src, uint64_t num_records, uint64_t decoded_data_size,
             const FieldFilter& field_filter, BackwardWriter* dest,
             std::vector<size_t>* limits);

 protected:
  void Done() override;

 private:
  // Information about one proto tag.
  struct TagData {
    // "data" contains varint encoded tag (1 to 5 bytes) followed by inline
    // numeric (if any) or zero otherwise.
    char data[kMaxLengthVarint32() + 1];
    // Length of the varint encoded tag.
    uint8_t size;
  };

  // SubmessageStackElement is used to keep information about started nested
  // submessages. Decoding works in non-recursive loop and this class keeps the
  // information needed to finalize one submessage.
  struct SubmessageStackElement {
    SubmessageStackElement(size_t end_of_submessage, TagData tag_data)
        : end_of_submessage(end_of_submessage), tag_data(tag_data) {}
    // The position of the end of submessage.
    size_t end_of_submessage;
    // Tag of this submessage.
    TagData tag_data;
  };

  // Node template that can be used to resolve the CallbackType of the node in
  // decoding phase.
  struct StateMachineNodeTemplate {
    // "bucket_index" and "buffer_within_bucket_index" identify the decoder
    // to read data from.
    uint32_t bucket_index;
    uint32_t buffer_within_bucket_index;
    // Proto tag of the node.
    uint32_t tag;
    // Tag subtype.
    internal::Subtype subtype;
    // Length of the varint encoded tag.
    uint8_t tag_length;
  };

  // Node of the state machine read from input.
  struct StateMachineNode {
    union {
      // Every state has callback assigned to it that performs the state action.
      // This is an address of a label in DecodeToBuffer method which is filled
      // when DecodeToBuffer is called.
      void* callback;
      // Used to verify there are no implicit loops in the state machine.
      size_t implicit_loop_id;
    };
    // Tag for the field decoded by this node.
    TagData tag_data;
    // Note: callback_type is after tag_data which is 7 bytes and may benefit
    // from being aligned.
    internal::CallbackType callback_type;
    union {
      // Buffer to read data from.
      ChainReader* buffer;
      // In filtering mode, the node is updated in decoding phase based on the
      // current submessage stack and this template.
      StateMachineNodeTemplate* node_template;
    };
    // Node to move to after finishing the callback for this node.
    StateMachineNode* next_node;
  };

  // Note: If more bytes is needed in StateMachineNode, callback_type can be
  // moved to a separate vector with some refactoring.
  static_assert(sizeof(StateMachineNode) == 3 * sizeof(void*) + 8,
                "Unexpected padding in StateMachineNode.");

  struct Context;

  bool Parse(Context* context, Reader* src, const FieldFilter& field_filter);

  // Parse data buffers in "header_reader" and "reader" into
  // "context_->buffers". This method is used when filtering is disabled and all
  // filters are initially decompressed.
  bool ParseBuffers(Context* context, Reader* header_reader, Reader* src);

  // Parse data buffers in "header_reader" and "reader" into
  // "context_->data_buckets". When filtering is enabled, buckets are
  // decompressed on demand. "bucket_indices" contains bucket index for each
  // buffer. "first_buffer_indices" contains the index of first buffer for each
  // bucket.
  bool ParseBuffersForFitering(Context* context, Reader* header_reader,
                               Reader* src,
                               std::vector<uint32_t>* first_buffer_indices,
                               std::vector<uint32_t>* bucket_indices);

  // Precondition: filtering_enabled == true.
  ChainReader* GetBuffer(Context* context, uint32_t bucket_index,
                         uint32_t index_within_bucket);

  static bool ContainsImplicitLoop(
      std::vector<StateMachineNode>* state_machine_nodes);

  bool Decode(Context* context, uint64_t num_records, BackwardWriter* dest,
              std::vector<size_t>* limits);

  // Set callback_type in "node" based on "skipped_submessage_level",
  // "submessage_stack" and "node->node_template".
  bool SetCallbackType(
      Context* context, int skipped_submessage_level,
      const std::vector<SubmessageStackElement>& submessage_stack,
      StateMachineNode* node);
};

}  // namespace riegeli

#endif  // RIEGELI_CHUNK_ENCODING_TRANSPOSE_DECODER_H_
