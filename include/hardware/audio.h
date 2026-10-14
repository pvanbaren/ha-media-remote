#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The board's microphone and speaker, on a board that has them (BOARD_HAS_MIC
 * in its header): what Home Assistant's Assist needs to listen and answer.
 *
 * Board-independent, as encoder.h is: on a board without them begin() says
 * false and every other call is a harmless no-op. What is particular to a
 * board -- its codecs and how they are set up -- is behind audio_codec.h.
 *
 * Everything is 16 kHz, 16-bit, mono, which is what Assist takes.
 *
 * The microphone, once captureStart() has been called, fills a ring of about
 * two seconds that captureRead() drains: a voice request sends what it reads
 * as it goes, and a wake-word detector could read the same way. Nothing reads
 * it for you; a ring left undrained overwrites nothing, it drops what does not
 * fit, and captureDropped() says how much.
 *
 * The speaker plays an MP3 that is still arriving -- mp3Begin(), then
 * mp3Write() from whatever is downloading it, then mp3End() -- decoding each
 * frame as soon as it is in, at whatever rate it was made at. Sound starts
 * with the first frame. beep() is for the moment a request starts listening.
 */
namespace hw::audio {

constexpr uint32_t kSampleRate = 16000;

/** Idempotent. The codecs set up, the I2S port running, and the tasks that
 *  serve both started. False on a board without audio, or if a codec did not
 *  answer. */
bool begin();

/** True once begin() has succeeded. */
bool ready();

// --- Microphone -----------------------------------------------------------

/** Start keeping what the microphone hears. Anything already in the ring is
 *  discarded, so what is read next is from now on. */
bool captureStart();

/** Stop keeping it. What is in the ring can still be read. */
void captureStop();

bool capturing();

/** Up to `max` samples, oldest first. Never blocks; 0 when there are none. */
size_t captureRead(int16_t* out, size_t max);

/** Samples lost since captureStart() because the ring was full. */
uint32_t captureDropped();

/** The loudest sample in the last 16 ms or so, 0..32767, while capturing;
 *  0 otherwise. For a level meter. */
uint16_t inputPeak();

// --- Speaker --------------------------------------------------------------

/** 0..100. Applies to everything played after it. */
void setVolume(int percent);

/** A tone, played before this returns. Waits for a reply that is playing. */
void beep(uint16_t hz, uint16_t ms);

/** Start a reply. False while another is playing: stop() it first. */
bool mp3Begin();

/** Hand over the next bytes of the reply, from any one task. Waits, up to
 *  `timeout_ms`, while the player is that far behind; returns how many
 *  bytes it took, which is fewer than `len` only on a timeout or a stop(). */
size_t mp3Write(const uint8_t* data, size_t len, uint32_t timeout_ms);

/** No more bytes are coming: the reply plays to its end and finishes. */
void mp3End();

/** Cut the reply off now. */
void stop();

/** True from mp3Begin() until the last of the reply has been played or it
 *  was stopped. */
bool playing();

}  // namespace hw::audio
