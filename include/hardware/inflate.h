#pragma once

#include <cstddef>
#include <cstdint>

namespace hw {

/**
 * An embedded file that scripts/deflate_embeds.py compressed -- its length,
 * four bytes little endian, then a zlib stream -- inflated into PSRAM.
 *
 * `start` and `end` are the linker's symbols for the embedded copy. Returns
 * the original, claimed once and never freed, with its length in `out_len`;
 * nullptr when it will not inflate or there is no memory for it. Blocks for
 * as long as inflating takes: a few milliseconds per 100 KB.
 */
const uint8_t* inflateEmbedded(const uint8_t* start, const uint8_t* end,
                               size_t* out_len = nullptr);

}  // namespace hw
