#pragma once

/**
 * The BOOT button, as a pin rather than as a policy.
 *
 * What a long press *means* -- clear Wi-Fi and Home Assistant settings and
 * reboot into the portal -- lives in services/wifi_setup.cpp, which is board
 * independent. This is only the reading of the pin.
 */
namespace hw {

/** Idempotent; safe to call from more than one place. */
void bootButtonInit();

/** True while the button is held. Active LOW on every board so far, which is
 *  a detail of the wiring rather than of the caller. */
bool bootButtonPressed();

}  // namespace hw
