// Copyright (c) 1994-2006 Sun Microsystems Inc.
// All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// - Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// - Redistribution in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// - Neither the name of Sun Microsystems or the names of contributors may
// be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// The original source code covered by the above license above has been
// modified significantly by Google Inc.
// Copyright 2012 the V8 project authors. All rights reserved.

#include "src/assembler.h"

#include <cmath>
#include "src/api.h"
#include "src/base/cpu.h"
#include "src/base/functional.h"
#include "src/base/lazy-instance.h"
#include "src/base/platform/platform.h"
#include "src/base/utils/random-number-generator.h"
#include "src/builtins.h"
#include "src/codegen.h"
#include "src/counters.h"
#include "src/debug/debug.h"
#include "src/deoptimizer.h"
#include "src/execution.h"
#include "src/ic/ic.h"
#include "src/ic/stub-cache.h"
#include "src/profiler/cpu-profiler.h"
#include "src/regexp/jsregexp.h"
#include "src/regexp/regexp-macro-assembler.h"
#include "src/regexp/regexp-stack.h"
#include "src/runtime/runtime.h"
#include "src/simulator.h"  // For flushing instruction cache.
#include "src/snapshot/serialize.h"
#include "src/token.h"

#if V8_TARGET_ARCH_IA32
#include "src/ia32/assembler-ia32-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_X64
#include "src/x64/assembler-x64-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM64
#include "src/arm64/assembler-arm64-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM
#include "src/arm/assembler-arm-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_PPC
#include "src/ppc/assembler-ppc-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS
#include "src/mips/assembler-mips-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS64
#include "src/mips64/assembler-mips64-inl.h"  // NOLINT
#elif V8_TARGET_ARCH_X87
#include "src/x87/assembler-x87-inl.h"  // NOLINT
#else
#error "Unknown architecture."
#endif

// Include native regexp-macro-assembler.
#ifndef V8_INTERPRETED_REGEXP
#if V8_TARGET_ARCH_IA32
#include "src/regexp/ia32/regexp-macro-assembler-ia32.h"  // NOLINT
#elif V8_TARGET_ARCH_X64
#include "src/regexp/x64/regexp-macro-assembler-x64.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM64
#include "src/regexp/arm64/regexp-macro-assembler-arm64.h"  // NOLINT
#elif V8_TARGET_ARCH_ARM
#include "src/regexp/arm/regexp-macro-assembler-arm.h"  // NOLINT
#elif V8_TARGET_ARCH_PPC
#include "src/regexp/ppc/regexp-macro-assembler-ppc.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS
#include "src/regexp/mips/regexp-macro-assembler-mips.h"  // NOLINT
#elif V8_TARGET_ARCH_MIPS64
#include "src/regexp/mips64/regexp-macro-assembler-mips64.h"  // NOLINT
#elif V8_TARGET_ARCH_X87
#include "src/regexp/x87/regexp-macro-assembler-x87.h"  // NOLINT
#else  // Unknown architecture.
#error "Unknown architecture."
#endif  // Target architecture.
#endif  // V8_INTERPRETED_REGEXP

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// Common double constants.

struct DoubleConstant BASE_EMBEDDED {
double min_int;
double one_half;
double minus_one_half;
double negative_infinity;
double the_hole_nan;
double uint32_bias;
};

static DoubleConstant double_constants;

const char* const RelocInfo::kFillerCommentString = "DEOPTIMIZATION PADDING";

static bool math_exp_data_initialized = false;
static base::Mutex* math_exp_data_mutex = NULL;
static double* math_exp_constants_array = NULL;
static double* math_exp_log_table_array = NULL;

// -----------------------------------------------------------------------------
// Implementation of AssemblerBase

AssemblerBase::AssemblerBase(Isolate* isolate, void* buffer, int buffer_size)
    : isolate_(isolate),
      jit_cookie_(0),
      enabled_cpu_features_(0),
      emit_debug_code_(FLAG_debug_code),
      predictable_code_size_(false),
      // We may use the assembler without an isolate.
      serializer_enabled_(isolate && isolate->serializer_enabled()),
      constant_pool_available_(false) {
  if (FLAG_mask_constants_with_cookie && isolate != NULL)  {
    jit_cookie_ = isolate->random_number_generator()->NextInt();
  }
  own_buffer_ = buffer == NULL;
  if (buffer_size == 0) buffer_size = kMinimalBufferSize;
  DCHECK(buffer_size > 0);
  if (own_buffer_) buffer = NewArray<byte>(buffer_size);
  buffer_ = static_cast<byte*>(buffer);
  buffer_size_ = buffer_size;

  pc_ = buffer_;
}


AssemblerBase::~AssemblerBase() {
  if (own_buffer_) DeleteArray(buffer_);
}


void AssemblerBase::FlushICache(Isolate* isolate, void* start, size_t size) {
  if (size == 0) return;
  if (CpuFeatures::IsSupported(COHERENT_CACHE)) return;

#if defined(USE_SIMULATOR)
  Simulator::FlushICache(isolate->simulator_i_cache(), start, size);
#else
  CpuFeatures::FlushICache(start, size);
#endif  // USE_SIMULATOR
}


void AssemblerBase::FlushICacheWithoutIsolate(void* start, size_t size) {
  // Ideally we would just call Isolate::Current() here. However, this flushes
  // out issues because we usually only need the isolate when in the simulator.
  Isolate* isolate;
#if defined(USE_SIMULATOR)
  isolate = Isolate::Current();
#else
  isolate = nullptr;
#endif  // USE_SIMULATOR
  FlushICache(isolate, start, size);
}


// -----------------------------------------------------------------------------
// Implementation of PredictableCodeSizeScope

PredictableCodeSizeScope::PredictableCodeSizeScope(AssemblerBase* assembler)
    : PredictableCodeSizeScope(assembler, -1) {}


PredictableCodeSizeScope::PredictableCodeSizeScope(AssemblerBase* assembler,
                                                   int expected_size)
    : assembler_(assembler),
      expected_size_(expected_size),
      start_offset_(assembler->pc_offset()),
      old_value_(assembler->predictable_code_size()) {
  assembler_->set_predictable_code_size(true);
}


PredictableCodeSizeScope::~PredictableCodeSizeScope() {
  // TODO(svenpanne) Remove the 'if' when everything works.
  if (expected_size_ >= 0) {
    CHECK_EQ(expected_size_, assembler_->pc_offset() - start_offset_);
  }
  assembler_->set_predictable_code_size(old_value_);
}


// -----------------------------------------------------------------------------
// Implementation of CpuFeatureScope

#ifdef DEBUG
CpuFeatureScope::CpuFeatureScope(AssemblerBase* assembler, CpuFeature f)
    : assembler_(assembler) {
  DCHECK(CpuFeatures::IsSupported(f));
  old_enabled_ = assembler_->enabled_cpu_features();
  uint64_t mask = static_cast<uint64_t>(1) << f;
  // TODO(svenpanne) This special case below doesn't belong here!
#if V8_TARGET_ARCH_ARM
  // ARMv7 is implied by VFP3.
  if (f == VFP3) {
    mask |= static_cast<uint64_t>(1) << ARMv7;
  }
#endif
  assembler_->set_enabled_cpu_features(old_enabled_ | mask);
}


CpuFeatureScope::~CpuFeatureScope() {
  assembler_->set_enabled_cpu_features(old_enabled_);
}
#endif


bool CpuFeatures::initialized_ = false;
unsigned CpuFeatures::supported_ = 0;
unsigned CpuFeatures::cache_line_size_ = 0;


// -----------------------------------------------------------------------------
// Implementation of Label

int Label::pos() const {
  if (pos_ < 0) return -pos_ - 1;
  if (pos_ > 0) return  pos_ - 1;
  UNREACHABLE();
  return 0;
}


// -----------------------------------------------------------------------------
// Implementation of RelocInfoWriter and RelocIterator
//
// Relocation information is written backwards in memory, from high addresses
// towards low addresses, byte by byte.  Therefore, in the encodings listed
// below, the first byte listed it at the highest address, and successive
// bytes in the record are at progressively lower addresses.
//
// Encoding
//
// The most common modes are given single-byte encodings.  Also, it is
// easy to identify the type of reloc info and skip unwanted modes in
// an iteration.
//
// The encoding relies on the fact that there are fewer than 14
// different relocation modes using standard non-compact encoding.
//
// The first byte of a relocation record has a tag in its low 2 bits:
// Here are the record schemes, depending on the low tag and optional higher
// tags.
//
// Low tag:
//   00: embedded_object:      [6-bit pc delta] 00
//
//   01: code_target:          [6-bit pc delta] 01
//
//   10: short_data_record:    [6-bit pc delta] 10 followed by
//                             [6-bit data delta] [2-bit data type tag]
//
//   11: long_record           [6 bit reloc mode] 11
//                             followed by pc delta
//                             followed by optional data depending on type.
//
//  2-bit data type tags, used in short_data_record and data_jump long_record:
//   code_target_with_id: 00
//   position:            01
//   statement_position:  10
//   deopt_reason:        11
//
//  If a pc delta exceeds 6 bits, it is split into a remainder that fits into
//  6 bits and a part that does not. The latter is encoded as a long record
//  with PC_JUMP as pseudo reloc info mode. The former is encoded as part of
//  the following record in the usual way. The long pc jump record has variable
//  length:
//               pc-jump:        [PC_JUMP] 11
//                               [7 bits data] 0
//                                  ...
//                               [7 bits data] 1
//               (Bits 6..31 of pc delta, with leading zeroes
//                dropped, and last non-zero chunk tagged with 1.)

const int kTagBits = 2;
const int kTagMask = (1 << kTagBits) - 1;
const int kLongTagBits = 6;
const int kShortDataTypeTagBits = 2;
const int kShortDataBits = kBitsPerByte - kShortDataTypeTagBits;

const int kEmbeddedObjectTag = 0;
const int kCodeTargetTag = 1;
const int kLocatableTag = 2;
const int kDefaultTag = 3;

const int kSmallPCDeltaBits = kBitsPerByte - kTagBits;
const int kSmallPCDeltaMask = (1 << kSmallPCDeltaBits) - 1;
const int RelocInfo::kMaxSmallPCDelta = kSmallPCDeltaMask;

