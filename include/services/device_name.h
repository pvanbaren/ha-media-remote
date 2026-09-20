#pragma once

#include <cstddef>

namespace services::device {

/** Longest name kept, not counting the terminator: the Arduino network layer
 *  holds the hostname in 32 bytes. */
constexpr size_t kNameMaxLen = 31;

/** What the device is called on the network. It is the hostname, so DHCP
 *  and mDNS both use it and the portal is at http://<name>.local.
 *  config::kPortalHostname until one is set; read from NVS on first use. */
const char* name();

/** Clean `requested` into what a hostname may hold -- lowercase letters,
 *  digits and single hyphens, no hyphen at either end -- and store it. False,
 *  storing nothing, when nothing usable is left. Takes effect at the next
 *  boot: the name is handed to the network stack before the station starts. */
bool saveName(const char* requested);

/** `requested` cleaned as saveName() would, into `out`. Empty when nothing
 *  usable is left. */
void cleanName(const char* requested, char* out, size_t out_len);

}  // namespace services::device
