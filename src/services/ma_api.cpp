#include "services/ma_api.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "log.h"
#include "ha_internal.h"
#include "services/ha_client.h"
#include "services/yielding_client.h"

namespace services::ma {
namespace {

/** Its own namespace, and cleared by the BOOT reset with the Home Assistant
 *  settings: the token is a credential like theirs. */
constexpr char kPrefsNamespace[] = "ma";
constexpr char kPrefsUrlKey[] = "url";
constexpr char kPrefsTokenKey[] = "token";

char s_url[config::kMaUrlMaxLen + 1] = {};
char s_token[config::kMaTokenMaxLen + 1] = {};
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> s_generation{0};
char s_last_error[96] = {};

/** Collects a response body into the caller's buffer, for writeToStream():
 *  that call copes with both a Content-Length and a chunked body. */
class BufferSink : public Stream {
 public:
  BufferSink(char* buffer, size_t capacity)
      : _buffer(buffer), _capacity(capacity) {}

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    if (len > _capacity - _length) {
      _overflowed = true;
      return 0;  // a short write ends writeToStream() with an error
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
  char* _buffer;
  size_t _capacity;
  size_t _length = 0;
  bool _overflowed = false;
};

void copyLocked(char* out, size_t out_len, const char* from) {
  taskENTER_CRITICAL(&s_lock);
  strlcpy(out, from, out_len);
  taskEXIT_CRITICAL(&s_lock);
}

void setLocked(char* to, size_t to_len, const char* value) {
  taskENTER_CRITICAL(&s_lock);
  strlcpy(to, value, to_len);
  taskEXIT_CRITICAL(&s_lock);
}

/** Spaces and control characters off both ends, in place, as ha_client does
 *  for its settings: none belongs in a URL or a token, and a pasted line
 *  break would otherwise be stored as part of one. True when any went. */
bool trimSetting(char* s) {
  const size_t len = strlen(s);
  size_t start = 0;
  while (start < len && static_cast<unsigned char>(s[start]) <= ' ') {
    ++start;
  }
  size_t end = len;
  while (end > start && static_cast<unsigned char>(s[end - 1]) <= ' ') {
    --end;
  }
  if (start == 0 && end == len) {
    return false;
  }
  memmove(s, s + start, end - start);
  s[end - start] = '\0';
  return true;
}

}  // namespace

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.getString(kPrefsUrlKey, s_url, sizeof(s_url));
  prefs.getString(kPrefsTokenKey, s_token, sizeof(s_token));
  // Stored before settings were trimmed on the way in: trimmed now, and the
  // repair stored, an empty value removed.
  const bool url_trimmed = trimSetting(s_url);
  const bool token_trimmed = trimSetting(s_token);
  if (url_trimmed || token_trimmed) {
    LOG_WARN("MA: stored settings had spaces or control characters, trimmed");
    if (s_url[0] != '\0') {
      prefs.putString(kPrefsUrlKey, s_url);
    } else {
      prefs.remove(kPrefsUrlKey);
    }
    if (s_token[0] != '\0') {
      prefs.putString(kPrefsTokenKey, s_token);
    } else {
      prefs.remove(kPrefsTokenKey);
    }
  }
  prefs.end();
  char url[config::kMaUrlMaxLen + 1];
  effectiveUrl(url, sizeof(url));
  LOG_INFO("MA: %s, %s", url[0] != '\0' ? url : "no address",
                s_token[0] != '\0' ? "token stored" : "no token");
}

bool configured() {
  if (!hasStoredToken()) {
    return false;
  }
  char url[config::kMaUrlMaxLen + 1];
  effectiveUrl(url, sizeof(url));
  return url[0] != '\0';
}

const char* storedUrl() { return s_url; }

void effectiveUrl(char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  copyLocked(out, out_len, s_url);
  if (out[0] != '\0') {
    return;
  }
  // The add-on's port on Home Assistant's own host, over plain HTTP.
  char host[config::kHaBaseUrlMaxLen + 1];
  char prefix[config::kHaBaseUrlMaxLen + 1];
  uint16_t port = 0;
  bool tls = false;
  if (!ha::detail::serverAddress(host, sizeof(host), port, tls, prefix,
                                 sizeof(prefix))) {
    return;
  }
  snprintf(out, out_len, "http://%s:%u", host,
           static_cast<unsigned>(config::kMaDefaultPort));
}

bool hasStoredToken() {
  taskENTER_CRITICAL(&s_lock);
  const bool has = s_token[0] != '\0';
  taskEXIT_CRITICAL(&s_lock);
  return has;
}

void saveSettings(const char* url, const char* token) {
  char cleaned[config::kMaUrlMaxLen + 1] = {};
  if (url != nullptr) {
    // Trimmed, and without a trailing slash, so "<url>/api" is one slash.
    strlcpy(cleaned, url, sizeof(cleaned));
    trimSetting(cleaned);
    size_t n = strlen(cleaned);
    while (n > 0 && cleaned[n - 1] == '/') {
      cleaned[--n] = '\0';
    }
  }
  // Whitespace alone is no token, so it keeps the stored one as blank does.
  char entered[sizeof(s_token)] = {};
  if (token != nullptr) {
    strlcpy(entered, token, sizeof(entered));
    trimSetting(entered);
  }
  token = entered;
  const bool url_changed = strcmp(cleaned, s_url) != 0;
  const bool token_entered = token[0] != '\0';
  if (!url_changed && !token_entered) {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  if (url_changed) {
    setLocked(s_url, sizeof(s_url), cleaned);
    prefs.putString(kPrefsUrlKey, s_url);
  }
  if (token_entered) {
    setLocked(s_token, sizeof(s_token), token);
    prefs.putString(kPrefsTokenKey, s_token);
  }
  prefs.end();
  ++s_generation;
  char effective[config::kMaUrlMaxLen + 1];
  effectiveUrl(effective, sizeof(effective));
  LOG_INFO("MA: settings saved, %s", effective);
}

void clearToken() {
  if (!hasStoredToken()) {
    return;
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsTokenKey);
    prefs.end();
  }
  setLocked(s_token, sizeof(s_token), "");
  ++s_generation;
  LOG_INFO("MA: token cleared");
}

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  setLocked(s_url, sizeof(s_url), "");
  setLocked(s_token, sizeof(s_token), "");
  ++s_generation;
}

