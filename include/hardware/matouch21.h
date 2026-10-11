#pragma once

/**
 * What the Makerfabs MaTouch 2.1" panel needs before it works, beyond the
 * shared RGB output stage: the ST7701's register init over 3-wire SPI.
 *
 * displayInit() calls panelInit() before the RGB output stage starts, since
 * the panel ignores the pixel stream until it has been told how to take it.
 */
namespace hw::matouch21 {

/** Software-reset the ST7701 and send it its register init over 3-wire SPI,
 *  then release the pins -- GPIO 0, its data line, back to the BOOT button.
 *  False when the SPI bus could not be set up. */
bool panelInit();

}  // namespace hw::matouch21
