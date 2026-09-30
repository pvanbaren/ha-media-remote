#include "services/ha_client.h"
#include "services/yielding_client.h"

#include "ha_internal.h"
#include "log.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace services::ha {
namespace {

constexpr char kPrefsNamespace[] = "hamedia";
constexpr char kPrefsUrlKey[] = "url";
constexpr char kPrefsTokenKey[] = "tok";
constexpr char kPrefsSelectedKey[] = "sel";
constexpr char kPrefsMaEntryKey[] = "maid";
constexpr char kPrefsControlKey[] = "ctl";
constexpr char kPrefsControlInputKey[] = "ctlin";
/** The player's wake volume in percent: -1 off, 0..100 on. Absent until the
 *  portal first saves one, which is what lets the build's default stand. */
constexpr char kPrefsWakeVolumeKey[] = "wakevol";
/** Read back when nothing is stored. */
constexpr int32_t kWakeVolumeUnset = -2;

// Records come back from the template API delimited by ASCII unit (\037) and
// record (\036) separators rather than tabs or newlines, so a track title
// containing either still parses cleanly.
constexpr char kFieldSep = '\037';
constexpr char kRecordSep = '\036';

// Asking HA to render exactly the fields we need keeps a response to a few
// hundred bytes. GET /api/states would ship every attribute of every entity --
// source_list alone runs to tens of kilobytes on an AV receiver, which is more
// than this board wants to hold, let alone parse.
//
// NEVER use the whitespace-control forms {%- or -%} in these templates.
// Python classifies 0x1C..0x1F as whitespace, so "\037".strip() is empty --
// which means a trimming tag silently eats any separator next to it. That is
// not a theory: {%- endfor -%} ate every record separator here, and the fields
// then shifted by one per record.
constexpr char kPlayersTemplate[] =
    "{% for s in states.media_player | sort(attribute='name') %}"
    "{{s.entity_id}}\037{{s.name}}\037"
    "{{0 if s.state in ['unavailable','unknown'] else 1}}\036"
    "{% endfor %}";

// %% escapes are for snprintf. The %s are the player entity, the control
// entity -- the same string unless one was chosen separately -- and the
// player's input on the control entity as chosen in the portal (quotes
// escaped), or nothing.
//
// Volume, mute and the feature bits the slider tests come from `c`, not `e`.
// Where the two differ -- a Music Assistant player streaming into a receiver
// that owns the actual knob -- the player's own volume_level is either absent
// or a software gain nobody wants to touch, while the receiver's is the one
// that makes the room louder.
//
// What is playing comes from `m`: what the room is hearing. That is `e`,
// unless `c` is a separate device on some input other than the player's.
// Then it is the player that input is named after, where one is playing or
// paused -- a Triad zone switched to another of its linked players; its
// Music Assistant entity where there are two by that name, since that is the
// one with the art and the queue -- or failing that `c` itself, where it is
// playing or paused with artwork of its own: a receiver on its own Spotify or
// net radio. The title, the art, the progress, the transport buttons and the
// history all follow `m`, and the last field names it for the calls that go
// with them.
//
// Only that branch reads every media_player, so only while `c` is on another
// input does the subscription follow them all -- which Home Assistant rate
// limits to once a second.
constexpr char kStateTemplateFmt[] =
    "{%% set e = '%s' %%}"
    "{%% set c = '%s' %%}"
    // The input on `c` that is `e`: the one chosen in the portal, or failing
    // that one named after the player -- a zone amplifier listing its players
    // as inputs -- or nothing. See selectPlayerSource().
    "{%% set ci = '%s' %%}"
    "{%% set sl = state_attr(c,'source_list') or [] %%}"
    "{%% set fn = state_attr(e,'friendly_name') %%}"
    "{%% set ps = ci if ci else (fn if fn in sl else '') %%}"
    "{%% set cs = state_attr(c,'source') or '' %%}"
    "{%% set other = c != e and cs != '' and not (ps and cs == ps) %%}"
    "{%% set sp = '' %%}"
    "{%% if other %%}"
    "{%% set cands = states.media_player|selectattr('name','eq',cs)"
    "|selectattr('state','in',['playing','paused'])|list %%}"
    "{%% set mass = cands|selectattr('attributes.mass_player_type','defined')"
    "|list %%}"
    "{%% set sp = ((mass or cands)|map(attribute='entity_id')|list|first) "
    "or '' %%}"
    "{%% endif %%}"
    "{%% set m = sp if sp else (c if other and "
    "states(c) in ['playing','paused'] and state_attr(c,'entity_picture') "
    "else e) %%}"
    // An app that says what it is but not what it is playing -- a Roku does
    // that for every app but its TV tuner -- is titled with its own name
    // while it plays, rather than as nothing playing, and the name is then
    // not repeated underneath.
    "{%% set mt = state_attr(m,'media_title') or '' %%}"
    "{%% set an = state_attr(m,'app_name') or '' %%}"
    "{%% set at = an if not mt and states(m) in ['playing','paused'] "
    "else '' %%}"
    "{{states(m)}}\037"
    "{{mt or at}}\037"
    "{{state_attr(m,'media_artist') or state_attr(m,'media_album_name') or "
    "state_attr(m,'media_series_title') or ('' if at else an)}}\037"
    "{{state_attr(m,'entity_picture') or ''}}\037"
    "{{state_attr(m,'supported_features')|int(0)}}\037"
    "{{(state_attr(m,'media_duration') or 0)|float(0)|round(1)}}\037"
    "{{(state_attr(m,'media_position') or 0)|float(0)|round(1)}}\037"
    "{%% set pu = state_attr(m,'media_position_updated_at') %%}"
    "{{((as_timestamp(now())-as_timestamp(pu)) if pu else 0)|float(0)|round(1)}}\037"
    "{{state_attr(m,'friendly_name') or m}}\037"
    "{{state_attr(c,'volume_level')|float(-1)|round(3)}}\037"
    "{{1 if state_attr(c,'is_volume_muted') else 0}}\037"
    "{{state_attr(c,'supported_features')|int(0)}}\037"
    "{{states(c)}}\037"
    "{{ps}}\037"
    "{{cs}}\037"
    // The artist alone, not the subtitle's fallbacks, and what is playing by
    // them: what the room's history is kept by (services::history). From `m`,
    // since that is what the room is hearing.
    "{{state_attr(m,'media_artist') or ''}}\037"
    "{{state_attr(m,'media_content_id') or ''}}\037"
    "{{m if m != e else ''}}\037"
    // Both configured entities by name, for the status page.
    "{{state_attr(e,'friendly_name') or e}}\037"
    "{{state_attr(c,'friendly_name') or c}}\036";

// Every input on one entity, for the portal's dropdown.
constexpr char kSourcesTemplateFmt[] =
    "{%% for s in (state_attr('%s','source_list') or []) %%}{{s}}\036{%% endfor %%}";

char s_base_url[config::kHaBaseUrlMaxLen + 1] = {};
char s_token[config::kHaTokenMaxLen + 1] = {};
char s_selected[config::kEntityIdMaxLen] = {};
char s_ma_entry[config::kMaConfigEntryIdMaxLen + 1] = {};
/** -1 off, 0..100 percent; see playerWakeVolume(). */
int s_wake_volume_pct = -1;
/** Empty means "whatever is playing also carries the volume and the power",
 *  which is the ordinary case; controlEntity() resolves that. */
char s_control[config::kEntityIdMaxLen] = {};
/** The player's input on the control entity, chosen in the portal. Empty
 *  means "the input named after the player, if there is one". Under
 *  s_entity_lock like the two entities. */
char s_control_input[config::kSourceNameMaxLen] = {};
/** Guards s_selected and s_control: the portal writes them on the Arduino
 *  loop while the network task reads them. Held only for a copy of a few
 *  dozen bytes. */
portMUX_TYPE s_entity_lock = portMUX_INITIALIZER_UNLOCKED;

/** strlcpy, for inside the lock: no formatting, nothing that can block. */
void copyBounded(char* out, size_t out_len, const char* src) {
  if (out_len == 0) {
    return;
  }
  size_t i = 0;
  for (; i + 1 < out_len && src[i] != '\0'; ++i) {
    out[i] = src[i];
  }
  out[i] = '\0';
}
char s_last_error[128] = {};
/** Bumped whenever something the state stream was opened against -- the
 *  server, the token, the player, the volume/power entity -- changes, so the
 *  stream can notice it is subscribed to the wrong thing and start again. */
std::atomic<uint32_t> s_generation{0};

void setError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
  va_end(args);
  LOG_WARN("HA: %s", s_last_error);
}

