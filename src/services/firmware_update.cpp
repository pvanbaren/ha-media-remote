#include "services/firmware_update.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "log.h"
#include "services/device_name.h"
#include "services/wifi_setup.h"
#include "services/yielding_client.h"

// scripts/firmware_version.py defines both for this file alone.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif
#ifndef FIRMWARE_ENV
#define FIRMWARE_ENV "unknown"
#endif

// The root certificates ESP-IDF builds in: its full bundle, the one a
// browser would trust. The core's TLS client takes it as a bundle to check
// the server against, rather than checking nothing.
extern const uint8_t kRootsStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kRootsEnd[] asm("_binary_x509_crt_bundle_end");

namespace services::update {
namespace {

/** The task the network work runs on, with its stack in PSRAM. Not the
 *  caller's: the loop's 8 KB stack, under a settings tap or a portal page, is
 *  too little for a TLS handshake on top -- the Home Assistant link's ran out
 *  of room there. And not in internal RAM, which the handshake wants all it
 *  can get of: on the Guition, with 56 KB of it free, a 12 KB stack there
 *  left too little and every connection to GitHub failed.
 *
 *  A stack in PSRAM is out of reach while flash is written -- the cache is
 *  off -- so this task never writes it. It downloads; the caller, on its own
 *  stack, writes what comes down. */
constexpr uint32_t kTaskStackBytes = 12288;
/** How much comes down before it is handed over to be written. */
constexpr size_t kChunkBytes = 16384;

/** A tag as releases are named, and nothing that could leave the path. */
bool validTag(const char* tag) {
  const size_t len = tag != nullptr ? strlen(tag) : 0;
  if (len == 0 || len >= sizeof(CheckResult::tag)) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = tag[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' &&
        c != '_') {
      return false;
    }
  }
  return true;
}

/** "v1.5.0", or "v1.5.0-23-gc83026a", as major, minor and patch. Anything
 *  after the patch is a build after that release, and counts as it. */
bool parseVersion(const char* text, long out[3]) {
  const char* p = text;
  if (*p == 'v' || *p == 'V') {
    ++p;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isdigit(static_cast<unsigned char>(*p))) {
      return false;
    }
    char* end = nullptr;
    out[i] = strtol(p, &end, 10);
    p = end;
    if (i < 2) {
      if (*p != '.') {
        return false;
      }
      ++p;
    }
  }
  return true;
}

/** Whether release `latest` is newer than this build. A build whose version
 *  does not read as one -- a bare hash, built where no tag could be seen --
 *  is taken as older than any release. */
bool isNewer(const char* latest) {
  long have[3];
  long want[3];
  if (!parseVersion(latest, want)) {
    return strcmp(latest, FIRMWARE_VERSION) != 0;
  }
  if (!parseVersion(FIRMWARE_VERSION, have)) {
    return true;
  }
  for (int i = 0; i < 3; ++i) {
    if (want[i] != have[i]) {
      return want[i] > have[i];
    }
  }
  return false;
}

void setError(char* out, size_t len, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(out, len, format, args);
  va_end(args);
}

/** What a failed HTTPClient call says, in words. */
void describeHttp(char* out, size_t len, int code) {
  if (code < 0) {
    setError(out, len, "%s", HTTPClient::errorToString(code).c_str());
  } else {
    setError(out, len, "GitHub answered %d", code);
  }
}

void prepare(YieldingClientSecure& client, HTTPClient& http) {
  client.setCACertBundle(kRootsStart, kRootsEnd - kRootsStart);
  http.setConnectTimeout(config::kFirmwareTimeoutMs);
  http.setTimeout(config::kFirmwareTimeoutMs);
  http.setUserAgent("ha-media-remote/" FIRMWARE_VERSION);
  // A new connection for each request. Kept open, HTTPClient sends a
  // redirect's request down the connection it has -- the download's, to
  // GitHub's file host, would go to github.com.
  http.setReuse(false);
}

/** The release image for this board, at `tag`. */
String imageUrl(const char* tag) {
  String url("https://github.com/");
  url += config::kFirmwareRepo;
  url += "/releases/download/";
  url += tag;
  url += "/firmware-" FIRMWARE_ENV ".bin";
  return url;
}

