#pragma once

/**
 * What the Guition ESP32-4848S040 needs beyond the shared RGB output stage
 * and touch path: the ST7701's register init, and a stand-in for the BOOT
 * button it does not have.
 *
 * displayInit() resets the panel (panelReset()), then starts the RGB output
 * stage, then sends the rest of the init (panelInit()), so the panel comes
 * out of its sleep with the clock already running: see display.cpp for why.
 */
namespace hw::guition4848 {

/** Software-reset the ST7701 over 3-wire SPI, and wait out the reset. */
void panelReset();

/** Send the ST7701 its register init over 3-wire SPI, sleep-out and display
 *  on included, leaving its chip select high so that the SD card's lines,
 *  which it shares, are nothing to it. Expects panelReset() first. */
bool panelInit();

/** Whether a finger was on the glass at the touch sampler's last read --
 *  the contact itself, read from any task, for hw::bootButtonPressed(). */
bool touchHeld();

}  // namespace hw::guition4848
