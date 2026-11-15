#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <type_traits>

#include "services/tls_gate.h"

namespace services {

/**
 * A network client that sleeps for a millisecond whenever it has nothing to
 * give, instead of returning at once.
 *
 * NetworkClientSecure::read() and available() report "no data yet" without
 * waiting, and the code that waits on them does so by asking again: HTTPClient
 * reads a response body in a loop that pauses on delay(0), and a chunked body
 * or a header line goes through Stream::timedRead(), which does not pause at
 * all. delay(0) yields only to tasks of equal or higher priority. So a slow
 * response had the waiting task spin, and on core 0 the idle task -- the one
 * the task watchdog watches -- never ran. A browse refresh that Home Assistant
 * took five seconds over rebooted the device. Priority 0, the idle task's own,
 * is not enough to prevent it: priority inheritance through the shared
 * connection's mutex can lift the spinning task above the idle task for as
 * long as it holds the lock (see setup() in app.cpp).
 *
 * Sleeping here turns every one of those loops, whoever wrote it, into one
 * that blocks while it waits, at any priority. A millisecond is nothing
 * against a network round trip, and nothing is charged while data is
 * flowing: only an empty answer sleeps.
 */
template <typename Base>
class Yielding : public Base {
 public:
  int available() override {
    const int n = Base::available();
    if (n <= 0) {
      delay(1);
    }
    return n;
  }

  int read() override {
    const int c = Base::read();
    if (c < 0) {
      delay(1);
    }
    return c;
  }

  int read(uint8_t* buf, size_t size) override {
    const int n = Base::read(buf, size);
    if (n <= 0 && size > 0) {
      delay(1);
    }
    return n;
  }

  // A TLS client's connect takes services::tls's gate for its handshake:
  // one at a time, each with room in internal RAM. Only the outermost call,
  // as the overloads call one another.
  using Base::connect;  // the others, with certificates, unchanged
  int connect(IPAddress ip, uint16_t port) override {
    return gated([&] { return Base::connect(ip, port); });
  }
  int connect(const char* host, uint16_t port) override {
    return gated([&] { return Base::connect(host, port); });
  }
  int connect(IPAddress ip, uint16_t port, int32_t timeout) override {
    return gated([&] { return Base::connect(ip, port, timeout); });
  }
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    return gated([&] { return Base::connect(host, port, timeout); });
  }

 private:
  static constexpr bool kTls = std::is_base_of_v<NetworkClientSecure, Base>;
  bool in_call_ = false;

  template <typename F>
  int gated(F&& call) {
    if (!kTls || in_call_) {
      return call();
    }
    in_call_ = true;
    tls::begin();
    const int r = call();
    tls::end();
    in_call_ = false;
    return r;
  }
};

using YieldingClient = Yielding<WiFiClient>;
using YieldingClientSecure = Yielding<WiFiClientSecure>;

}  // namespace services