const int kChunkBits = 7;
const int kChunkMask = (1 << kChunkBits) - 1;
const int kLastChunkTagBits = 1;
const int kLastChunkTagMask = 1;
const int kLastChunkTag = 1;

const int kCodeWithIdTag = 0;
const int kNonstatementPositionTag = 1;
const int kStatementPositionTag = 2;
const int kDeoptReasonTag = 3;


uint32_t RelocInfoWriter::WriteLongPCJump(uint32_t pc_delta) {
  // Return if the pc_delta can fit in kSmallPCDeltaBits bits.
  // Otherwise write a variable length PC jump for the bits that do
  // not fit in the kSmallPCDeltaBits bits.
  if (is_uintn(pc_delta, kSmallPCDeltaBits)) return pc_delta;
  WriteMode(RelocInfo::PC_JUMP);
  uint32_t pc_jump = pc_delta >> kSmallPCDeltaBits;
  DCHECK(pc_jump > 0);
  // Write kChunkBits size chunks of the pc_jump.
  for (; pc_jump > 0; pc_jump = pc_jump >> kChunkBits) {
    byte b = pc_jump & kChunkMask;
    *--pos_ = b << kLastChunkTagBits;
  }
  // Tag the last chunk so it can be identified.
  *pos_ = *pos_ | kLastChunkTag;
  // Return the remaining kSmallPCDeltaBits of the pc_delta.
  return pc_delta & kSmallPCDeltaMask;
}


void RelocInfoWriter::WriteShortTaggedPC(uint32_t pc_delta, int tag) {
  // Write a byte of tagged pc-delta, possibly preceded by an explicit pc-jump.
  pc_delta = WriteLongPCJump(pc_delta);
  *--pos_ = pc_delta << kTagBits | tag;
}


void RelocInfoWriter::WriteShortTaggedData(intptr_t data_delta, int tag) {
  *--pos_ = static_cast<byte>(data_delta << kShortDataTypeTagBits | tag);
}


void RelocInfoWriter::WriteMode(RelocInfo::Mode rmode) {
  STATIC_ASSERT(RelocInfo::NUMBER_OF_MODES <= (1 << kLongTagBits));
  *--pos_ = static_cast<int>((rmode << kTagBits) | kDefaultTag);
}


void RelocInfoWriter::WriteModeAndPC(uint32_t pc_delta, RelocInfo::Mode rmode) {
  // Write two-byte tagged pc-delta, possibly preceded by var. length pc-jump.
  pc_delta = WriteLongPCJump(pc_delta);
  WriteMode(rmode);
  *--pos_ = pc_delta;
}


void RelocInfoWriter::WriteIntData(int number) {
  for (int i = 0; i < kIntSize; i++) {
    *--pos_ = static_cast<byte>(number);
    // Signed right shift is arithmetic shift.  Tested in test-utils.cc.
    number = number >> kBitsPerByte;
  }
}


void RelocInfoWriter::WriteData(intptr_t data_delta) {
  for (int i = 0; i < kIntptrSize; i++) {
    *--pos_ = static_cast<byte>(data_delta);
    // Signed right shift is arithmetic shift.  Tested in test-utils.cc.
    data_delta = data_delta >> kBitsPerByte;
  }
}


void RelocInfoWriter::WritePosition(int pc_delta, int pos_delta,
                                    RelocInfo::Mode rmode) {
  int pos_type_tag = (rmode == RelocInfo::POSITION) ? kNonstatementPositionTag
                                                    : kStatementPositionTag;
  // Check if delta is small enough to fit in a tagged byte.
  if (is_intn(pos_delta, kShortDataBits)) {
    WriteShortTaggedPC(pc_delta, kLocatableTag);
    WriteShortTaggedData(pos_delta, pos_type_tag);
  } else {
    // Otherwise, use costly encoding.
    WriteModeAndPC(pc_delta, rmode);
    WriteIntData(pos_delta);
  }
}


void RelocInfoWriter::FlushPosition() {
  if (!next_position_candidate_flushed_) {
    WritePosition(next_position_candidate_pc_delta_,
                  next_position_candidate_pos_delta_, RelocInfo::POSITION);
    next_position_candidate_pos_delta_ = 0;
    next_position_candidate_pc_delta_ = 0;
    next_position_candidate_flushed_ = true;
  }
}


void RelocInfoWriter::Write(const RelocInfo* rinfo) {
  RelocInfo::Mode rmode = rinfo->rmode();
  if (rmode != RelocInfo::POSITION) {
    FlushPosition();
  }
#ifdef DEBUG
  byte* begin_pos = pos_;
#endif
  DCHECK(rinfo->rmode() < RelocInfo::NUMBER_OF_MODES);
  DCHECK(rinfo->pc() - last_pc_ >= 0);
  // Use unsigned delta-encoding for pc.
  uint32_t pc_delta = static_cast<uint32_t>(rinfo->pc() - last_pc_);

  // The two most common modes are given small tags, and usually fit in a byte.
  if (rmode == RelocInfo::EMBEDDED_OBJECT) {
    WriteShortTaggedPC(pc_delta, kEmbeddedObjectTag);
  } else if (rmode == RelocInfo::CODE_TARGET) {
    WriteShortTaggedPC(pc_delta, kCodeTargetTag);
    DCHECK(begin_pos - pos_ <= RelocInfo::kMaxCallSize);
  } else if (rmode == RelocInfo::CODE_TARGET_WITH_ID) {
    // Use signed delta-encoding for id.
    DCHECK_EQ(static_cast<int>(rinfo->data()), rinfo->data());
    int id_delta = static_cast<int>(rinfo->data()) - last_id_;
    // Check if delta is small enough to fit in a tagged byte.
    if (is_intn(id_delta, kShortDataBits)) {
      WriteShortTaggedPC(pc_delta, kLocatableTag);
      WriteShortTaggedData(id_delta, kCodeWithIdTag);
    } else {
      // Otherwise, use costly encoding.
      WriteModeAndPC(pc_delta, rmode);
      WriteIntData(id_delta);
    }
    last_id_ = static_cast<int>(rinfo->data());
  } else if (rmode == RelocInfo::DEOPT_REASON) {
    DCHECK(rinfo->data() < (1 << kShortDataBits));
    WriteShortTaggedPC(pc_delta, kLocatableTag);
    WriteShortTaggedData(rinfo->data(), kDeoptReasonTag);
  } else if (RelocInfo::IsPosition(rmode)) {
    // Use signed delta-encoding for position.
    DCHECK_EQ(static_cast<int>(rinfo->data()), rinfo->data());
    int pos_delta = static_cast<int>(rinfo->data()) - last_position_;
    if (rmode == RelocInfo::STATEMENT_POSITION) {
      WritePosition(pc_delta, pos_delta, rmode);
    } else {
      DCHECK_EQ(rmode, RelocInfo::POSITION);
      if (pc_delta != 0 || last_mode_ != RelocInfo::POSITION) {
        FlushPosition();
        next_position_candidate_pc_delta_ = pc_delta;
        next_position_candidate_pos_delta_ = pos_delta;
      } else {
        next_position_candidate_pos_delta_ += pos_delta;
      }
      next_position_candidate_flushed_ = false;
    }
    last_position_ = static_cast<int>(rinfo->data());
  } else {
    WriteModeAndPC(pc_delta, rmode);
    if (RelocInfo::IsComment(rmode)) {
      WriteData(rinfo->data());
    } else if (RelocInfo::IsConstPool(rmode) ||
               RelocInfo::IsVeneerPool(rmode) ||
               RelocInfo::IsDebugBreakSlotAtCall(rmode)) {
      WriteIntData(static_cast<int>(rinfo->data()));
    }
  }
  last_pc_ = rinfo->pc();
  last_mode_ = rmode;
#ifdef DEBUG
  DCHECK(begin_pos - pos_ <= kMaxSize);
#endif
}


inline int RelocIterator::AdvanceGetTag() {
  return *--pos_ & kTagMask;
}


inline RelocInfo::Mode RelocIterator::GetMode() {
  return static_cast<RelocInfo::Mode>((*pos_ >> kTagBits) &
                                      ((1 << kLongTagBits) - 1));
}


inline void RelocIterator::ReadShortTaggedPC() {
  rinfo_.pc_ += *pos_ >> kTagBits;
}


inline void RelocIterator::AdvanceReadPC() {
  rinfo_.pc_ += *--pos_;
}


void RelocIterator::AdvanceReadId() {
  int x = 0;
  for (int i = 0; i < kIntSize; i++) {
    x |= static_cast<int>(*--pos_) << i * kBitsPerByte;
  }
  last_id_ += x;
  rinfo_.data_ = last_id_;
}


void RelocIterator::AdvanceReadInt() {
  int x = 0;
  for (int i = 0; i < kIntSize; i++) {
    x |= static_cast<int>(*--pos_) << i * kBitsPerByte;
  }
  rinfo_.data_ = x;
}


void RelocIterator::AdvanceReadPosition() {
  int x = 0;
  for (int i = 0; i < kIntSize; i++) {
    x |= static_cast<int>(*--pos_) << i * kBitsPerByte;
  }
  last_position_ += x;
  rinfo_.data_ = last_position_;
}


void RelocIterator::AdvanceReadData() {
  intptr_t x = 0;
  for (int i = 0; i < kIntptrSize; i++) {
    x |= static_cast<intptr_t>(*--pos_) << i * kBitsPerByte;
  }
  rinfo_.data_ = x;
}


void RelocIterator::AdvanceReadLongPCJump() {
  // Read the 32-kSmallPCDeltaBits most significant bits of the
  // pc jump in kChunkBits bit chunks and shift them into place.
  // Stop when the last chunk is encountered.
  uint32_t pc_jump = 0;
  for (int i = 0; i < kIntSize; i++) {
    byte pc_jump_part = *--pos_;
    pc_jump |= (pc_jump_part >> kLastChunkTagBits) << i * kChunkBits;
    if ((pc_jump_part & kLastChunkTagMask) == 1) break;
  }
  // The least significant kSmallPCDeltaBits bits will be added
  // later.
  rinfo_.pc_ += pc_jump << kSmallPCDeltaBits;
}


