// The canonical architectural event record and its byte-exact encoding.
//
// This is the one serialiser for the V-008 frozen interface
// (config/contracts/event_v1.json). The harness, the bring-up tap's host side and
// any later differential runner must go through it, so there is exactly one
// definition of what an event is and exactly one byte layout.
//
// ------------------------------------------------------------------ the shape
//
// * `EventRecord` is one architectural event: one hart, one instruction (or one
//   memory visibility of one previously retired store). A two-wide retirement is
//   two records, each built from its own lane's ports -- the type has no "the
//   cycle's snapshot" member, so a shared snapshot is not representable.
// * `MOSAIC_EVENT_FIELDS` is the field list, in encoding order, with the width of
//   every field in bits. It is the single source of truth inside the C++: the
//   struct members, the field-index enum, the offset table, the encoder, the
//   decoder and the text form are all expansions of it. A field added here and
//   not to the schema (or the other way round) is rejected by
//   `tools/check_event_contract.py`, which is wired into `make check`.
// * Widths here are the frozen wire widths. The checker evaluates the schema's
//   width expression against the profile geometry and against the RTL's own
//   derived widths, so a geometry change that would truncate a field fails the
//   check instead of silently narrowing the record.
//
// ------------------------------------------------------------ the version rule
//
// The schema version is checked, not merely stored. `EncodeEvent` takes the
// version the caller's stream claims (`producer_schema_version`); if it is not
// the version this codec was compiled against the encoder refuses and writes
// nothing. A decoder applies the same rule to the record header. The checker
// proves the compiled constant equals config/contracts/event_v1.json's
// `schema_version`, so "the version I was compiled against" is the frozen schema
// and not a number someone typed twice.
//
// ------------------------------------------------------- what this file is not
//
// No dynamic allocation and no iostream anywhere in the codec: the encoder takes
// a caller-owned buffer, the decoder takes a caller-owned record, and the text
// form writes into a caller-owned char buffer. A trace path that allocates is a
// trace path that can change the timing of the thing it is measuring.

#ifndef MOSAIC_EVENT_CODEC_H_
#define MOSAIC_EVENT_CODEC_H_

#include <cstddef>
#include <cstdint>

namespace mosaic {

// The event kinds, with the frozen values. A kind's value is part of the wire
// format, so it lives in an X-macro next to the field list rather than in the
// codec's .cpp, and the checker compares it with the schema's table.
#define MOSAIC_EVENT_KINDS(K) \
  K(RETIRE, 1)                \
  K(TRAP, 2)                  \
  K(MEM_VISIBLE, 3)

// The record's fields, in encoding order, with the width of each in bits.
#define MOSAIC_EVENT_FIELDS(X) \
  X(retire_seq, 8)             \
  X(hart_id, 1)                \
  X(cycle, 64)                 \
  X(kind, 2)                   \
  X(pc_before, 64)             \
  X(pc_after, 64)              \
  X(insn_bits, 32)             \
  X(rob_id, 14)                \
  X(gpr_write_valid, 1)        \
  X(rd, 5)                     \
  X(rd_value, 64)              \
  X(csr_write_valid, 1)        \
  X(csr_addr, 12)              \
  X(csr_value, 64)             \
  X(mem_is_store, 1)           \
  X(mem_addr, 64)              \
  X(mem_data, 64)              \
  X(mem_size, 3)               \
  X(trap_cause, 64)            \
  X(trap_tval, 64)             \
  X(trap_epc, 64)

// The version this codec was compiled against. tools/check_event_contract.py
// requires it to equal config/contracts/event_v1.json's `schema_version`.
#define MOSAIC_EVENT_SCHEMA_VERSION 1

// One C++ type per field width. The primary template is deliberately undefined:
// a width that has no representation here is a compile error, not a silent
// 64-bit container.
template <unsigned Bits>
struct EventFieldType;
template <>
struct EventFieldType<1> {
  using type = uint8_t;
};
template <>
struct EventFieldType<2> {
  using type = uint8_t;
};
template <>
struct EventFieldType<3> {
  using type = uint8_t;
};
template <>
struct EventFieldType<5> {
  using type = uint8_t;
};
template <>
struct EventFieldType<8> {
  using type = uint8_t;
};
template <>
struct EventFieldType<12> {
  using type = uint16_t;
};
template <>
struct EventFieldType<14> {
  using type = uint16_t;
};
template <>
struct EventFieldType<32> {
  using type = uint32_t;
};
template <>
struct EventFieldType<64> {
  using type = uint64_t;
};

// The kind values as named constants, expanded from MOSAIC_EVENT_KINDS.
enum EventKind {
#define MOSAIC_EVENT_KIND_DEFINE(name, value) kEventKind##name = value,
  MOSAIC_EVENT_KINDS(MOSAIC_EVENT_KIND_DEFINE)
#undef MOSAIC_EVENT_KIND_DEFINE
};

// One architectural event. Every member is one field of
// config/contracts/event_v1.json, in the schema's encoding order.
struct EventRecord {
#define MOSAIC_EVENT_MEMBER(name, bits) EventFieldType<bits>::type name;
  MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_MEMBER)
#undef MOSAIC_EVENT_MEMBER
};

