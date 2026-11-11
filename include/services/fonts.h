#pragma once

#include <cstddef>

/**
 * Fonts from GitHub: the font pack the remote draws Chinese, Japanese,
 * Korean, Thai, Cyrillic, Greek, the rest of Latin and symbols from
 * (hardware/font_pack.h), downloaded on request into the flash partition
 * kept for it.
 *
 * The pack is the asset config::kFontPackAsset of the pre-release
 * config::kFontPackTag, which scripts/build_font_pack.py made. It comes down
 * the way firmware does (services::update::download()), and is drawn from as
 * soon as it is written -- no restart.
 */
namespace services::fonts {

/** The scripts the pack in flash holds, "Korean, Chinese, Japanese, Thai";
 *  empty when there is none. */
void describe(char* out, size_t len);

/** The pack in flash in short, for a line on the remote's screen: "11
 *  fonts, 5,228 KB"; empty when there is none. */
void summary(char* out, size_t len);

/** Whether the pack in flash is config::kFontPackTag's. */
bool current();

/** Download the pack and write it, calling `progress` with 0-100 as it comes
 *  down. False with `error` saying why; the pack that was there is gone
 *  then too. Blocks: a minute or so. Loop only. */
bool download(void (*progress)(int percent), char* error, size_t error_len);

/** Serve the portal's /fonts page: what is there, and a download. Call
 *  before Wi-Fi comes up, as for any portal page. */
void init();

/** A download asked for on the portal page, which the loop runs, where the
 *  screen can show it: true once. */
bool takeDownloadRequest();

}  // namespace services::fonts