// --- The work, on its own task ------------------------------------------------

/** The one job running, or last run. The task writes it until `done`; the
 *  caller reads it after. Meanwhile they share only the atomics: the
 *  download's length, and the chunk handed over -- its length set by the task
 *  once it is full, put back to 0 by the caller once it is written. */
struct Job {
  bool install = false;
  /** What a download fetches. */
  char url[200] = {};
  CheckResult check;
  bool ok = false;
  char error[sizeof(CheckResult::error)] = {};
  uint8_t* chunk = nullptr;
  std::atomic<int> total{0};
  std::atomic<int> chunk_len{0};
  /** The caller could not write: the task stops downloading. */
  std::atomic<bool> abort{false};
  std::atomic<bool> done{false};
};
Job s_job;
std::atomic<bool> s_busy{false};

/** The task's stack and control block, claimed on first use and kept: a
 *  task cannot free its own stack, and one freed under it is worse. */
StackType_t* s_stack = nullptr;
StaticTask_t s_tcb;
TaskHandle_t s_task = nullptr;

/** The latest release's tag: github.com's /releases/latest answers with a
 *  redirect to the release's own page, which ends in it. That is one small
 *  answer, where the API's is tens of KB of notes and assets to read. */
void runCheck(CheckResult& out) {
  YieldingClientSecure client;
  HTTPClient http;
  prepare(client, http);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* keys[] = {"Location"};

  String url("https://github.com/");
  url += config::kFirmwareRepo;
  url += "/releases/latest";
  if (!http.begin(client, url)) {
    setError(out.error, sizeof(out.error), "bad address");
    return;
  }
  http.collectHeaders(keys, 1);
  int code = http.GET();
  const String location = http.header("Location");
  http.end();
  if (code == 404) {
    setError(out.error, sizeof(out.error), "no release on GitHub");
    return;
  }
  if (code != 301 && code != 302) {
    describeHttp(out.error, sizeof(out.error), code);
    return;
  }
  const int at = location.lastIndexOf("/releases/tag/");
  const String tag = at >= 0 ? location.substring(at + 14) : String();
  if (!validTag(tag.c_str())) {
    setError(out.error, sizeof(out.error), "no release tag in GitHub's answer");
    return;
  }
  snprintf(out.tag, sizeof(out.tag), "%s", tag.c_str());

  if (!isNewer(out.tag)) {
    out.result = Check::kUpToDate;
    return;
  }
  // Newer: is there an image for this board in it? Its download answers
  // with a redirect to where the file is kept, or 404.
  if (!http.begin(client, imageUrl(out.tag))) {
    setError(out.error, sizeof(out.error), "bad address");
    return;
  }
  code = http.sendRequest("HEAD");
  http.end();
  if (code == 301 || code == 302 || code == 200) {
    out.result = Check::kNewer;
  } else if (code == 404) {
    out.result = Check::kNoImage;
  } else {
    describeHttp(out.error, sizeof(out.error), code);
  }
}

/** Hand the chunk over and wait for the caller to take it; false if it gave
 *  up instead. */
bool handOver(int len) {
  s_job.chunk_len.store(len);
  while (s_job.chunk_len.load() != 0) {
    if (s_job.abort.load()) {
      return false;
    }
    delay(1);
  }
  return true;
}