// Field indices, in encoding order, for code that iterates rather than names.
enum EventField {
#define MOSAIC_EVENT_FIELD_ENUM(name, bits) kEventField_##name,
  MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_FIELD_ENUM)
#undef MOSAIC_EVENT_FIELD_ENUM
  kEventFieldCount
};

namespace event_detail {

constexpr uint16_t OctetsOf(uint16_t bits) {
  return static_cast<uint16_t>((bits + 7u) / 8u);
}

// The field widths as a table, so the offset of field N is the sum of the
// octet widths of fields 0..N-1 and the record size is the sum of all of them.
// Computed here once rather than written down twice.
constexpr uint8_t kFieldBits[] = {
#define MOSAIC_EVENT_BITS_TABLE(name, bits) static_cast<uint8_t>(bits),
    MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_BITS_TABLE)
#undef MOSAIC_EVENT_BITS_TABLE
};

constexpr uint16_t kFieldCount = static_cast<uint16_t>(sizeof(kFieldBits));

constexpr uint16_t OffsetOf(uint16_t index) {
  uint16_t offset = 0;
  for (uint16_t i = 0; i < index && i < kFieldCount; ++i) {
    offset = static_cast<uint16_t>(offset + OctetsOf(kFieldBits[i]));
  }
  return offset;
}

constexpr uint16_t kSchemaHeaderOctets = 4;
constexpr uint16_t kRecordOctets =
    static_cast<uint16_t>(kSchemaHeaderOctets + OffsetOf(kFieldCount));

}  // namespace event_detail

// The frozen record size in bytes: the 4-byte schema-version header plus every
// field. The checker recomputes it from the schema's own offsets.
constexpr size_t kEventRecordBytes = event_detail::kRecordOctets;

// ------------------------------------------------------------------ accessors

uint32_t EventSchemaVersion();
size_t EventRecordBytes();
size_t EventFieldCount();
const char* EventFieldName(size_t index);
unsigned EventFieldBits(size_t index);
size_t EventFieldOffset(size_t index);

// The field's value widened to 64 bits; undefined index yields 0.
uint64_t EventFieldValue(const EventRecord& record, size_t index);

// The kind's name ("RETIRE", "TRAP", "MEM_VISIBLE"), or "?" for an unknown
// value. Never null.
const char* EventKindName(unsigned kind);

// ------------------------------------------------------------------- encoding

enum class EncodeStatus {
  kOk,
  kVersionMismatch,  // producer_schema_version != the version compiled in
  kBufferTooSmall,   // capacity < kEventRecordBytes
  kFieldOverflow,    // a value has bits set above its field width
};

const char* EncodeStatusName(EncodeStatus status);

// Writes one record, little-endian, header then fields in declaration order.
// `producer_schema_version` is the version the caller's stream claims; a
// mismatch refuses to encode and writes nothing. On success exactly
// kEventRecordBytes bytes are written and out_written is set to that value.
EncodeStatus EncodeEvent(const EventRecord& record,
                         uint32_t producer_schema_version, uint8_t* out,
                         size_t capacity, size_t* out_written);

// The inverse. Rejects a record whose header is not
// `expected_schema_version`, a buffer shorter than kEventRecordBytes, and a
// field whose high bits are set beyond its width's container (which a
// well-formed encoder cannot produce).
EncodeStatus DecodeEvent(const uint8_t* in, size_t length,
                         uint32_t expected_schema_version, EventRecord* out);

// The canonical text form: `name=value` for every field in declaration order,
// each value hexadecimal with 2*ceil(bits/8) digits, no trailing newline.
// Returns the number of characters written, or 0 if capacity is too small or
// the record is invalid. Deterministic: the same record always formats the same.
size_t FormatEvent(const EventRecord& record, char* out, size_t capacity);

}  // namespace mosaic

#endif  // MOSAIC_EVENT_CODEC_H_
