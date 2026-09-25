#pragma once

/**
 * What ha_client.cpp and ha_stream.cpp share and nobody else needs: the JSON
 * helpers, the state template and its parser, and the bookkeeping the state
 * stream keeps with the REST side. Private to src/services/.
 */

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "services/ha_client.h"

namespace services::ha::detail {

/** Append `value` as a JSON string literal, every control character escaped. */
void appendJson(String& out, const char* value);
/** Read the JSON string literal at `p` (its opening quote) into `out`, decoding
 *  escapes; returns what follows the closing quote, or nullptr. */
const char* readJson(const char* p, char* out, size_t out_len);
/** Step over one JSON value of any kind; nullptr if it never terminates. */
const char* skipJson(const char* p);

inline const char* skipJsonSpace(const char* p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
    ++p;
  }
  return p;
}

/** Walk the members of the JSON object at `p`, calling f(key, value) for
 *  each. Values are stepped over whole, so nested objects cannot be mistaken
 *  for the outer one's members. False if the text is malformed. */
template <typename F>
bool eachMember(const char* p, F&& f) {
  if (p == nullptr) {
    return false;
  }
  p = skipJsonSpace(p);
  if (*p != '{') {
    return false;
  }
  ++p;
  for (;;) {
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
      ++p;
    }
    if (*p == '}') {
      return true;
    }
    if (*p != '"') {
      return false;
    }
    char key[24];
    const char* after = readJson(p, key, sizeof(key));
    if (after == nullptr) {
      return false;
    }
    after = skipJsonSpace(after);
    if (*after != ':') {
      return false;
    }
    const char* value = skipJsonSpace(after + 1);
    f(key, value);
    p = skipJson(value);
    if (p == nullptr) {
      return false;
    }
  }
}

/** Walk the elements of the JSON array at `p`, calling f(value) for each
 *  until it returns false. False if the text is malformed. */
template <typename F>
bool eachElement(const char* p, F&& f) {
  if (p == nullptr) {
    return false;
  }
  p = skipJsonSpace(p);
  if (*p != '[') {
    return false;
  }
  ++p;
  for (;;) {
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
      ++p;
    }
    if (*p == ']') {
      return true;
    }
    if (*p == '\0') {
      return false;
    }
    if (!f(p)) {
      return true;
    }
    p = skipJson(p);
    if (p == nullptr) {
      return false;
    }
  }
}

/** Record `message` as lastError() and log it. */
void reportError(const char* message);
/** Log the internal heap alongside `what`: the numbers a failed TLS handshake
 *  needs next to it. */
void logHeap(const char* what);

/** The state template for `entity_id` and the configured volume/power
 *  entity, ready to render. Empty (with lastError() set) for a bad id. */
String stateTemplate(const char* entity_id);
/** Parse one rendering of stateTemplate() into `out`, stamping sampled_ms. */
void parseState(const String& body, PlayerState& out);

/** Changes whenever the server, token, player or control entity does. */
uint32_t settingsGeneration();

/** Close the kept-alive REST connection, so its TLS session is not held
 *  alongside the stream's. Safe from any task. */
void dropRestConnection();

/** The base URL taken apart for a socket: host, port (the scheme's default
 *  when none is given), whether it is TLS, and any path in front of /api. */
bool serverAddress(char* host, size_t host_len, uint16_t& port, bool& tls,
                   char* prefix, size_t prefix_len);

/** Create the stream's locks. From ha::init(), before any task exists. */
void streamInit();

enum class StreamCall : uint8_t {
  /** Home Assistant ran it. */
  kOk,
  /** Sent, and Home Assistant refused it or never answered. Not retried
   *  elsewhere: it may well have happened. */
  kFailed,
  /** Never sent -- no stream, or its owner could not take it in time. Safe to
   *  send another way. */
  kNotSent,
};

/** Run a service over the stream: from any task, blocking until the answer.
 *  With `response`, the service is asked for its return_response and the
 *  whole result message lands there -- the response object inside it, which
 *  the REST side's parsers find by key just as they do in a REST body. */
StreamCall streamCall(const char* domain, const char* service,
                      const String& service_data, uint32_t timeout_ms,
                      String* response = nullptr);

}  // namespace services::ha::detail