/** Read the JSON string literal starting at `p` (which must be its opening
 *  quote) into `out`, and return the character after the closing quote.
 *
 *  The counterpart to appendJsonString below, and hand-rolled for the same
 *  reason the writer is: what is needed here is three fields from a flat array
 *  of objects, and a JSON library would cost flash and a working buffer to do
 *  it. Escapes are handled because a station name may well contain a quote or
 *  an apostrophe; \uXXXX is decoded to UTF-8 so that an accented name arrives
 *  as the font expects rather than as a literal backslash-u.
 *
 *  nullptr when the string is unterminated, which is the only malformed case
 *  that matters -- a truncated response is the realistic failure, not invalid
 *  JSON from Home Assistant. */
const char* readJsonString(const char* p, char* out, size_t out_len) {
  if (p == nullptr || *p != '"') {
    return nullptr;
  }
  ++p;
  size_t filled = 0;
  const size_t limit = out_len > 0 ? out_len - 1 : 0;

  auto put = [&](char c) {
    if (filled < limit) {
      out[filled++] = c;
    }
  };

  while (*p != '\0') {
    if (*p == '"') {
      if (out_len > 0) {
        out[filled] = '\0';
      }
      return p + 1;
    }
    if (*p != '\\') {
      put(*p++);
      continue;
    }
    ++p;
    switch (*p) {
      case 'n': put('\n'); ++p; break;
      case 't': put('\t'); ++p; break;
      case 'r': put('\r'); ++p; break;
      case 'b': put('\b'); ++p; break;
      case 'f': put('\f'); ++p; break;
      case '"': put('"'); ++p; break;
      case '\\': put('\\'); ++p; break;
      case '/': put('/'); ++p; break;
      case 'u': {
        ++p;
        uint32_t code = 0;
        int digits = 0;
        for (; digits < 4 && isxdigit(static_cast<unsigned char>(*p));
             ++digits, ++p) {
          const char c = *p;
          code = (code << 4) |
                 static_cast<uint32_t>(c <= '9' ? c - '0'
                                                : (c | 0x20) - 'a' + 10);
        }
        if (digits < 4) {
          return nullptr;
        }
        // Surrogate halves are dropped rather than paired: they only appear
        // outside the BMP, and the embedded font has nothing there anyway.
        if (code >= 0xD800 && code <= 0xDFFF) {
          break;
        }
        if (code < 0x80) {
          put(static_cast<char>(code));
        } else if (code < 0x800) {
          put(static_cast<char>(0xC0 | (code >> 6)));
          put(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          put(static_cast<char>(0xE0 | (code >> 12)));
          put(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          put(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      case '\0':
        return nullptr;
      default:
        put(*p++);
        break;
    }
  }
  return nullptr;
}

/** Step over one JSON value, whatever it is, and return what follows it.
 *  nullptr if it never terminates. */
const char* skipJsonValue(const char* p) {
  if (p == nullptr) {
    return nullptr;
  }
  while (*p == ' ') {
    ++p;
  }
  if (*p == '"') {
    // Walk the string properly rather than looking for the next quote: an
    // escaped quote inside it is not the end.
    for (++p; *p != '\0'; ++p) {
      if (*p == '\\') {
        if (*++p == '\0') {
          return nullptr;
        }
        continue;
      }
      if (*p == '"') {
        return p + 1;
      }
    }
    return nullptr;
  }
  if (*p == '{' || *p == '[') {
    const char open = *p;
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    for (; *p != '\0'; ++p) {
      if (*p == '"') {
        p = skipJsonValue(p);
        if (p == nullptr) {
          return nullptr;
        }
        --p;  // the loop's ++p
        continue;
      }
      if (*p == open) {
        ++depth;
      } else if (*p == close) {
        if (--depth == 0) {
          return p + 1;
        }
      }
    }
    return nullptr;
  }
  // A bare literal: number, true, false, null.
  while (*p != '\0' && *p != ',' && *p != '}' && *p != ']') {
    ++p;
  }
  return p;
}

/** Fields pulled out of one library item. */
struct ItemFields {
  char* uri;
  size_t uri_len;
  char* name;
  size_t name_len;
  char* image;
  size_t image_len;
};

/** Read one object of the items array into `fields`, and return what follows
 *  it. `p` must point at its opening brace.
 *
 *  Walks the object's own members rather than searching the text for keys,
 *  because the text is not flat: an album carries a nested "artists" array
 *  whose objects have their own "name" and "image", and a scan that stopped at
 *  the first '}' would end inside one of them -- reading the artist's fields
 *  for some items and treating the remaining artists as further albums. Only
 *  members at this object's own depth are taken, and every other value is
 *  stepped over whole. */
const char* readLibraryItem(const char* p, const ItemFields& fields) {
  if (p == nullptr || *p != '{') {
    return nullptr;
  }
  ++p;

  while (*p != '\0') {
    while (*p == ' ' || *p == ',') {
      ++p;
    }
    if (*p == '}') {
      return p + 1;
    }
    if (*p != '"') {
      return nullptr;
    }

    char key[24];
    const char* after_key = readJsonString(p, key, sizeof(key));
    if (after_key == nullptr) {
      return nullptr;
    }
    while (*after_key == ' ') {
      ++after_key;
    }
    if (*after_key != ':') {
      return nullptr;
    }
    const char* value = after_key + 1;
    while (*value == ' ') {
      ++value;
    }

    // Only strings are wanted; "image": null is how an item with no artwork
    // arrives, and leaving the field empty is exactly right for it.
    char* dest = nullptr;
    size_t dest_len = 0;
    if (strcmp(key, "uri") == 0) {
      dest = fields.uri;
      dest_len = fields.uri_len;
    } else if (strcmp(key, "name") == 0) {
      dest = fields.name;
      dest_len = fields.name_len;
    } else if (strcmp(key, "image") == 0) {
      dest = fields.image;
      dest_len = fields.image_len;
    }
    if (dest != nullptr && *value == '"') {
      readJsonString(value, dest, dest_len);
    }

    p = skipJsonValue(value);
    if (p == nullptr) {
      return nullptr;
    }
  }
  return nullptr;
}

/** Append `value` to `out` as a JSON string literal, quotes included.
 *
 *  Hand-rolled rather than ArduinoJson because ArduinoJson only emits the
 *  named escape sequences and writes every other control character raw. That
 *  is invalid JSON -- RFC 8259 requires \u00XX for the whole 0x00-0x1F range
 *  -- and the templates below are delimited by 0x1F/0x1E, so serialising them
 *  with ArduinoJson produced a body Home Assistant rejected with a 400. */
void appendJsonString(String& out, const char* value) {
  out += '"';
  for (const char* p = value; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char escape[7];
          snprintf(escape, sizeof(escape), "\\u%04X", c);
          out += escape;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  out += '"';
}

void copyField(char* dst, size_t dst_len, const String& src) {
  if (dst_len == 0) {
    return;  // dst_len - 1 would wrap, and the memcpy would take the heap
  }
  const size_t limit = dst_len - 1;
  const size_t n = src.length() < limit ? src.length() : limit;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

/** The object_id half of an entity_id, i.e. everything past the dot. */
const char* objectId(const char* entity_id) {
  const char* dot = strchr(entity_id, '.');
  return dot != nullptr ? dot + 1 : entity_id;
}

/** Relabel entries whose friendly name is not unique.
 *
 *  Duplicate names are the norm, not the exception: a Music Assistant player
 *  and the device it wraps report the same name, and so does every TV an
 *  integration discovers twice. Two identical rows give the picker no way to
 *  choose between them, so both fall back to their object_id, which is unique
 *  by construction. */
void disambiguateNames(PlayerEntry* entries, size_t count) {
  bool duplicate[config::kMaxPlayers] = {};
  // The scratch flags are sized for the cap fetchPlayers honours. Clamped
  // anyway: this is a stack array, and a caller that ever handed over a
  // longer list would write past it rather than fail visibly.
  if (count > config::kMaxPlayers) {
    count = config::kMaxPlayers;
  }

  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (strcmp(entries[i].name, entries[j].name) == 0) {
        duplicate[i] = true;
        duplicate[j] = true;
      }
    }
  }

  // Rewrite only after the whole scan, so renaming one entry cannot hide a
  // collision that has not been looked at yet.
  for (size_t i = 0; i < count; ++i) {
    if (duplicate[i]) {
      snprintf(entries[i].name, sizeof(entries[i].name), "%s",
               objectId(entries[i].entity_id));
    }
  }
}

/** entity_id comes back from HA, but it gets interpolated into a Jinja string
 *  literal -- hold it to the characters HA itself allows. */
bool validEntityId(const char* entity_id) {
  if (entity_id == nullptr || entity_id[0] == '\0') {
    return false;
  }
  bool has_dot = false;
  for (const char* p = entity_id; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '.') {
      has_dot = true;
      continue;
    }
    if (!islower(c) && !isdigit(c) && c != '_') {
      return false;
    }
  }
  return has_dot;
}

// The HA connection, held across requests rather than rebuilt per call. See
// httpPost() for why, and for why access has to be serialised.
services::YieldingClient s_plain;
services::YieldingClientSecure s_secure;
HTTPClient s_http;
SemaphoreHandle_t s_http_mutex = nullptr;

/** Serialises access to the shared connection above.
 *
 *  The mutex is made in init(), on the Arduino task before the poll task
 *  exists, and deliberately never here. Making it on first use would race:
 *  both tasks can find it null at the same moment, each make one, and each
 *  then lock a different mutex -- which is no lock at all, and leaves s_http
 *  and s_secure driven from two tasks at once. That is exactly the shape of
 *  the `pbuf_free: p->ref > 0` crash. */
struct ConnectionGuard {
  ConnectionGuard() {
    if (s_http_mutex != nullptr) {
      xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    }
  }
  ~ConnectionGuard() {
    if (s_http_mutex != nullptr) {
      xSemaphoreGive(s_http_mutex);
    }
  }
};

/** The host part of s_base_url -- the name the resolver is actually asked
 *  for, with the scheme, the port and any path taken off.
 *
 *  Not IPv6-literal aware: a bracketed address would come back truncated at
 *  the first colon. Nothing else in this file handles one either, and an HA
 *  install reached by a raw IPv6 address is not a setup this has seen. */
void hostFromBaseUrl(char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  const char* start = strstr(s_base_url, "://");
  start = (start != nullptr) ? start + 3 : s_base_url;
  size_t n = 0;
  while (start[n] != '\0' && start[n] != '/' && start[n] != ':' &&
         n + 1 < out_len) {
    ++n;
  }
  memcpy(out, start, n);
  out[n] = '\0';
}

/** What the last lookup returned, so a change in it can be noticed rather
 *  than passing silently. Unset until the first successful resolve. */
IPAddress s_server_ip;

/**
 * Resolve the base URL's host and print what came back.
 *
 * There is no resolver on this device and no hosts file to consult. The name
 * goes to WiFi.hostByName(), which is a thin wrapper over lwIP's
 * dns_gethostbyname(): lwIP sends a UDP query to the DNS servers that
 * arrived in the DHCP lease -- the router, on an ordinary home network --
 * and caches the answer for its TTL in a table of four entries. A name
 * already in that cache resolves without a packet going anywhere, which is
 * why a request can keep failing against a stale address long after the
 * record behind it changed -- and why the timing printed below is only
 * evidence about the link on the first lookup after boot. A cache hit
 * costs microseconds however bad the network is. Nothing here asks for mDNS, so a .local name would take an entirely
 * different path.
 *
 * Which is why this prints the resolvers alongside the answer. A device
 * handed a filtering or captive resolver gets a perfectly well-formed reply
 * pointing somewhere that is not the server, and every symptom of that -- a
 * refused connection, a handshake that never completes, a timeout -- looks
 * exactly like the server being down. The address, and which resolver
 * produced it, are the two facts that tell those apart.
 */
void logServerAddress(const char* why) {
  char host[config::kHaBaseUrlMaxLen + 1] = {};
  hostFromBaseUrl(host, sizeof(host));
  if (host[0] == '\0') {
    return;
  }

  IPAddress literal;
  if (literal.fromString(host)) {
    // Configured by address, so there is no lookup to get wrong.
    s_server_ip = literal;
    LOG_INFO("HA: server %s is a literal address (%s)", host, why);
    return;
  }

  IPAddress ip;
  const unsigned long started_ms = millis();
  const bool ok = WiFi.hostByName(host, ip) == 1;
  const unsigned long took_ms = millis() - started_ms;

  if (!ok) {
    LOG_WARN("HA: DNS %s -> no answer in %lu ms (%s)", host, took_ms,
                  why);
  } else {
    LOG_INFO("HA: DNS %s -> %s in %lu ms (%s)", host,
                  ip.toString().c_str(), took_ms, why);
    if (s_server_ip != IPAddress() && ip != s_server_ip) {
      LOG_INFO("HA: that address changed, it was %s",
                    s_server_ip.toString().c_str());
    }
    s_server_ip = ip;
  }

  // RSSI belongs on the failure line rather than only at association: a
  // link can be fine when it is joined and unusable ten minutes later, and
  // the moment a request gave up is the one worth knowing it at.
  //
  // Read it for what it is, though. RSSI is the strength of frames this
  // device did receive, so it says nothing about the noise they arrived
  // over and nothing at all about whether this end is being heard. A
  // healthy number here alongside a request that timed out is not a
  // contradiction -- it is the ordinary look of a lossy link.
  LOG_INFO("HA: resolver %s / %s, device %s, gateway %s, RSSI %d dBm",
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str(),
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                static_cast<int>(WiFi.RSSI()));
}

/** True until the first request of this association has said where it is
 *  going. Set again on a drop, so a new lease's resolver is reported too. */
bool s_address_pending = true;

/** POST `body` to `path`; the response text lands in `response`.
 *  `timeout_ms` is the read limit -- most calls want the default. */
bool httpPost(const char* path, const String& body, String& response,
              uint16_t timeout_ms = config::kHaHttpTimeoutMs) {
  response = "";

  if (!configured()) {
    setError("not configured");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setError("wifi down");
    s_address_pending = true;  // a new association may bring a new resolver
    return false;
  }

  if (s_address_pending) {
    s_address_pending = false;
    logServerAddress("first request");
  }

  String url(s_base_url);
  url += path;

  // One connection, held open across requests. A client per call meant a full
  // TLS handshake every poll -- every 2 s while playing -- and mbedTLS's 16 KB
  // in and out buffers cycling through the heap each time. Kept alive, they
  // are allocated once and never returned to fragment anything.
  //
  // Shared state, so calls have to be serialised: fetchState runs on the poll
  // task while the transport buttons, the volume drag and the portal's player
  // list all call in from the Arduino loop.
  ConnectionGuard connection;

  WiFiClient* client = &s_plain;
  if (usesTls()) {
    // Local HA installs are almost always behind a self-signed or internal-CA
    // cert, and the board has no clock to validate notBefore/notAfter against
    // at boot. Pin a root CA here if the server must be authenticated.
    s_secure.setInsecure();
    client = &s_secure;
  }

  // Kept alive only while there is no state stream. With one open, this
  // connection is for the occasional library load or command fallback, and
  // holding a second TLS session's worth of internal RAM for those is the
  // wrong trade: the stream is the connection that is always wanted.
  const bool keep_alive = config::kHaKeepAlive && !streamIsOpen();
  HTTPClient& http = s_http;
  http.setReuse(keep_alive);
  http.setTimeout(timeout_ms);
  http.setConnectTimeout(config::kHaHttpTimeoutMs);
  if (!http.begin(*client, url)) {
    setError("bad URL");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + s_token);
  http.addHeader("Content-Type", "application/json");

  const unsigned long started_ms = millis();
  int code = http.POST(body);

  if (code <= 0 && keep_alive) {
    // A kept-alive connection the server has since closed fails on the first
    // write. That is the ordinary cost of keep-alive rather than a fault, so
    // drop the socket and give the retry a fresh one before reporting
    // anything. Headers do not survive begin(), so they go on again.
    http.end();
    client->stop();
    if (http.begin(*client, url)) {
      http.addHeader("Authorization", String("Bearer ") + s_token);
      http.addHeader("Content-Type", "application/json");
      code = http.POST(body);
    }
  }

  if (code <= 0) {
    logHttpRequest("POST", url.c_str(), code, body.length(), -1,
                   millis() - started_ms);
    setError("%s", HTTPClient::errorToString(code).c_str());
    // A TLS handshake that cannot allocate reports as a plain connection
    // failure, so the heap numbers go out alongside it: mbedTLS wants 16 KB in
    // and 16 KB out, and it is the largest contiguous block that decides
    // whether it gets them.
    //
    // Internal RAM specifically. mbedTLS cannot use PSRAM for these, and on a
    // board that has 8 MB of it a plain MALLOC_CAP_8BIT total reads as several
    // megabytes free while the allocation that actually failed had nowhere to
    // go -- which makes a genuine exhaustion look like anything but.
    LOG_WARN("HA: POST %s transport failure (internal heap free %u, "
                  "largest %u)",
                  url.c_str(),
                  static_cast<unsigned>(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    // Where it was trying to go, resolved again now rather than remembered.
    // A connection refused at an address nothing is listening on reads in
    // this log exactly like one the server itself turned away.
    logServerAddress("after transport failure");
    http.end();
    return false;
  }
  if (code < 200 || code >= 300) {
    // HA explains itself in the body -- a template error names the line that
    // failed, an auth failure says so. The status card only has room for a
    // clipped version, so the whole thing goes to the console along with the
    // request that caused it.
    String detail = http.getString();
    detail.trim();
    logHttpRequest("POST", url.c_str(), code, body.length(), detail.length(),
                   millis() - started_ms);
    LOG_WARN("HA: request body: %s", body.c_str());
    if (detail.length() > 0) {
      LOG_WARN("HA: response body: %s", detail.c_str());
    }
    http.end();

    if (code == 401 || code == 403) {
      setError("token rejected (%d)", code);
    } else if (detail.length() > 0) {
      setError("HTTP %d: %s", code, detail.c_str());
    } else {
      setError("HTTP %d", code);
    }
    return false;
  }

  response = http.getString();
  logHttpRequest("POST", url.c_str(), code, body.length(), response.length(),
                 millis() - started_ms);
  http.end();
  s_last_error[0] = '\0';
  return true;
}

/** Render a Jinja template server-side via POST /api/template. */
bool renderTemplate(const char* tmpl, String& out) {
  String body("{\"template\":");
  appendJsonString(body, tmpl);
  body += '}';
  return httpPost("/api/template", body, out);
}

PlaybackState parsePlaybackState(const String& s) {
  if (s == "playing") {
    return PlaybackState::kPlaying;
  }
  if (s == "paused" || s == "buffering") {
    return PlaybackState::kPaused;
  }
  if (s == "idle" || s == "standby") {
    return PlaybackState::kIdle;
  }
  if (s == "off") {
    return PlaybackState::kOff;
  }
  if (s == "unavailable" || s == "unknown") {
    return PlaybackState::kUnavailable;
  }
  return PlaybackState::kUnknown;
}

/** Pull the next field starting at `pos`, advancing past its delimiter. */
String nextField(const String& src, int& pos) {
  const int len = static_cast<int>(src.length());
  if (pos < 0 || pos > len) {
    return String();
  }
  int end = pos;
  while (end < len && src[end] != kFieldSep && src[end] != kRecordSep) {
    ++end;
  }
  String field = src.substring(pos, end);
  pos = end + 1;
  return field;
}

}  // namespace

void init() {
  // Before the poll task exists, so ConnectionGuard never has to make it.
  if (s_http_mutex == nullptr) {
    s_http_mutex = xSemaphoreCreateMutex();
  }
  detail::streamInit();

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  prefs.getString(kPrefsUrlKey, s_base_url, sizeof(s_base_url));
  prefs.getString(kPrefsTokenKey, s_token, sizeof(s_token));
  prefs.getString(kPrefsSelectedKey, s_selected, sizeof(s_selected));
  prefs.getString(kPrefsMaEntryKey, s_ma_entry, sizeof(s_ma_entry));
  prefs.getString(kPrefsControlKey, s_control, sizeof(s_control));
  prefs.getString(kPrefsControlInputKey, s_control_input,
                  sizeof(s_control_input));
  const int32_t wake = prefs.getInt(kPrefsWakeVolumeKey, kWakeVolumeUnset);
  prefs.end();

  if (wake == kWakeVolumeUnset) {
    s_wake_volume_pct =
        config::kPlayerWakeVolume >= 0.0f
            ? static_cast<int>(config::kPlayerWakeVolume * 100.0f + 0.5f)
            : -1;
  } else {
    s_wake_volume_pct = wake >= 0 && wake <= 100 ? static_cast<int>(wake) : -1;
  }

  if (s_selected[0] == '\0' && config::kDefaultPlayerEntityId[0] != '\0') {
    // Held in RAM only, not written back: a device that has never been through
    // the picker should still follow the configured default if it changes in a
    // later build, rather than pinning the first one it ever saw.
    snprintf(s_selected, sizeof(s_selected), "%s",
             config::kDefaultPlayerEntityId);
    LOG_INFO("HA: defaulting to %s", s_selected);
  }

  if (s_control[0] == '\0' && config::kDefaultControlEntityId[0] != '\0') {
    snprintf(s_control, sizeof(s_control), "%s",
             config::kDefaultControlEntityId);
  }

  if (s_base_url[0] != '\0') {
    LOG_INFO("HA: %s (token %s, player %s, volume/power %s)", s_base_url,
                  s_token[0] != '\0' ? "set" : "missing",
                  s_selected[0] != '\0' ? s_selected : "unset",
                  s_control[0] != '\0' ? s_control : "same as player");
  }
}

bool configured() { return s_base_url[0] != '\0' && s_token[0] != '\0'; }

const char* baseUrl() { return s_base_url; }
bool usesTls() { return strncmp(s_base_url, "https://", 8) == 0; }
const char* token() { return s_token; }
const char* storedBaseUrl() { return s_base_url; }
bool hasStoredToken() { return s_token[0] != '\0'; }

void saveCredentials(const char* base_url, const char* token_in) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }

  if (base_url != nullptr && base_url[0] != '\0') {
    snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
    // Trailing slashes would double up against the /api/... paths.
    size_t len = strlen(s_base_url);
    while (len > 0 && s_base_url[len - 1] == '/') {
      s_base_url[--len] = '\0';
    }
    prefs.putString(kPrefsUrlKey, s_base_url);
    ++s_generation;
  }

  // The portal shows a stored token as a placeholder rather than echoing it,
  // so an empty field means "leave it alone", not "clear it".
  if (token_in != nullptr && token_in[0] != '\0') {
    snprintf(s_token, sizeof(s_token), "%s", token_in);
    prefs.putString(kPrefsTokenKey, s_token);
    ++s_generation;
  }
  prefs.end();
}

void clearCredentials() {
  s_base_url[0] = '\0';
  s_token[0] = '\0';
  taskENTER_CRITICAL(&s_entity_lock);
  s_selected[0] = '\0';
  s_control_input[0] = '\0';
  taskEXIT_CRITICAL(&s_entity_lock);
  ++s_generation;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

const char* selectedEntity() { return s_selected; }

const char* entityLabel(const char* entity_id) {
  return entity_id != nullptr ? objectId(entity_id) : "";
}

void selectEntity(const char* entity_id) {
  if (entity_id == nullptr) {
    return;
  }
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(s_selected, sizeof(s_selected), entity_id);
  taskEXIT_CRITICAL(&s_entity_lock);
  ++s_generation;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsSelectedKey, s_selected);
    prefs.end();
  }
}

int fetchPlayers(PlayerEntry* out, size_t capacity) {
  String body;
  if (!renderTemplate(kPlayersTemplate, body)) {
    return -1;
  }

  size_t count = 0;
  int pos = 0;
  const int len = static_cast<int>(body.length());
  while (pos < len && count < capacity) {
    const String entity_id = nextField(body, pos);
    if (entity_id.length() == 0) {
      break;
    }
    const String name = nextField(body, pos);
    const String available = nextField(body, pos);

    PlayerEntry& entry = out[count];
    copyField(entry.entity_id, sizeof(entry.entity_id), entity_id);
    if (name.length() > 0) {
      copyField(entry.name, sizeof(entry.name), name);
    } else {
      // No friendly name. The object_id is all that is left, and it is what
      // the row would fall back to as a duplicate anyway -- so it falls back
      // to the same thing here, rather than to a dotted id in one case and an
      // undotted one in the other.
      snprintf(entry.name, sizeof(entry.name), "%s", objectId(entry.entity_id));
    }
    entry.available = available.toInt() != 0;
    ++count;
  }

  disambiguateNames(out, count);

  LOG_DEBUG("HA: %u media players", static_cast<unsigned>(count));
  if (count == capacity) {
    LOG_WARN("HA: list hit the %u entry cap - raise config::kMaxPlayers",
                  static_cast<unsigned>(capacity));
  }
  return static_cast<int>(count);
}

bool fetchState(const char* entity_id, PlayerState& out) {
  const String tmpl = detail::stateTemplate(entity_id);
  if (tmpl.length() == 0) {
    return false;
  }

  String body;
  if (!renderTemplate(tmpl.c_str(), body)) {
    return false;
  }
  detail::parseState(body, out);
  return true;
}

namespace detail {

String stateTemplate(const char* entity_id) {
  if (!validEntityId(entity_id)) {
    setError("bad entity id");
    return String();
  }

  char control_copy[config::kEntityIdMaxLen];
  copyControlEntity(control_copy, sizeof(control_copy));
  const char* control = control_copy;
  if (!validEntityId(control)) {
    control = entity_id;
  }

  // Into a single-quoted Jinja literal, so its quotes and backslashes are
  // escaped: an input can be called anything.
  char input_copy[config::kSourceNameMaxLen];
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(input_copy, sizeof(input_copy), s_control_input);
  taskEXIT_CRITICAL(&s_entity_lock);
  char input[2 * config::kSourceNameMaxLen] = {};
  size_t n = 0;
  for (const char* p = input_copy; *p != '\0' && n + 2 < sizeof(input); ++p) {
    if (*p == '\'' || *p == '\\') {
      input[n++] = '\\';
    }
    input[n++] = *p;
  }
  input[n] = '\0';

  char tmpl[sizeof(kStateTemplateFmt) + 2 * config::kEntityIdMaxLen +
            sizeof(input)];
  snprintf(tmpl, sizeof(tmpl), kStateTemplateFmt, entity_id, control, input);
  return String(tmpl);
}

void parseState(const String& body, PlayerState& out) {
  int pos = 0;
  const String state = nextField(body, pos);
  const String title = nextField(body, pos);
  const String subtitle = nextField(body, pos);
  const String picture = nextField(body, pos);
  const String features = nextField(body, pos);
  const String duration = nextField(body, pos);
  const String position = nextField(body, pos);
  const String position_age = nextField(body, pos);
  const String name = nextField(body, pos);
  const String volume = nextField(body, pos);
  const String muted = nextField(body, pos);
  const String control_features = nextField(body, pos);
  const String control_state = nextField(body, pos);
  const String player_source = nextField(body, pos);
  const String control_source = nextField(body, pos);
  const String artist = nextField(body, pos);
  const String track = nextField(body, pos);
  const String media_entity = nextField(body, pos);
  const String player_name = nextField(body, pos);
  const String control_name = nextField(body, pos);

  out = PlayerState{};
  out.playback = parsePlaybackState(state);
  copyField(out.name, sizeof(out.name), name);
  copyField(out.title, sizeof(out.title), title);
  copyField(out.subtitle, sizeof(out.subtitle), subtitle);
  copyField(out.picture, sizeof(out.picture), picture);
  out.supported_features =
      static_cast<uint32_t>(strtoul(features.c_str(), nullptr, 10));
  out.duration_s = duration.toFloat();
  // A player with no volume_level renders as -1 and stays negative, which is
  // how the UI knows to leave the slider off rather than draw it at zero.
  out.volume = volume.toFloat();
  out.muted = muted.toInt() != 0;
  out.control_features =
      static_cast<uint32_t>(strtoul(control_features.c_str(), nullptr, 10));
  out.control_off = control_state == "off" || control_state == "standby";
  copyField(out.player_source, sizeof(out.player_source), player_source);
  copyField(out.control_source, sizeof(out.control_source), control_source);
  copyField(out.artist, sizeof(out.artist), artist);
  copyField(out.track, sizeof(out.track), track);
  copyField(out.media_entity, sizeof(out.media_entity), media_entity);
  copyField(out.player_name, sizeof(out.player_name), player_name);
  copyField(out.control_name, sizeof(out.control_name), control_name);

  // media_position is a snapshot taken at media_position_updated_at; the
  // template reports how stale that is so the bar starts in the right place.
  const float age =
      out.playback == PlaybackState::kPlaying ? position_age.toFloat() : 0.0f;
  out.position_s = position.toFloat() + age;
  out.sampled_ms = millis();
}

}  // namespace detail

namespace {

/** One service call, over the state stream when it is open and over REST
 *  when it is not -- or when the stream's owner is too busy to take it, which
 *  is the same as not open from here. `data` is the service_data object, which
 *  is also exactly the REST body. */
bool serviceCall(const char* domain, const char* service, const String& data,
                 uint16_t timeout_ms = config::kHaHttpTimeoutMs) {
  switch (detail::streamCall(domain, service, data, timeout_ms)) {
    case detail::StreamCall::kOk:
      s_last_error[0] = '\0';
      return true;
    case detail::StreamCall::kFailed:
      return false;
    case detail::StreamCall::kNotSent:
      break;
  }
  char path[96];
  snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
  String response;
  return httpPost(path, data, response, timeout_ms);
}

/** serviceCall() for a service that answers with data: `response` gets the
 *  answer, which holds the service's response object either way. Over the
 *  stream when it is open, so a library load or a search never needs a TLS
 *  session of its own beside the stream's. */
bool serviceQuery(const char* domain, const char* service, const String& data,
                  String& response,
                  uint16_t timeout_ms = config::kHaHttpTimeoutMs) {
  response = "";
  switch (detail::streamCall(domain, service, data, timeout_ms, &response)) {
    case detail::StreamCall::kOk:
      s_last_error[0] = '\0';
      return true;
    case detail::StreamCall::kFailed:
      return false;
    case detail::StreamCall::kNotSent:
      break;
  }
  char path[112];
  snprintf(path, sizeof(path), "/api/services/%s/%s?return_response", domain,
           service);
  return httpPost(path, data, response, timeout_ms);
}

}  // namespace

bool callService(const char* service, const char* entity_id) {
  if (!validEntityId(entity_id)) {
    setError("bad entity id");
    return false;
  }

  String body("{\"entity_id\":");
  appendJsonString(body, entity_id);
  body += '}';

  const bool ok = serviceCall("media_player", service, body);
  LOG_INFO("HA: media_player.%s %s -> %s", service, entity_id,
                ok ? "ok" : s_last_error);
  return ok;
}

bool selectSource(const char* entity_id, const char* source) {
  if (!validEntityId(entity_id) || source == nullptr || source[0] == '\0') {
    setError("bad source");
    return false;
  }

  String body("{\"entity_id\":");
  appendJsonString(body, entity_id);
  body += ",\"source\":";
  appendJsonString(body, source);
  body += '}';

  const bool ok = serviceCall("media_player", "select_source", body);
  LOG_INFO("HA: select_source %s \"%s\" -> %s", entity_id, source,
                ok ? "ok" : s_last_error);
  return ok;
}

bool setVolume(const char* entity_id, float level) {
  if (!validEntityId(entity_id)) {
    setError("bad entity id");
    return false;
  }
  if (level < 0.0f) {
    level = 0.0f;
  } else if (level > 1.0f) {
    level = 1.0f;
  }

  char level_text[8];
  snprintf(level_text, sizeof(level_text), "%.3f", level);

  String body("{\"entity_id\":");
  appendJsonString(body, entity_id);
  body += ",\"volume_level\":";
  body += level_text;
  body += '}';

  const bool ok = serviceCall("media_player", "volume_set", body);
  LOG_DEBUG("HA: volume_set %s %s -> %s", entity_id, level_text,
                ok ? "ok" : s_last_error);
  return ok;
}

const char* controlEntity() {
  return s_control[0] != '\0' ? s_control : s_selected;
}

const char* storedControlEntity() { return s_control; }

void copySelectedEntity(char* out, size_t out_len) {
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(out, out_len, s_selected);
  taskEXIT_CRITICAL(&s_entity_lock);
}

void copyControlEntity(char* out, size_t out_len) {
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(out, out_len, s_control[0] != '\0' ? s_control : s_selected);
  taskEXIT_CRITICAL(&s_entity_lock);
}

const char* storedControlInput() { return s_control_input; }

void selectControlInput(const char* input) {
  if (input == nullptr) {
    return;
  }
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(s_control_input, sizeof(s_control_input), input);
  taskEXIT_CRITICAL(&s_entity_lock);
  ++s_generation;  // the state template names it

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsControlInputKey, s_control_input);
  prefs.end();
  LOG_INFO("HA: player's input on the volume device: %s",
                s_control_input[0] != '\0' ? s_control_input
                                            : "the one named after it");
}

float playerWakeVolume() {
  return s_wake_volume_pct >= 0 ? s_wake_volume_pct / 100.0f : -1.0f;
}

int playerWakeVolumePercent() { return s_wake_volume_pct; }

void savePlayerWakeVolumePercent(int percent) {
  s_wake_volume_pct = percent >= 0 && percent <= 100 ? percent : -1;
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putInt(kPrefsWakeVolumeKey, s_wake_volume_pct);
  prefs.end();
  if (s_wake_volume_pct >= 0) {
    LOG_INFO("HA: player volume on wake: %d%%", s_wake_volume_pct);
  } else {
    LOG_INFO("HA: player volume on wake: left alone");
  }
}

int fetchSources(const char* entity_id, char (*out)[config::kSourceNameMaxLen],
                 int capacity) {
  if (!validEntityId(entity_id) || out == nullptr || capacity <= 0) {
    return -1;
  }
  char tmpl[sizeof(kSourcesTemplateFmt) + config::kEntityIdMaxLen];
  snprintf(tmpl, sizeof(tmpl), kSourcesTemplateFmt, entity_id);
  String body;
  if (!renderTemplate(tmpl, body)) {
    return -1;
  }
  int count = 0;
  int pos = 0;
  const int len = static_cast<int>(body.length());
  while (pos < len && count < capacity) {
    const String source = nextField(body, pos);
    if (source.length() > 0) {
      copyField(out[count++], config::kSourceNameMaxLen, source);
    }
  }
  return count;
}

bool controlIsSeparate() {
  return s_control[0] != '\0' && strcmp(s_control, s_selected) != 0;
}

void selectControlEntity(const char* entity_id) {
  if (entity_id == nullptr) {
    return;
  }
  // An empty string is a real choice, not a missing one: it means "follow the
  // media player", so it is stored rather than ignored.
  taskENTER_CRITICAL(&s_entity_lock);
  copyBounded(s_control, sizeof(s_control), entity_id);
  taskEXIT_CRITICAL(&s_entity_lock);
  ++s_generation;

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsControlKey, s_control);
  prefs.end();
  LOG_INFO("HA: volume/power entity %s",
                s_control[0] != '\0' ? s_control : "follows the player");
}

