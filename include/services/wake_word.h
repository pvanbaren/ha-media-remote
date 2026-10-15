#pragma once

#include <cstdint>

/**
 * The wake word, heard on the remote itself: one of microWakeWord's models,
 * the one the Voice setting chose, run by TensorFlow Lite Micro on what the
 * microphone hears, 30 ms at a time. Nothing leaves the remote until it is
 * heard; what follows is a voice request like any other (services::voice).
 *
 * It does not listen while a request is listening or a reply is playing --
 * the remote's own voice is no wake word -- and starts afresh after.
 *
 * On a board without a microphone, or one built without any of the models,
 * init() says false and nothing here does anything.
 */
namespace services::wake_word {

/** The wake words, in the order the Voice setting lists them. */
enum class Word : uint8_t { kHeyJarvis, kOkayNabu, kHeyMycroft, kAlexa };
constexpr int kWordCount = 4;

/** Whether this build carries `word`'s model. */
bool has(Word word);
/** `word` as said: "Hey Jarvis". */
const char* phraseOf(Word word);

/** Once, after services::voice::init(). False without a microphone or a
 *  model. Listening starts off. */
bool init();

/** The wake word to listen for, from any task, before init() or after: the
 *  model changes over within a run or two. Hey Jarvis until set. */
void setWord(Word word);
Word word();

/** Listen for the wake word, or stop. */
void setListening(bool on);
bool listening();

/** The wake word listened for, as said, for the log and Home Assistant:
 *  "Hey Jarvis". */
const char* phrase();

/** True once per time the wake word was heard. `position` is where in the
 *  microphone's stream it ended -- a little before where it was heard, by
 *  config::kWakeWordPreRollMs -- for hw::audio::captureStartAt(), so the
 *  request hears what was said straight after it. */
bool takeDetection(uint32_t& position);

/** How the detector has been doing since the last call, for the log. */
struct Stats {
  /** Times the model ran, one per 30 ms of sound. */
  uint32_t runs = 0;
  uint32_t mean_us = 0;
  uint32_t max_us = 0;
  /** The highest of the model's smoothed confidence, 0..255; it fires at
   *  `cutoff`, the model's own. */
  uint8_t max_score = 0;
  uint8_t cutoff = 0;
  uint32_t detections = 0;
  /** Microphone samples it fell too far behind to read. */
  uint32_t dropped = 0;
};
void takeStats(Stats& out);

}  // namespace services::wake_word