inline int RelocIterator::GetShortDataTypeTag() {
  return *pos_ & ((1 << kShortDataTypeTagBits) - 1);
}


inline void RelocIterator::ReadShortTaggedId() {
  int8_t signed_b = *pos_;
  // Signed right shift is arithmetic shift.  Tested in test-utils.cc.
  last_id_ += signed_b >> kShortDataTypeTagBits;
  rinfo_.data_ = last_id_;
}


inline void RelocIterator::ReadShortTaggedPosition() {
  int8_t signed_b = *pos_;
  // Signed right shift is arithmetic shift.  Tested in test-utils.cc.
  last_position_ += signed_b >> kShortDataTypeTagBits;
  rinfo_.data_ = last_position_;
}


inline void RelocIterator::ReadShortTaggedData() {
  uint8_t unsigned_b = *pos_;
  rinfo_.data_ = unsigned_b >> kTagBits;
}


static inline RelocInfo::Mode GetPositionModeFromTag(int tag) {
  DCHECK(tag == kNonstatementPositionTag ||
         tag == kStatementPositionTag);
  return (tag == kNonstatementPositionTag) ?
         RelocInfo::POSITION :
         RelocInfo::STATEMENT_POSITION;
}


void RelocIterator::next() {
  DCHECK(!done());
  // Basically, do the opposite of RelocInfoWriter::Write.
  // Reading of data is as far as possible avoided for unwanted modes,
  // but we must always update the pc.
  //
  // We exit this loop by returning when we find a mode we want.
  while (pos_ > end_) {
    int tag = AdvanceGetTag();
    if (tag == kEmbeddedObjectTag) {
      ReadShortTaggedPC();
      if (SetMode(RelocInfo::EMBEDDED_OBJECT)) return;
    } else if (tag == kCodeTargetTag) {
      ReadShortTaggedPC();
      if (SetMode(RelocInfo::CODE_TARGET)) return;
    } else if (tag == kLocatableTag) {
      ReadShortTaggedPC();
      Advance();
      int data_type_tag = GetShortDataTypeTag();
      if (data_type_tag == kCodeWithIdTag) {
        if (SetMode(RelocInfo::CODE_TARGET_WITH_ID)) {
          ReadShortTaggedId();
          return;
        }
      } else if (data_type_tag == kDeoptReasonTag) {
        if (SetMode(RelocInfo::DEOPT_REASON)) {
          ReadShortTaggedData();
          return;
        }
      } else {
        DCHECK(data_type_tag == kNonstatementPositionTag ||
               data_type_tag == kStatementPositionTag);
        if (mode_mask_ & RelocInfo::kPositionMask) {
          // Always update the position if we are interested in either
          // statement positions or non-statement positions.
          ReadShortTaggedPosition();
          if (SetMode(GetPositionModeFromTag(data_type_tag))) return;
        }
      }
    } else {
      DCHECK(tag == kDefaultTag);
      RelocInfo::Mode rmode = GetMode();
      if (rmode == RelocInfo::PC_JUMP) {
        AdvanceReadLongPCJump();
      } else {
        AdvanceReadPC();
        if (rmode == RelocInfo::CODE_TARGET_WITH_ID) {
          if (SetMode(rmode)) {
            AdvanceReadId();
            return;
          }
          Advance(kIntSize);
        } else if (RelocInfo::IsComment(rmode)) {
          if (SetMode(rmode)) {
            AdvanceReadData();
            return;
          }
          Advance(kIntptrSize);
        } else if (RelocInfo::IsPosition(rmode)) {
          if (mode_mask_ & RelocInfo::kPositionMask) {
            // Always update the position if we are interested in either
            // statement positions or non-statement positions.
            AdvanceReadPosition();
            if (SetMode(rmode)) return;
          } else {
            Advance(kIntSize);
          }
        } else if (RelocInfo::IsConstPool(rmode) ||
                   RelocInfo::IsVeneerPool(rmode) ||
                   RelocInfo::IsDebugBreakSlotAtCall(rmode)) {
          if (SetMode(rmode)) {
            AdvanceReadInt();
            return;
          }
          Advance(kIntSize);
        } else if (SetMode(static_cast<RelocInfo::Mode>(rmode))) {
          return;
        }
      }
    }
  }
  if (code_age_sequence_ != NULL) {
    byte* old_code_age_sequence = code_age_sequence_;
    code_age_sequence_ = NULL;
    if (SetMode(RelocInfo::CODE_AGE_SEQUENCE)) {
      rinfo_.data_ = 0;
      rinfo_.pc_ = old_code_age_sequence;
      return;
    }
  }
  done_ = true;
}


RelocIterator::RelocIterator(Code* code, int mode_mask) {
  rinfo_.host_ = code;
  rinfo_.pc_ = code->instruction_start();
  rinfo_.data_ = 0;
  // Relocation info is read backwards.
  pos_ = code->relocation_start() + code->relocation_size();
  end_ = code->relocation_start();
  done_ = false;
  mode_mask_ = mode_mask;
  last_id_ = 0;
  last_position_ = 0;
  byte* sequence = code->FindCodeAgeSequence();
  // We get the isolate from the map, because at serialization time
  // the code pointer has been cloned and isn't really in heap space.
  Isolate* isolate = code->map()->GetIsolate();
  if (sequence != NULL && !Code::IsYoungSequence(isolate, sequence)) {
    code_age_sequence_ = sequence;
  } else {
    code_age_sequence_ = NULL;
  }
  if (mode_mask_ == 0) pos_ = end_;
  next();
}


RelocIterator::RelocIterator(const CodeDesc& desc, int mode_mask) {
  rinfo_.pc_ = desc.buffer;
  rinfo_.data_ = 0;
  // Relocation info is read backwards.
  pos_ = desc.buffer + desc.buffer_size;
  end_ = pos_ - desc.reloc_size;
  done_ = false;
  mode_mask_ = mode_mask;
  last_id_ = 0;
  last_position_ = 0;
  code_age_sequence_ = NULL;
  if (mode_mask_ == 0) pos_ = end_;
  next();
}


// -----------------------------------------------------------------------------
// Implementation of RelocInfo


#ifdef DEBUG
bool RelocInfo::RequiresRelocation(const CodeDesc& desc) {
  // Ensure there are no code targets or embedded objects present in the
  // deoptimization entries, they would require relocation after code
  // generation.
  int mode_mask = RelocInfo::kCodeTargetMask |
                  RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT) |
                  RelocInfo::ModeMask(RelocInfo::CELL) |
                  RelocInfo::kApplyMask;
  RelocIterator it(desc, mode_mask);
  return !it.done();
}
#endif


#ifdef ENABLE_DISASSEMBLER
const char* RelocInfo::RelocModeName(RelocInfo::Mode rmode) {
  switch (rmode) {
    case NONE32:
      return "no reloc 32";
    case NONE64:
      return "no reloc 64";
    case EMBEDDED_OBJECT:
      return "embedded object";
    case CONSTRUCT_CALL:
      return "code target (js construct call)";
    case DEBUGGER_STATEMENT:
      return "debugger statement";
    case CODE_TARGET:
      return "code target";
    case CODE_TARGET_WITH_ID:
      return "code target with id";
    case CELL:
      return "property cell";
    case RUNTIME_ENTRY:
      return "runtime entry";
    case COMMENT:
      return "comment";
    case POSITION:
      return "position";
    case STATEMENT_POSITION:
      return "statement position";
    case EXTERNAL_REFERENCE:
      return "external reference";
    case INTERNAL_REFERENCE:
      return "internal reference";
    case INTERNAL_REFERENCE_ENCODED:
      return "encoded internal reference";
    case DEOPT_REASON:
      return "deopt reason";
    case CONST_POOL:
      return "constant pool";
    case VENEER_POOL:
      return "veneer pool";
    case DEBUG_BREAK_SLOT_AT_POSITION:
      return "debug break slot at position";
    case DEBUG_BREAK_SLOT_AT_RETURN:
      return "debug break slot at return";
    case DEBUG_BREAK_SLOT_AT_CALL:
      return "debug break slot at call";
    case DEBUG_BREAK_SLOT_AT_CONSTRUCT_CALL:
      return "debug break slot at construct call";
    case CODE_AGE_SEQUENCE:
      return "code age sequence";
    case GENERATOR_CONTINUATION:
      return "generator continuation";
    case NUMBER_OF_MODES:
    case PC_JUMP:
      UNREACHABLE();
      return "number_of_modes";
  }
  return "unknown relocation type";
}


void RelocInfo::Print(Isolate* isolate, std::ostream& os) {  // NOLINT
  os << static_cast<const void*>(pc_) << "  " << RelocModeName(rmode_);
  if (IsComment(rmode_)) {
    os << "  (" << reinterpret_cast<char*>(data_) << ")";
  } else if (rmode_ == DEOPT_REASON) {
    os << "  (" << Deoptimizer::GetDeoptReason(
                       static_cast<Deoptimizer::DeoptReason>(data_)) << ")";
  } else if (rmode_ == EMBEDDED_OBJECT) {
    os << "  (" << Brief(target_object()) << ")";
  } else if (rmode_ == EXTERNAL_REFERENCE) {
    ExternalReferenceEncoder ref_encoder(isolate);
    os << " ("
       << ref_encoder.NameOfAddress(isolate, target_external_reference())
       << ")  (" << static_cast<const void*>(target_external_reference())
       << ")";
  } else if (IsCodeTarget(rmode_)) {
    Code* code = Code::GetCodeFromTargetAddress(target_address());
    os << " (" << Code::Kind2String(code->kind()) << ")  ("
       << static_cast<const void*>(target_address()) << ")";
    if (rmode_ == CODE_TARGET_WITH_ID) {
      os << " (id=" << static_cast<int>(data_) << ")";
    }
  } else if (IsPosition(rmode_)) {
    os << "  (" << data() << ")";
  } else if (IsRuntimeEntry(rmode_) &&
             isolate->deoptimizer_data() != NULL) {
    // Depotimization bailouts are stored as runtime entries.
    int id = Deoptimizer::GetDeoptimizationId(
        isolate, target_address(), Deoptimizer::EAGER);
    if (id != Deoptimizer::kNotDeoptimizationEntry) {
      os << "  (deoptimization bailout " << id << ")";
    }
  } else if (IsConstPool(rmode_)) {
    os << " (size " << static_cast<int>(data_) << ")";
  }

  os << "\n";
}
#endif  // ENABLE_DISASSEMBLER


