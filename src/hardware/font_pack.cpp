#include "hardware/font_pack.h"

#include <esp_heap_caps.h>
#include <esp_partition.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace hw::font_pack {
namespace {

constexpr char kMagic[8] = {'H', 'M', 'R', 'F', 'O', 'N', 'T', 'S'};
constexpr uint32_t kFormat = 1;
/** The header's share of the pack; the fonts start after it. */
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kEntriesAt = 48;
constexpr uint32_t kEntryBytes = 32;
/** Erased a block at a time, ahead of the writes: a sector at a time is
 *  sixteen times the calls for the same flash. */
constexpr uint32_t kEraseBytes = 65536;

void setError(char* out, size_t len, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(out, len, format, args);
  va_end(args);
}

const esp_partition_t* partition() {
  static const esp_partition_t* s_partition = nullptr;
  static bool s_looked = false;
  if (!s_looked) {
    s_looked = true;
    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  }
  return s_partition;
}

void* psram(size_t bytes) {
  void* mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return mem != nullptr ? mem : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

uint32_t le32(const uint8_t* at) {
  return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 |
         static_cast<uint32_t>(at[2]) << 16 |
         static_cast<uint32_t>(at[3]) << 24;
}

/** CRC-32 as zlib reckons it, which is what the pack's builder writes. */
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
  static uint32_t* s_table = nullptr;
  if (s_table == nullptr) {
    s_table = static_cast<uint32_t*>(psram(256 * sizeof(uint32_t)));
    if (s_table == nullptr) {
      return ~crc;  // no room: it will not match, and the write fails
    }
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      s_table[i] = c;
    }
  }
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc = s_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

/** Read the header in `h` into `out`, its fonts' places as offsets for now.
 *  False when it is not a pack this firmware reads, or does not fit. */
bool parseHeader(const uint8_t* h, uint32_t room, Info& out) {
  if (memcmp(h, kMagic, sizeof(kMagic)) != 0 || le32(h + 8) != kFormat) {
    return false;
  }
  out.bytes = le32(h + 12);
  out.count = static_cast<int>(le32(h + 20));
  if (out.bytes < kHeaderBytes || out.bytes > room || out.count < 1 ||
      out.count > kMaxFonts) {
    return false;
  }
  memcpy(out.tag, h + 24, sizeof(out.tag) - 1);
  out.tag[sizeof(out.tag) - 1] = '\0';
  for (int i = 0; i < out.count; ++i) {
    const uint8_t* e = h + kEntriesAt + kEntryBytes * i;
    Font& font = out.fonts[i];
    memcpy(font.script, e, 4);
    font.script[4] = '\0';
    const uint32_t offset = le32(e + 4);
    font.length = le32(e + 8);
    memcpy(font.name, e + 12, 20);
    font.name[20] = '\0';
    if (offset < kHeaderBytes || (offset & 3) != 0 || font.length == 0 ||
        font.length > out.bytes - offset) {
      return false;
    }
    font.data = reinterpret_cast<const uint8_t*>(offset);
  }
  return true;
}

/** The pack as mapped, and whether it has been looked for since it last
 *  changed. */
Info* s_info = nullptr;
bool s_mapped = false;
bool s_looked = false;
esp_partition_mmap_handle_t s_map = 0;
uint32_t s_generation = 1;

/** A write under way. */
struct Writer {
  bool active = false;
  uint32_t total = 0;
  uint32_t written = 0;
  uint32_t erased = 0;
  /** The header, held back until the rest checks out. */
  uint8_t* header = nullptr;
};
Writer s_write;

void unmap() {
  if (s_mapped) {
    esp_partition_munmap(s_map);
    s_mapped = false;
  }
  s_looked = false;
  ++s_generation;
}

void load() {
  const esp_partition_t* part = partition();
  if (part == nullptr) {
    return;
  }
  if (s_info == nullptr) {
    s_info = static_cast<Info*>(psram(sizeof(Info)));
    if (s_info == nullptr) {
      return;
    }
  }
  auto* header = static_cast<uint8_t*>(psram(kHeaderBytes));
  if (header == nullptr) {
    return;
  }
  const bool read =
      esp_partition_read(part, 0, header, kHeaderBytes) == ESP_OK;
  const bool parsed = read && parseHeader(header, part->size, *s_info);
  heap_caps_free(header);
  if (!parsed) {
    return;  // never written, or a download that did not finish
  }
  const void* base = nullptr;
  const esp_err_t err = esp_partition_mmap(part, 0, s_info->bytes,
                                           ESP_PARTITION_MMAP_DATA, &base,
                                           &s_map);
  if (err != ESP_OK) {
    LOG_WARN("Fonts: could not map the font pack (%s)", esp_err_to_name(err));
    return;
  }
  s_mapped = true;
  for (int i = 0; i < s_info->count; ++i) {
    Font& font = s_info->fonts[i];
    font.data = static_cast<const uint8_t*>(base) +
                reinterpret_cast<uintptr_t>(font.data);
  }
}

void freeWriter() {
  heap_caps_free(s_write.header);
  s_write = Writer{};
}

}  // namespace