const char* maConfigEntry() { return s_ma_entry; }

void saveMaConfigEntry(const char* entry_id) {
  if (entry_id == nullptr) {
    return;
  }
  snprintf(s_ma_entry, sizeof(s_ma_entry), "%s", entry_id);

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsMaEntryKey, s_ma_entry);
  prefs.end();
  LOG_INFO("HA: music assistant entry %s",
                s_ma_entry[0] != '\0' ? s_ma_entry : "cleared");
}

int fetchLibrary(const char* media_type, const char* order_by, int limit,
                 LibraryItem* out, size_t capacity) {
  if (out == nullptr || capacity == 0 || media_type == nullptr) {
    return -1;
  }
  if (s_ma_entry[0] == '\0') {
    setError("no music assistant entry id");
    return -1;
  }
  if (limit <= 0 || limit > static_cast<int>(capacity)) {
    limit = static_cast<int>(capacity);
  }

  String body("{\"config_entry_id\":");
  appendJsonString(body, s_ma_entry);
  body += ",\"media_type\":";
  appendJsonString(body, media_type);
  if (order_by != nullptr && order_by[0] != '\0') {
    body += ",\"order_by\":";
    appendJsonString(body, order_by);
  }
  body += ",\"limit\":";
  body += limit;
  body += '}';

  String response;
  if (!serviceQuery("music_assistant", "get_library", body, response)) {
    return -1;
  }

  const char* p = strstr(response.c_str(), "\"items\":[");
  if (p == nullptr) {
    setError("no items in library response");
    LOG_WARN("HA: response body: %s", response.c_str());
    return -1;
  }
  p += 9;

  int count = 0;
  while (count < limit) {
    while (*p == ' ' || *p == ',') {
      ++p;
    }
    if (*p != '{') {
      break;  // ']' or anything else ends the array
    }

    LibraryItem& item = out[count];
    item = LibraryItem{};
    snprintf(item.media_type, sizeof(item.media_type), "%s", media_type);

    const ItemFields fields{item.uri,   sizeof(item.uri),
                            item.name,  sizeof(item.name),
                            item.image, sizeof(item.image)};
    const char* next = readLibraryItem(p, fields);
    if (next == nullptr) {
      break;
    }
    p = next;

    if (item.uri[0] != '\0' && item.name[0] != '\0') {
      ++count;
    }
  }

  LOG_DEBUG("HA: %d %s%s from the library%s%s", count, media_type,
                count == 1 ? "" : "s", order_by != nullptr ? " by " : "",
                order_by != nullptr ? order_by : "");
  return count;
}

