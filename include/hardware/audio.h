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
 * The microphone fills a ring of about two seconds while any reader wants
 * it, and each reader drains it from its own place: the wake-word detector,
 * all the time, and a voice request, from where the wake word ended or from
 * the moment it starts. A reader that falls two seconds behind loses the
 * oldest of what it had not read, and captureDropped() says how much.
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

/** Who is reading the microphone. Each reads from one task. */
enum class Reader : uint8_t { kRequest, kWakeWord };

/** Start `reader` reading from now on. */
bool captureStart(Reader reader);

/** Start `reader` reading from `position`, a place another reader's
 *  capturePosition() gave: a request picks up where the wake word ended. As
 *  far back as the ring still holds. */
bool captureStartAt(Reader reader, uint32_t position);

/** Stop `reader`. What it has not read yet, up to now, can still be read;
 *  nothing after, though another reader keeps the ring filling. */
void captureStop(Reader reader);

bool capturing(Reader reader);

/** Up to `max` samples for `reader`, oldest first. Never blocks; 0 when
 *  there are none. */
size_t captureRead(Reader reader, int16_t* out, size_t max);

/** Where the microphone's stream is now: the next sample it will write. */
uint32_t captureNow();

/** Where in the microphone's stream `reader` will read next. */
uint32_t capturePosition(Reader reader);

/** The microphone's level over the `samples` before `position`, RMS in
 *  dBFS -- 0 the loudest, -96 silence -- as far back as the ring still
 *  holds: how loud the wake word was, for which of two remotes heard it
 *  nearer. */
float levelBefore(uint32_t position, uint32_t samples);

/** Samples `reader` lost since it started, by falling too far behind. */
uint32_t captureDropped(Reader reader);

/** The loudest sample in the last 16 ms or so, 0..32767, while any reader
 *  is capturing; 0 otherwise. For a level meter. */
uint16_t inputPeak();

// --- Speaker --------------------------------------------------------------

/** 0..100. Applies to everything played after it. */
void setVolume(int percent);

/** A tone, played before this returns. Waits for a reply that is playing. */
void beep(uint16_t hz, uint16_t ms);

/** The short sounds that stand in for a screen, where there is none. */
enum class Cue : uint8_t {
  kPlay,   // two notes rising
  kPause,  // two notes falling
  kNext,      // three notes stepping up: the next track
  kPrevious,  // three notes stepping down: the one before
  kTick,   // one quiet click: the volume moved a step
  kLimit,  // two quick clicks: the volume is at an end
  kFailed,  // two low notes falling: a voice request failed, and Home
            // Assistant could not be asked to say so
  kWarm,   // no sound: the amplifier on, so a sound soon after is not
           // held up by its warm-up -- for a finger that has just landed
};

/** Play a cue and return at once: it is queued for the player's task.
 *  Dropped while a reply is playing, or when cues come faster than they can
 *  be played. */
void cue(Cue cue);

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
