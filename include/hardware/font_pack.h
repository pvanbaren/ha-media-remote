#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The font pack: outline fonts for what the UI faces lack -- Korean,
 * Chinese, Japanese, Thai, Cyrillic, Greek, the rest of Latin, symbols --
 * kept in the flash partition the partition table calls spiffs, which nothing else
 * uses. scripts/build_font_pack.py makes it; services::fonts downloads it
 * from GitHub and writes it here; hardware/outline_fonts.h draws from it.
 *
 * Read in place: the partition is mapped into the address space, so the
 * fonts take no RAM.
 *
 * The pack's first 4 KB says what is in it (scripts/build_font_pack.py has
 * the layout), and is written last, once the rest is down and checks out:
 * a download that stops part way leaves no pack, rather than a broken one.
 *
 * Loop only, reading and writing alike.
 */
namespace hw::font_pack {

constexpr int kMaxFonts = 16;

struct Font {
  /** ISO 15924: "Kore", "Hans", "Jpan", "Thai". */
  char script[5];
  /** "Korean". */
  char name[21];
  const uint8_t* data;
  uint32_t length;
};

struct Info {
  /** The release it came from: "fonts-1". */
  char tag[24];
  uint32_t bytes;
  int count;
  /** In the order to look in. */
  Font fonts[kMaxFonts];
};

/** The pack in flash, mapped the first time it is asked for; nullptr when
 *  there is none, or while one is being written. */
const Info* info();

/** Changes whenever the pack does -- one written, or one begun -- so that
 *  what was drawn from the last can be let go of. */
uint32_t generation();

/** How many bytes a pack may be: the partition's size, 0 when this flash
 *  layout has none. */
uint32_t capacity();

/** Write a new pack, `total` bytes, in order. The pack in flash goes at
 *  once. False with `error` saying why. */
bool beginWrite(uint32_t total, char* error, size_t error_len);
bool write(const uint8_t* data, size_t len, char* error, size_t error_len);
/** Check what was written against the pack's own checksum, then write the
 *  header that makes it the pack. */
bool endWrite(char* error, size_t error_len);
/** Give up a write: no pack until the next. */
void abortWrite();

}  // namespace hw::font_pack