int searchArtists(const char* name, LibraryItem* out, size_t capacity) {
  if (out == nullptr || capacity == 0 || name == nullptr || name[0] == '\0') {
    return -1;
  }
  if (s_ma_entry[0] == '\0') {
    setError("no music assistant entry id");
    return -1;
  }

  String body("{\"config_entry_id\":");
  appendJsonString(body, s_ma_entry);
  body += ",\"name\":";
  appendJsonString(body, name);
  body += ",\"media_type\":[\"artist\"],\"limit\":";
  body += static_cast<int>(capacity);
  body += '}';

  String response;
  if (!serviceQuery("music_assistant", "search", body, response,
                    config::kHaServiceTimeoutMs)) {
    return -1;
  }

  // {"artists":[...],"albums":[],...} -- the same item objects fetchLibrary
  // parses, so readLibraryItem does the work here too.
  const char* p = strstr(response.c_str(), "\"artists\":[");
  if (p == nullptr) {
    setError("no artists in search response");
    LOG_WARN("HA: response body: %s", response.c_str());
    return -1;
  }
  p += 11;

  int count = 0;
  while (count < static_cast<int>(capacity)) {
    while (*p == ' ' || *p == ',') {
      ++p;
    }
    if (*p != '{') {
      break;
    }

    LibraryItem& item = out[count];
    item = LibraryItem{};
    snprintf(item.media_type, sizeof(item.media_type), "%s", "artist");

    const ItemFields fields{item.uri,   sizeof(item.uri),
                            item.name,  sizeof(item.name),
                            item.image, sizeof(item.image)};
    const char* next = readLibraryItem(p, fields);
    if (next == nullptr) {
      break;
    }
    p = next;

    if (item.uri[0] != '\0' && item.name[0] != '\0') {
      ++count;
    }
  }

  LOG_DEBUG("HA: %d artist%s matching \"%s\"", count,
                count == 1 ? "" : "s", name);
  return count;
}

