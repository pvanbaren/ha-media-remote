#pragma once

/**
 * The one part of audio that depends on the board: its codecs, set up over
 * its I2C bus.
 *
 * hardware/audio.cpp runs the I2S port, the microphone ring and the MP3
 * player the same way on any board; a board with BOARD_HAS_MIC supplies
 * these, and the pins in its header. The I2S port is the controller on every
 * side, so the codecs are slaves clocked from its MCLK, which is running by
 * the time init() is called.
 */
namespace hw::audio::codec {

/** Set the codecs up for 16 kHz 16-bit I2S, microphone and speaker both.
 *  False if either did not answer. The speaker amplifier is left off. */
bool init(int volume_percent);

/** 0..100, the speaker codec's digital volume. */
void setVolume(int percent);

/** The speaker amplifier on or off. Off whenever nothing is playing, so the
 *  speaker does not hiss. */
void speaker(bool on);

}  // namespace hw::audio::codec