/** Download `url`, a chunk at a time, for the caller to write. */
bool runDownload(const char* url, char* error, size_t error_len) {
  YieldingClientSecure client;
  HTTPClient http;
  prepare(client, http);
  // GitHub sends the download on to where it keeps its files, another host.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  if (!http.begin(client, url)) {
    setError(error, error_len, "bad address");
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    describeHttp(error, error_len, code);
    http.end();
    return false;
  }
  const int total = http.getSize();
  if (total <= 0) {
    setError(error, error_len, "the download came with no length");
    http.end();
    return false;
  }
  s_job.total.store(total);

  NetworkClient* stream = http.getStreamPtr();
  int received = 0;
  size_t filled = 0;
  unsigned long heard_ms = millis();
  bool ok = true;
  while (received < total) {
    if (s_job.abort.load()) {
      ok = false;
      break;
    }
    const int ready = stream != nullptr ? stream->available() : -1;
    if (ready <= 0) {
      if (stream == nullptr ||
          (!stream->connected() && stream->available() <= 0) ||
          millis() - heard_ms > config::kFirmwareTimeoutMs) {
        setError(error, error_len, "the download stopped at %d%%",
                 static_cast<int>(100LL * received / total));
        ok = false;
        break;
      }
      continue;  // available() has slept a millisecond
    }
    size_t want = kChunkBytes - filled;
    want = want < static_cast<size_t>(ready) ? want
                                             : static_cast<size_t>(ready);
    const int got = stream->read(s_job.chunk + filled, want);
    if (got <= 0) {
      continue;
    }
    filled += static_cast<size_t>(got);
    received += got;
    heard_ms = millis();
    if (filled == kChunkBytes || received >= total) {
      if (!handOver(static_cast<int>(filled))) {
        ok = false;
        break;
      }
      filled = 0;
    }
  }
  http.end();
  return ok;
}

void worker(void*) {
  if (s_job.install) {
    s_job.ok = runDownload(s_job.url, s_job.error, sizeof(s_job.error));
  } else {
    runCheck(s_job.check);
  }
  s_job.done.store(true);
  // The caller deletes it: its stack is s_stack, kept for the next job.
  vTaskSuspend(nullptr);
}

/** Start the job set up in s_job on the task. False, with `error` saying
 *  why, when it could not: no Wi-Fi, or no memory for the task. */
