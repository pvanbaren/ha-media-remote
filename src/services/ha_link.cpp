#include "services/ha_link.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "log.h"
#include "services/device_name.h"
#include "services/ha_client.h"
#include "services/websocket.h"
#include "services/wifi_setup.h"
#include "services/yielding_client.h"

namespace services::ha_link {
namespace {

/** How long the token the remote keeps lasts, in days: as long as Home
 *  Assistant will let a long-lived token last in practice. Revoking it in the
 *  user's profile, or a reset here, ends it sooner. */
constexpr int kTokenLifespanDays = 3650;
constexpr uint32_t kTimeoutMs = 8000;
/** The longest reply taken. The list of a user's tokens grows with every
 *  browser and app ever signed in -- well past 4 KB on a household's admin
 *  account -- and anything over 4 KB is allocated from PSRAM. */
constexpr size_t kMaxReply = 64 * 1024;

char s_base[config::kHaBaseUrlMaxLen + 1] = {};
char s_name[64] = {};
/** "http://<this remote's address>/", which Home Assistant takes as the OAuth
 *  client id. A private address is allowed as one, and a redirect to the
 *  same host needs no page of client metadata, so the remote has nothing to
 *  publish. Its address rather than its .local name: a phone has to reach
 *  it, and Android does not resolve .local names. */
char s_client_id[32] = {};
char s_page[40] = {};
char s_qr[40] = {};
/** Carried through the sign-in and checked on the way back, so a code only
 *  comes from a sign-in this remote started. One use. */
char s_state[17] = {};
bool s_code_pending = false;
char s_code[96] = {};
char s_error[96] = {};

void setError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_error, sizeof(s_error), fmt, args);
  va_end(args);
  LOG_WARN("HA link: %s", s_error);
}

void urlEncode(String& out, const char* s) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
       *p != '\0'; ++p) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      out += static_cast<char>(*p);
    } else {
      out += '%';
      out += kHex[*p >> 4];
      out += kHex[*p & 0x0F];
    }
  }
}

/** The value of "key":"..." in `json`, unescaped as far as a token or an id
 *  needs. False when it is not there or not a string.
 *
 *  The key with its colon, as Home Assistant writes it (compact, no space):
 *  a reply's "type":"result" has the word as a value, ahead of the
 *  "result": that carries the token, and a match on the quoted word alone
 *  took the first and found no string after it. */
bool jsonString(const String& json, const char* key, char* out, size_t len) {
  char pattern[48];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  int at = json.indexOf(pattern);
  if (at < 0) {
    return false;
  }
  at += strlen(pattern);
  while (at < static_cast<int>(json.length()) && json[at] == ' ') {
    ++at;
  }
  if (at >= static_cast<int>(json.length()) || json[at] != '"') {
    return false;
  }
  ++at;
  size_t n = 0;
  while (at < static_cast<int>(json.length()) && json[at] != '"') {
    char c = json[at++];
    if (c == '\\' && at < static_cast<int>(json.length())) {
      c = json[at++];
    }
    if (n + 1 < len) {
      out[n++] = c;
    }
  }
  out[n] = '\0';
  return n > 0;
}

/** Scheme, host, port and path of s_base, for the WebSocket. */
struct Endpoint {
  bool tls = false;
  char host[64] = {};
  uint16_t port = 80;
  char path[48] = {};
};

bool parseBase(Endpoint& e) {
  const char* p = s_base;
  if (strncmp(p, "https://", 8) == 0) {
    e.tls = true;
    e.port = 443;
    p += 8;
  } else if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  } else {
    return false;
  }
  size_t n = 0;
  while (*p != '\0' && *p != ':' && *p != '/' && n + 1 < sizeof(e.host)) {
    e.host[n++] = *p++;
  }
  e.host[n] = '\0';
  if (*p == ':') {
    e.port = static_cast<uint16_t>(atoi(++p));
    while (*p != '\0' && *p != '/') {
      ++p;
    }
  }
  snprintf(e.path, sizeof(e.path), "%s/api/websocket", *p == '/' ? p : "");
  return e.host[0] != '\0';
}

