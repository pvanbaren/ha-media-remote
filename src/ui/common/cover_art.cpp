#include "ui/cover_art.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "services/ha_client.h"
#include "services/yielding_client.h"
#include "ui/canvas.h"

namespace ui::cover {
namespace {

enum class Format : uint8_t { kUnknown, kJpeg, kPng };

// prepare() runs on the artwork worker while draw() runs on the Arduino loop,
// so everything below is guarded. The lock is only held across the decode and the
// short install step -- never across the download, which would stall a repaint
// for as long as the fetch takes.
SemaphoreHandle_t s_mutex = nullptr;

char s_picture[192] = {};
/** Claimed once by init() and never freed. */
uint8_t* s_buffer = nullptr;
size_t s_buffer_capacity = 0;
/** Bytes of s_buffer currently holding a cover; 0 when nothing is cached. */
size_t s_buffer_len = 0;
Format s_format = Format::kUnknown;
/** Art exists but was too big to hold; draw() re-streams it each repaint. */
bool s_stream_only = false;
/** Source pixel size, for the centre-crop scale. Zero when not known. */
uint16_t s_src_w = 0;
uint16_t s_src_h = 0;

/** The mutex is made in init(), on the Arduino task before the worker
 *  exists, and deliberately never here. prepare() runs on the artwork worker
 *  and draw() on the Arduino loop, either can be first, and two tasks that each
 *  make their own mutex end up locking different ones -- which would leave
 *  both s_buffer and LovyanGFX's single global PNG decoder unguarded. */
void lock() {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}

void unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

struct Guard {
  Guard() { lock(); }
  ~Guard() { unlock(); }
};

/** Drop the cached cover. The buffer itself stays -- it is claimed once and
 *  reused, so only the metadata is cleared. Caller holds the lock. */
void resetLocked() {
  s_buffer_len = 0;
  s_format = Format::kUnknown;
  s_stream_only = false;
  s_src_w = 0;
  s_src_h = 0;
}

bool hasArtLocked() {
  return s_picture[0] != '\0' && (s_buffer_len > 0 || s_stream_only);
}

/** Cleared for the session once a server refuses a rewritten size. */
bool s_size_rewrite_ok = true;

/** Smallest power of two >= n. */
int nextPowerOfTwo(int n) {
  int value = 1;
  while (value < n) {
    value <<= 1;
  }
  return value;
}

/** Ask the source for art near `target_px` rather than whatever the
 *  integration defaulted to.
 *
 *  Storing the art compressed is already the cheap option -- a decoded
 *  240x240 RGB565 is 115 KB against tens of KB for the JPEG -- so the only way
 *  to shrink the cache further is to not fetch the big one.
 *
 *  Servers offer a ladder of sizes rather than any value: Music Assistant's
 *  image proxy accepts 0, 80, 160, 256, 512 and 1024, and answers 400 to
 *  anything else. Rounding up to a power of two lands on that ladder for every
 *  size worth asking for (a 240 px panel gets 256, which on one album cut
 *  88 KB to 28 KB) without upscaling. Where it does not, prepare() retries the
 *  untouched URL and gives up rewriting for the session.
 *
 *  The target is board::kCoverArtRequestPx rather than the panel size: a board
 *  may prefer to upscale a smaller cover, and only the board knows whether
 *  that trade is worth making. */
bool requestPanelSize(String& url, int target_px) {
  if (!config::kCoverArtRequestPanelSize || !s_size_rewrite_ok) {
    return false;
  }

  int at = -1;
  for (int i = 0; (i = url.indexOf("size=", i)) >= 0; i += 5) {
    // Has to be a whole query parameter, not the tail of e.g. "&imagesize=".
    if (i > 0 && (url[i - 1] == '?' || url[i - 1] == '&')) {
      at = i;
      break;
    }
  }
  if (at < 0) {
    return false;
  }

  const int value_start = at + 5;
  int value_end = value_start;
  while (value_end < static_cast<int>(url.length()) &&
         isdigit(static_cast<unsigned char>(url[value_end]))) {
    ++value_end;
  }
  if (value_end == value_start) {
    return false;
  }

  const long current = url.substring(value_start, value_end).toInt();
  const int wanted = nextPowerOfTwo(target_px);
  if (current <= wanted) {
    return false;  // already no larger than the panel needs
  }

  url = url.substring(0, value_start) + String(wanted) +
        url.substring(value_end);
  return true;
}

/** Absolute URL for an entity_picture value. `resized` reports whether the
 *  size parameter was rewritten, so a refusal can be retried as-is. */
String pictureUrl(const char* picture, bool* resized = nullptr) {
  String url;
  if (strncmp(picture, "http://", 7) == 0 ||
      strncmp(picture, "https://", 8) == 0) {
    url = picture;
  } else {
    url = services::ha::baseUrl();
    if (picture[0] != '/') {
      url += '/';
    }
    url += picture;
  }
  const bool rewritten = requestPanelSize(url, board::kCoverArtRequestPx);
  if (resized != nullptr) {
    *resized = rewritten;
  }
  return url;
}

Format formatFromMagic(const uint8_t* data, size_t len) {
  if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return Format::kJpeg;
  }
  if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
      data[3] == 'G') {
    return Format::kPng;
  }
  return Format::kUnknown;
}