bool playMedia(const char* entity_id, const char* uri, const char* media_type,
               bool radio_mode) {
  if (!validEntityId(entity_id) || uri == nullptr || uri[0] == '\0') {
    setError("bad media id");
    return false;
  }

  String body("{\"entity_id\":");
  appendJsonString(body, entity_id);
  body += ",\"media_id\":";
  appendJsonString(body, uri);
  if (media_type != nullptr && media_type[0] != '\0') {
    // The URI already encodes the type, but saying it outright means an
    // artist is played as an artist rather than resolved to something else.
    body += ",\"media_type\":";
    appendJsonString(body, media_type);
  }
  if (radio_mode) {
    body += ",\"radio_mode\":true";
  }
  body += '}';

  const bool ok = serviceCall("music_assistant", "play_media", body,
                              config::kHaServiceTimeoutMs);
  LOG_INFO("HA: play_media%s %s on %s -> %s",
                radio_mode ? " (radio)" : "", uri, entity_id,
                ok ? "ok" : s_last_error);
  return ok;
}

float interpolatedPosition(const PlayerState& state) {
  if (state.duration_s <= 0.0f) {
    return -1.0f;
  }
  float position = state.position_s;
  if (state.playback == PlaybackState::kPlaying && state.sampled_ms != 0) {
    position += (millis() - state.sampled_ms) / 1000.0f;
  }
  if (position < 0.0f) {
    return 0.0f;
  }
  if (position > state.duration_s) {
    return state.duration_s;
  }
  return position;
}

