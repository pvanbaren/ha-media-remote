/**
 * The state stream: Home Assistant's WebSocket API, subscribed to the state
 * template with render_template. See streamOpen() in ha_client.h for what it
 * is for; this is how.
 *
 * The protocol, as much of it as is used:
 *
 *   server  {"type":"auth_required","ha_version":"..."}
 *   client  {"type":"auth","access_token":"..."}
 *   server  {"type":"auth_ok",...}             or auth_invalid
 *   client  {"id":1,"type":"render_template","template":"...",...}
 *   server  {"id":1,"type":"result","success":true,"result":null}
 *   server  {"id":1,"type":"event","event":{"result":"<rendered>",...}}
 *           ...another event whenever anything the template reads changes
 *   client  {"id":7,"type":"call_service","domain":...,"service_data":{...}}
 *   server  {"id":7,"type":"result","success":true,...}
 *
 * A call that wants data -- Music Assistant's library and search -- adds
 * "return_response":true and gets it back in the result. Those go this way
 * too while the stream is open, not over REST: mbedTLS takes its buffers from
 * internal RAM only, and a REST session beside the stream's, with a cover or
 * a thumbnail being fetched as well, is one session more than it holds.
 *
 * One task owns the socket: whichever calls streamOpen(). A service called
 * from any other task is handed over through a one-deep slot and runs on the
 * owner's next pass, so the TLS context is only ever driven from one task --
 * the rule the REST connection needs a mutex to keep.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>

#include "config.h"
#include "log.h"
#include "ha_internal.h"
#include "services/ha_client.h"
#include "services/websocket.h"
#include "services/yielding_client.h"

namespace services::ha {
namespace {

// The stream's own clients, never the REST connection's: that one is shared
// under a mutex, and this one is owned outright.
services::YieldingClient s_plain;
services::YieldingClientSecure s_secure;
services::WebSocket s_ws;

std::atomic<bool> s_open{false};
TaskHandle_t s_owner = nullptr;
uint32_t s_generation = 0;
int s_next_id = 1;
int s_subscription_id = 0;

/** The latest rendering not yet handed to streamService()'s caller. */
String s_pending;
bool s_have_pending = false;
unsigned long s_pending_ms = 0;

unsigned long s_last_heard_ms = 0;
unsigned long s_last_ping_ms = 0;

/** A service the owner itself is waiting on, inline. */
int s_own_call_id = 0;
bool s_own_call_done = false;
bool s_own_call_ok = false;
/** Where the owner's own call wants its result message; nullptr for none. */
String* s_own_call_response = nullptr;

/** A service handed over by another task. One at a time: callers take
 *  s_handoff_mutex for the whole call. State moves under s_handoff_lock,
 *  and the body is copied out by the owner under it too, so a caller that
 *  gave up can reuse the slot without racing a send. */
enum class Handoff : uint8_t { kFree, kQueued, kSent, kDone };
constexpr size_t kHandoffMax = 768;
struct {
  Handoff state = Handoff::kFree;
  int id = 0;
  bool ok = false;
  /** The caller wants the result message back. */
  bool want_response = false;
  /** That message: copied by the owner outside the lock, attached under it,
   *  and taken and freed by the caller. */
  char* response = nullptr;
  char body[kHandoffMax] = {};
  char error[96] = {};
} s_handoff;
portMUX_TYPE s_handoff_lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t s_handoff_mutex = nullptr;
SemaphoreHandle_t s_handoff_done = nullptr;
/** The owner's copy of a handed-over body, taken under the lock. */
char s_handoff_sending[kHandoffMax] = {};

using detail::eachMember;

/** The "message" of an error object, for the log and lastError(). */
void errorMessage(const char* error_value, char* out, size_t out_len) {
  out[0] = '\0';
  eachMember(error_value, [&](const char* key, const char* value) {
    if (strcmp(key, "message") == 0 && *value == '"') {
      detail::readJson(value, out, out_len);
    }
  });
}

bool send(const String& message) {
  if (!s_ws.sendText(message.c_str(), message.length())) {
    return false;
  }
  return true;
}

/** Wrap `body` -- the members after the id -- as a request, and send it.
 *  Returns its id, or 0 if the send failed. */