uint32_t generation() { return s_generation; }

int call(const char* command, const String& args, char* out, size_t out_len,
         uint32_t timeout_ms) {
  if (out == nullptr || out_len < 2) {
    return -1;
  }
  out[0] = '\0';
  char url[config::kMaUrlMaxLen + 8];
  effectiveUrl(url, config::kMaUrlMaxLen + 1);
  char token[config::kMaTokenMaxLen + 1];
  copyLocked(token, sizeof(token), s_token);
  if (url[0] == '\0' || token[0] == '\0') {
    snprintf(s_last_error, sizeof(s_last_error), "not configured");
    return -1;
  }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(s_last_error, sizeof(s_last_error), "wifi down");
    return -1;
  }
  strlcat(url, "/api", sizeof(url));

  String body("{\"command\":");
  ha::detail::appendJson(body, command);
  body += ",\"args\":";
  body += args;
  body += '}';

  // Clients before the HTTPClient: automatics are destroyed in reverse, and
  // ~HTTPClient() stops whichever client it was given -- which must still
  // exist when it does. See cover_art.cpp.
  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  HTTPClient http;
  const bool tls = strncmp(url, "https://", 8) == 0;
  WiFiClient& client = tls ? static_cast<WiFiClient&>(secure) : plain;
  if (tls) {
    secure.setInsecure();  // a LAN server's certificate, and no clock to check it by
  }
  http.setReuse(false);
  http.setTimeout(timeout_ms);
  http.setConnectTimeout(config::kHaHttpTimeoutMs);
  if (!http.begin(client, url)) {
    snprintf(s_last_error, sizeof(s_last_error), "bad URL");
    return -1;
  }
  http.addHeader("Authorization", String("Bearer ") + token);
  http.addHeader("Content-Type", "application/json");

  const unsigned long started_ms = millis();
  const int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    snprintf(s_last_error, sizeof(s_last_error), "%s",
             code == HTTP_CODE_UNAUTHORIZED ? "token rejected"
             : code > 0                     ? "server refused the command"
                                            : "no answer");
    ha::logHttpRequest("POST", url, code, body.length(), -1,
                       millis() - started_ms);
    LOG_WARN("MA: %s: %s (%d)", command, s_last_error, code);
    http.end();
    return -1;
  }

  BufferSink sink(out, out_len - 1);
  const int got = http.writeToStream(&sink);
  http.end();
  ha::logHttpRequest("POST", url, code, body.length(),
                     static_cast<int>(sink.length()), millis() - started_ms);
  if (got < 0 || sink.overflowed()) {
    snprintf(s_last_error, sizeof(s_last_error), "%s",
             sink.overflowed() ? "answer too large" : "answer cut short");
    LOG_WARN("MA: %s: %s", command, s_last_error);
    return -1;
  }
  out[sink.length()] = '\0';
  s_last_error[0] = '\0';
  return static_cast<int>(sink.length());
}

const char* lastError() { return s_last_error; }

}  // namespace services::ma
