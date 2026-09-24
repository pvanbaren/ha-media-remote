#pragma once

#include <Arduino.h>
#include <NetworkClient.h>

#include <cstddef>
#include <cstdint>

namespace services {

/**
 * The client half of RFC 6455, over a NetworkClient -- which here is one of
 * the services::Yielding clients, so that every wait in it blocks rather than
 * spins (see yielding_client.h for why that matters on this board).
 *
 * Deliberately small. Text messages only, no extensions (no permessage-deflate:
 * the messages this carries are a few hundred bytes, and inflate would want a
 * 32 KB window), no subprotocols. Pings are answered and a close is honoured
 * inside poll(), so a caller only ever sees whole text messages.
 *
 * Not thread-safe. One task owns an instance, as one task owns a TLS context.
 */
class WebSocket {
 public:
  enum class Poll : uint8_t {
    /** Nothing was waiting. Returned at once rather than blocking. */
    kNone,
    /** `out` holds one complete text message. */
    kMessage,
    /** A message was waiting but exceeded `max_len`; it was read and dropped. */
    kDropped,
    /** The connection is gone, by a close frame, a read error or a timeout. */
    kClosed,
  };

  /** Connect `client` to host:port and perform the opening handshake on
   *  `path`. `client` must outlive the connection. False on any failure, with
   *  the reason in error(). */
  bool open(NetworkClient& client, const char* host, uint16_t port,
            const char* path, uint32_t timeout_ms);

  bool connected();

  /** Send one text message, masked as a client must. */
  bool sendText(const char* data, size_t len);

  /** Read one message if one has started to arrive. Once a frame has begun,
   *  waits up to the open() timeout for the rest of it. */
  Poll poll(String& out, size_t max_len);

  /** Send a close frame and drop the connection. */
  void close();

  const char* error() const { return error_; }

 private:
  bool readExact(uint8_t* buf, size_t len);
  bool writeAll(const uint8_t* buf, size_t len);
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  bool discard(uint64_t len);
  bool readLine(char* buf, size_t len);
  void fail(const char* why);

  NetworkClient* client_ = nullptr;
  uint32_t timeout_ms_ = 5000;
  char error_[64] = {};
};

}  // namespace services
