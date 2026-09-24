#include "services/websocket.h"

#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace services {
namespace {

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

/** The GUID RFC 6455 section 1.3 appends to the key before hashing it. */
constexpr char kAcceptGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** What the server must send back for `key`: base64(sha1(key + GUID)). */
bool expectedAccept(const char* key, char* out, size_t out_len) {
  char joined[64];
  const int n = snprintf(joined, sizeof(joined), "%s%s", key, kAcceptGuid);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(joined)) {
    return false;
  }
  unsigned char digest[20];
  if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(joined), n, digest) != 0) {
    return false;
  }
  size_t written = 0;
  return mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), out_len,
                               &written, digest, sizeof(digest)) == 0;
}

}  // namespace

void WebSocket::fail(const char* why) {
  snprintf(error_, sizeof(error_), "%s", why);
  if (client_ != nullptr) {
    client_->stop();
  }
}

bool WebSocket::connected() {
  return client_ != nullptr && client_->connected();
}

bool WebSocket::readExact(uint8_t* buf, size_t len) {
  const unsigned long started = millis();
  size_t got = 0;
  while (got < len) {
    // A Yielding client sleeps a millisecond when this comes back empty, so
    // the loop blocks rather than spins while the rest of a frame is in
    // flight.
    const int n = client_->read(buf + got, len - got);
    if (n > 0) {
      got += static_cast<size_t>(n);
      continue;
    }
    if (!client_->connected() && client_->available() <= 0) {
      return false;
    }
    if (millis() - started >= timeout_ms_) {
      return false;
    }
  }
  return true;
}

bool WebSocket::writeAll(const uint8_t* buf, size_t len) {
  size_t sent = 0;
  const unsigned long started = millis();
  while (sent < len) {
    const size_t n = client_->write(buf + sent, len - sent);
    if (n == 0) {
      if (!client_->connected() || millis() - started >= timeout_ms_) {
        return false;
      }
      delay(1);
      continue;
    }
    sent += n;
  }
  return true;
}

bool WebSocket::readLine(char* buf, size_t len) {
  size_t filled = 0;
  for (;;) {
    uint8_t c = 0;
    if (!readExact(&c, 1)) {
      return false;
    }
    if (c == '\n') {
      break;
    }
    if (c != '\r' && filled + 1 < len) {
      buf[filled++] = static_cast<char>(c);
    }
  }
  buf[filled] = '\0';
  return true;
}

bool WebSocket::open(NetworkClient& client, const char* host, uint16_t port,
                     const char* path, uint32_t timeout_ms) {
  client_ = &client;
  timeout_ms_ = timeout_ms;
  error_[0] = '\0';

  if (!client.connect(host, port, static_cast<int32_t>(timeout_ms))) {
    fail("connect failed");
    return false;
  }

  // Sixteen random bytes, base64: the nonce the server has to hash back.
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  char key[32];
  size_t key_len = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(key), sizeof(key),
                            &key_len, nonce, sizeof(nonce)) != 0) {
    fail("key encode failed");
    return false;
  }
  key[key_len] = '\0';

  char request[384];
  const int n = snprintf(request, sizeof(request),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s:%u\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "\r\n",
                         path, host, static_cast<unsigned>(port), key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(request) ||
      !writeAll(reinterpret_cast<const uint8_t*>(request), n)) {
    fail("handshake write failed");
    return false;
  }

  char line[160];
  if (!readLine(line, sizeof(line))) {
    fail("no handshake response");
    return false;
  }
  // "HTTP/1.1 101 Switching Protocols". Anything else -- a 404 from a proxy
  // that does not pass the upgrade, a 400 -- is a server that will not talk.
  const char* status = strchr(line, ' ');
  if (status == nullptr || atoi(status + 1) != 101) {
    char why[64];
    snprintf(why, sizeof(why), "upgrade refused: %.40s", line);
    fail(why);
    return false;
  }

  char expected[32];
  if (!expectedAccept(key, expected, sizeof(expected))) {
    fail("accept hash failed");
    return false;
  }
  bool accepted = false;
  for (int lines = 0; lines < 32; ++lines) {
    if (!readLine(line, sizeof(line))) {
      fail("handshake headers cut off");
      return false;
    }
    if (line[0] == '\0') {
      break;  // end of headers
    }
    constexpr char kAccept[] = "sec-websocket-accept:";
    if (strncasecmp(line, kAccept, sizeof(kAccept) - 1) == 0) {
      const char* value = line + sizeof(kAccept) - 1;
      while (*value == ' ') {
        ++value;
      }
      accepted = strcmp(value, expected) == 0;
    }
  }
  if (!accepted) {
    fail("bad Sec-WebSocket-Accept");
    return false;
  }
  return true;
}