#ifdef VERIFY_HEAP
void RelocInfo::Verify(Isolate* isolate) {
  switch (rmode_) {
    case EMBEDDED_OBJECT:
      Object::VerifyPointer(target_object());
      break;
    case CELL:
      Object::VerifyPointer(target_cell());
      break;
    case DEBUGGER_STATEMENT:
    case CONSTRUCT_CALL:
    case CODE_TARGET_WITH_ID:
    case CODE_TARGET: {
      // convert inline target address to code object
      Address addr = target_address();
      CHECK(addr != NULL);
      // Check that we can find the right code object.
      Code* code = Code::GetCodeFromTargetAddress(addr);
      Object* found = isolate->FindCodeObject(addr);
      CHECK(found->IsCode());
      CHECK(code->address() == HeapObject::cast(found)->address());
      break;
    }
    case INTERNAL_REFERENCE:
    case INTERNAL_REFERENCE_ENCODED: {
      Address target = target_internal_reference();
      Address pc = target_internal_reference_address();
      Code* code = Code::cast(isolate->FindCodeObject(pc));
      CHECK(target >= code->instruction_start());
      CHECK(target <= code->instruction_end());
      break;
    }
    case RUNTIME_ENTRY:
    case COMMENT:
    case POSITION:
    case STATEMENT_POSITION:
    case EXTERNAL_REFERENCE:
    case DEOPT_REASON:
    case CONST_POOL:
    case VENEER_POOL:
    case DEBUG_BREAK_SLOT_AT_POSITION:
    case DEBUG_BREAK_SLOT_AT_RETURN:
    case DEBUG_BREAK_SLOT_AT_CALL:
    case DEBUG_BREAK_SLOT_AT_CONSTRUCT_CALL:
    case GENERATOR_CONTINUATION:
    case NONE32:
    case NONE64:
      break;
    case NUMBER_OF_MODES:
    case PC_JUMP:
      UNREACHABLE();
      break;
    case CODE_AGE_SEQUENCE:
      DCHECK(Code::IsYoungSequence(isolate, pc_) || code_age_stub()->IsCode());
      break;
  }
}
#endif  // VERIFY_HEAP


int RelocInfo::DebugBreakCallArgumentsCount(intptr_t data) {
  return static_cast<int>(data);
}


// -----------------------------------------------------------------------------
// Implementation of ExternalReference

void ExternalReference::SetUp() {
  double_constants.min_int = kMinInt;
  double_constants.one_half = 0.5;
  double_constants.minus_one_half = -0.5;
  double_constants.the_hole_nan = bit_cast<double>(kHoleNanInt64);
  double_constants.negative_infinity = -V8_INFINITY;
  double_constants.uint32_bias =
    static_cast<double>(static_cast<uint32_t>(0xFFFFFFFF)) + 1;

  math_exp_data_mutex = new base::Mutex();
}


void ExternalReference::InitializeMathExpData() {
  // Early return?
  if (math_exp_data_initialized) return;

  base::LockGuard<base::Mutex> lock_guard(math_exp_data_mutex);
  if (!math_exp_data_initialized) {
    // If this is changed, generated code must be adapted too.
    const int kTableSizeBits = 11;
    const int kTableSize = 1 << kTableSizeBits;
    const double kTableSizeDouble = static_cast<double>(kTableSize);

    math_exp_constants_array = new double[9];
    // Input values smaller than this always return 0.
    math_exp_constants_array[0] = -708.39641853226408;
    // Input values larger than this always return +Infinity.
    math_exp_constants_array[1] = 709.78271289338397;
    math_exp_constants_array[2] = V8_INFINITY;
    // The rest is black magic. Do not attempt to understand it. It is
    // loosely based on the "expd" function published at:
    // http://herumi.blogspot.com/2011/08/fast-double-precision-exponential.html
    const double constant3 = (1 << kTableSizeBits) / std::log(2.0);
    math_exp_constants_array[3] = constant3;
    math_exp_constants_array[4] =
        static_cast<double>(static_cast<int64_t>(3) << 51);
    math_exp_constants_array[5] = 1 / constant3;
    math_exp_constants_array[6] = 3.0000000027955394;
    math_exp_constants_array[7] = 0.16666666685227835;
    math_exp_constants_array[8] = 1;

    math_exp_log_table_array = new double[kTableSize];
    for (int i = 0; i < kTableSize; i++) {
      double value = std::pow(2, i / kTableSizeDouble);
      uint64_t bits = bit_cast<uint64_t, double>(value);
      bits &= (static_cast<uint64_t>(1) << 52) - 1;
      double mantissa = bit_cast<double, uint64_t>(bits);
      math_exp_log_table_array[i] = mantissa;
    }

    math_exp_data_initialized = true;
  }
}


void ExternalReference::TearDownMathExpData() {
  delete[] math_exp_constants_array;
  math_exp_constants_array = NULL;
  delete[] math_exp_log_table_array;
  math_exp_log_table_array = NULL;
  delete math_exp_data_mutex;
  math_exp_data_mutex = NULL;
}


ExternalReference::ExternalReference(Builtins::CFunctionId id, Isolate* isolate)
  : address_(Redirect(isolate, Builtins::c_function_address(id))) {}


ExternalReference::ExternalReference(
    ApiFunction* fun,
    Type type = ExternalReference::BUILTIN_CALL,
    Isolate* isolate = NULL)
  : address_(Redirect(isolate, fun->address(), type)) {}


ExternalReference::ExternalReference(Builtins::Name name, Isolate* isolate)
  : address_(isolate->builtins()->builtin_address(name)) {}


ExternalReference::ExternalReference(Runtime::FunctionId id, Isolate* isolate)
    : address_(Redirect(isolate, Runtime::FunctionForId(id)->entry)) {}


ExternalReference::ExternalReference(const Runtime::Function* f,
                                     Isolate* isolate)
    : address_(Redirect(isolate, f->entry)) {}


ExternalReference ExternalReference::isolate_address(Isolate* isolate) {
  return ExternalReference(isolate);
}


ExternalReference::ExternalReference(StatsCounter* counter)
  : address_(reinterpret_cast<Address>(counter->GetInternalPointer())) {}


ExternalReference::ExternalReference(Isolate::AddressId id, Isolate* isolate)
  : address_(isolate->get_address_from_id(id)) {}


ExternalReference::ExternalReference(const SCTableReference& table_ref)
  : address_(table_ref.address()) {}


ExternalReference ExternalReference::
    incremental_marking_record_write_function(Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate,
      FUNCTION_ADDR(IncrementalMarking::RecordWriteFromCode)));
}


ExternalReference ExternalReference::
    store_buffer_overflow_function(Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate,
      FUNCTION_ADDR(StoreBuffer::StoreBufferOverflow)));
}


ExternalReference ExternalReference::delete_handle_scope_extensions(
    Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate,
      FUNCTION_ADDR(HandleScope::DeleteExtensions)));
}


ExternalReference ExternalReference::get_date_field_function(
    Isolate* isolate) {
  return ExternalReference(Redirect(isolate, FUNCTION_ADDR(JSDate::GetField)));
}


ExternalReference ExternalReference::get_make_code_young_function(
    Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate, FUNCTION_ADDR(Code::MakeCodeAgeSequenceYoung)));
}


ExternalReference ExternalReference::get_mark_code_as_executed_function(
    Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate, FUNCTION_ADDR(Code::MarkCodeAsExecuted)));
}


ExternalReference ExternalReference::date_cache_stamp(Isolate* isolate) {
  return ExternalReference(isolate->date_cache()->stamp_address());
}


ExternalReference ExternalReference::stress_deopt_count(Isolate* isolate) {
  return ExternalReference(isolate->stress_deopt_count_address());
}


ExternalReference ExternalReference::new_deoptimizer_function(
    Isolate* isolate) {
  return ExternalReference(
      Redirect(isolate, FUNCTION_ADDR(Deoptimizer::New)));
}


ExternalReference ExternalReference::compute_output_frames_function(
    Isolate* isolate) {
  return ExternalReference(
      Redirect(isolate, FUNCTION_ADDR(Deoptimizer::ComputeOutputFrames)));
}


ExternalReference ExternalReference::log_enter_external_function(
    Isolate* isolate) {
  return ExternalReference(
      Redirect(isolate, FUNCTION_ADDR(Logger::EnterExternal)));
}


ExternalReference ExternalReference::log_leave_external_function(
    Isolate* isolate) {
  return ExternalReference(
      Redirect(isolate, FUNCTION_ADDR(Logger::LeaveExternal)));
}


ExternalReference ExternalReference::keyed_lookup_cache_keys(Isolate* isolate) {
  return ExternalReference(isolate->keyed_lookup_cache()->keys_address());
}


ExternalReference ExternalReference::keyed_lookup_cache_field_offsets(
    Isolate* isolate) {
  return ExternalReference(
      isolate->keyed_lookup_cache()->field_offsets_address());
}


ExternalReference ExternalReference::roots_array_start(Isolate* isolate) {
  return ExternalReference(isolate->heap()->roots_array_start());
}


ExternalReference ExternalReference::allocation_sites_list_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->allocation_sites_list_address());
}


ExternalReference ExternalReference::address_of_stack_limit(Isolate* isolate) {
  return ExternalReference(isolate->stack_guard()->address_of_jslimit());
}


ExternalReference ExternalReference::address_of_real_stack_limit(
    Isolate* isolate) {
  return ExternalReference(isolate->stack_guard()->address_of_real_jslimit());
}


ExternalReference ExternalReference::address_of_regexp_stack_limit(
    Isolate* isolate) {
  return ExternalReference(isolate->regexp_stack()->limit_address());
}


ExternalReference ExternalReference::new_space_start(Isolate* isolate) {
  return ExternalReference(isolate->heap()->NewSpaceStart());
}


ExternalReference ExternalReference::store_buffer_top(Isolate* isolate) {
  return ExternalReference(isolate->heap()->store_buffer_top_address());
}


ExternalReference ExternalReference::new_space_mask(Isolate* isolate) {
  return ExternalReference(reinterpret_cast<Address>(
      isolate->heap()->NewSpaceMask()));
}


