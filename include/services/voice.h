#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Voice commands: Home Assistant's Assist pipeline, spoken to and answered
 * on the remote itself, on a board with a microphone and a speaker.
 *
 * A request runs over the state stream (see ha_client.h), so it needs the
 * stream open: speech to text, the conversation agent and text to speech are
 * whatever the preferred pipeline in Home Assistant has. The microphone's
 * audio goes up as it is heard; Home Assistant decides when the speech has
 * ended, or finish() does, and the reply is downloaded and played as it
 * arrives (hw::audio).
 *
 * start() and the rest may be called from any task. The work happens on the
 * stream's owner and on a small task of this module's own.
 */
namespace services::voice {

enum class Phase : uint8_t {
  kIdle,
  /** Asked for; Home Assistant has not answered yet. */
  kStarting,
  /** The microphone is being sent. */
  kListening,
  /** The speech is in; Home Assistant is working out the reply. */
  kThinking,
  /** The reply is being played. */
  kReplying,
  /** It went wrong: Status::error says how. Until the next start(). */
  kFailed,
};

struct Status {
  Phase phase = Phase::kIdle;
  /** What Home Assistant heard. Empty until it has. */
  char heard[160] = {};
  /** What it answered, as text. Empty until it has. */
  char reply[240] = {};
  char error[96] = {};
};

/** Once, after services::ha::init(): starts the audio and hooks into the
 *  stream. Harmless on a board without a microphone. */
void init();

/** True when a request can be started now: there is a microphone, and the
 *  stream is open. */
bool available();

/** Start a request: a beep, then listening. False when one is already
 *  under way or none is available. Cuts off a reply still playing. */
bool start();

/** The speaker is done talking: stop listening and let Home Assistant work
 *  on what it has. Home Assistant also stops it by itself, at silence. */
void finish();

/** Drop the request, and any reply playing. */
void cancel();

/** True from start() until the reply has finished: the stream's owner keeps
 *  other work back meanwhile, so the audio is not held up behind it. */
bool busy();

/** The microphone's loudest recent sample, 0..32767, while listening. */
uint16_t level();

void status(Status& out);

}  // namespace services::voice