bool WebSocket::sendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!connected()) {
    return false;
  }
  uint8_t header[14];
  size_t h = 0;
  header[h++] = 0x80 | opcode;  // FIN: this client never fragments
  if (len < 126) {
    header[h++] = 0x80 | static_cast<uint8_t>(len);
  } else if (len <= 0xFFFF) {
    header[h++] = 0x80 | 126;
    header[h++] = static_cast<uint8_t>(len >> 8);
    header[h++] = static_cast<uint8_t>(len);
  } else {
    header[h++] = 0x80 | 127;
    for (int shift = 56; shift >= 0; shift -= 8) {
      header[h++] = static_cast<uint8_t>(static_cast<uint64_t>(len) >> shift);
    }
  }
  uint8_t mask[4];
  esp_fill_random(mask, sizeof(mask));
  memcpy(header + h, mask, sizeof(mask));
  h += sizeof(mask);
  if (!writeAll(header, h)) {
    return false;
  }

  // Masked in small pieces rather than into a copy of the whole payload.
  uint8_t chunk[128];
  for (size_t done = 0; done < len;) {
    const size_t n = (len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk);
    for (size_t i = 0; i < n; ++i) {
      chunk[i] = data[done + i] ^ mask[(done + i) & 3];
    }
    if (!writeAll(chunk, n)) {
      return false;
    }
    done += n;
  }
  return true;
}

bool WebSocket::sendText(const char* data, size_t len) {
  if (!sendFrame(kOpText, reinterpret_cast<const uint8_t*>(data), len)) {
    fail("send failed");
    return false;
  }
  return true;
}

bool WebSocket::discard(uint64_t len) {
  uint8_t sink[128];
  while (len > 0) {
    const size_t n = len < sizeof(sink) ? static_cast<size_t>(len) : sizeof(sink);
    if (!readExact(sink, n)) {
      return false;
    }
    len -= n;
  }
  return true;
}

WebSocket::Poll WebSocket::poll(String& out, size_t max_len) {
  if (client_ == nullptr) {
    return Poll::kClosed;
  }
  if (client_->available() <= 0) {
    return client_->connected() ? Poll::kNone : Poll::kClosed;
  }

  out = "";
  bool dropping = false;
  bool in_message = false;
  for (;;) {
    uint8_t head[2];
    if (!readExact(head, sizeof(head))) {
      fail("read failed");
      return Poll::kClosed;
    }
    const bool fin = (head[0] & 0x80) != 0;
    const uint8_t opcode = head[0] & 0x0F;
    const bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7F;
    if (len == 126) {
      uint8_t ext[2];
      if (!readExact(ext, sizeof(ext))) {
        fail("read failed");
        return Poll::kClosed;
      }
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (!readExact(ext, sizeof(ext))) {
        fail("read failed");
        return Poll::kClosed;
      }
      len = 0;
      for (uint8_t b : ext) {
        len = (len << 8) | b;
      }
    }
    // A server must not mask (RFC 6455 5.1), but unmasking costs nothing and
    // refusing would be pedantry.
    uint8_t mask[4] = {};
    if (masked && !readExact(mask, sizeof(mask))) {
      fail("read failed");
      return Poll::kClosed;
    }

    if (opcode >= kOpClose) {
      // Control frames: at most 125 bytes, never fragmented, and allowed in
      // the middle of a fragmented message.
      uint8_t payload[125];
      const size_t n = len <= sizeof(payload) ? static_cast<size_t>(len) : 0;
      if (len > sizeof(payload) || !readExact(payload, n)) {
        fail("bad control frame");
        return Poll::kClosed;
      }
      for (size_t i = 0; masked && i < n; ++i) {
        payload[i] ^= mask[i & 3];
      }
      if (opcode == kOpPing) {
        if (!sendFrame(kOpPong, payload, n)) {
          fail("pong failed");
          return Poll::kClosed;
        }
      } else if (opcode == kOpClose) {
        // Echo the status code back, which is the whole closing handshake.
        sendFrame(kOpClose, payload, n >= 2 ? 2 : 0);
        fail("closed by server");
        return Poll::kClosed;
      }
      continue;  // a pong, or a ping answered: keep reading the message
    }

    if (opcode == kOpContinuation ? !in_message
                                  : (in_message || (opcode != kOpText &&
                                                    opcode != kOpBinary))) {
      fail("bad frame sequence");
      return Poll::kClosed;
    }
    in_message = true;

    if (dropping || out.length() + len > max_len) {
      // Too big to hold. Read it off the wire anyway, or the stream is lost.
      if (!dropping) {
        dropping = true;
        out = "";
      }
      if (!discard(len)) {
        fail("read failed");
        return Poll::kClosed;
      }
    } else {
      if (!out.reserve(out.length() + static_cast<size_t>(len))) {
        fail("out of memory");
        return Poll::kClosed;
      }
      uint8_t chunk[128];
      for (uint64_t done = 0; done < len;) {
        const size_t n = (len - done) < sizeof(chunk)
                             ? static_cast<size_t>(len - done)
                             : sizeof(chunk);
        if (!readExact(chunk, n)) {
          fail("read failed");
          return Poll::kClosed;
        }
        // The mask runs from the start of this frame's payload, not the
        // message's.
        for (size_t i = 0; masked && i < n; ++i) {
          chunk[i] ^= mask[(done + i) & 3];
        }
        out.concat(reinterpret_cast<const char*>(chunk), n);
        done += n;
      }
    }

    if (fin) {
      return dropping ? Poll::kDropped : Poll::kMessage;
    }
  }
}

void WebSocket::close() {
  if (client_ == nullptr) {
    return;
  }
  if (client_->connected()) {
    // 1000, normal closure.
    const uint8_t status[2] = {0x03, 0xE8};
    sendFrame(kOpClose, status, sizeof(status));
  }
  client_->stop();
}

}  // namespace services
