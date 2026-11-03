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
 *
 * Two switches. A build has voice only if its env opts in, with
 * -DVOICE_ASSIST in its build_flags, on a board with a microphone; without
 * it, the audio, this and the wake word are stubs, and none of their code or
 * the wake word's model is in the image. And a build that has it can be
 * told not to use it -- the Voice setting, on the device and in the portal,
 * stored with saveSetting(): Disabled, the default, or the wake word to
 * listen for.
 */
namespace services::voice {

/** Whether this build has voice at all (-DVOICE_ASSIST). */
constexpr bool built() {
#if defined(VOICE_ASSIST)
  return true;
#else
  return false;
#endif
}

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
 *  stream, and reads the Voice setting. Harmless in a build without voice. */
void init();

/** The Voice setting as one value: 0 for Disabled, or 1 + the
 *  wake_word::Word listened for. Disabled until a wake word is chosen: a
 *  remote never listens until someone has asked it to. Always 0 in a build
 *  without voice. */
int setting();
/** Whether requests may be made, and the wake word is listened for: a wake
 *  word is chosen. */
bool enabled();
/** Store the setting, 0 to kWordCount, a word this build has. Disabled, a
 *  request under way is dropped and the wake word stops listening; a word
 *  chosen, it listens for that one. False for a value it cannot take. */
bool saveSetting(int value);

/** What a voice request can ask of this remote's own player, with no
 *  player named, instead of asking Assist. */
enum class Command : uint8_t {
  kPlay,        // "play Khatia Buniatishvili": the something comes with it
  kResume,      // "play", "resume", "continue"
  kPause,       // "pause"
  kNext,        // "skip", "next"
  kPrevious,    // "previous", "go back"
  kVolumeUp,    // "volume up", "louder", "turn it up"
  kVolumeDown,  // "volume down", "quieter", "turn it down"
  kTurnOff,     // "stop", "turn off": the volume device off
};

/** What carries out a Command: called on the stream's owner with it, and
 *  for kPlay the something as heard ("" otherwise), to copy and act on from
 *  another task. It returns whether it takes the request; one it does not
 *  goes to Assist as any other -- as does "play Khatia Buniatishvili on
 *  the stereo", which names its player. Without one, every request goes
 *  to Assist. */
void setCommandHandler(bool (*handler)(Command command, const char* query));

/** True when a request can be started now: there is a microphone, the
 *  Voice setting is on, and the stream is open. */
bool available();

/** Start a request: a beep, then listening. False when one is already
 *  under way or none is available. Cuts off a reply still playing. One
 *  that fails says so: "Sorry, ..." in the pipeline's voice, or a falling
 *  tone when Home Assistant cannot be reached to speak it. */
bool start();

/** Start a request after a wake word: listening from `position` in the
 *  microphone's stream (services::wake_word::takeDetection()), so what was
 *  said straight after the wake word is heard, and a beep meanwhile. */
bool startAt(uint32_t position);

/** Speak `text` in the pipeline's voice: Home Assistant's text to speech
 *  alone, played as it arrives, as a reply is. False when a request is
 *  under way or none is available. */
bool say(const char* text);

/** The speaker is done talking: stop listening and let Home Assistant work
 *  on what it has. Home Assistant also stops it by itself, at silence. */
void finish();

/** Drop the request, and any reply playing. */
void cancel();

/** True from start() until the reply has finished -- or, for a request
 *  that failed, until the apology for it has begun: the stream's owner
 *  keeps other work back meanwhile, so the audio is not held up behind it. */
bool busy();

/** The microphone's loudest recent sample, 0..32767, while listening. */
uint16_t level();

void status(Status& out);

}  // namespace services::voice