Format formatFromContentType(const String& content_type) {
  if (content_type.indexOf("png") >= 0) {
    return Format::kPng;
  }
  if (content_type.indexOf("jpeg") >= 0 || content_type.indexOf("jpg") >= 0) {
    return Format::kJpeg;
  }
  return Format::kUnknown;
}

/** Width and height from a PNG IHDR chunk. */
bool pngSize(const uint8_t* d, size_t len, uint16_t& w, uint16_t& h) {
  // 8-byte signature, 4-byte chunk length, "IHDR", width, height (big-endian).
  if (len < 24 || memcmp(d + 12, "IHDR", 4) != 0) {
    return false;
  }
  w = static_cast<uint16_t>((d[16] << 8) | d[17]);
  h = static_cast<uint16_t>((d[20] << 8) | d[21]);
  return w > 0 && h > 0;
}

/** Walk JPEG markers to the frame header and read its dimensions. */
bool jpegSize(const uint8_t* d, size_t len, uint16_t& w, uint16_t& h) {
  size_t i = 2;  // past SOI
  while (i + 9 < len) {
    if (d[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t marker = d[i + 1];
    // Standalone markers carry no length field.
    if (marker == 0xD8 || marker == 0x01 || marker == 0xFF ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      ++i;
      continue;
    }
    if (marker == 0xDA) {  // start of scan: dimensions would have come first
      return false;
    }
    const size_t seg_len = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
    // SOF0..SOF15, less the DHT/JPG/DAC markers interleaved in that range.
    const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                        marker != 0xC8 && marker != 0xCC;
    if (is_sof) {
      h = static_cast<uint16_t>((d[i + 5] << 8) | d[i + 6]);
      w = static_cast<uint16_t>((d[i + 7] << 8) | d[i + 8]);
      return w > 0 && h > 0;
    }
    if (seg_len < 2) {
      return false;
    }
    i += 2 + seg_len;
  }
  return false;
}

/** Feeds LovyanGFX's image decoders straight from the socket.
 *  This build of LovyanGFX takes a DataWrapper but has no Arduino Stream
 *  overload, and a DataWrapper is what the file-backed drawJpgFile path uses
 *  anyway -- so art too large to cache decodes from here instead, with only
 *  the decoder's own working buffer in RAM. Forward-only: a socket cannot be
 *  rewound, which is fine for JPEG and PNG since both decode in one pass. */
class StreamWrapper : public lgfx::DataWrapper {
 public:
  StreamWrapper(WiFiClient* stream, uint32_t length)
      : lgfx::DataWrapper(), _stream(stream), _length(length) {
    need_transaction = true;
  }

  int read(uint8_t* buf, uint32_t len) override { return read(buf, len, len); }

  int read(uint8_t* buf, uint32_t maximum_len, uint32_t required_len) override {
    const uint32_t remaining = _length > _pos ? _length - _pos : 0;
    if (maximum_len > remaining) {
      maximum_len = remaining;
    }
    if (required_len > maximum_len) {
      required_len = maximum_len;
    }

    // Fill to maximum_len, not merely to required_len.
    //
    // LovyanGFX asks for `len` bytes but passes required_len = 2 whatever it
    // actually wants (LGFXBase.cpp, image_decoder_t::read_data). Honouring
    // only the minimum and returning is therefore a short read, and the two
    // decoders disagree about whether that is allowed: pngle is a push parser
    // and simply asks again, while every header read in lgfx_jd_prepare is
    //
    //     if (infunc(dev, seg, len) != len) return JDR_INP;
    //
    // So stopping at two bytes failed *every* JPEG whose bytes were not
    // already sitting in the socket buffer, and no PNG at all -- a thoroughly
    // misleading way for a bug to present itself. It looked like particular
    // images were malformed. They were not; the reader was.
    //
    // Bounded above by the Content-Length clamp, which every source here
    // provides, and otherwise by the timeout and the connection dropping.
    //
    // Elapsed time rather than a deadline: millis() wraps every 49.7 days,
    // and `millis() < start + timeout` is false at once when the sum wraps.
    uint32_t filled = 0;
    const unsigned long started = millis();
    while (filled < maximum_len &&
           millis() - started < config::kHaHttpTimeoutMs) {
      const int available = _stream->available();
      if (available <= 0) {
        if (!_stream->connected()) {
          break;
        }
        delay(1);
        continue;
      }
      uint32_t want = maximum_len - filled;
      if (want > static_cast<uint32_t>(available)) {
        want = static_cast<uint32_t>(available);
      }
      const int got = _stream->readBytes(buf + filled, want);
      if (got <= 0) {
        break;
      }
      filled += static_cast<uint32_t>(got);
    }
    _pos += filled;
    return static_cast<int>(filled);
  }

  void skip(int32_t offset) override {
    if (offset > 0) {
      seek(_pos + static_cast<uint32_t>(offset));
    }
  }

  bool seek(uint32_t offset) override {
    if (offset < _pos) {
      return false;  // no rewinding a socket
    }
    uint8_t scratch[64];
    while (_pos < offset) {
      uint32_t chunk = offset - _pos;
      if (chunk > sizeof(scratch)) {
        chunk = sizeof(scratch);
      }
      if (read(scratch, chunk, chunk) <= 0) {
        return false;
      }
    }
    return true;
  }

  void close(void) override {}
  int32_t tell(void) override { return static_cast<int32_t>(_pos); }

 private:
  WiFiClient* _stream;
  uint32_t _length;
  uint32_t _pos = 0;
};

/** Collects a response body into a fixed buffer, for HTTPClient's
 *  writeToStream(). That call is the reason to have this at all: it handles
 *  both a Content-Length and a chunked body, reads the response to its end
 *  -- which is what leaves a kept-alive connection clean for the next
 *  request -- and closes the connection itself on any error. */
class BufferSink : public Stream {
 public:
  BufferSink(uint8_t* buffer, size_t capacity)
      : _buffer(buffer), _capacity(capacity) {}

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    if (len > _capacity - _length) {
      _overflowed = true;
      return 0;  // writeToStream() takes a short write as an error
    }
    memcpy(_buffer + _length, data, len);
    _length += len;
    return len;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t length() const { return _length; }
  bool overflowed() const { return _overflowed; }

 private:
  uint8_t* _buffer;
  size_t _capacity;
  size_t _length = 0;
  bool _overflowed = false;
};

/**
 * The one connection thumbnails keep open between fetches.
 *
 * Measured, a thumbnail was 515-613 ms of TLS handshake and then 40-100 ms of
 * everything else, and a list's worth came from two hosts. So one connection,
 * kept alive across a batch and reopened only when the host changes, turns
 * sixteen handshakes into two. The artwork worker orders its queue by host
 * for exactly this reason.
 *
 * Only the artwork worker uses it, so it needs no lock. Created on first use
 * and never destroyed. The clients come before the HTTPClient for the reason
 * given in prepare(), though it never matters here.
 */
struct ThumbConnection {
  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  HTTPClient http;
  /** scheme://host:port the open connection, if any, belongs to. */
  String origin;

  void close() {
    plain.stop();
    secure.stop();
  }
};

ThumbConnection& thumbConnection() {
  static ThumbConnection* connection = new ThumbConnection();
  return *connection;
}

/** Downloads land here before decoding, so the cover-art lock is held for a
 *  decode from memory rather than for however long the network takes. 64 KB
 *  is many times a resized thumbnail -- they measured 2 to 8 KB -- and
 *  anything larger decodes straight off the socket instead. PSRAM, claimed
 *  by the worker on its first fetch and never freed. */
constexpr size_t kThumbBufferBytes = 64 * 1024;
uint8_t* s_thumb_buffer = nullptr;

/** Host and port out of an http(s) URL, the way HTTPClient will read them.
 *  False for anything it would be unwise to second-guess, such as an IPv6
 *  literal. */
bool splitHost(const String& url, String& host, uint16_t& port) {
  const bool https = url.startsWith("https://");
  const int start = https ? 8 : (url.startsWith("http://") ? 7 : -1);
  if (start < 0) {
    return false;
  }
  int end = static_cast<int>(url.length());
  for (const char stop : {'/', '?', '#'}) {
    const int at = url.indexOf(stop, start);
    if (at >= 0 && at < end) {
      end = at;
    }
  }
  String authority = url.substring(start, end);
  const int at = authority.lastIndexOf('@');
  if (at >= 0) {
    authority = authority.substring(at + 1);
  }
  if (authority.length() == 0 || authority[0] == '[') {
    return false;
  }
  port = https ? 443 : 80;
  const int colon = authority.indexOf(':');
  if (colon >= 0) {
    const long parsed = authority.substring(colon + 1).toInt();
    if (parsed <= 0 || parsed > 65535) {
      return false;
    }
    port = static_cast<uint16_t>(parsed);
    authority = authority.substring(0, colon);
  }
  host = authority;
  return host.length() > 0;
}

/** "https://host:port", or empty when the URL will not split. */
String originOf(const String& url) {
  String host;
  uint16_t port = 0;
  if (!splitHost(url, host, port)) {
    return String();
  }
  return String(url.startsWith("https://") ? "https://" : "http://") + host +
         ":" + String(port);
}

/** Rewrite an artwork URL to ask its host for something near `size` pixels.
 *
 *  Not a nicety. The Music Assistant library hands back whatever the provider
 *  stored, which for a 38 px thumbnail meant downloading 1.9 MB across ten
 *  images -- one of them 601 KB -- and decoding each one to throw almost all
 *  of it away. Several failed outright, which is what a decoder does when an
 *  image is far larger than anything it was asked to plan for.
 *
 *  Host-specific because there is no general way to say it. Both forms below
 *  are documented resize conventions, and both are the hosts Music Assistant
 *  actually returns:
 *
 *    googleusercontent.com   trailing "=w600-h600-p" or "=s600"
 *    resources.tidal.com     a "/750x750.jpg" path segment
 *
 *  Anything else is left exactly as it came. The caller retries with the
 *  original URL if the rewritten one is refused, so a host that changes its
 *  convention costs a round trip rather than a missing image. */
String thumbnailUrl(const char* url, int size, bool& rewritten) {
  rewritten = false;
  String out(url);
  if (size <= 0) {
    return out;
  }

  if (out.indexOf("googleusercontent.com") >= 0) {
    // The size suffix is everything after the last '=', and only when that
    // '=' comes after the last '/' -- otherwise it is a query parameter.
    const int eq = out.lastIndexOf('=');
    if (eq > out.lastIndexOf('/')) {
      const String suffix = out.substring(eq + 1);
      // "w600-h600-p" or "s600" -- a letter then a digit. Checking the digit
      // matters: a query like "?size=small" would otherwise look like one.
      if (suffix.length() >= 2 &&
          (suffix[0] == 'w' || suffix[0] == 's') &&
          isdigit(static_cast<unsigned char>(suffix[1]))) {
        out = out.substring(0, eq + 1) + "w" + String(size) + "-h" +
              String(size) + "-p";
        rewritten = true;
        return out;
      }
    }
    return out;
  }

  if (out.indexOf("resources.tidal.com") >= 0) {
    // Tidal serves a fixed ladder; anything off it is a 404, so round up to
    // the smallest that covers the request.
    static constexpr int kSizes[] = {80, 160, 320, 640, 750, 1280};
    int want = kSizes[0];
    for (const int candidate : kSizes) {
      want = candidate;
      if (candidate >= size) {
        break;
      }
    }
    const int slash = out.lastIndexOf('/');
    const int dot = out.lastIndexOf('.');
    if (slash > 0 && dot > slash) {
      const String name = out.substring(slash + 1, dot);
      if (name.indexOf('x') > 0) {
        out = out.substring(0, slash + 1) + String(want) + "x" + String(want) +
              out.substring(dot);
        rewritten = true;
      }
    }
  }
  return out;
}

/** Open the art URL. Leaves `http` connected on success. */
bool openArt(HTTPClient& http, WiFiClient& plain, WiFiClientSecure& secure,
             const String& url, int& content_length, Format& format) {
  WiFiClient* client = &plain;
  if (url.startsWith("https://")) {
    secure.setInsecure();
    client = &secure;
  }

  http.setTimeout(config::kHaHttpTimeoutMs);
  http.setConnectTimeout(config::kHaHttpTimeoutMs);
  if (!http.begin(*client, url)) {
    return false;
  }

  // Only ever send the long-lived token to Home Assistant itself. An
  // entity_picture can be an absolute URL to anywhere -- Music Assistant
  // serves art off its own port -- and an integration chooses that value, so
  // attaching the token to it would hand the credential to a third-party host.
  // It also upsets servers that do not expect the header: Music Assistant's
  // image proxy answers 400 to an unexpected Authorization.
  const char* base = services::ha::baseUrl();
  if (base[0] != '\0' && url.startsWith(base)) {
    http.addHeader("Authorization", String("Bearer ") + services::ha::token());
  }

  const char* headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);

  const unsigned long started_ms = millis();
  const int code = http.GET();
  content_length = code == HTTP_CODE_OK ? http.getSize() : -1;
  services::ha::logHttpRequest("GET", url.c_str(), code, 0, content_length,
                               millis() - started_ms);

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  format = formatFromContentType(http.header("Content-Type"));
  return true;
}

/** Read exactly `len` bytes from the open response. */
bool readBody(HTTPClient& http, uint8_t* out, size_t len) {
  WiFiClient* stream = http.getStreamPtr();
  size_t filled = 0;
  // Elapsed rather than a deadline, which fails at once when millis() wraps.
  const unsigned long started = millis();

  while (filled < len && millis() - started < config::kHaHttpTimeoutMs) {
    if (stream->available() <= 0) {
      if (!stream->connected()) {
        break;
      }
      delay(2);
      continue;
    }
    const int read = stream->readBytes(out + filled, len - filled);
    if (read <= 0) {
      break;
    }
    filled += static_cast<size_t>(read);
  }
  return filled == len;
}

}  // namespace

