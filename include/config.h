#pragma once

#include <cstddef>
#include <cstdint>

/**
 * How the remote behaves, not what it runs on.
 *
 * Nothing here knows the panel size, the pins or the touch controller -- those
 * are in board/, included only by src/ui/ and src/hardware/. Everything in
 * this file is equally true of a round 240 px panel and a square 720 px one.
 */
namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "MediaRemote-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** Device name until one is set in the portal: the hostname, and the mDNS
 *  host (no ".local" suffix) -- http://media-remote.local. See
 *  services::device::name(). */
constexpr char kPortalHostname[] = "media-remote";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// Touch is the primary input; BOOT only exists as the Wi-Fi/HA escape hatch,
// so it has no short-tap action. The pin itself is in board/.
constexpr unsigned long kBootResetHoldMs = 3000UL;

/** How much the serial console says, from least to most. Each level includes
 *  the ones before it. See log.h. */
enum class LogLevel : uint8_t {
  kNone = 0,
  /** Something is broken: a panel that did not come up, a template Home
   *  Assistant rejected, memory that could not be had. */
  kError,
  /** Something failed that the firmware works around: a dropped request, a
   *  stream that closed early, a cover too big to cache. */
  kWarn,
  /** What the device is doing, once per thing worth knowing: boot, settings,
   *  the stream opening, a command, an artist noted. */
  kInfo,
  /** Every request, every state change, every repaint decision: for chasing
   *  something down. A playing player makes this a line a second or more. */
  kDebug,
  /** Anything finer still. */
  kVerbose,
};

/** The console's level, fixed at build time: everything below it is compiled
 *  out, arguments and all, so a quiet build pays nothing for the lines it
 *  does not print. kDebug adds a line per HTTP request and per state change. */
constexpr LogLevel kLogLevel = LogLevel::kInfo;

}  // namespace config
