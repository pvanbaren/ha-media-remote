#include "hardware/inflate.h"

#include <esp_heap_caps.h>

#include <cstring>

#include <lgfx/utility/lgfx_miniz.h>

#include "log.h"

namespace hw {

const uint8_t* inflateEmbedded(const uint8_t* start, const uint8_t* end,
                               size_t* out_len) {
  if (out_len != nullptr) {
    *out_len = 0;
  }
  if (start == nullptr || end == nullptr || end - start < 4) {
    return nullptr;
  }
  uint32_t length = 0;
  memcpy(&length, start, sizeof(length));  // little endian, as is the chip
  auto* out = static_cast<uint8_t*>(
      heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // The decompressor's state is 11 KB: on the heap, not a caller's stack,
  // which LovyanGFX's own mem-to-mem call would put it on.
  auto* state = static_cast<lgfx_tinfl_decompressor*>(heap_caps_malloc(
      sizeof(lgfx_tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (out == nullptr || state == nullptr) {
    heap_caps_free(out);
    heap_caps_free(state);
    LOG_WARN("Inflate: no memory for %lu bytes",
             static_cast<unsigned long>(length));
    return nullptr;
  }

  lgfx_tinfl_init(state);
  size_t in_size = static_cast<size_t>(end - start) - 4;
  size_t out_size = length;
  const lgfx_tinfl_status status = lgfx_tinfl_decompress(
      state, start + 4, &in_size, out, out, &out_size,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  heap_caps_free(state);
  if (status != TINFL_STATUS_DONE || out_size != length) {
    heap_caps_free(out);
    LOG_WARN("Inflate: failed (status %d, %lu of %lu bytes)",
             static_cast<int>(status), static_cast<unsigned long>(out_size),
             static_cast<unsigned long>(length));
    return nullptr;
  }
  if (out_len != nullptr) {
    *out_len = length;
  }
  return out;
}

}  // namespace hw