const Info* info() {
  if (s_write.active) {
    return nullptr;
  }
  if (!s_looked) {
    s_looked = true;
    load();
  }
  return s_mapped ? s_info : nullptr;
}

uint32_t generation() { return s_generation; }

uint32_t capacity() {
  const esp_partition_t* part = partition();
  return part != nullptr ? part->size : 0;
}

bool beginWrite(uint32_t total, char* error, size_t error_len) {
  const esp_partition_t* part = partition();
  if (part == nullptr) {
    setError(error, error_len, "this flash layout has no room for fonts");
    return false;
  }
  if (total <= kHeaderBytes || total > part->size) {
    setError(error, error_len, "the fonts are %u KB, with room for %u KB",
             static_cast<unsigned>(total / 1024),
             static_cast<unsigned>(part->size / 1024));
    return false;
  }
  if (s_write.active) {
    freeWriter();
  }
  unmap();
  s_write.header = static_cast<uint8_t*>(psram(kHeaderBytes));
  if (s_write.header == nullptr) {
    setError(error, error_len, "no memory to write the fonts");
    return false;
  }
  s_write.active = true;
  s_write.total = total;
  // The header goes first, so that from here until the end there is no pack.
  const uint32_t first = kEraseBytes < part->size ? kEraseBytes : part->size;
  if (esp_partition_erase_range(part, 0, first) != ESP_OK) {
    setError(error, error_len, "could not erase the font flash");
    freeWriter();
    return false;
  }
  s_write.erased = first;
  return true;
}

bool write(const uint8_t* data, size_t len, char* error, size_t error_len) {
  const esp_partition_t* part = partition();
  if (!s_write.active || part == nullptr) {
    setError(error, error_len, "no font write under way");
    return false;
  }
  if (len > s_write.total - s_write.written) {
    setError(error, error_len, "more came down than the fonts' length");
    return false;
  }
  if (s_write.written < kHeaderBytes) {
    const size_t n = len < kHeaderBytes - s_write.written
                         ? len
                         : kHeaderBytes - s_write.written;
    memcpy(s_write.header + s_write.written, data, n);
    s_write.written += n;
    data += n;
    len -= n;
  }
  if (len == 0) {
    return true;
  }
  const uint32_t end = s_write.written + len;
  while (s_write.erased < end) {
    const uint32_t left = part->size - s_write.erased;
    const uint32_t n = left < kEraseBytes ? left : kEraseBytes;
    if (esp_partition_erase_range(part, s_write.erased, n) != ESP_OK) {
      setError(error, error_len, "could not erase the font flash");
      return false;
    }
    s_write.erased += n;
  }
  if (esp_partition_write(part, s_write.written, data, len) != ESP_OK) {
    setError(error, error_len, "could not write the font flash");
    return false;
  }
  s_write.written = end;
  return true;
}

bool endWrite(char* error, size_t error_len) {
  const esp_partition_t* part = partition();
  if (!s_write.active || part == nullptr) {
    setError(error, error_len, "no font write under way");
    return false;
  }
  bool ok = true;
  Info* parsed = static_cast<Info*>(psram(sizeof(Info)));
  auto* buf = static_cast<uint8_t*>(psram(kHeaderBytes));
  if (s_write.written != s_write.total) {
    setError(error, error_len, "the fonts came down short");
    ok = false;
  } else if (parsed == nullptr || buf == nullptr) {
    setError(error, error_len, "no memory to check the fonts");
    ok = false;
  } else if (!parseHeader(s_write.header, part->size, *parsed) ||
             parsed->bytes != s_write.total) {
    setError(error, error_len, "that is not a font pack this firmware reads");
    ok = false;
  }
  // What is in flash, read back, against the checksum the pack carries.
  if (ok) {
    uint32_t crc = 0;
    for (uint32_t at = kHeaderBytes; ok && at < s_write.total;
         at += kHeaderBytes) {
      const uint32_t left = s_write.total - at;
      const uint32_t n = left < kHeaderBytes ? left : kHeaderBytes;
      ok = esp_partition_read(part, at, buf, n) == ESP_OK;
      crc = crc32(crc, buf, n);
    }
    if (!ok || crc != le32(s_write.header + 16)) {
      setError(error, error_len, "the fonts did not check out");
      ok = false;
    }
  }
  if (ok && esp_partition_write(part, 0, s_write.header, kHeaderBytes) !=
                ESP_OK) {
    setError(error, error_len, "could not write the font flash");
    ok = false;
  }
  const uint32_t total = s_write.total;
  heap_caps_free(parsed);
  heap_caps_free(buf);
  freeWriter();
  unmap();  // looked for afresh: the new pack, or none
  if (ok) {
    LOG_INFO("Fonts: font pack written, %u KB",
             static_cast<unsigned>(total / 1024));
  }
  return ok;
}

void abortWrite() {
  if (s_write.active) {
    freeWriter();
    unmap();
  }
}

}  // namespace hw::font_pack