ExternalReference ExternalReference::new_space_allocation_top_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->NewSpaceAllocationTopAddress());
}


ExternalReference ExternalReference::new_space_allocation_limit_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->NewSpaceAllocationLimitAddress());
}


ExternalReference ExternalReference::old_space_allocation_top_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->OldSpaceAllocationTopAddress());
}


ExternalReference ExternalReference::old_space_allocation_limit_address(
    Isolate* isolate) {
  return ExternalReference(isolate->heap()->OldSpaceAllocationLimitAddress());
}


ExternalReference ExternalReference::handle_scope_level_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_level_address(isolate));
}


ExternalReference ExternalReference::handle_scope_next_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_next_address(isolate));
}


ExternalReference ExternalReference::handle_scope_limit_address(
    Isolate* isolate) {
  return ExternalReference(HandleScope::current_limit_address(isolate));
}


ExternalReference ExternalReference::scheduled_exception_address(
    Isolate* isolate) {
  return ExternalReference(isolate->scheduled_exception_address());
}


ExternalReference ExternalReference::address_of_pending_message_obj(
    Isolate* isolate) {
  return ExternalReference(isolate->pending_message_obj_address());
}


ExternalReference ExternalReference::address_of_min_int() {
  return ExternalReference(reinterpret_cast<void*>(&double_constants.min_int));
}


ExternalReference ExternalReference::address_of_one_half() {
  return ExternalReference(reinterpret_cast<void*>(&double_constants.one_half));
}


ExternalReference ExternalReference::address_of_minus_one_half() {
  return ExternalReference(
      reinterpret_cast<void*>(&double_constants.minus_one_half));
}


ExternalReference ExternalReference::address_of_negative_infinity() {
  return ExternalReference(
      reinterpret_cast<void*>(&double_constants.negative_infinity));
}


ExternalReference ExternalReference::address_of_the_hole_nan() {
  return ExternalReference(
      reinterpret_cast<void*>(&double_constants.the_hole_nan));
}


ExternalReference ExternalReference::address_of_uint32_bias() {
  return ExternalReference(
      reinterpret_cast<void*>(&double_constants.uint32_bias));
}


ExternalReference ExternalReference::is_profiling_address(Isolate* isolate) {
  return ExternalReference(isolate->cpu_profiler()->is_profiling_address());
}


ExternalReference ExternalReference::invoke_function_callback(
    Isolate* isolate) {
  Address thunk_address = FUNCTION_ADDR(&InvokeFunctionCallback);
  ExternalReference::Type thunk_type = ExternalReference::PROFILING_API_CALL;
  ApiFunction thunk_fun(thunk_address);
  return ExternalReference(&thunk_fun, thunk_type, isolate);
}


ExternalReference ExternalReference::invoke_accessor_getter_callback(
    Isolate* isolate) {
  Address thunk_address = FUNCTION_ADDR(&InvokeAccessorGetterCallback);
  ExternalReference::Type thunk_type =
      ExternalReference::PROFILING_GETTER_CALL;
  ApiFunction thunk_fun(thunk_address);
  return ExternalReference(&thunk_fun, thunk_type, isolate);
}


#ifndef V8_INTERPRETED_REGEXP

ExternalReference ExternalReference::re_check_stack_guard_state(
    Isolate* isolate) {
  Address function;
#if V8_TARGET_ARCH_X64
  function = FUNCTION_ADDR(RegExpMacroAssemblerX64::CheckStackGuardState);
#elif V8_TARGET_ARCH_IA32
  function = FUNCTION_ADDR(RegExpMacroAssemblerIA32::CheckStackGuardState);
#elif V8_TARGET_ARCH_ARM64
  function = FUNCTION_ADDR(RegExpMacroAssemblerARM64::CheckStackGuardState);
#elif V8_TARGET_ARCH_ARM
  function = FUNCTION_ADDR(RegExpMacroAssemblerARM::CheckStackGuardState);
#elif V8_TARGET_ARCH_PPC
  function = FUNCTION_ADDR(RegExpMacroAssemblerPPC::CheckStackGuardState);
#elif V8_TARGET_ARCH_MIPS
  function = FUNCTION_ADDR(RegExpMacroAssemblerMIPS::CheckStackGuardState);
#elif V8_TARGET_ARCH_MIPS64
  function = FUNCTION_ADDR(RegExpMacroAssemblerMIPS::CheckStackGuardState);
#elif V8_TARGET_ARCH_X87
  function = FUNCTION_ADDR(RegExpMacroAssemblerX87::CheckStackGuardState);
#else
  UNREACHABLE();
#endif
  return ExternalReference(Redirect(isolate, function));
}


ExternalReference ExternalReference::re_grow_stack(Isolate* isolate) {
  return ExternalReference(
      Redirect(isolate, FUNCTION_ADDR(NativeRegExpMacroAssembler::GrowStack)));
}

ExternalReference ExternalReference::re_case_insensitive_compare_uc16(
    Isolate* isolate) {
  return ExternalReference(Redirect(
      isolate,
      FUNCTION_ADDR(NativeRegExpMacroAssembler::CaseInsensitiveCompareUC16)));
}


ExternalReference ExternalReference::re_word_character_map() {
  return ExternalReference(
      NativeRegExpMacroAssembler::word_character_map_address());
}

ExternalReference ExternalReference::address_of_static_offsets_vector(
    Isolate* isolate) {
  return ExternalReference(
      reinterpret_cast<Address>(isolate->jsregexp_static_offsets_vector()));
}

ExternalReference ExternalReference::address_of_regexp_stack_memory_address(
    Isolate* isolate) {
  return ExternalReference(
      isolate->regexp_stack()->memory_address());
}

ExternalReference ExternalReference::address_of_regexp_stack_memory_size(
    Isolate* isolate) {
  return ExternalReference(isolate->regexp_stack()->memory_size_address());
}

#endif  // V8_INTERPRETED_REGEXP


ExternalReference ExternalReference::math_log_double_function(
    Isolate* isolate) {
  typedef double (*d2d)(double x);
  return ExternalReference(Redirect(isolate,
                                    FUNCTION_ADDR(static_cast<d2d>(std::log)),
                                    BUILTIN_FP_CALL));
}


ExternalReference ExternalReference::math_exp_constants(int constant_index) {
  DCHECK(math_exp_data_initialized);
  return ExternalReference(
      reinterpret_cast<void*>(math_exp_constants_array + constant_index));
}


ExternalReference ExternalReference::math_exp_log_table() {
  DCHECK(math_exp_data_initialized);
  return ExternalReference(reinterpret_cast<void*>(math_exp_log_table_array));
}


ExternalReference ExternalReference::page_flags(Page* page) {
  return ExternalReference(reinterpret_cast<Address>(page) +
                           MemoryChunk::kFlagsOffset);
}


ExternalReference ExternalReference::ForDeoptEntry(Address entry) {
  return ExternalReference(entry);
}


ExternalReference ExternalReference::cpu_features() {
  DCHECK(CpuFeatures::initialized_);
  return ExternalReference(&CpuFeatures::supported_);
}


ExternalReference ExternalReference::debug_is_active_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->is_active_address());
}


ExternalReference ExternalReference::debug_after_break_target_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->after_break_target_address());
}


ExternalReference
    ExternalReference::debug_restarter_frame_function_pointer_address(
        Isolate* isolate) {
  return ExternalReference(
      isolate->debug()->restarter_frame_function_pointer_address());
}


ExternalReference ExternalReference::vector_store_virtual_register(
    Isolate* isolate) {
  return ExternalReference(isolate->vector_store_virtual_register_address());
}


double power_helper(double x, double y) {
  int y_int = static_cast<int>(y);
  if (y == y_int) {
    return power_double_int(x, y_int);  // Returns 1 if exponent is 0.
  }
  if (y == 0.5) {
    return (std::isinf(x)) ? V8_INFINITY
                           : fast_sqrt(x + 0.0);  // Convert -0 to +0.
  }
  if (y == -0.5) {
    return (std::isinf(x)) ? 0 : 1.0 / fast_sqrt(x + 0.0);  // Convert -0 to +0.
  }
  return power_double_double(x, y);
}


// Helper function to compute x^y, where y is known to be an
// integer. Uses binary decomposition to limit the number of
// multiplications; see the discussion in "Hacker's Delight" by Henry
// S. Warren, Jr., figure 11-6, page 213.
double power_double_int(double x, int y) {
  double m = (y < 0) ? 1 / x : x;
  unsigned n = (y < 0) ? -y : y;
  double p = 1;
  while (n != 0) {
    if ((n & 1) != 0) p *= m;
    m *= m;
    if ((n & 2) != 0) p *= m;
    m *= m;
    n >>= 2;
  }
  return p;
}


double power_double_double(double x, double y) {
#if (defined(__MINGW64_VERSION_MAJOR) &&                              \
     (!defined(__MINGW64_VERSION_RC) || __MINGW64_VERSION_RC < 1)) || \
    defined(V8_OS_AIX)
  // MinGW64 and AIX have a custom implementation for pow.  This handles certain
  // special cases that are different.
  if ((x == 0.0 || std::isinf(x)) && y != 0.0 && std::isfinite(y)) {
    double f;
    double result = ((x == 0.0) ^ (y > 0)) ? V8_INFINITY : 0;
    /* retain sign if odd integer exponent */
    return ((std::modf(y, &f) == 0.0) && (static_cast<int64_t>(y) & 1))
               ? copysign(result, x)
               : result;
  }

  if (x == 2.0) {
    int y_int = static_cast<int>(y);
    if (y == y_int) {
      return std::ldexp(1.0, y_int);
    }
  }
#endif

  // The checks for special cases can be dropped in ia32 because it has already
  // been done in generated code before bailing out here.
  if (std::isnan(y) || ((x == 1 || x == -1) && std::isinf(y))) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::pow(x, y);
}


#ifdef SRUK_MATH_POW
//
// The following is adapted from fdlibm's (http://www.netlib.org/fdlibm)
// implementation of the C pow function.
//

