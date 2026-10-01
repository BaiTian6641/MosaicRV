#include "event_codec.h"

#include <cstdio>
#include <cstring>

namespace mosaic {

namespace {

struct FieldInfo {
  const char* name;
  uint16_t bits;
};

// The field table, expanded from the same X-macro the struct is. There is no
// second copy of the field list anywhere in the C++.
const FieldInfo kFields[] = {
#define MOSAIC_EVENT_FIELD_INFO(name, bits) {#name, static_cast<uint16_t>(bits)},
    MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_FIELD_INFO)
#undef MOSAIC_EVENT_FIELD_INFO
};

const uint16_t kFieldCount = static_cast<uint16_t>(sizeof(kFields) / sizeof(kFields[0]));

// A record field is written as ceil(bits/8) little-endian bytes; the top bits of
// the top byte are zero because a value wider than its field is refused.
uint16_t OctetsOf(uint16_t bits) { return static_cast<uint16_t>((bits + 7u) / 8u); }

bool ValueFits(uint64_t value, uint16_t bits) {
  if (bits >= 64) return true;
  return (value >> bits) == 0;
}

// The value of field `index`, widened to 64 bits. The switch is generated from
// the field list, so a field added to the list but forgotten here cannot compile.
uint64_t ReadField(const EventRecord& record, uint16_t index) {
  switch (index) {
#define MOSAIC_EVENT_FIELD_READ(name, bits) \
  case kEventField_##name:                  \
    return static_cast<uint64_t>(record.name);
    MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_FIELD_READ)
#undef MOSAIC_EVENT_FIELD_READ
    default:
      return 0;
  }
}

void WriteField(EventRecord* record, uint16_t index, uint64_t value) {
  switch (index) {
#define MOSAIC_EVENT_FIELD_WRITE(name, bits)                                       \
  case kEventField_##name:                                                         \
    record->name = static_cast<EventFieldType<bits>::type>(value);                 \
    return;
    MOSAIC_EVENT_FIELDS(MOSAIC_EVENT_FIELD_WRITE)
#undef MOSAIC_EVENT_FIELD_WRITE
    default:
      return;
  }
}

struct KindInfo {
  unsigned value;
  const char* name;
};

const KindInfo kKinds[] = {
#define MOSAIC_EVENT_KIND_INFO(name, value) {value, #name},
    MOSAIC_EVENT_KINDS(MOSAIC_EVENT_KIND_INFO)
#undef MOSAIC_EVENT_KIND_INFO
};

}  // namespace

uint32_t EventSchemaVersion() {
  return static_cast<uint32_t>(MOSAIC_EVENT_SCHEMA_VERSION);
}

size_t EventRecordBytes() { return static_cast<size_t>(event_detail::kRecordOctets); }

size_t EventFieldCount() { return static_cast<size_t>(kFieldCount); }

const char* EventFieldName(size_t index) {
  return index < kFieldCount ? kFields[index].name : "?";
}

unsigned EventFieldBits(size_t index) {
  return index < kFieldCount ? kFields[index].bits : 0u;
}

size_t EventFieldOffset(size_t index) {
  if (index > kFieldCount) return event_detail::kRecordOctets;
  return static_cast<size_t>(event_detail::kSchemaHeaderOctets) +
         event_detail::OffsetOf(static_cast<uint16_t>(index));
}

uint64_t EventFieldValue(const EventRecord& record, size_t index) {
  return index < kFieldCount ? ReadField(record, static_cast<uint16_t>(index)) : 0u;
}

const char* EventKindName(unsigned kind) {
  for (size_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); ++i) {
    if (kKinds[i].value == kind) return kKinds[i].name;
  }
  return "?";
}

const char* EncodeStatusName(EncodeStatus status) {
  switch (status) {
    case EncodeStatus::kOk:
      return "ok";
    case EncodeStatus::kVersionMismatch:
      return "schema-version-mismatch";
    case EncodeStatus::kBufferTooSmall:
      return "buffer-too-small";
    case EncodeStatus::kFieldOverflow:
      return "field-overflow";
  }
  return "?";
}