bool startJob(char* error, size_t error_len) {
  if (WiFi.status() != WL_CONNECTED) {
    setError(error, error_len, "no Wi-Fi");
    return false;
  }
  if (s_stack == nullptr) {
    s_stack = static_cast<StackType_t*>(heap_caps_malloc(
        kTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_job.install && s_job.chunk == nullptr) {
    s_job.chunk = static_cast<uint8_t*>(
        heap_caps_malloc(kChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_stack == nullptr || (s_job.install && s_job.chunk == nullptr)) {
    setError(error, error_len, "no memory to start");
    return false;
  }
  s_job.total.store(0);
  s_job.chunk_len.store(0);
  s_job.abort.store(false);
  s_job.done.store(false);
  s_task = xTaskCreateStatic(worker, "fw_update", kTaskStackBytes, nullptr,
                             tskIDLE_PRIORITY + 1, s_stack, &s_tcb);
  if (s_task == nullptr) {
    setError(error, error_len, "no memory to start");
    return false;
  }
  return true;
}

/** Wait for the task to finish, then delete it, so its stack and control
 *  block can be used again. */
void endJob() {
  while (!s_job.done.load()) {
    delay(5);
  }
  // Deleted only once it has suspended itself, not while it may still be
  // running on the other core: its stack and control block are used again.
  for (int i = 0; i < 200 && s_task != nullptr &&
                  eTaskGetState(s_task) != eSuspended;
       ++i) {
    delay(1);
  }
  if (s_task != nullptr) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
}

/** Hand what the task downloads to `sink`, here, on the caller's stack,
 *  calling `progress` with each new percentage. */
bool writeDownload(const Sink& sink, void (*progress)(int percent),
                   char* error, size_t error_len) {
  bool begun = false;
  bool ok = true;
  int written = 0;
  int shown = -1;
  int total = 0;
  if (progress != nullptr) {
    progress(0);
  }
  for (;;) {
    const int len = s_job.chunk_len.load();
    if (len > 0) {
      total = s_job.total.load();
      if (!begun) {
        begun = true;
        if (!sink.begin(static_cast<size_t>(total), error, error_len)) {
          begun = false;
          ok = false;
          break;
        }
      }
      if (!sink.write(s_job.chunk, static_cast<size_t>(len), error,
                      error_len)) {
        ok = false;
        break;
      }
      written += len;
      s_job.chunk_len.store(0);
      const int pct = static_cast<int>(100LL * written / total);
      if (progress != nullptr && pct != shown) {
        shown = pct;
        progress(pct);
      }
    } else if (s_job.done.load()) {
      break;
    } else {
      delay(2);
    }
  }
  if (!ok) {
    s_job.abort.store(true);
  }
  endJob();
  if (ok && !s_job.ok) {
    setError(error, error_len, "%s", s_job.error);
    ok = false;
  }
  if (ok && (!begun || written != total)) {
    setError(error, error_len, "the download came down short");
    ok = false;
  }
  if (!ok) {
    if (begun) {
      sink.abort();
    }
    return false;
  }
  return sink.end(error, error_len);
}

// The image goes into the app slot not running. Update checks it as it ends
// -- its length, its checksum, and that it is for this chip -- and only then
// marks it to boot.

bool imageBegin(size_t total, char* error, size_t error_len) {
  if (!Update.begin(total)) {
    setError(error, error_len, "%s", Update.errorString());
    return false;
  }
  return true;
}

bool imageWrite(const uint8_t* data, size_t len, char* error,
                size_t error_len) {
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    setError(error, error_len, "%s", Update.errorString());
    return false;
  }
  return true;
}

bool imageEnd(char* error, size_t error_len) {
  if (!Update.end()) {
    setError(error, error_len, "%s", Update.errorString());
    return false;
  }
  return true;
}

void imageAbort() { Update.abort(); }

constexpr Sink kImageSink = {imageBegin, imageWrite, imageEnd, imageAbort};

// --- The portal page ------------------------------------------------------------

/** The tag the page asked to install, for the loop. */
char s_requested_tag[sizeof(CheckResult::tag)] = {};
bool s_install_requested = false;
/** Why the last install failed, for the page; empty when none has. */
char s_last_install_error[sizeof(CheckResult::error)] = {};

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
 * /firmware: which firmware this is, whether GitHub has a newer one, and a
 * button to install it.
 *
 * The check is on request, ?check=1, and blocks the page a few seconds. The
 * install button posts its tag; the page answers at once and the loop does
 * the download, with the progress on the remote's own screen -- the page
 * would wait the whole time otherwise, and a browser gives up first.
 */
void handlePage(WebServer& server) {
  String page;
  page.reserve(2048);
  page += "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width,initial-scale=1'><title>";
  appendEscaped(page, services::device::name());
  page += " - Firmware</title></head><body style='font-family:verdana'>"
          "<h3>Firmware</h3><p>This remote runs <b>";
  appendEscaped(page, FIRMWARE_VERSION);
  page += "</b>, built for <b>" FIRMWARE_ENV "</b>.</p>";

  if (server.method() == HTTP_POST) {
    const String tag = server.arg("tag");
    if (!validTag(tag.c_str())) {
      page += "<p>That is not a release.</p>";
    } else if (s_busy.load() || s_install_requested) {
      page += "<p>An update is already under way.</p>";
    } else {
      snprintf(s_requested_tag, sizeof(s_requested_tag), "%s", tag.c_str());
      s_install_requested = true;
      page += "<p>Installing <b>";
      appendEscaped(page, s_requested_tag);
      page += "</b>. The remote downloads it, showing how far it has got on "
              "its screen, and restarts -- a minute or so. Then come back to "
              "this page to see the version it runs.</p>";
    }
  } else if (server.hasArg("check")) {
    CheckResult result;
    check(result);
    switch (result.result) {
      case Check::kNewer:
        page += "<p><b>";
        appendEscaped(page, result.tag);
        page += "</b> is out.</p><form method='post' action='/firmware'>"
                "<input type='hidden' name='tag' value='";
        appendEscaped(page, result.tag);
        page += "'><button>Install ";
        appendEscaped(page, result.tag);
        page += "</button></form><p><a href='https://github.com/";
        page += config::kFirmwareRepo;
        page += "/releases/tag/";
        appendEscaped(page, result.tag);
        page += "'>What is new in it</a></p>";
        break;
      case Check::kUpToDate:
        page += "<p>Up to date: <b>";
        appendEscaped(page, result.tag);
        page += "</b> is the latest release.</p>";
        break;
      case Check::kNoImage:
        page += "<p><b>";
        appendEscaped(page, result.tag);
        page += "</b> is out, but has no image for this board.</p>";
        break;
      case Check::kFailed:
        page += "<p>Could not ask GitHub: ";
        appendEscaped(page, result.error);
        page += ".</p>";
        break;
    }
  }
  if (s_last_install_error[0] != '\0') {
    page += "<p>The last install failed: ";
    appendEscaped(page, s_last_install_error);
    page += ".</p>";
  }
  page += "<p><a href='/firmware?check=1'>Check for an update</a> (a few "
          "seconds) &middot; <a href='/'>Menu</a></p></body></html>";
  server.send(200, "text/html", page);
}

}  // namespace

const char* currentVersion() { return FIRMWARE_VERSION; }

const char* boardEnv() { return FIRMWARE_ENV; }

void check(CheckResult& out) {
  out = CheckResult{};
  bool expected = false;
  if (!s_busy.compare_exchange_strong(expected, true)) {
    setError(out.error, sizeof(out.error), "an update is under way");
    return;
  }
  s_job.install = false;
  s_job.check = CheckResult{};
  if (startJob(out.error, sizeof(out.error))) {
    endJob();
    out = s_job.check;
  }
  s_busy.store(false);
  switch (out.result) {
    case Check::kNewer:
      LOG_INFO("Firmware: %s is out (this is %s)", out.tag, FIRMWARE_VERSION);
      break;
    case Check::kUpToDate:
      LOG_INFO("Firmware: up to date, %s is the latest (this is %s)", out.tag,
               FIRMWARE_VERSION);
      break;
    case Check::kNoImage:
      LOG_INFO("Firmware: %s has no image for " FIRMWARE_ENV, out.tag);
      break;
    case Check::kFailed:
      LOG_WARN("Firmware: check failed: %s", out.error);
      break;
  }
}

bool install(const char* tag, void (*progress)(int percent), char* error,
             size_t error_len) {
  if (!validTag(tag)) {
    setError(error, error_len, "not a release");
    return false;
  }
  bool expected = false;
  if (!s_busy.compare_exchange_strong(expected, true)) {
    setError(error, error_len, "an update is under way");
    return false;
  }
  LOG_INFO("Firmware: installing %s for " FIRMWARE_ENV " (this is %s)", tag,
           FIRMWARE_VERSION);
  s_job.install = true;
  snprintf(s_job.url, sizeof(s_job.url), "%s", imageUrl(tag).c_str());
  s_job.ok = false;
  s_job.error[0] = '\0';
  const bool ok = startJob(error, error_len) &&
                  writeDownload(kImageSink, progress, error, error_len);
  s_busy.store(false);
  if (ok) {
    s_last_install_error[0] = '\0';
    LOG_INFO("Firmware: %s written, boots next", tag);
  } else {
    snprintf(s_last_install_error, sizeof(s_last_install_error), "%s", error);
    LOG_WARN("Firmware: install of %s failed: %s", tag, error);
  }
  return ok;
}

bool download(const char* url, const Sink& sink,
              void (*progress)(int percent), char* error, size_t error_len) {
  if (url == nullptr || strlen(url) >= sizeof(s_job.url)) {
    setError(error, error_len, "bad address");
    return false;
  }
  bool expected = false;
  if (!s_busy.compare_exchange_strong(expected, true)) {
    setError(error, error_len, "an update is under way");
    return false;
  }
  s_job.install = true;
  snprintf(s_job.url, sizeof(s_job.url), "%s", url);
  s_job.ok = false;
  s_job.error[0] = '\0';
  const bool ok = startJob(error, error_len) &&
                  writeDownload(sink, progress, error, error_len);
  s_busy.store(false);
  return ok;
}

void init() { wifiAddWebPage("/firmware", handlePage); }

bool takeInstallRequest(char* tag, size_t tag_len) {
  if (!s_install_requested) {
    return false;
  }
  s_install_requested = false;
  snprintf(tag, tag_len, "%s", s_requested_tag);
  return true;
}

}  // namespace services::update