#ifdef __thumb__
__attribute__((naked)) static double sruk_math_pow(double x, double y) {
  asm(".fpu neon");
  asm("push    {r4, r5, r6, r7, lr}");
  asm("sub     sp, sp, #84");
  asm("movs    r4, #0");
  asm("movt    r4, 16448");
  asm("strd    r2, [sp]");
  asm("movs    r3, #0");
  asm("strd    r0, [sp, #8]");
  asm("movt    r3, 16864");
  asm("ldr     r5, [sp, #12]");
  asm("ldr     r6, [sp, #4]");
  asm("bic     r2, r5, #-2147483648");
  asm("bic     r6, r6, #-2147483648");
  asm("cmp     r2, r3");
  asm("it      le");
  asm("cmple   r6, r4");
  asm("ite     le");
  asm("movle   r4, #0");
  asm("movgt   r4, #1");
  asm("bgt     .sruk_L41");
  asm("movw    r7, #65535");
  asm("movt    r7, 15887");
  asm("cmp     r5, r7");
  asm("ble     .sruk_L41");
  asm("ubfx    r5, r5, #0, #20");
  asm("movw    r7, #39054");
  asm("movt    r7, 3");
  asm("asrs    r2, r2, #20");
  asm("cmp     r5, r7");
  asm("orr     r3, r5, #1069547520");
  asm("strd    r0, [sp, #16]");
  asm("subw    ip, r2, #1023");
  asm("orr     r1, r3, #3145728");
  asm("it      le");
  asm("movle   r0, r4");
  asm("ble     .sruk_L44");
  asm("movw    r3, #46713");
  asm("movt    r3, 11");
  asm("cmp     r5, r3");
  asm("ittte   gt");
  asm("subwgt  ip, r2, #1022");
  asm("subgt   r1, r1, #1048576");
  asm("movgt   r0, r4");
  asm("movle   r4, #262144");
  asm("it      le");
  asm("movle   r0, #1");
  asm(".sruk_L44:");
  asm("ldr     r3, .sruk_L50+136");
  asm("lsls    r0, r0, #3");
  asm("str     r1, [sp, #20]");
  asm("fconstd d5, #112");
  asm(".sruk_LPIC2:");
  asm("add     r3, pc");
  asm("fldd    d25, [sp, #16]");
  asm("add     r3, r3, r0");
  asm("fldd    d27, [r3]");
  asm("fldd    d3, .sruk_L50");
  asm("fldd    d19, .sruk_L50+8");
  asm("fldd    d26, .sruk_L50+16");
  asm("asrs    r6, r1, #1");
  asm("orr     r6, r6, #536870912");
  asm("movs    r1, #0");
  asm("add     r6, r6, #524288");
  asm("movs    r2, #0");
  asm("add     r4, r4, r6");
  asm("movs    r3, #0");
  asm("strd    r2, [sp, #32]");
  asm("fldd    d24, .sruk_L50+24");
  asm("str     r4, [sp, #36]");
  asm("fldd    d22, .sruk_L50+32");
  asm("faddd   d16, d25, d27");
  asm("fldd    d4, [sp, #32]");
  asm("fldd    d21, .sruk_L50+40");
  asm("fldd    d6, .sruk_L50+48");
  asm("fldd    d7, .sruk_L50+56");
  asm("fldd    d31, .sruk_L50+64");
  asm("ldr     r6, .sruk_L50+140");
  asm("fldd    d20, [sp]");
  asm("ldr     r5, .sruk_L50+144");
  asm("fstd    d20, [sp, #64]");
  asm(".sruk_LPIC3:");
  asm("add     r6, pc");
  asm("str     r1, [sp, #64]");
  asm("add     r6, r6, r0");
  asm("fldd    d30, [r6]");
  asm(".sruk_LPIC4:");
  asm("add     r5, pc");
  asm("fldd    d28, [sp, #64]");
  asm("add     r0, r0, r5");
  asm("fldd    d29, [r0]");
  asm("mov     r4, r1");
  asm("movt    r4, 16352");
  asm("fdivd   d5, d5, d16");
  asm("fsubd   d23, d25, d27");
  asm("fmuld   d18, d23, d5");
  asm("fmuld   d16, d18, d18");
  asm("fstd    d18, [sp, #24]");
  asm("str     r1, [sp, #24]");
  asm("fldd    d17, [sp, #24]");
  asm("fmacd   d19, d16, d3");
  asm("fmacd   d26, d19, d16");
  asm("fnmacd  d23, d4, d17");
  asm("fsubd   d27, d4, d27");
  asm("fmacd   d24, d26, d16");
  asm("fsubd   d25, d25, d27");
  asm("fnmacd  d23, d25, d17");
  asm("fmacd   d22, d24, d16");
  asm("fmuld   d23, d23, d5");
  asm("fmsr    s11, ip @ int");
  asm("faddd   d19, d18, d17");
  asm("fmacd   d21, d22, d16");
  asm("fmuld   d19, d19, d23");
  asm("fmuld   d16, d16, d16");
  asm("fmuld   d24, d17, d17");
  asm("fmacd   d19, d16, d21");
  asm("fconstd d16, #8");
  asm("faddd   d21, d24, d16");
  asm("faddd   d21, d21, d19");
  asm("fstd    d21, [sp, #32]");
  asm("str     r1, [sp, #32]");
  asm("fldd    d21, [sp, #32]");
  asm("fsubd   d16, d21, d16");
  asm("fsubd   d24, d16, d24");
  asm("fsubd   d19, d19, d24");
  asm("fmuld   d18, d19, d18");
  asm("fmacd   d18, d23, d21");
  asm("fmuld   d17, d17, d21");
  asm("faddd   d16, d17, d18");
  asm("fstd    d16, [sp, #40]");
  asm("str     r1, [sp, #40]");
  asm("fldd    d19, [sp, #40]");
  asm("fsubd   d17, d19, d17");
  asm("fsubd   d18, d18, d17");
  asm("fmuld   d18, d18, d6");
  asm("fmacd   d18, d19, d7");
  asm("fmuld   d19, d19, d31");
  asm("faddd   d18, d18, d30");
  asm("faddd   d16, d19, d18");
  asm("faddd   d16, d16, d29");
  asm("fsitod  d22, s11");
  asm("faddd   d16, d22, d16");
  asm("fstd    d16, [sp, #56]");
  asm("str     r1, [sp, #56]");
  asm("fldd    d16, [sp, #56]");
  asm("fsubd   d22, d16, d22");
  asm("fsubd   d29, d22, d29");
  asm("fsubd   d19, d29, d19");
  asm("fsubd   d18, d18, d19");
  asm("fmuld   d17, d18, d20");
  asm("fsubd   d23, d20, d28");
  asm("fmacd   d17, d16, d23");
  asm("fmuld   d16, d16, d28");
  asm("faddd   d18, d17, d16");
  asm("fstd    d18, [sp, #72]");
  asm("fcpyd   d28, d16");
  asm("ldr     r0, [sp, #76]");
  asm("bic     r5, r0, #-2147483648");
  asm("cmp     r5, r4");
  asm("ble     .sruk_L45");
  asm("asrs    r5, r5, #20");
  asm("mov     r4, #1048576");
  asm("subw    r5, r5, #1022");
  asm("strd    r2, [sp, #48]");
  asm("asrs    r4, r4, r5");
  asm("movw    r6, #65535");
  asm("add     r4, r4, r0");
  asm("movt    r6, 15");
  asm("ubfx    r3, r4, #20, #11");
  asm("ubfx    r1, r4, #0, #20");
  asm("subw    r2, r3, #1023");
  asm("orr     r1, r1, #1048576");
  asm("asrs    r6, r6, r2");
  asm("rsb     r3, r3, #1040");
  asm("bic     r4, r4, r6");
  asm("str     r4, [sp, #52]");
  asm("fldd    d28, [sp, #48]");
  asm("fsubd   d28, d16, d28");
  asm("adds    r3, r3, #3");
  asm("cmp     r0, #0");
  asm("asr     r1, r1, r3");
  asm("it      lt");
  asm("rsblt   r1, r1, #0");
  asm("lsls    r1, r1, #20");
  asm("faddd   d18, d17, d28");
  asm(".sruk_L45:");
  asm("fstd    d18, [sp, #48]");
  asm("movs    r2, #0");
  asm("str     r2, [sp, #48]");
  asm("fldd    d18, .sruk_L50+72");
  asm("fldd    d16, [sp, #48]");
  asm("fldd    d25, .sruk_L50+80");
  asm("fldd    d23, .sruk_L50+88");
  asm("fldd    d24, .sruk_L50+96");
  asm("fldd    d22, .sruk_L50+104");
  asm("fldd    d21, .sruk_L50+112");
  asm("fldd    d20, .sruk_L50+120");
  asm("fldd    d19, .sruk_L50+128");
  asm("fsubd   d28, d16, d28");
  asm("fmuld   d18, d16, d18");
  asm("fsubd   d17, d17, d28");
  asm("fmacd   d18, d17, d25");
  asm("fmuld   d23, d16, d23");
  asm("faddd   d16, d23, d18");
  asm("fmuld   d17, d16, d16");
  asm("fmscd   d22, d17, d24");
  asm("fmacd   d21, d17, d22");
  asm("fmscd   d20, d17, d21");
  asm("fmacd   d19, d17, d20");
  asm("fcpyd   d20, d16");
  asm("fnmacd  d20, d17, d19");
  asm("fcpyd   d19, d20");
  asm("fconstd d20, #0");
  asm("fmuld   d17, d16, d19");
  asm("fsubd   d19, d19, d20");
  asm("fdivd   d19, d17, d19");
  asm("fsubd   d17, d16, d23");
  asm("fsubd   d17, d18, d17");
  asm("fmacd   d17, d16, d17");
  asm("fsubd   d17, d19, d17");
  asm("fconstd d18, #112");
  asm("fsubd   d16, d17, d16");
  asm("fsubd   d16, d18, d16");
  asm("fstd    d16, [sp, #72]");
  asm("ldr     r3, [sp, #76]");
  asm("add     r3, r3, r1");
  asm("str     r3, [sp, #76]");
  asm("ldrd    r0, [sp, #72]");
  asm("add     sp, sp, #84");
  asm("pop     {r4, r5, r6, r7, pc}");
  asm(".sruk_L41:");
  asm("ldrd    r0, [sp, #8]");
  asm("ldrd    r2, [sp]");
  asm("bl      _ZN2v88internal19power_double_doubleEdd(PLT)");
  asm("add     sp, sp, #84");
  asm("pop     {r4, r5, r6, r7, pc}");

  asm(".align  3");
  asm(".sruk_L50:");
  asm(".word   1246056175");
  asm(".word   1070235176");
  asm(".word   2479479653");
  asm(".word   1070433866");
  asm(".word   2837266689");
  asm(".word   1070691424");
  asm(".word   1368335949");
  asm(".word   1070945621");
  asm(".word   3681528831");
  asm(".word   1071345078");
  asm(".word   858993411");
  asm(".word   1071854387");
  asm(".word   3694789629");
  asm(".word   1072613129");
  asm(".word   341508597");
  asm(".word   -1103220768");
  asm(".word   3758096384");
  asm(".word   1072613129");
  asm(".word   212364345");
  asm(".word   -1105175455");
  asm(".word   4277811695");
  asm(".word   1072049730");
  asm(".word   0");
  asm(".word   1072049731");
  asm(".word   1925096656");
  asm(".word   1046886249");
  asm(".word   3318901745");
  asm(".word   1052491073");
  asm(".word   2938494508");
  asm(".word   1058100842");
  asm(".word   381599123");
  asm(".word   1063698796");
  asm(".word   1431655742");
  asm(".word   1069897045");
  asm(".word   .sruk_LANCHOR0-(.sruk_LPIC2+4)");
  asm(".word   .sruk_LANCHOR1-(.sruk_LPIC3+4)");
  asm(".word   .sruk_LANCHOR2-(.sruk_LPIC4+4)");

  asm(".align  3");
  asm(".sruk_LANCHOR0 = . + 0");
  asm(".word   0");
  asm(".word   1072693248");
  asm(".word   0");
  asm(".word   1073217536");
  asm(".sruk_LANCHOR1 = . + 0");
  asm(".word   0");
  asm(".word   0");
  asm(".word   1137692678");
  asm(".word   1045233131");
  asm(".sruk_LANCHOR2 = . + 0");
  asm(".word   0");
  asm(".word   0");
  asm(".word   1073741824");
  asm(".word   1071822851");
}
#else