EncodeStatus EncodeEvent(const EventRecord& record,
                         uint32_t producer_schema_version, uint8_t* out,
                         size_t capacity, size_t* out_written) {
  if (out_written != nullptr) *out_written = 0;
  if (producer_schema_version != EventSchemaVersion()) {
    return EncodeStatus::kVersionMismatch;
  }
  if (out == nullptr || capacity < kEventRecordBytes) {
    return EncodeStatus::kBufferTooSmall;
  }
  for (uint16_t i = 0; i < kFieldCount; ++i) {
    if (!ValueFits(ReadField(record, i), kFields[i].bits)) {
      return EncodeStatus::kFieldOverflow;
    }
  }

  // Header: the version this record was encoded against, little-endian.
  const uint32_t version = EventSchemaVersion();
  for (unsigned byte = 0; byte < 4; ++byte) {
    out[byte] = static_cast<uint8_t>((version >> (8u * byte)) & 0xFFu);
  }
  size_t at = 4;
  for (uint16_t i = 0; i < kFieldCount; ++i) {
    uint64_t value = ReadField(record, i);
    const uint16_t octets = OctetsOf(kFields[i].bits);
    for (uint16_t byte = 0; byte < octets; ++byte) {
      out[at++] = static_cast<uint8_t>((value >> (8u * byte)) & 0xFFu);
    }
  }
  if (out_written != nullptr) *out_written = at;
  return EncodeStatus::kOk;
}

EncodeStatus DecodeEvent(const uint8_t* in, size_t length,
                         uint32_t expected_schema_version, EventRecord* out) {
  if (in == nullptr || out == nullptr) return EncodeStatus::kBufferTooSmall;
  if (length < kEventRecordBytes) return EncodeStatus::kBufferTooSmall;

  uint32_t version = 0;
  for (unsigned byte = 0; byte < 4; ++byte) {
    version |= static_cast<uint32_t>(in[byte]) << (8u * byte);
  }
  if (version != expected_schema_version) return EncodeStatus::kVersionMismatch;

  EventRecord decoded{};
  size_t at = 4;
  for (uint16_t i = 0; i < kFieldCount; ++i) {
    const uint16_t octets = OctetsOf(kFields[i].bits);
    uint64_t value = 0;
    for (uint16_t byte = 0; byte < octets; ++byte) {
      value |= static_cast<uint64_t>(in[at + byte]) << (8u * byte);
    }
    at += octets;
    if (!ValueFits(value, kFields[i].bits)) return EncodeStatus::kFieldOverflow;
    WriteField(&decoded, i, value);
  }
  *out = decoded;
  return EncodeStatus::kOk;
}

size_t FormatEvent(const EventRecord& record, char* out, size_t capacity) {
  if (out == nullptr) return 0;
  size_t needed = 1;  // the terminating NUL
  for (uint16_t i = 0; i < kFieldCount; ++i) {
    if (i != 0) needed += 1;
    needed += std::strlen(kFields[i].name) + 1 + 2u * OctetsOf(kFields[i].bits);
  }
  if (capacity < needed) return 0;

  size_t at = 0;
  for (uint16_t i = 0; i < kFieldCount; ++i) {
    if (i != 0) out[at++] = ' ';
    const size_t name_len = std::strlen(kFields[i].name);
    std::memcpy(out + at, kFields[i].name, name_len);
    at += name_len;
    out[at++] = '=';
    const int digits = static_cast<int>(2u * OctetsOf(kFields[i].bits));
    const int written = std::snprintf(
        out + at, capacity - at, "%0*llx", digits,
        static_cast<unsigned long long>(ReadField(record, i)));
    at += static_cast<size_t>(written < 0 ? 0 : written);
  }
  out[at] = '\0';
  return at;
}

}  // namespace mosaic
