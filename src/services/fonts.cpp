#include "services/fonts.h"

#include <Arduino.h>
#include <WebServer.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/font_pack.h"
#include "log.h"
#include "services/device_name.h"
#include "services/firmware_update.h"
#include "services/wifi_setup.h"

namespace services::fonts {
namespace {

String packUrl() {
  String url("https://github.com/");
  url += config::kFirmwareRepo;
  url += "/releases/download/";
  url += config::kFontPackTag;
  url += "/";
  url += config::kFontPackAsset;
  return url;
}

// The download goes straight into the font partition.

bool packBegin(size_t total, char* error, size_t error_len) {
  return hw::font_pack::beginWrite(static_cast<uint32_t>(total), error,
                                   error_len);
}

bool packWrite(const uint8_t* data, size_t len, char* error,
               size_t error_len) {
  return hw::font_pack::write(data, len, error, error_len);
}

bool packEnd(char* error, size_t error_len) {
  return hw::font_pack::endWrite(error, error_len);
}

void packAbort() { hw::font_pack::abortWrite(); }

constexpr update::Sink kPackSink = {packBegin, packWrite, packEnd, packAbort};

/** A download asked for on the page, for the loop. */
bool s_requested = false;
/** Why the last download failed, for the page; empty when none has. */
char s_last_error[64] = {};

void appendEscaped(String& out, const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p; break;
    }
  }
}

/**
 * /fonts: the fonts on this remote, and a button to download them.
 *
 * The page answers at once and the loop does the download, with the progress
 * on the remote's own screen, as for firmware: the page would wait the whole
 * time otherwise, and a browser gives up first.
 */
void handlePage(WebServer& server) {
  String page;
  page.reserve(1536);
  page += "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width,initial-scale=1'><title>";
  appendEscaped(page, services::device::name());
  page += " - Fonts</title></head><body style='font-family:verdana'>"
          "<h3>Fonts</h3><p>";
  page += config::kFontPackScripts;
  page += " are drawn from fonts the remote downloads from GitHub into its "
          "own flash: ";
  page += config::kFontPackSize;
  page += ", a minute or so. Without them, those letters are blanks.</p>";

  if (server.method() == HTTP_POST) {
    if (s_requested) {
      page += "<p>A download is already under way.</p>";
    } else {
      s_requested = true;
      page += "<p>Downloading. The remote shows how far it has got on its "
              "screen. Then come back to this page to see the fonts it "
              "has.</p>";
    }
  } else {
    char scripts[192];
    describe(scripts, sizeof(scripts));
    const hw::font_pack::Info* pack = hw::font_pack::info();
    if (pack == nullptr) {
      page += "<p>This remote has none yet.</p>";
    } else {
      page += "<p>This remote has <b>";
      appendEscaped(page, scripts);
      page += "</b> (";
      appendEscaped(page, pack->tag);
      char size[24];
      snprintf(size, sizeof(size), ", %u KB)",
               static_cast<unsigned>(pack->bytes / 1024));
      page += size;
      page += current() ? ", the latest.</p>" : ". Newer fonts are out.</p>";
    }
    page += "<form method='post' action='/fonts'><button>Download fonts"
            "</button></form>";
  }
  if (s_last_error[0] != '\0') {
    page += "<p>The last download failed: ";
    appendEscaped(page, s_last_error);
    page += ".</p>";
  }
  page += "<p><a href='/fonts'>Refresh</a> &middot; <a href='/'>Menu</a>"
          "</p></body></html>";
  server.send(200, "text/html", page);
}

}  // namespace

void describe(char* out, size_t len) {
  if (len == 0) {
    return;
  }
  out[0] = '\0';
  const hw::font_pack::Info* pack = hw::font_pack::info();
  size_t used = 0;
  for (int i = 0; pack != nullptr && i < pack->count && used < len; ++i) {
    const int n = snprintf(out + used, len - used, "%s%s", i ? ", " : "",
                           pack->fonts[i].name);
    used += n > 0 ? static_cast<size_t>(n) : 0;
  }
}

void summary(char* out, size_t len) {
  if (len == 0) {
    return;
  }
  out[0] = '\0';
  const hw::font_pack::Info* pack = hw::font_pack::info();
  if (pack != nullptr) {
    const unsigned kb = static_cast<unsigned>(pack->bytes / 1024);
    if (kb >= 1000) {
      snprintf(out, len, "%d fonts, %u,%03u KB", pack->count, kb / 1000,
               kb % 1000);
    } else {
      snprintf(out, len, "%d fonts, %u KB", pack->count, kb);
    }
  }
}

bool current() {
  const hw::font_pack::Info* pack = hw::font_pack::info();
  return pack != nullptr && strcmp(pack->tag, config::kFontPackTag) == 0;
}

bool download(void (*progress)(int percent), char* error, size_t error_len) {
  LOG_INFO("Fonts: downloading %s", config::kFontPackTag);
  const bool ok = update::download(packUrl().c_str(), kPackSink, progress,
                                   error, error_len);
  if (ok) {
    s_last_error[0] = '\0';
    char scripts[192];
    describe(scripts, sizeof(scripts));
    LOG_INFO("Fonts: %s downloaded: %s", config::kFontPackTag, scripts);
  } else {
    snprintf(s_last_error, sizeof(s_last_error), "%s", error);
    LOG_WARN("Fonts: download failed: %s", error);
  }
  return ok;
}

void init() { wifiAddWebPage("/fonts", handlePage); }

bool takeDownloadRequest() {
  if (!s_requested) {
    return false;
  }
  s_requested = false;
  return true;
}

}  // namespace services::fonts