#define HI(x) *(1 + reinterpret_cast<int*>(&x))
#define LO(x) *reinterpret_cast<int*>(&x)

static const double bp[] = {1.0, 1.5};
static const double dp_h[] = {0.0, 5.84962487220764160156e-01};
static const double dp_l[] = {0.0, 1.35003920212974897128e-08};
static const double L1 =  5.99999999999994648725e-01;
static const double L2 =  4.28571428578550184252e-01;
static const double L3 =  3.33333329818377432918e-01;
static const double L4 =  2.72728123808534006489e-01;
static const double L5 =  2.30660745775561754067e-01;
static const double L6 =  2.06975017800338417784e-01;
static const double P1 =  1.66666666666666019037e-01;
static const double P2 = -2.77777777770155933842e-03;
static const double P3 =  6.61375632143793436117e-05;
static const double P4 = -1.65339022054652515390e-06;
static const double P5 =  4.13813679705723846039e-08;
static const double lg2 = 6.93147180559945286227e-01;
static const double lg2_h = 6.93147182464599609375e-01;
static const double lg2_l = -1.90465429995776804525e-09;
static const double cp_ = 9.61796693925975554329e-01;
static const double cp_h = 9.61796700954437255859e-01;
static const double cp_l = -7.02846165095275826516e-09;