void init() {
  // Before the poll task exists, so lock() never has to make it.
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }

  const size_t wanted = board::kCoverArtBufferBytes;
  if (wanted == 0) {
    LOG_INFO("Cover art: caching disabled, art will stream");
    return;
  }

  // PSRAM, so the internal DMA-capable heap stays free for the things that
  // need it -- mbedTLS above all. Taken now, before Wi-Fi, while the heap is
  // still unfragmented, and never given back.
  s_buffer = static_cast<uint8_t*>(
      heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_buffer == nullptr) {
    LOG_WARN("Cover art: no %u byte PSRAM buffer, art will stream",
                  static_cast<unsigned>(wanted));
    return;
  }

  s_buffer_capacity = wanted;
  LOG_INFO("Cover art: %u byte buffer in PSRAM, %u internal free after",
                static_cast<unsigned>(wanted),
                static_cast<unsigned>(heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void clear() {
  Guard guard;
  s_picture[0] = '\0';
  resetLocked();
}

bool hasArt() {
  Guard guard;
  return hasArtLocked();
}

uint32_t identity() {
  Guard guard;
  // FNV-1a over the picture, then what is held of it.
  uint32_t h = 2166136261u;
  auto mix = [&](uint8_t byte) {
    h ^= byte;
    h *= 16777619u;
  };
  for (const char* p = s_picture; *p != '\0'; ++p) {
    mix(static_cast<uint8_t>(*p));
  }
  const uint32_t len = static_cast<uint32_t>(s_buffer_len);
  for (int shift = 0; shift < 32; shift += 8) {
    mix(static_cast<uint8_t>(len >> shift));
  }
  mix(s_stream_only ? 1 : 0);
  return h;
}

void prepare(const char* picture) {
  if (picture == nullptr || picture[0] == '\0') {
    clear();
    return;
  }

  {
    Guard guard;
    if (strcmp(picture, s_picture) == 0 && hasArtLocked()) {
      return;  // already cached
    }
  }

  if (s_buffer == nullptr) {
    // Nothing to hold it in, so there is nothing to fetch here. draw() streams
    // the image and works the format out from the response itself, and
    // probing it now would only spend a round trip per track change to learn
    // what we already know.
    Guard guard;
    resetLocked();
    snprintf(s_picture, sizeof(s_picture), "%s", picture);
    s_stream_only = true;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Invalidate before fetching. The buffer is reused rather than reallocated,
  // so the incoming image overwrites the outgoing one -- dropping s_buffer_len
  // to zero first is what stops draw() reading it while it is being filled.
  // A plain backdrop shows meanwhile.
  {
    Guard guard;
    resetLocked();
    snprintf(s_picture, sizeof(s_picture), "%s", picture);
  }

  // Order matters, and not for style. C++ destroys automatics in reverse
  // declaration order, and ~HTTPClient() does `if (_client) _client->stop()`
  // on a raw pointer to whichever client below was handed to begin(). Declared
  // the other way round, that pointer dangles: the client is destroyed first,
  // ~WiFiClientSecure has already closed the socket, and HTTPClient then closes
  // it a second time through a dead vtable. lwIP notices when the last ACK
  // arrives and trips `pbuf_free: p->ref > 0` on the tcpip thread, with nothing
  // of this firmware in the backtrace. Clients first, HTTPClient last.
  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  HTTPClient http;
  int content_length = -1;
  Format format = Format::kUnknown;

  bool resized = false;
  String url = pictureUrl(picture, &resized);
  bool opened = openArt(http, plain, secure, url, content_length, format);

  if (!opened && resized) {
    // The ladder of sizes a proxy accepts is its own business -- it answers
    // 400 with the list. Retry untouched, and stop rewriting for this session
    // rather than paying the failed request on every track.
    LOG_INFO(
        "Cover art: resized request refused, retrying the original URL");
    s_size_rewrite_ok = false;
    url = pictureUrl(picture);
    opened = openArt(http, plain, secure, url, content_length, format);
  }

  if (!opened) {
    Guard guard;
    s_picture[0] = '\0';
    return;
  }

  if (content_length > static_cast<int>(config::kCoverArtMaxBytes)) {
    LOG_WARN("Cover art: %d bytes is past the hard limit, skipping",
                  content_length);
    http.end();
    Guard guard;
    s_picture[0] = '\0';
    return;
  }

  // The buffer already exists; this only decides whether the image fits it.
  // No allocation happens per track, which is the whole point -- a free and a
  // differently-sized malloc on every track change is what fragments a heap.
  if (content_length <= 0 ||
      static_cast<size_t>(content_length) > s_buffer_capacity) {
    // No Content-Length to check against, or larger than the buffer: keep the
    // URL and re-stream on each repaint. Slower, and it costs a fetch each
    // time,
    // but the art still shows.
    http.end();
    Guard guard;
    s_format = format;
    s_stream_only = true;
    LOG_INFO("Cover art: %d bytes vs %u byte buffer, streaming instead",
                  content_length, static_cast<unsigned>(s_buffer_capacity));
    return;
  }

  const size_t len = static_cast<size_t>(content_length);
  // Read straight into the shared buffer, without holding the lock. The
  // invalidation above already left s_buffer_len at zero, so draw() will not
  // look at the buffer -- and taking the lock across a download would stall
  // repaints for the length of a fetch.
  const bool complete = readBody(http, s_buffer, len);
  http.end();

  Guard guard;
  if (!complete) {
    LOG_WARN("Cover art: short read, dropping");
    resetLocked();
    s_picture[0] = '\0';
    return;
  }

  s_buffer_len = len;
  s_format = format != Format::kUnknown ? format
                                        : formatFromMagic(s_buffer, len);
  if (s_format == Format::kPng) {
    pngSize(s_buffer, len, s_src_w, s_src_h);
  } else if (s_format == Format::kJpeg) {
    jpegSize(s_buffer, len, s_src_w, s_src_h);
  }
  LOG_DEBUG("Cover art: cached %u bytes, %ux%u",
                static_cast<unsigned>(len), s_src_w, s_src_h);
}

namespace {

/** The cover at its own size, for upscaleInto(), and the source column of
 *  each column of the box it is scaled into. PSRAM, kept between covers and
 *  remade only when a cover's size differs. */
/** The sprite object too, made on first use: static, it was 368 bytes of
 *  internal RAM. */
lgfx::LGFX_Sprite* s_native = nullptr;
int16_t* s_columns = nullptr;
int s_columns_len = 0;

/**
 * Draw the cached cover, smaller than the box, into the canvas -- decoded at
 * its own size into s_native, then scaled up into the frame's pixels a row at
 * a time. Nearest neighbour, as LovyanGFX's own scaled decode is, so it looks
 * the same; but LovyanGFX scales as it decodes, block by block through its
 * drawing calls, and on a 480 px panel in PSRAM that cost about 200 ms
 * whatever the source size. This is a decode of a quarter of the pixels and
 * then plain copies. False, having drawn nothing, when it cannot -- `dst` not
 * the canvas, no memory -- and the caller decodes the slow way instead.
 *
 * Under the art lock, like everything that reads s_buffer.
 */
bool upscaleInto(uint16_t* dst, int x, int y, int diameter, float scale) {
  const int w = s_src_w;
  const int h = s_src_h;
  if (s_native == nullptr) {
    void* mem = heap_caps_malloc(sizeof(lgfx::LGFX_Sprite),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
      return false;
    }
    s_native = new (mem) lgfx::LGFX_Sprite();
  }
  if (s_native->width() != w || s_native->height() != h) {
    s_native->deleteSprite();
    s_native->setColorDepth(ui::canvasColorDepth());
    s_native->setPsram(true);
    if (s_native->createSprite(w, h) == nullptr) {
      return false;
    }
  }
  if (s_columns_len < diameter) {
    heap_caps_free(s_columns);
    s_columns = static_cast<int16_t*>(heap_caps_malloc(
        sizeof(int16_t) * diameter, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_columns_len = s_columns != nullptr ? diameter : 0;
    if (s_columns == nullptr) {
      return false;
    }
  }
  const bool decoded =
      s_format == Format::kPng
          ? s_native->drawPng(s_buffer, s_buffer_len, 0, 0, w, h)
          : s_native->drawJpg(s_buffer, s_buffer_len, 0, 0, w, h);
  if (!decoded) {
    return false;
  }

  // Centre-crop: the source pixel under each box pixel, centres aligned.
  const float inv = 1.0f / scale;
  const float src_x0 = w * 0.5f - diameter * 0.5f * inv;
  const float src_y0 = h * 0.5f - diameter * 0.5f * inv;
  for (int i = 0; i < diameter; ++i) {
    int sx = static_cast<int>(src_x0 + (i + 0.5f) * inv);
    s_columns[i] = static_cast<int16_t>(sx < 0 ? 0 : sx >= w ? w - 1 : sx);
  }
  const auto* src = static_cast<const uint16_t*>(s_native->getBuffer());
  const int side = board::kDisplayWidth;
  const int col0 = x < 0 ? -x : 0;
  const int col1 = x + diameter > side ? side - x : diameter;
  int last_sy = -1;
  uint16_t* last_row = nullptr;
  for (int j = 0; j < diameter; ++j) {
    const int dy = y + j;
    if (dy < 0 || dy >= board::kDisplayHeight) {
      continue;
    }
    int sy = static_cast<int>(src_y0 + (j + 0.5f) * inv);
    sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
    uint16_t* row = dst + static_cast<size_t>(dy) * side + x;
    if (sy == last_sy && last_row != nullptr) {
      // The same source row as the one above: copy that rather than map it
      // again -- a 1.9x scale repeats most rows.
      memcpy(row + col0, last_row + col0,
             sizeof(uint16_t) * static_cast<size_t>(col1 - col0));
    } else {
      const uint16_t* line = src + static_cast<size_t>(sy) * w;
      for (int i = col0; i < col1; ++i) {
        row[i] = line[s_columns[i]];
      }
    }
    last_sy = sy;
    last_row = row;
  }
  return true;
}

}  // namespace

bool draw(lgfx::LGFXBase& gfx, int x, int y, int diameter) {
  Guard guard;

  if (!hasArtLocked() || diameter <= 0) {
    return false;
  }

  if (s_buffer != nullptr && s_buffer_len > 0) {
    // Centre-crop: scale so the shorter source edge covers the circle and let
    // LovyanGFX clip the overhang against the box. A scale of 0 means fit
    // inside the box instead, which is what happens when the header did not
    // give up its dimensions.
    float scale = 0.0f;
    if (s_src_w > 0 && s_src_h > 0) {
      const float sx = static_cast<float>(diameter) / s_src_w;
      const float sy = static_cast<float>(diameter) / s_src_h;
      scale = sx > sy ? sx : sy;
    }
    // Smaller than the box and going into the canvas: decode at its own size
    // and scale it up in one pass, rather than through the scaled decode.
    if (scale > 1.0f) {
      uint16_t* dst = ui::canvasPixels(gfx);
      if (dst != nullptr && upscaleInto(dst, x, y, diameter, scale)) {
        return true;
      }
    }
    const bool drawn =
        s_format == Format::kPng
            ? gfx.drawPng(s_buffer, s_buffer_len, x, y, diameter, diameter, 0,
                          0, scale, scale, datum_t::middle_center)
            : gfx.drawJpg(s_buffer, s_buffer_len, x, y, diameter, diameter, 0,
                          0, scale, scale, datum_t::middle_center);
    if (!drawn) {
      LOG_WARN(
          "Cover art: decode failed (%u bytes, %ux%u, fmt %d, scale %.3f)",
          static_cast<unsigned>(s_buffer_len), s_src_w, s_src_h,
          static_cast<int>(s_format), scale);
    }
    return drawn;
  }

  if (!s_stream_only || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // Order matters, and not for style. C++ destroys automatics in reverse
  // declaration order, and ~HTTPClient() does `if (_client) _client->stop()`
  // on a raw pointer to whichever client below was handed to begin(). Declared
  // the other way round, that pointer dangles: the client is destroyed first,
  // ~WiFiClientSecure has already closed the socket, and HTTPClient then closes
  // it a second time through a dead vtable. lwIP notices when the last ACK
  // arrives and trips `pbuf_free: p->ref > 0` on the tcpip thread, with nothing
  // of this firmware in the backtrace. Clients first, HTTPClient last.
  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  HTTPClient http;
  int content_length = -1;
  Format format = Format::kUnknown;
  if (!openArt(http, plain, secure, pictureUrl(s_picture), content_length,
               format)) {
    return false;
  }
  if (format == Format::kUnknown) {
    format = s_format;
  }

  StreamWrapper source(
      http.getStreamPtr(),
      content_length > 0 ? static_cast<uint32_t>(content_length) : ~0u);
  // Dimensions are unknown without buffering the header, so this path fits the
  // art inside the circle rather than cropping it to fill (scale 0 = fit).
  // Square art -- which is nearly all album art -- lands identically either
  // way.
  const bool drawn =
      format == Format::kPng
          ? gfx.drawPng(&source, x, y, diameter, diameter, 0, 0, 0.0f, 0.0f,
                        datum_t::middle_center)
          : gfx.drawJpg(&source, x, y, diameter, diameter, 0, 0, 0.0f, 0.0f,
                        datum_t::middle_center);
  http.end();
  if (!drawn) {
    LOG_WARN("Cover art: streamed decode failed (%d bytes, fmt %d)",
                  content_length, static_cast<int>(format));
  }
  return drawn;
}

bool fetchThumb(lgfx::LGFXBase& gfx, const char* url, int x, int y,
                int size) {
  if (url == nullptr || url[0] == '\0' || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  ThumbConnection& conn = thumbConnection();
  conn.http.setReuse(true);
  if (s_thumb_buffer == nullptr) {
    s_thumb_buffer = static_cast<uint8_t*>(
        heap_caps_malloc(kThumbBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }

  bool rewritten = false;
  String target = thumbnailUrl(url, size, rewritten);
  bool retried_stale = false;

  // At most three: once more if a kept-alive connection turns out to have
  // been dropped by the server, and once more unresized if the host refused
  // the size asked for.
  for (int attempt = 0; attempt < 3; ++attempt) {
    const String origin = originOf(target);
    if (origin.length() == 0 || origin != conn.origin) {
      conn.close();  // HTTPClient would otherwise send this host's request to the last one
      conn.origin = origin;
    }
    WiFiClient& client = target.startsWith("https://")
                             ? static_cast<WiFiClient&>(conn.secure)
                             : conn.plain;
    const bool reusing = client.connected();

    int content_length = -1;
    Format format = Format::kUnknown;
    const bool opened = openArt(conn.http, conn.plain, conn.secure, target,
                                content_length, format);

    if (!opened) {
      // Whatever a refusal left on the wire -- the rest of an error body --
      // must not be read as the start of the next response.
      conn.close();
      if (reusing && !retried_stale) {
        retried_stale = true;
        continue;
      }
      if (rewritten && target != url) {
        // Possibly the host declining the size we asked for -- it answers 400
        // with the sizes it will accept. Retry untouched and take the
        // bandwidth hit once.
        LOG_DEBUG("Art: %s refused, retrying unresized", target.c_str());
        target = String(url);
        continue;
      }
      // Internal RAM at the moment it failed. A fresh WiFiClientSecure needs
      // tens of KB of it for the mbedTLS context, and when that allocation is
      // what failed the request never reaches the wire -- which reads as the
      // host refusing something it was never asked.
      LOG_WARN("Art: open failed (internal heap free %u, largest %u)",
                    static_cast<unsigned>(heap_caps_get_free_size(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      return false;
    }

    bool drawn = false;
    int bytes = content_length;
    const bool buffered =
        s_thumb_buffer != nullptr &&
        content_length <= static_cast<int>(kThumbBufferBytes);  // -1 too

    if (buffered) {
      // Download all of it first, then decode from memory: the lock below is
      // shared with the cover art the loop draws, and holding it across a
      // network read would stall the now-playing screen for the length of it.
      BufferSink sink(s_thumb_buffer, kThumbBufferBytes);
      const int got = conn.http.writeToStream(&sink);
      if (got <= 0 || sink.overflowed()) {
        LOG_WARN("Art: body failed for %s (%d%s)", target.c_str(), got,
                      sink.overflowed() ? ", larger than the buffer" : "");
        conn.close();
        conn.http.end();
        return false;
      }
      bytes = static_cast<int>(sink.length());
      if (format == Format::kUnknown) {
        format = formatFromMagic(s_thumb_buffer, sink.length());
      }

      // Serialised against every other decode on the device: LovyanGFX keeps
      // ONE pngle_t in a file-scope static (LGFXBase.cpp: "static pngle_t*
      // pngle"), so two tasks decoding PNGs at once tear up each other's
      // state, and the damage surfaces later as a wild pointer elsewhere.
      Guard guard;
      // Fit, not crop: artwork is often wider than it is tall and carries a
      // name, so cropping to a square would cut it in half. Scale 0 = fit.
      drawn = format == Format::kPng
                  ? gfx.drawPng(s_thumb_buffer, sink.length(), x, y, size, size,
                                0, 0, 0.0f, 0.0f, datum_t::middle_center)
                  : gfx.drawJpg(s_thumb_buffer, sink.length(), x, y, size, size,
                                0, 0, 0.0f, 0.0f, datum_t::middle_center);
    } else {
      // Too big to hold: decode off the socket, as every thumbnail once did.
      StreamWrapper source(
          conn.http.getStreamPtr(),
          content_length > 0 ? static_cast<uint32_t>(content_length) : ~0u);
      Guard guard;
      drawn = format == Format::kPng
                  ? gfx.drawPng(&source, x, y, size, size, 0, 0, 0.0f, 0.0f,
                                datum_t::middle_center)
                  : gfx.drawJpg(&source, x, y, size, size, 0, 0, 0.0f, 0.0f,
                                datum_t::middle_center);
      // A decoder that stops at the end of the image can leave bytes of it
      // unread, and those would be read as the next response's headers.
      if (content_length <= 0 || source.tell() != content_length) {
        conn.close();
      }
    }
    conn.http.end();  // keeps the connection open when the server allows it

    if (!drawn) {
      // Name the likely cause rather than leaving a bare failure. TJpgD
      // decodes baseline JPEG only -- lgfx_tjpgd.c returns JDR_FMT3 with the
      // comment "may be progressive JPEG" -- and some hosts serve nothing
      // else at any size, so this is a limitation to recognise, not a bug.
      LOG_WARN(
          "Art: decode failed for %s (%d bytes, %s)%s", target.c_str(), bytes,
          format == Format::kPng ? "png" : "jpeg",
          format == Format::kPng
              ? ""
              : " - progressive JPEG is not supported by this decoder");
    }
    return drawn;
  }
  return false;
}

void releaseThumbConnection() {
  ThumbConnection& conn = thumbConnection();
  conn.close();
  conn.origin = String();
}

}  // namespace ui::cover