void refreshOwnUrls() {
  const String ip = WiFi.localIP().toString();
  snprintf(s_client_id, sizeof(s_client_id), "http://%s/", ip.c_str());
  snprintf(s_page, sizeof(s_page), "http://%s/ha", ip.c_str());
  snprintf(s_qr, sizeof(s_qr), "HTTP://%s/HA", ip.c_str());
}

void newState() {
  snprintf(s_state, sizeof(s_state), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
}

void sendPage(WebServer& server, int status, const char* message) {
  String page;
  page.reserve(512);
  page += "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width,initial-scale=1'><title>";
  page += services::device::name();
  page += "</title></head><body style='font-family:verdana;text-align:center;"
          "margin-top:3em'><h3>";
  page += message;
  page += "</h3></body></html>";
  server.send(status, "text/html", page);
}

/** /ha: on to Home Assistant's sign-in, with this remote as the client. */
void handleStart(WebServer& server) {
  if (!ready()) {
    sendPage(server, 503,
             "The remote has not found Home Assistant yet. Set its URL in "
             "the portal, or wait a moment and scan again.");
    return;
  }
  refreshOwnUrls();
  newState();
  String url(s_base);
  url += "/auth/authorize?response_type=code&client_id=";
  urlEncode(url, s_client_id);
  url += "&redirect_uri=";
  String redirect(s_client_id);
  redirect += "ha_auth";
  urlEncode(url, redirect.c_str());
  url += "&state=";
  url += s_state;
  server.sendHeader("Location", url);
  server.send(302, "text/plain", "");
  LOG_INFO("HA link: sign-in started at %s", s_base);
}

/** /ha_auth: where Home Assistant sends the browser back with the code. */
void handleCallback(WebServer& server) {
  const String state = server.arg("state");
  const String code = server.arg("code");
  if (code.isEmpty() || s_state[0] == '\0' || state != s_state) {
    sendPage(server, 400,
             "That sign-in is out of date. Scan the code on the remote "
             "again.");
    return;
  }
  snprintf(s_code, sizeof(s_code), "%s", code.c_str());
  s_state[0] = '\0';
  s_code_pending = true;
  sendPage(server, 200,
           "Signed in. The remote is finishing the link; you can close "
           "this page.");
}

/** POST a form to Home Assistant's auth endpoints, which take no token. */
int postForm(const char* path, const String& body, String& response) {
  static WiFiClient plain;
  static WiFiClientSecure secure;
  const bool tls = strncmp(s_base, "https://", 8) == 0;
  if (tls) {
    // As for every other request: local installs are self-signed, and the
    // board has no clock to check a certificate's dates against.
    secure.setInsecure();
  }
  HTTPClient http;
  http.setTimeout(kTimeoutMs);
  String url(s_base);
  url += path;
  if (!http.begin(tls ? static_cast<WiFiClient&>(secure) : plain, url)) {
    return -1;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int status = http.POST(body);
  response = status > 0 ? http.getString() : String();
  http.end();
  return status;
}

/** The start of a reply Home Assistant refused, for the log -- but never
 *  one that succeeded, which may carry a token or a session. */
void logReply(const char* what, const String& reply) {
  if (reply.indexOf("\"success\":true") >= 0 ||
      reply.indexOf("access_token") >= 0) {
    LOG_WARN("HA link: %s (a %u-byte reply not understood)", what,
             static_cast<unsigned>(reply.length()));
    return;
  }
  LOG_WARN("HA link: %s: %.200s", what, reply.c_str());
}

/** What every token this firmware asks for is called, before the remote's
 *  own name: how an earlier one is recognised as the remote's to replace. */
constexpr char kTokenNamePrefix[] = "Media Remote (";
/** Names tried before giving up: the remote's own, then " 2" up to this. */
constexpr int kMaxTokenNames = 20;

/** The refresh token behind a Home Assistant access token: the "iss" of its
 *  payload, which is a JWT's middle part, base64url-encoded -- an id, a
 *  time issued and an expiry, under a hundred bytes. False for anything that
 *  is not one: a token pasted in the portal can be any string.
 *
 *  Never inlined, nor is mintToken() below: the link runs on the loop's
 *  8 KB stack, and folded into complete() their buffers sat underneath the
 *  TLS handshake of the code exchange, which overflowed it. */
__attribute__((noinline)) bool tokenIssuer(const char* jwt, char* out,
                                           size_t out_len) {
  const char* start = strchr(jwt, '.');
  const char* end = start != nullptr ? strchr(start + 1, '.') : nullptr;
  if (end == nullptr || end - start - 1 > 180) {
    return false;
  }
  // base64url to base64, padded, for mbedTLS.
  char b64[184];
  size_t n = 0;
  for (const char* p = start + 1; p < end; ++p) {
    b64[n++] = *p == '-' ? '+' : *p == '_' ? '/' : *p;
  }
  while (n % 4 != 0) {
    b64[n++] = '=';
  }
  unsigned char payload[140];
  size_t got = 0;
  if (mbedtls_base64_decode(payload, sizeof(payload) - 1, &got,
                            reinterpret_cast<const unsigned char*>(b64),
                            n) != 0) {
    return false;
  }
  payload[got] = '\0';
  return jsonString(String(reinterpret_cast<const char*>(payload)), "iss", out,
                    out_len);
}

/** Whether a token in the reply to auth/refresh_tokens is called `name`.
 *  Home Assistant refuses a second long-lived token under a name, so one
 *  that is taken is passed over. */
bool nameTaken(const String& tokens, const char* name) {
  String needle = "\"client_name\":\"";
  needle += name;
  needle += "\"";
  return tokens.indexOf(needle) >= 0;
}

/** Whether the token with refresh id `id` is a long-lived one this firmware
 *  made: its name starts with kTokenNamePrefix. Only such a token is ever
 *  deleted -- one pasted in the portal may be in use elsewhere. */
bool isRemoteToken(const String& tokens, const char* id) {
  String needle = "\"id\":\"";
  needle += id;
  needle += "\"";
  const int at = tokens.indexOf(needle);
  if (at < 0) {
    return false;
  }
  const int begin = tokens.lastIndexOf('{', at);
  const int end = tokens.indexOf('}', at);
  const String entry = tokens.substring(begin < 0 ? 0 : begin,
                                        end < 0 ? tokens.length() : end);
  String prefix = "\"client_name\":\"";
  prefix += kTokenNamePrefix;
  return entry.indexOf(prefix) >= 0 &&
         entry.indexOf("long_lived_access_token") >= 0;
}

/** Read messages until one contains `needle`, or the time runs out. */
bool awaitMessage(WebSocket& ws, const char* needle, String& out) {
  const unsigned long started = millis();
  while (millis() - started < kTimeoutMs) {
    switch (ws.poll(out, kMaxReply)) {
      case WebSocket::Poll::kMessage:
        if (out.indexOf(needle) >= 0) {
          return true;
        }
        break;
      case WebSocket::Poll::kDropped:
        LOG_WARN("HA link: a reply over %u bytes was dropped",
                 static_cast<unsigned>(kMaxReply));
        break;
      case WebSocket::Poll::kClosed:
        return false;
      default:
        delay(10);
        break;
    }
  }
  return false;
}

/** With a short session's access token: a long-lived token for this remote,
 *  and the Music Assistant entry id while the connection is open. */
__attribute__((noinline)) bool mintToken(const char* access, char* token,
                                         size_t token_len, char* ma_entry,
                                         size_t ma_len) {
  Endpoint e;
  if (!parseBase(e)) {
    setError("cannot parse %s", s_base);
    return false;
  }
  static services::YieldingClient plain;
  static services::YieldingClientSecure secure;
  if (e.tls) {
    secure.setInsecure();
  }
  NetworkClient& client =
      e.tls ? static_cast<NetworkClient&>(secure) : plain;
  WebSocket ws;
  if (!ws.open(client, e.host, e.port, e.path, kTimeoutMs)) {
    setError("WebSocket: %s", ws.error());
    return false;
  }

  String msg;
  bool ok = false;
  do {
    if (!awaitMessage(ws, "auth_required", msg)) {
      setError("WebSocket: no auth request");
      break;
    }
    String auth = "{\"type\":\"auth\",\"access_token\":\"";
    auth += access;
    auth += "\"}";
    ws.sendText(auth.c_str(), auth.length());
    if (!awaitMessage(ws, "\"type\":\"auth_", msg) ||
        msg.indexOf("auth_ok") < 0) {
      logReply("WebSocket sign-in refused", msg);
      setError("WebSocket: sign-in refused");
      break;
    }

    char name[64];
    snprintf(name, sizeof(name), "%s%s)", kTokenNamePrefix,
             services::device::name());

    // Home Assistant wants every message's id higher than the last on the
    // connection, so they come from one counter.
    int next_id = 1;
    char reply_id[12];
    auto send = [&](String body) {
      const int id = next_id++;
      snprintf(reply_id, sizeof(reply_id), "\"id\":%d,", id);
      String req = "{\"id\":";
      req += id;
      req += ",";
      req += body;
      req += "}";
      ws.sendText(req.c_str(), req.length());
    };

    // The token this remote holds now, if it was made by an earlier link to
    // this Home Assistant: replaced rather than left behind. Found by its
    // own id -- the "iss" inside it -- and never by name, since two remotes
    // can share a name, and deleting by it took the other one's token.
    send("\"type\":\"auth/refresh_tokens\"");
    String tokens;
    const bool have_list = awaitMessage(ws, reply_id, tokens);
    char old_id[48];
    if (have_list &&
        tokenIssuer(services::ha::token(), old_id, sizeof(old_id)) &&
        isRemoteToken(tokens, old_id)) {
      String del = "\"type\":\"auth/delete_refresh_token\",";
      del += "\"refresh_token_id\":\"";
      del += old_id;
      del += "\"";
      send(del);
      if (awaitMessage(ws, reply_id, msg) &&
          msg.indexOf("\"success\":true") >= 0) {
        LOG_INFO("HA link: removed this remote's earlier token");
        // Its name is free again: the list no longer says so.
        tokens = String();
        send("\"type\":\"auth/refresh_tokens\"");
        awaitMessage(ws, reply_id, tokens);
      } else {
        logReply("could not remove the earlier token", msg);
      }
    }

    // Home Assistant keeps one long-lived token per name and refuses a
    // second, and another remote, or a link this one lost track of, may
    // hold this name already. So the first of the name, "name 2", "name 3"
    // and on that the list does not show taken -- and should a name still
    // be refused, the next one.
    for (int i = 1; i <= kMaxTokenNames && !ok; ++i) {
      char asked[72];
      if (i == 1) {
        snprintf(asked, sizeof(asked), "%s", name);
      } else {
        snprintf(asked, sizeof(asked), "%s %d", name, i);
      }
      if (tokens.length() > 0 && nameTaken(tokens, asked)) {
        continue;
      }
      String req = "\"type\":\"auth/long_lived_access_token\",";
      req += "\"client_name\":\"";
      req += asked;
      req += "\",\"lifespan\":";
      req += kTokenLifespanDays;
      send(req);
      if (awaitMessage(ws, reply_id, msg) &&
          msg.indexOf("\"success\":true") >= 0 &&
          jsonString(msg, "result", token, token_len)) {
        ok = true;
        LOG_INFO("HA link: long-lived token \"%s\" created", asked);
      } else {
        logReply("long-lived token refused", msg);
      }
    }
    tokens = String();  // the list can be tens of KB; done with it
    if (!ok) {
      setError("no long-lived token from Home Assistant");
      break;
    }

    // Best effort: the remote works without it, and the portal can set it.
    if (ma_entry != nullptr && ma_len > 0) {
      ma_entry[0] = '\0';
      send("\"type\":\"config_entries/get\",\"domain\":\"music_assistant\"");
      if (awaitMessage(ws, reply_id, msg)) {
        jsonString(msg, "entry_id", ma_entry, ma_len);
      }
    }
  } while (false);
  ws.close();
  return ok;
}

}  // namespace

void init() {
  wifiAddWebPage("/ha", handleStart);
  wifiAddWebPage("/HA", handleStart);
  wifiAddWebPage("/ha_auth", handleCallback);
}

bool discover() {
  if (ready()) {
    return true;
  }
  // A URL typed in the portal wins: it is the one someone chose.
  const char* stored = services::ha::storedBaseUrl();
  if (stored != nullptr && stored[0] != '\0') {
    snprintf(s_base, sizeof(s_base), "%s", stored);
    snprintf(s_name, sizeof(s_name), "%s", stored);
  } else {
    const int found = MDNS.queryService("home-assistant", "tcp");
    if (found <= 0) {
      setError("no Home Assistant answered on mDNS");
      return false;
    }
    for (int i = 0; i < found; ++i) {
      LOG_INFO("HA link: found %s at %s:%u (%s)", MDNS.hostname(i).c_str(),
               MDNS.address(i).toString().c_str(),
               static_cast<unsigned>(MDNS.port(i)),
               MDNS.txt(i, "location_name").c_str());
    }
    // The first to answer. Its own idea of its address where that is one
    // this remote can resolve; otherwise the address it answered from --
    // a .local name would need mDNS for every request after this.
    const String internal = MDNS.txt(0, "internal_url");
    const IPAddress ip = MDNS.address(0);
    if (!internal.isEmpty() && internal.indexOf(".local") < 0) {
      snprintf(s_base, sizeof(s_base), "%s", internal.c_str());
    } else if (ip != IPAddress()) {
      snprintf(s_base, sizeof(s_base), "%s://%s:%u",
               internal.startsWith("https://") ? "https" : "http",
               ip.toString().c_str(), static_cast<unsigned>(MDNS.port(0)));
    } else {
      setError("Home Assistant answered without an address");
      return false;
    }
    size_t len = strlen(s_base);
    while (len > 0 && s_base[len - 1] == '/') {
      s_base[--len] = '\0';
    }
    const String location = MDNS.txt(0, "location_name");
    snprintf(s_name, sizeof(s_name), "%s",
             location.isEmpty() ? s_base : location.c_str());
  }
  refreshOwnUrls();
  LOG_INFO("HA link: %s, sign in at %s", s_base, s_page);
  return true;
}

bool ready() { return s_base[0] != '\0'; }

const char* pageUrl() { return s_page; }

const char* qrText() { return s_qr; }

const char* serverName() { return s_name; }

bool codeArrived() { return s_code_pending; }

bool complete() {
  if (!s_code_pending) {
    setError("no sign-in to finish");
    return false;
  }
  s_code_pending = false;

  String body = "grant_type=authorization_code&code=";
  urlEncode(body, s_code);
  body += "&client_id=";
  urlEncode(body, s_client_id);
  String response;
  const int status = postForm("/auth/token", body, response);
  s_code[0] = '\0';
  if (status != 200) {
    logReply("token exchange refused", response);
    setError("sign-in not accepted (HTTP %d)", status);
    return false;
  }
  char access[config::kHaTokenMaxLen + 1];
  char refresh[160];
  if (!jsonString(response, "access_token", access, sizeof(access))) {
    setError("no access token in the reply");
    return false;
  }
  if (!jsonString(response, "refresh_token", refresh, sizeof(refresh))) {
    refresh[0] = '\0';
  }

  char token[config::kHaTokenMaxLen + 1];
  char ma_entry[48];
  const bool minted = mintToken(access, token, sizeof(token), ma_entry,
                                sizeof(ma_entry));

  // The short session has done its job either way; leave nothing of it in
  // the user's profile.
  if (refresh[0] != '\0') {
    String revoke = "token=";
    urlEncode(revoke, refresh);
    String ignored;
    postForm("/auth/revoke", revoke, ignored);
  }
  if (!minted) {
    return false;
  }

  services::ha::saveCredentials(s_base, token);
  if (ma_entry[0] != '\0' && services::ha::maConfigEntry()[0] == '\0') {
    services::ha::saveMaConfigEntry(ma_entry);
    LOG_INFO("HA link: Music Assistant entry %s", ma_entry);
  }
  LOG_INFO("HA link: linked to %s", s_base);
  return true;
}

const char* lastError() { return s_error; }

void reset() {
  s_base[0] = '\0';
  s_name[0] = '\0';
  s_state[0] = '\0';
  s_code_pending = false;
}

}  // namespace services::ha_link