// Returns Math.pow(x, y). This function is called by MathPowStub when y isn't
// an integer (or when it is an integer but doesn't fit in an 'int').
static double sruk_math_pow(double x, double y) {
  DCHECK(static_cast<int>(y) != y);

  int hx = HI(x);
  int hy = HI(y);

  int ix = hx & 0x7fffffff;
  int iy = hy & 0x7fffffff;

  // Avoid complicated cases (underflow, overflow and subnormal values).
  if (iy > 0x40400000 ||  // |y| > 32 or y is NaN.
      ix > 0x41e00000 ||  // |x| > (2 ^ 31) or x is NaN.
      hx < 0x3e100000) {  // x < 1 / (2 ^ 30).
    return power_double_double(x, y);
  }

  DCHECK(static_cast<long long>(y) != y);

  //
  // The rest is fdlibm's algorithm, but we have removed the handling of
  // complicated cases (see above).
  //

  double ax = x;

  int n = ((ix) >> 20) - 0x3ff;
  int j = ix & 0x000fffff;
  ix = j | 0x3ff00000;
  int k;
  if (j <= 0x3988e) {
    k = 0;
  } else if (j < 0xbb67a) {
    k = 1;
  } else {
    k = 0;
    n += 1;
    ix -= 0x00100000;
  }
  HI(ax) = ix;

  double u = ax - bp[k];
  double v = 1.0 / (ax + bp[k]);
  double ss = u * v;
  double s_h = ss;
  LO(s_h) = 0;
  double t_h = 0.0;
  HI(t_h) = ((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18);
  double t_l = ax - (t_h - bp[k]);
  double s_l = v * ((u - s_h * t_h) - s_h * t_l);
  double s2 = ss * ss;
  double r = s2 * s2 * (L1 + s2 * (L2 +s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
  r += s_l * (s_h + ss);
  s2  = s_h * s_h;
  t_h = 3.0 + s2 + r;
  LO(t_h) = 0;
  t_l = r - ((t_h - 3.0) - s2);
  u = s_h * t_h;
  v = s_l * t_h + t_l * ss;
  double p_h = u + v;
  LO(p_h) = 0;
  double p_l = v - (p_h - u);
  double z_h = cp_h * p_h;
  double z_l = cp_l * p_h + p_l * cp_ + dp_l[k];
  double t = static_cast<double>(n);
  double t1 = (((z_h + z_l) + dp_h[k]) + t);
  LO(t1) = 0;
  double t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);

  double y1 = y;
  LO(y1) = 0;
  p_l = (y - y1) * t1 + y * t2;
  p_h = y1 * t1;
  double z = p_l + p_h;
  j = HI(z);

  int i = j & 0x7fffffff;
  k = (i >> 20) - 0x3ff;
  n = 0;
  if (i > 0x3fe00000) {
    n = j + (0x00100000 >> (k + 1));
    k = ((n & 0x7fffffff) >> 20) - 0x3ff;
    t = 0.0;
    HI(t) = (n & ~(0x000fffff >> k));
    n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
    if (j < 0)
      n = -n;
    p_h -= t;
  }
  t = p_l + p_h;
  LO(t) = 0;
  u = t * lg2_h;
  v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
  z = u + v;
  double w = v - (z - u);
  t  = z * z;
  t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  r = (z * t1) / (t1 - 2.0) - (w + z * w);
  z = 1.0 - (r - z);
  HI(z) += (n << 20);
  return z;
}
#endif
#endif


ExternalReference ExternalReference::power_double_double_function(
    Isolate* isolate) {
  return ExternalReference(Redirect(isolate,
                                    FUNCTION_ADDR(power_double_double),
                                    BUILTIN_FP_FP_CALL));
}


#ifdef SRUK_MATH_POW
ExternalReference ExternalReference::power_double_double_function_2(
    Isolate* isolate) {
  return ExternalReference(Redirect(isolate,
                                    FUNCTION_ADDR(sruk_math_pow),
                                    BUILTIN_FP_FP_CALL));
}
#endif


ExternalReference ExternalReference::power_double_int_function(
    Isolate* isolate) {
  return ExternalReference(Redirect(isolate,
                                    FUNCTION_ADDR(power_double_int),
                                    BUILTIN_FP_INT_CALL));
}


bool EvalComparison(Token::Value op, double op1, double op2) {
  DCHECK(Token::IsCompareOp(op));
  switch (op) {
    case Token::EQ:
    case Token::EQ_STRICT: return (op1 == op2);
    case Token::NE: return (op1 != op2);
    case Token::LT: return (op1 < op2);
    case Token::GT: return (op1 > op2);
    case Token::LTE: return (op1 <= op2);
    case Token::GTE: return (op1 >= op2);
    default:
      UNREACHABLE();
      return false;
  }
}


ExternalReference ExternalReference::mod_two_doubles_operation(
    Isolate* isolate) {
  return ExternalReference(Redirect(isolate,
                                    FUNCTION_ADDR(modulo),
                                    BUILTIN_FP_FP_CALL));
}


ExternalReference ExternalReference::debug_step_in_fp_address(
    Isolate* isolate) {
  return ExternalReference(isolate->debug()->step_in_fp_addr());
}


ExternalReference ExternalReference::fixed_typed_array_base_data_offset() {
  return ExternalReference(reinterpret_cast<void*>(
      FixedTypedArrayBase::kDataOffset - kHeapObjectTag));
}


bool operator==(ExternalReference lhs, ExternalReference rhs) {
  return lhs.address() == rhs.address();
}


bool operator!=(ExternalReference lhs, ExternalReference rhs) {
  return !(lhs == rhs);
}


size_t hash_value(ExternalReference reference) {
  return base::hash<Address>()(reference.address());
}


std::ostream& operator<<(std::ostream& os, ExternalReference reference) {
  os << static_cast<const void*>(reference.address());
  const Runtime::Function* fn = Runtime::FunctionForEntry(reference.address());
  if (fn) os << "<" << fn->name << ".entry>";
  return os;
}


void PositionsRecorder::RecordPosition(int pos) {
  DCHECK(pos != RelocInfo::kNoPosition);
  DCHECK(pos >= 0);
  state_.current_position = pos;
  LOG_CODE_EVENT(assembler_->isolate(),
                 CodeLinePosInfoAddPositionEvent(jit_handler_data_,
                                                 assembler_->pc_offset(),
                                                 pos));
}


void PositionsRecorder::RecordStatementPosition(int pos) {
  DCHECK(pos != RelocInfo::kNoPosition);
  DCHECK(pos >= 0);
  state_.current_statement_position = pos;
  LOG_CODE_EVENT(assembler_->isolate(),
                 CodeLinePosInfoAddStatementPositionEvent(
                     jit_handler_data_,
                     assembler_->pc_offset(),
                     pos));
}


bool PositionsRecorder::WriteRecordedPositions() {
  bool written = false;

  // Write the statement position if it is different from what was written last
  // time.
  if (state_.current_statement_position != state_.written_statement_position) {
    EnsureSpace ensure_space(assembler_);
    assembler_->RecordRelocInfo(RelocInfo::STATEMENT_POSITION,
                                state_.current_statement_position);
    state_.written_position = state_.current_statement_position;
    state_.written_statement_position = state_.current_statement_position;
    written = true;
  }

  // Write the position if it is different from what was written last time and
  // also different from the statement position that was just written.
  if (state_.current_position != state_.written_position) {
    EnsureSpace ensure_space(assembler_);
    assembler_->RecordRelocInfo(RelocInfo::POSITION, state_.current_position);
    state_.written_position = state_.current_position;
    written = true;
  }

  // Return whether something was written.
  return written;
}


ConstantPoolBuilder::ConstantPoolBuilder(int ptr_reach_bits,
                                         int double_reach_bits) {
  info_[ConstantPoolEntry::INTPTR].entries.reserve(64);
  info_[ConstantPoolEntry::INTPTR].regular_reach_bits = ptr_reach_bits;
  info_[ConstantPoolEntry::DOUBLE].regular_reach_bits = double_reach_bits;
}


ConstantPoolEntry::Access ConstantPoolBuilder::NextAccess(
    ConstantPoolEntry::Type type) const {
  const PerTypeEntryInfo& info = info_[type];

  if (info.overflow()) return ConstantPoolEntry::OVERFLOWED;

  int dbl_count = info_[ConstantPoolEntry::DOUBLE].regular_count;
  int dbl_offset = dbl_count * kDoubleSize;
  int ptr_count = info_[ConstantPoolEntry::INTPTR].regular_count;
  int ptr_offset = ptr_count * kPointerSize + dbl_offset;

  if (type == ConstantPoolEntry::DOUBLE) {
    // Double overflow detection must take into account the reach for both types
    int ptr_reach_bits = info_[ConstantPoolEntry::INTPTR].regular_reach_bits;
    if (!is_uintn(dbl_offset, info.regular_reach_bits) ||
        (ptr_count > 0 &&
         !is_uintn(ptr_offset + kDoubleSize - kPointerSize, ptr_reach_bits))) {
      return ConstantPoolEntry::OVERFLOWED;
    }
  } else {
    DCHECK(type == ConstantPoolEntry::INTPTR);
    if (!is_uintn(ptr_offset, info.regular_reach_bits)) {
      return ConstantPoolEntry::OVERFLOWED;
    }
  }

  return ConstantPoolEntry::REGULAR;
}


ConstantPoolEntry::Access ConstantPoolBuilder::AddEntry(
    ConstantPoolEntry& entry, ConstantPoolEntry::Type type) {
  DCHECK(!emitted_label_.is_bound());
  PerTypeEntryInfo& info = info_[type];
  const int entry_size = ConstantPoolEntry::size(type);
  bool merged = false;

  if (entry.sharing_ok()) {
    // Try to merge entries
    std::vector<ConstantPoolEntry>::iterator it = info.shared_entries.begin();
    int end = static_cast<int>(info.shared_entries.size());
    for (int i = 0; i < end; i++, it++) {
      if ((entry_size == kPointerSize) ? entry.value() == it->value()
                                       : entry.value64() == it->value64()) {
        // Merge with found entry.
        entry.set_merged_index(i);
        merged = true;
        break;
      }
    }
  }

  // By definition, merged entries have regular access.
  DCHECK(!merged || entry.merged_index() < info.regular_count);
  ConstantPoolEntry::Access access =
      (merged ? ConstantPoolEntry::REGULAR : NextAccess(type));

  // Enforce an upper bound on search time by limiting the search to
  // unique sharable entries which fit in the regular section.
  if (entry.sharing_ok() && !merged && access == ConstantPoolEntry::REGULAR) {
    info.shared_entries.push_back(entry);
  } else {
    info.entries.push_back(entry);
  }

  // We're done if we found a match or have already triggered the
  // overflow state.
  if (merged || info.overflow()) return access;

  if (access == ConstantPoolEntry::REGULAR) {
    info.regular_count++;
  } else {
    info.overflow_start = static_cast<int>(info.entries.size()) - 1;
  }

  return access;
}


void ConstantPoolBuilder::EmitSharedEntries(Assembler* assm,
                                            ConstantPoolEntry::Type type) {
  PerTypeEntryInfo& info = info_[type];
  std::vector<ConstantPoolEntry>& shared_entries = info.shared_entries;
  const int entry_size = ConstantPoolEntry::size(type);
  int base = emitted_label_.pos();
  DCHECK(base > 0);
  int shared_end = static_cast<int>(shared_entries.size());
  std::vector<ConstantPoolEntry>::iterator shared_it = shared_entries.begin();
  for (int i = 0; i < shared_end; i++, shared_it++) {
    int offset = assm->pc_offset() - base;
    shared_it->set_offset(offset);  // Save offset for merged entries.
    if (entry_size == kPointerSize) {
      assm->dp(shared_it->value());
    } else {
      assm->dq(shared_it->value64());
    }
    DCHECK(is_uintn(offset, info.regular_reach_bits));

    // Patch load sequence with correct offset.
    assm->PatchConstantPoolAccessInstruction(shared_it->position(), offset,
                                             ConstantPoolEntry::REGULAR, type);
  }
}


void ConstantPoolBuilder::EmitGroup(Assembler* assm,
                                    ConstantPoolEntry::Access access,
                                    ConstantPoolEntry::Type type) {
  PerTypeEntryInfo& info = info_[type];
  const bool overflow = info.overflow();
  std::vector<ConstantPoolEntry>& entries = info.entries;
  std::vector<ConstantPoolEntry>& shared_entries = info.shared_entries;
  const int entry_size = ConstantPoolEntry::size(type);
  int base = emitted_label_.pos();
  DCHECK(base > 0);
  int begin;
  int end;

  if (access == ConstantPoolEntry::REGULAR) {
    // Emit any shared entries first
    EmitSharedEntries(assm, type);
  }

  if (access == ConstantPoolEntry::REGULAR) {
    begin = 0;
    end = overflow ? info.overflow_start : static_cast<int>(entries.size());
  } else {
    DCHECK(access == ConstantPoolEntry::OVERFLOWED);
    if (!overflow) return;
    begin = info.overflow_start;
    end = static_cast<int>(entries.size());
  }

  std::vector<ConstantPoolEntry>::iterator it = entries.begin();
  if (begin > 0) std::advance(it, begin);
  for (int i = begin; i < end; i++, it++) {
    // Update constant pool if necessary and get the entry's offset.
    int offset;
    ConstantPoolEntry::Access entry_access;
    if (!it->is_merged()) {
      // Emit new entry
      offset = assm->pc_offset() - base;
      entry_access = access;
      if (entry_size == kPointerSize) {
        assm->dp(it->value());
      } else {
        assm->dq(it->value64());
      }
    } else {
      // Retrieve offset from shared entry.
      offset = shared_entries[it->merged_index()].offset();
      entry_access = ConstantPoolEntry::REGULAR;
    }

    DCHECK(entry_access == ConstantPoolEntry::OVERFLOWED ||
           is_uintn(offset, info.regular_reach_bits));

    // Patch load sequence with correct offset.
    assm->PatchConstantPoolAccessInstruction(it->position(), offset,
                                             entry_access, type);
  }
}


// Emit and return position of pool.  Zero implies no constant pool.
int ConstantPoolBuilder::Emit(Assembler* assm) {
  bool emitted = emitted_label_.is_bound();
  bool empty = IsEmpty();

  if (!emitted) {
    // Mark start of constant pool.  Align if necessary.
    if (!empty) assm->DataAlign(kDoubleSize);
    assm->bind(&emitted_label_);
    if (!empty) {
      // Emit in groups based on access and type.
      // Emit doubles first for alignment purposes.
      EmitGroup(assm, ConstantPoolEntry::REGULAR, ConstantPoolEntry::DOUBLE);
      EmitGroup(assm, ConstantPoolEntry::REGULAR, ConstantPoolEntry::INTPTR);
      if (info_[ConstantPoolEntry::DOUBLE].overflow()) {
        assm->DataAlign(kDoubleSize);
        EmitGroup(assm, ConstantPoolEntry::OVERFLOWED,
                  ConstantPoolEntry::DOUBLE);
      }
      if (info_[ConstantPoolEntry::INTPTR].overflow()) {
        EmitGroup(assm, ConstantPoolEntry::OVERFLOWED,
                  ConstantPoolEntry::INTPTR);
      }
    }
  }

  return !empty ? emitted_label_.pos() : 0;
}


// Platform specific but identical code for all the platforms.


void Assembler::RecordDeoptReason(const int reason,
                                  const SourcePosition position) {
  if (FLAG_trace_deopt || isolate()->cpu_profiler()->is_profiling()) {
    EnsureSpace ensure_space(this);
    int raw_position = position.IsUnknown() ? 0 : position.raw();
    RecordRelocInfo(RelocInfo::POSITION, raw_position);
    RecordRelocInfo(RelocInfo::DEOPT_REASON, reason);
  }
}


void Assembler::RecordComment(const char* msg) {
  if (FLAG_code_comments) {
    EnsureSpace ensure_space(this);
    RecordRelocInfo(RelocInfo::COMMENT, reinterpret_cast<intptr_t>(msg));
  }
}


void Assembler::RecordGeneratorContinuation() {
  EnsureSpace ensure_space(this);
  RecordRelocInfo(RelocInfo::GENERATOR_CONTINUATION);
}


void Assembler::RecordDebugBreakSlot(RelocInfo::Mode mode, int call_argc) {
  EnsureSpace ensure_space(this);
  DCHECK(RelocInfo::IsDebugBreakSlot(mode));
  intptr_t data = static_cast<intptr_t>(call_argc);
  RecordRelocInfo(mode, data);
}


void Assembler::DataAlign(int m) {
  DCHECK(m >= 2 && base::bits::IsPowerOfTwo32(m));
  while ((pc_offset() & (m - 1)) != 0) {
    db(0);
  }
}
}  // namespace internal
}  // namespace v8