const char* lastError() { return s_last_error; }

namespace detail {

void appendJson(String& out, const char* value) { appendJsonString(out, value); }

const char* readJson(const char* p, char* out, size_t out_len) {
  return readJsonString(p, out, out_len);
}

const char* skipJson(const char* p) { return skipJsonValue(p); }

void reportError(const char* message) { setError("%s", message); }

uint32_t settingsGeneration() { return s_generation; }

void dropRestConnection() {
  ConnectionGuard connection;
  s_http.end();
  s_secure.stop();
  s_plain.stop();
}

bool serverAddress(char* host, size_t host_len, uint16_t& port, bool& tls,
                   char* prefix, size_t prefix_len) {
  if (host_len == 0 || prefix_len == 0) {
    return false;
  }
  tls = usesTls();
  port = tls ? 443 : 80;
  hostFromBaseUrl(host, host_len);
  prefix[0] = '\0';
  if (host[0] == '\0') {
    return false;
  }
  const char* start = strstr(s_base_url, "://");
  start = (start != nullptr) ? start + 3 : s_base_url;
  const char* rest = start + strlen(host);
  if (*rest == ':') {
    port = static_cast<uint16_t>(strtoul(rest + 1, nullptr, 10));
    while (*rest != '\0' && *rest != '/') {
      ++rest;
    }
  }
  // Whatever path the base URL carries -- an install behind a reverse proxy
  // at /ha -- goes in front of /api/websocket just as it goes in front of
  // /api/template.
  snprintf(prefix, prefix_len, "%s", rest);
  return port != 0;
}

void logHeap(const char* what) {
  LOG_DEBUG("HA: %s (internal heap free %u, largest %u)", what,
                static_cast<unsigned>(heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

}  // namespace detail

void logHttpRequest(const char* method, const char* url, int status,
                    int request_bytes, int response_bytes,
                    unsigned long elapsed_ms) {
  // A request that worked is detail; one that did not is worth a line at the
  // ordinary level, so it is seen without turning the rest on.
  const bool failed = status <= 0 || status >= 400;
  if (!logging::enabled(failed ? config::LogLevel::kWarn
                               : config::LogLevel::kDebug)) {
    return;
  }
  char received[12];
  if (response_bytes < 0) {
    snprintf(received, sizeof(received), "?");
  } else {
    snprintf(received, sizeof(received), "%d", response_bytes);
  }
  if (failed) {
    LOG_WARN("HTTP %s %s -> %d (tx %d, rx %s bytes, %lu ms)", method, url,
             status, request_bytes, received, elapsed_ms);
  } else {
    LOG_DEBUG("HTTP %s %s -> %d (tx %d, rx %s bytes, %lu ms)", method, url,
              status, request_bytes, received, elapsed_ms);
  }
}

}  // namespace services::ha
