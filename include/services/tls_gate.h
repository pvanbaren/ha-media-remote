#pragma once

namespace services::tls {

/**
 * One TLS handshake at a time, and none begun without room for it.
 *
 * mbedTLS here takes its 16 KB in and 16 KB out record buffers, and the
 * handshake's working memory, from internal RAM, never PSRAM: a session is
 * ~40 KB of it, and a handshake briefly more. At boot the Home Assistant
 * poll, its stream and the list's thumbnails each opened one together, and
 * on a board with no PSRAM for small allocations internal RAM fell to a few
 * hundred bytes -- Wi-Fi's receive buffers failing to allocate meanwhile.
 *
 * Every TLS client goes through services::Yielding, which brackets its
 * connect with these: begin() waits for any other handshake to finish, then
 * up to config::kTlsRoomWaitMs for config::kTlsRoomBytes of internal RAM to
 * be free, and goes ahead regardless after that -- a connection that might
 * still work is not refused here. From any task; not re-entrant per client.
 */
void begin();
void end();

}  // namespace services::tls