int sendRequest(const char* body) {
  const int id = s_next_id++;
  String message("{\"id\":");
  message += id;
  message += ',';
  message += body;
  message += '}';
  return send(message) ? id : 0;
}

void finishHandoff(bool ok, const char* error) {
  bool signal = false;
  taskENTER_CRITICAL(&s_handoff_lock);
  if (s_handoff.state == Handoff::kSent) {
    s_handoff.state = Handoff::kDone;
    s_handoff.ok = ok;
    snprintf(s_handoff.error, sizeof(s_handoff.error), "%s", error);
    signal = true;
  }
  taskEXIT_CRITICAL(&s_handoff_lock);
  if (signal) {
    xSemaphoreGive(s_handoff_done);
  }
}

void closeStream(const char* why) {
  if (!s_open) {
    return;
  }
  s_open = false;
  s_ws.close();
  s_have_pending = false;
  s_pending = "";
  LOG_INFO("HA: stream closed, %s", why);
  finishHandoff(false, "stream closed");
  s_own_call_done = true;
  s_own_call_ok = false;
}

void handleMessage(const String& text) {
  int id = 0;
  char type[24] = {};
  bool success = false;
  const char* event = nullptr;
  const char* error = nullptr;
  const bool parsed =
      eachMember(text.c_str(), [&](const char* key, const char* value) {
        if (strcmp(key, "id") == 0) {
          id = atoi(value);
        } else if (strcmp(key, "type") == 0 && *value == '"') {
          detail::readJson(value, type, sizeof(type));
        } else if (strcmp(key, "success") == 0) {
          success = *value == 't';
        } else if (strcmp(key, "event") == 0) {
          event = value;
        } else if (strcmp(key, "error") == 0) {
          error = value;
        }
      });
  if (!parsed) {
    LOG_WARN("HA: stream message not understood: %.80s", text.c_str());
    return;
  }

  if (strcmp(type, "event") == 0 && id == s_subscription_id) {
    // The rendering, as a JSON string. Decoding only ever shrinks it, so the
    // text's own length is enough room.
    eachMember(event, [&](const char* key, const char* value) {
      if (strcmp(key, "result") == 0 && *value == '"') {
        const size_t room = text.length() + 1;
        char* rendered = static_cast<char*>(malloc(room));
        if (rendered == nullptr) {
          return;
        }
        if (detail::readJson(value, rendered, room) != nullptr) {
          s_pending = rendered;
          s_have_pending = true;
          s_pending_ms = millis();
        }
        free(rendered);
      } else if (strcmp(key, "error") == 0 && *value == '"') {
        char message[96];
        detail::readJson(value, message, sizeof(message));
        LOG_ERROR("HA: stream template error: %s", message);
      }
    });
    return;
  }

  if (strcmp(type, "result") != 0) {
    return;  // "pong", or something this does not use
  }

  char message[96] = {};
  if (!success && error != nullptr) {
    errorMessage(error, message, sizeof(message));
  }

  if (id == s_subscription_id) {
    if (!success) {
      char why[128];
      snprintf(why, sizeof(why), "stream subscribe refused: %s", message);
      detail::reportError(why);
      closeStream("subscription refused");
    }
    return;
  }
  if (id == s_own_call_id) {
    s_own_call_done = true;
    s_own_call_ok = success;
    if (success && s_own_call_response != nullptr) {
      *s_own_call_response = text;
    }
    if (!success) {
      detail::reportError(message[0] != '\0' ? message : "service failed");
    }
    return;
  }

  // A handed-over call's data is copied before the lock is taken -- no
  // allocating inside a critical section -- and only when it is that call's.
  bool wanted = false;
  taskENTER_CRITICAL(&s_handoff_lock);
  wanted = success && s_handoff.state == Handoff::kSent &&
           s_handoff.id == id && s_handoff.want_response;
  taskEXIT_CRITICAL(&s_handoff_lock);
  char* copy = nullptr;
  if (wanted) {
    copy = static_cast<char*>(heap_caps_malloc(
        text.length() + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
      copy = static_cast<char*>(malloc(text.length() + 1));
    }
    if (copy != nullptr) {
      memcpy(copy, text.c_str(), text.length() + 1);
    }
  }

  bool signal = false;
  taskENTER_CRITICAL(&s_handoff_lock);
  if (s_handoff.state == Handoff::kSent && s_handoff.id == id) {
    s_handoff.state = Handoff::kDone;
    s_handoff.ok = success && (!wanted || copy != nullptr);
    s_handoff.response = copy;
    copy = nullptr;
    snprintf(s_handoff.error, sizeof(s_handoff.error), "%s",
             message[0] != '\0'  ? message
             : success            ? "out of memory"
                                  : "service failed");
    signal = true;
  }
  taskEXIT_CRITICAL(&s_handoff_lock);
  free(copy);  // the caller gave up meanwhile
  if (signal) {
    xSemaphoreGive(s_handoff_done);
  }
}

/** Send a handed-over service, if one is waiting. */
void takeHandoff() {
  bool picked = false;
  int id = 0;
  taskENTER_CRITICAL(&s_handoff_lock);
  if (s_handoff.state == Handoff::kQueued) {
    memcpy(s_handoff_sending, s_handoff.body, sizeof(s_handoff_sending));
    id = s_next_id++;
    s_handoff.id = id;
    s_handoff.state = Handoff::kSent;
    picked = true;
  }
  taskEXIT_CRITICAL(&s_handoff_lock);
  if (!picked) {
    return;
  }
  String message("{\"id\":");
  message += id;
  message += ',';
  message += s_handoff_sending;
  message += '}';
  if (!send(message)) {
    closeStream(s_ws.error());
  }
}

/** Read what has arrived and keep the connection alive, for up to `wait_ms`.
 *  Returns early once `until` says so. */
template <typename Until>
void pump(uint32_t wait_ms, Until until) {
  const unsigned long started = millis();
  for (;;) {
    if (!s_open) {
      return;
    }
    takeHandoff();

    bool busy = false;
    String text;
    switch (s_ws.poll(text, config::kHaStreamMaxMessage)) {
      case WebSocket::Poll::kMessage:
        s_last_heard_ms = millis();
        handleMessage(text);
        busy = true;
        break;
      case WebSocket::Poll::kDropped:
        s_last_heard_ms = millis();
        LOG_WARN("HA: stream message over the size limit, dropped");
        busy = true;
        break;
      case WebSocket::Poll::kClosed:
        closeStream(s_ws.error()[0] != '\0' ? s_ws.error() : "connection lost");
        return;
      case WebSocket::Poll::kNone:
        break;
    }

    const unsigned long now = millis();
    if (now - s_last_heard_ms >= config::kHaStreamSilentMs) {
      closeStream("nothing heard");
      return;
    }
    if (now - s_last_ping_ms >= config::kHaStreamPingMs) {
      s_last_ping_ms = now;
      if (sendRequest("\"type\":\"ping\"") == 0) {
        closeStream(s_ws.error());
        return;
      }
    }

    if (until() || now - started >= wait_ms) {
      return;
    }
    if (!busy) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

/** Wait for one whole message, during the handshake before there is
 *  anything else to do with them. */
bool receive(String& text, uint32_t timeout_ms) {
  const unsigned long started = millis();
  for (;;) {
    switch (s_ws.poll(text, config::kHaStreamMaxMessage)) {
      case WebSocket::Poll::kMessage:
        return true;
      case WebSocket::Poll::kClosed:
        return false;
      case WebSocket::Poll::kDropped:
      case WebSocket::Poll::kNone:
        break;
    }
    if (millis() - started >= timeout_ms) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/** The "type" of a message, for the handshake's fixed exchange. */
void typeOf(const String& text, char* out, size_t out_len) {
  out[0] = '\0';
  eachMember(text.c_str(), [&](const char* key, const char* value) {
    if (strcmp(key, "type") == 0 && *value == '"') {
      detail::readJson(value, out, out_len);
    }
  });
}

/** Fail the open: say why, and give the socket back. */
bool refuse(const char* why) {
  char message[128];
  snprintf(message, sizeof(message), "stream: %s", why);
  detail::reportError(message);
  s_ws.close();
  return false;
}

}  // namespace

bool streamOpen(const char* entity_id) {
  if (s_open) {
    return true;
  }
  if (!configured() || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  const String tmpl = detail::stateTemplate(entity_id);
  if (tmpl.length() == 0) {
    return false;
  }

  char host[config::kHaBaseUrlMaxLen + 1];
  char prefix[config::kHaBaseUrlMaxLen + 1];
  uint16_t port = 0;
  bool tls = false;
  if (!detail::serverAddress(host, sizeof(host), port, tls, prefix,
                             sizeof(prefix))) {
    return refuse("bad base URL");
  }
  char path[sizeof(prefix) + 16];
  snprintf(path, sizeof(path), "%s/api/websocket", prefix);

  s_owner = xTaskGetCurrentTaskHandle();
  s_generation = detail::settingsGeneration();

  // One TLS session at a time: the REST connection's goes before this one's
  // is made, rather than after, so the two never both hold their 32 KB.
  detail::dropRestConnection();

  NetworkClient* client = &s_plain;
  if (tls) {
    // As for REST: a local install's certificate is usually self-signed, and
    // there is no clock to check its dates against.
    s_secure.setInsecure();
    // The connect timeout below covers TCP only. Left to itself the handshake
    // may take two minutes, all of it on the task that would otherwise be
    // polling -- the screen frozen on old state while a stalled server makes
    // up its mind.
    s_secure.setHandshakeTimeout(config::kHaStreamHandshakeMs / 1000);
    client = &s_secure;
  }
  LOG_INFO("HA: stream connecting to %s:%u%s", host,
                static_cast<unsigned>(port), path);
  if (!s_ws.open(*client, host, port, path, config::kHaHttpTimeoutMs)) {
    detail::logHeap("stream connect failed");
    return refuse(s_ws.error());
  }

  String text;
  char type[24];
  if (!receive(text, config::kHaHttpTimeoutMs)) {
    return refuse("no greeting");
  }
  typeOf(text, type, sizeof(type));
  if (strcmp(type, "auth_required") != 0) {
    return refuse("unexpected greeting");
  }

  String auth("{\"type\":\"auth\",\"access_token\":");
  detail::appendJson(auth, token());
  auth += '}';
  if (!send(auth) || !receive(text, config::kHaHttpTimeoutMs)) {
    return refuse("no answer to auth");
  }
  typeOf(text, type, sizeof(type));
  if (strcmp(type, "auth_ok") != 0) {
    return refuse("token rejected");
  }

  String subscribe("\"type\":\"render_template\",\"template\":");
  detail::appendJson(subscribe, tmpl.c_str());
  // Without this a template error is only written to Home Assistant's log,
  // and the stream just goes quiet.
  subscribe += ",\"report_errors\":true";
  s_subscription_id = sendRequest(subscribe.c_str());
  if (s_subscription_id == 0) {
    return refuse("subscribe failed");
  }

  s_open = true;
  s_have_pending = false;
  s_last_heard_ms = s_last_ping_ms = millis();
  LOG_INFO("HA: stream open, following %s", entity_id);
  detail::logHeap("stream open");
  return true;
}

bool streamIsOpen() { return s_open; }

void streamClose() { closeStream("asked to"); }

bool streamService(PlayerState& out, uint32_t wait_ms) {
  if (!s_open) {
    return false;
  }
  if (detail::settingsGeneration() != s_generation) {
    closeStream("settings changed");
    return false;
  }

  // Wait for a state, then keep reading for the settle time after the
  // latest one, so a burst hands on only its last word.
  pump(wait_ms, [] {
    return s_have_pending &&
           millis() - s_pending_ms >= config::kHaStreamSettleMs;
  });
  if (!s_open || !s_have_pending) {
    return false;
  }
  detail::parseState(s_pending, out);
  s_have_pending = false;
  s_pending = "";
  return true;
}

namespace detail {

void streamInit() {
  if (s_handoff_mutex == nullptr) {
    s_handoff_mutex = xSemaphoreCreateMutex();
  }
  if (s_handoff_done == nullptr) {
    s_handoff_done = xSemaphoreCreateBinary();
  }
}

StreamCall streamCall(const char* domain, const char* service,
                      const String& service_data, uint32_t timeout_ms,
                      String* response) {
  if (!config::kHaStreamEnabled || !s_open) {
    return StreamCall::kNotSent;
  }

  String body("\"type\":\"call_service\",\"domain\":");
  appendJson(body, domain);
  body += ",\"service\":";
  appendJson(body, service);
  body += ",\"service_data\":";
  body += service_data;
  if (response != nullptr) {
    body += ",\"return_response\":true";
  }

  if (xTaskGetCurrentTaskHandle() == s_owner) {
    // The owner itself, from handlePlayerWake() or a power flag: send and
    // read until the answer comes back, storing any state that arrives
    // meanwhile for the next streamService().
    s_own_call_done = false;
    s_own_call_response = response;
    s_own_call_id = sendRequest(body.c_str());
    if (s_own_call_id == 0) {
      s_own_call_response = nullptr;
      closeStream(s_ws.error());
      return StreamCall::kNotSent;
    }
    pump(timeout_ms, [] { return s_own_call_done; });
    const bool done = s_own_call_done;
    s_own_call_id = 0;
    s_own_call_response = nullptr;
    if (!done) {
      reportError("no answer to service call");
      return StreamCall::kFailed;
    }
    return s_own_call_ok ? StreamCall::kOk : StreamCall::kFailed;
  }

  if (body.length() >= kHandoffMax || s_handoff_mutex == nullptr) {
    return StreamCall::kNotSent;
  }
  xSemaphoreTake(s_handoff_mutex, portMAX_DELAY);
  xSemaphoreTake(s_handoff_done, 0);  // a signal left by a caller that gave up

  taskENTER_CRITICAL(&s_handoff_lock);
  memcpy(s_handoff.body, body.c_str(), body.length() + 1);
  s_handoff.ok = false;
  s_handoff.want_response = response != nullptr;
  s_handoff.error[0] = '\0';
  s_handoff.state = Handoff::kQueued;
  taskEXIT_CRITICAL(&s_handoff_lock);

  // Picked up promptly, or not at all: an owner deep in a library load would
  // leave a button press waiting seconds, and REST can carry it now.
  const unsigned long started = millis();
  bool taken = false;
  while (millis() - started < config::kHaStreamPickupMs) {
    taskENTER_CRITICAL(&s_handoff_lock);
    taken = s_handoff.state != Handoff::kQueued;
    taskEXIT_CRITICAL(&s_handoff_lock);
    if (taken) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (!taken) {
    taskENTER_CRITICAL(&s_handoff_lock);
    if (s_handoff.state == Handoff::kQueued) {
      s_handoff.state = Handoff::kFree;
    } else {
      taken = true;  // taken in the last moment after all
    }
    taskEXIT_CRITICAL(&s_handoff_lock);
    if (!taken) {
      xSemaphoreGive(s_handoff_mutex);
      return StreamCall::kNotSent;
    }
  }

  xSemaphoreTake(s_handoff_done, pdMS_TO_TICKS(timeout_ms));
  bool ok = false;
  bool done = false;
  char error[sizeof(s_handoff.error)] = {};
  char* answer = nullptr;
  taskENTER_CRITICAL(&s_handoff_lock);
  done = s_handoff.state == Handoff::kDone;
  ok = done && s_handoff.ok;
  memcpy(error, s_handoff.error, sizeof(error));
  answer = s_handoff.response;
  s_handoff.response = nullptr;
  // Freed either way. A late answer to a call given up on carries an id
  // nothing is waiting for any more, and is ignored.
  s_handoff.state = Handoff::kFree;
  taskEXIT_CRITICAL(&s_handoff_lock);
  xSemaphoreGive(s_handoff_mutex);
  if (answer != nullptr) {
    if (ok && response != nullptr) {
      *response = answer;
    }
    free(answer);
  }

  if (!done) {
    reportError("no answer to service call");
    return StreamCall::kFailed;
  }
  if (!ok) {
    reportError(error);
    return StreamCall::kFailed;
  }
  return StreamCall::kOk;
}

}  // namespace detail
}  // namespace services::ha
