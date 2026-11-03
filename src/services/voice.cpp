/**
 * A voice request, end to end. The protocol, as much of it as is used:
 *
 *   client  {"id":9,"type":"assist_pipeline/run","start_stage":"stt",
 *            "end_stage":"stt","input":{"sample_rate":16000}}
 *   server  {"id":9,"type":"result","success":true,"result":null}
 *   server  event run-start: runner_data.stt_binary_handler_id
 *   client  binary: the handler id, then 16-bit 16 kHz mono audio, again and
 *           again; the handler id alone for the end of the speech
 *   server  events stt-vad-start, stt-vad-end (it heard the speech stop),
 *           stt-end (stt_output.text), run-end; or error, with a code and a
 *           message, and then run-end
 *
 * The words are heard first and acted on second. "Play <something>",
 * "pause", "skip", "volume up" and the like, with no player named, are
 * this remote's to carry out (setCommandHandler()). Anything else is a
 * second run, from the words:
 *
 *   client  {"id":10,"type":"assist_pipeline/run","start_stage":"intent",
 *            "end_stage":"tts","input":{"text":"turn off the lights"}}
 *   server  event run-start, with the reply's audio URL already, as
 *           tts_output.url; then intent-end (the reply's text, at
 *           intent_output.response.speech.plain.speech), tts-end, run-end
 *
 * Home Assistant holds a GET of the reply's URL until the speech has been
 * made, so the download is started as that run starts and is connected and
 * waiting by the time the reply exists. A run that fails before it makes
 * the speech gives the download up.
 *
 * Three tasks take part. Any task calls start() and the rest. The stream's
 * owner runs hook() and listener(): the request, the audio and the events,
 * as the socket is that task's alone. This module's own task plays the beep
 * and downloads the reply, which would hold the owner up for seconds.
 */

#include "services/voice.h"

// The headless build has no board.
#if defined(BOARD_HEADER)
#include "board/board.h"
#endif

#if defined(VOICE_ASSIST)

#if !defined(BOARD_HAS_MIC)
#error "VOICE_ASSIST is for a board with a microphone (BOARD_HAS_MIC)"
#endif

#include <Arduino.h>
#include <Preferences.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>

#include "config.h"
#include "ha_internal.h"
#include "hardware/audio.h"
#include "log.h"
#include "services/ha_client.h"
#include "services/wake_word.h"
#include "services/yielding_client.h"

namespace services::voice {
namespace {

using ha::detail::eachMember;
using ha::detail::readJson;

constexpr hw::audio::Reader kMic = hw::audio::Reader::kRequest;

constexpr char kPrefsNamespace[] = "voice";
/** The Voice setting: 0 Disabled, or 1 + a wake_word::Word. */
constexpr char kPrefsWakeKey[] = "wake";
/** Before there was a choice of wake word, an on/off switch, on by default.
 *  Not read -- a remote waits to be given a wake word -- and removed when
 *  the setting is next stored. */
constexpr char kPrefsOldEnabledKey[] = "enabled";

/** The Voice setting, read in init(). */
std::atomic<uint8_t> s_setting{0};
/** setCommandHandler()'s. */
std::atomic<bool (*)(Command, const char*)> s_command_handler{nullptr};

constexpr char kRunBody[] =
    "\"type\":\"assist_pipeline/run\",\"start_stage\":\"stt\","
    "\"end_stage\":\"stt\",\"input\":{\"sample_rate\":16000}";
/** The words heard, to Assist: the text appended as JSON and closed. */
constexpr char kTextBodyStart[] =
    "\"type\":\"assist_pipeline/run\",\"start_stage\":\"intent\","
    "\"end_stage\":\"tts\",\"input\":{\"text\":";
/** say()'s: text to speech alone, the text appended as JSON and closed. */
constexpr char kSayBodyStart[] =
    "\"type\":\"assist_pipeline/run\",\"start_stage\":\"tts\","
    "\"end_stage\":\"tts\",\"input\":{\"text\":";
/** How long Home Assistant has to start the run before it counts as lost. */
constexpr unsigned long kStartTimeoutMs = 5000;
/** The beep that says it is listening. */
constexpr uint16_t kBeepHz = 880;
constexpr uint16_t kBeepMs = 120;
/** Said after a request that failed: nothing was heard in it, or anything
 *  else went wrong -- the conversation agent erred, say. An answer that is
 *  itself an apology, an intent not matched, is Home Assistant's own. */
constexpr char kSorryNotHeard[] = "Sorry, I didn't catch that.";
constexpr char kSorryFailed[] = "Sorry, Home Assistant couldn't answer that.";

struct Job {
  enum Kind : uint8_t { kListen, kFetch } kind;
  /** kListen from a wake word: the microphone from `position` on. */
  bool from_position;
  uint32_t position;
};

std::atomic<Phase> s_phase{Phase::kIdle};
/** start() and cancel(), for the owner's next pass. */
std::atomic<bool> s_want_run{false};
/** The request start() or say() wants sent: written before s_want_run is
 *  set, read by the owner after it sees it. */
String s_run_body;
/** What the run is: listening, the words heard going to Assist, or say()'s
 *  text alone. The last two have no microphone, and fetch the reply as
 *  they start; say()'s failures are a tone, never an apology. */
enum class RunKind : uint8_t { kListen, kText, kSay };
RunKind s_kind = RunKind::kListen;
std::atomic<bool> s_want_cancel{false};
/** A request cancelled before Home Assistant had answered it: there was no
 *  audio channel yet to end it on, so it is kept until run-start brings
 *  one, ended there, and only then dropped. No new request starts while it
 *  waits, which is a moment. */
std::atomic<bool> s_end_on_start{false};
/** Set by this module's task once the microphone is being kept. */
std::atomic<bool> s_capture_began{false};
std::atomic<bool> s_fetching{false};
/** Set when the request a download belongs to has failed or been dropped:
 *  Home Assistant will never make its speech, and the download stops
 *  waiting for it. Cleared as a download is queued. */
std::atomic<bool> s_fetch_abort{false};
/** An apology owed for a request that failed, said by the owner once that
 *  request's run is over; nullptr when none is. */
std::atomic<const char*> s_sorry{nullptr};

/** The texts, for status(); under s_lock. */
Status s_status;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t s_jobs = nullptr;
/** The reply's URL, written by the owner before it queues kFetch. */
char s_reply_url[256] = {};
/** The words a listening run heard, for what follows it. The owner's. */
char s_heard[sizeof(Status::heard)] = {};

// The owner's alone.
int s_run_id = 0;
int s_handler = -1;
bool s_audio_done = false;
bool s_run_ended = false;
bool s_fetch_queued = false;
/** Home Assistant has answered with run-start. */
bool s_run_started = false;
unsigned long s_run_sent_ms = 0;
unsigned long s_speech_end_ms = 0;
/** One message of audio being gathered, and the message itself: in PSRAM,
 *  taken by init() on a board with a microphone and nowhere else. */
int16_t* s_pcm = nullptr;
size_t s_pcm_fill = 0;
uint8_t* s_frame = nullptr;
/** Said once a request, when the audio has fallen a second behind. */
bool s_lag_logged = false;

template <size_t N>
void setText(char (Status::*field)[N], const char* text) {
  taskENTER_CRITICAL(&s_lock);
  snprintf(s_status.*field, N, "%s", text);
  taskEXIT_CRITICAL(&s_lock);
}

/** The request has failed, for `why`. Out loud too: `sorry` is said once
 *  its run is over, or with nullptr -- the stream gone or not answering,
 *  so Home Assistant cannot be asked to say anything -- a tone at once.
 *  say()'s own failures, an apology's among them, are the tone. */
void fail(const char* why, const char* sorry) {
  hw::audio::captureStop(kMic);
  s_fetch_abort = true;
  setText(&Status::error, why);
  s_phase = Phase::kFailed;
  LOG_WARN("VOICE: %s", why);
  if (sorry != nullptr && s_kind != RunKind::kSay) {
    s_sorry = sorry;
  } else {
    hw::audio::cue(hw::audio::Cue::kFailed);
  }
}

/** The microphone, as much as there is, out in whole messages; the last,
 *  short one and the end marker once it has stopped. A listening run's
 *  only: a text run, after one, has no audio. */
void pumpAudio() {
  if (s_kind != RunKind::kListen || s_handler < 0 || s_audio_done ||
      !s_capture_began) {
    return;
  }
  size_t this_pass = 0;
  for (;;) {
    // Whether it was still capturing, asked before reading: stopped then
    // and nothing read now means everything has been read.
    const bool capturing = hw::audio::capturing(kMic);
    const size_t n = hw::audio::captureRead(
        kMic, s_pcm + s_pcm_fill, config::kVoiceFrameSamples - s_pcm_fill);
    s_pcm_fill += n;
    // A second's audio waiting at once: this task was kept from it, by
    // something on the stream's owner. Two seconds and it is lost.
    this_pass += n;
    if (this_pass >= hw::audio::kSampleRate && !s_lag_logged) {
      s_lag_logged = true;
      LOG_WARN("VOICE: the audio fell %lu ms behind",
               static_cast<unsigned long>(this_pass * 1000 /
                                          hw::audio::kSampleRate));
    }
    const bool drained = !capturing && n == 0;
    if (s_pcm_fill == config::kVoiceFrameSamples ||
        (drained && s_pcm_fill > 0)) {
      memcpy(s_frame + 1, s_pcm, s_pcm_fill * 2);
      if (!ha::streamSendBinary(s_frame, 1 + s_pcm_fill * 2)) {
        return;  // the stream closed; the listener has heard
      }
      s_pcm_fill = 0;
      continue;
    }
    if (drained) {
      ha::streamSendBinary(s_frame, 1);  // the handler alone: the end
      s_audio_done = true;
      s_speech_end_ms = millis();
      if (hw::audio::captureDropped(kMic) > 0) {
        LOG_WARN("VOICE: %lu samples of speech lost",
                 static_cast<unsigned long>(hw::audio::captureDropped(kMic)));
      }
      Phase listening = Phase::kListening;
      s_phase.compare_exchange_strong(listening, Phase::kThinking);
    }
    return;
  }
}

void queueFetch() {
  if (s_fetch_queued || s_reply_url[0] == '\0') {
    return;
  }
  s_fetch_queued = true;
  s_fetch_abort = false;
  s_fetching = true;
  const Job job = {Job::kFetch, false, 0};
  xQueueSend(s_jobs, &job, 0);
}

/** The sayings of each Command but kPlay, whole: lower case, without the
 *  full stop Whisper ends a sentence with, and without "please" or another
 *  of kLeadIns. */
struct Saying {
  const char* words;
  Command command;
};
/** What can come before a command and change nothing: "let's play Khatia
 *  Buniatishvili", "can you please pause". Any number, in any order.
 *  "Press", "Jarvis" and the like are the end of a wake word heard again:
 *  "Hey Jarvis, pause" can come back as "Press pause." */
const char* const kLeadIns[] = {"let's ",       "lets ",      "can you ",
                                "could you ",   "would you ", "will you ",
                                "go ahead and ", "okay ",     "ok ",
                                "now ",         "please ",    "press ",
                                "jarvis ",      "nabu ",      "mycroft ",
                                "alexa ",       nullptr};

const Saying kSayings[] = {
    {"pause", Command::kPause},
    {"pause it", Command::kPause},
    {"pause the music", Command::kPause},
    {"pause music", Command::kPause},
    {"play", Command::kResume},
    {"play music", Command::kResume},
    {"play the music", Command::kResume},
    {"resume", Command::kResume},
    {"resume the music", Command::kResume},
    {"resume music", Command::kResume},
    {"unpause", Command::kResume},
    {"continue", Command::kResume},
    {"continue playing", Command::kResume},
    {"start the music", Command::kResume},
    {"skip", Command::kNext},
    {"skipped", Command::kNext},  // Whisper's way with "skip it"
    {"skip ahead", Command::kNext},
    {"skip it", Command::kNext},
    {"skip this", Command::kNext},
    {"skip this song", Command::kNext},
    {"skip this track", Command::kNext},
    {"skip song", Command::kNext},
    {"skip track", Command::kNext},
    {"skip the song", Command::kNext},
    {"next", Command::kNext},
    {"next song", Command::kNext},
    {"next track", Command::kNext},
    {"next one", Command::kNext},
    {"play the next song", Command::kNext},
    {"previous", Command::kPrevious},
    {"previous song", Command::kPrevious},
    {"previous track", Command::kPrevious},
    {"play the previous song", Command::kPrevious},
    {"go back", Command::kPrevious},
    {"increase volume", Command::kVolumeUp},
    {"increase the volume", Command::kVolumeUp},
    {"volume up", Command::kVolumeUp},
    {"raise the volume", Command::kVolumeUp},
    {"raise volume", Command::kVolumeUp},
    {"turn it up", Command::kVolumeUp},
    {"turn up the volume", Command::kVolumeUp},
    {"turn the volume up", Command::kVolumeUp},
    {"turn up the music", Command::kVolumeUp},
    {"turn the music up", Command::kVolumeUp},
    {"louder", Command::kVolumeUp},
    {"make it louder", Command::kVolumeUp},
    {"decrease volume", Command::kVolumeDown},
    {"decrease the volume", Command::kVolumeDown},
    {"volume down", Command::kVolumeDown},
    {"lower the volume", Command::kVolumeDown},
    {"lower volume", Command::kVolumeDown},
    {"turn it down", Command::kVolumeDown},
    {"turn down the volume", Command::kVolumeDown},
    {"turn the volume down", Command::kVolumeDown},
    {"turn down the music", Command::kVolumeDown},
    {"turn the music down", Command::kVolumeDown},
    {"quieter", Command::kVolumeDown},
    {"softer", Command::kVolumeDown},
    {"make it quieter", Command::kVolumeDown},
    {"stop", Command::kTurnOff},
    {"stop it", Command::kTurnOff},
    {"stop the music", Command::kTurnOff},
    {"stop music", Command::kTurnOff},
    {"turn off", Command::kTurnOff},
    {"turn it off", Command::kTurnOff},
    {"turn off the music", Command::kTurnOff},
    {"turn the music off", Command::kTurnOff},
    {"turn off music", Command::kTurnOff},
    {"switch off", Command::kTurnOff},
    {"switch it off", Command::kTurnOff},
};

/** What the words heard ask of this remote's player, if anything: one of
 *  kSayings, or "play <something>" with the something, as heard, in
 *  `out`. False for anything else, "play Khatia Buniatishvili on the
 *  stereo" among them, which names its player and is Assist's. All of
 *  `heard`: see parseCommand(). */
bool parseWhole(const char* heard, Command& command, char* out,
                size_t out_len) {
  out[0] = '\0';
  char text[sizeof(Status::heard)];
  size_t n = 0;
  // Without commas, which Whisper puts in: "Jarvis, pause".
  for (const char* h = heard; *h != '\0' && n + 1 < sizeof(text); ++h) {
    if (*h != ',') {
      text[n++] = static_cast<char>(tolower(static_cast<unsigned char>(*h)));
    }
  }
  text[n] = '\0';
  // Whisper ends a sentence with a full stop.
  while (n > 0 && (ispunct(static_cast<unsigned char>(text[n - 1])) ||
                   isspace(static_cast<unsigned char>(text[n - 1])))) {
    text[--n] = '\0';
  }
  if (n >= 7 && strcmp(text + n - 7, " please") == 0) {
    n -= 7;
    text[n] = '\0';
    while (n > 0 && (text[n - 1] == ',' || text[n - 1] == ' ')) {
      text[--n] = '\0';
    }
  }
  const char* q = text;
  while (isspace(static_cast<unsigned char>(*q))) {
    ++q;
  }
  for (bool more = true; more;) {
    more = false;
    for (const char* const* lead = kLeadIns; *lead != nullptr; ++lead) {
      if (strncmp(q, *lead, strlen(*lead)) == 0) {
        q += strlen(*lead);
        more = true;
      }
    }
  }
  for (const Saying& s : kSayings) {
    if (strcmp(q, s.words) == 0) {
      command = s.command;
      return true;
    }
  }
  if (strncmp(q, "play ", 5) != 0) {
    return false;
  }
  command = Command::kPlay;
  q += 5;
  static const char* const kLeadIns[] = {"some music by ", "music by ",
                                         "songs by ",      "something by ",
                                         "some ",          nullptr};
  for (const char* const* lead = kLeadIns; *lead != nullptr; ++lead) {
    if (strncmp(q, *lead, strlen(*lead)) == 0) {
      q += strlen(*lead);
      break;
    }
  }
  // A player or a room named, or nothing to look for: Assist's.
  if (strstr(q, " on ") != nullptr || strstr(q, " in the ") != nullptr ||
      *q == '\0' || strcmp(q, "music") == 0 || strcmp(q, "something") == 0) {
    return false;
  }
  // Out as it was heard, capitals and all: the same span of `heard`.
  snprintf(out, out_len, "%.*s", static_cast<int>(text + n - q),
           heard + (q - text));
  return true;
}

/** What a Command is called on the status page and in the log. */
const char* commandName(Command command) {
  switch (command) {
    case Command::kPlay:
      return "Playing";
    case Command::kResume:
      return "Play";
    case Command::kPause:
      return "Pause";
    case Command::kNext:
      return "Next";
    case Command::kPrevious:
      return "Previous";
    case Command::kVolumeUp:
      return "Volume up";
    case Command::kVolumeDown:
      return "Volume down";
    case Command::kTurnOff:
      return "Turn off";
  }
  return "";
}

/** Words a command can come after, as a clause of their own, and still be
 *  this remote's: the end of the wake word, heard again as words. With
 *  "Alexa", Whisper makes something of the "-xa" that the request's first
 *  moments hold -- "Turn it off. Play.", "Stop, play John Williams.", "Up,
 *  increase volume." -- often one of the phrases its prompt is given. */
constexpr int kMaxLeadInWords = 4;

/** parseWhole(), and failing that the same with a short clause before it
 *  left out: up to kMaxLeadInWords words ended by a full stop, a comma, a
 *  question mark or an exclamation mark, as Whisper ends one. */
bool parseCommand(const char* heard, Command& command, char* out,
                  size_t out_len) {
  if (parseWhole(heard, command, out, out_len)) {
    return true;
  }
  const char* at = heard;
  for (int words = 0; *at != '\0' && words < kMaxLeadInWords;) {
    while (*at == ' ') {
      ++at;
    }
    if (*at == '\0') {
      break;
    }
    while (*at != '\0' && *at != ' ' && strchr(".,?!", *at) == nullptr) {
      ++at;
    }
    ++words;
    if (*at != '\0' && strchr(".,?!", *at) != nullptr) {
      ++at;
      if (parseWhole(at, command, out, out_len)) {
        LOG_INFO("VOICE: \"%.*s\" before the command left out",
                 static_cast<int>(at - heard), heard);
        return true;
      }
    }
  }
  return false;
}

/** A listening run over, with the words heard: a Command to the command
 *  handler, if it takes it, and anything else to Assist as text, on the
 *  next pass. Owner only. */
void actOnHeard() {
  s_run_id = 0;
  if (s_heard[0] == '\0') {
    fail("nothing was heard", kSorryNotHeard);
    return;
  }
  char query[sizeof(s_heard)];
  Command command = Command::kPlay;
  auto* handler = s_command_handler.load();
  if (handler != nullptr &&
      parseCommand(s_heard, command, query, sizeof(query)) &&
      handler(command, query)) {
    char reply[sizeof(Status::reply)];
    snprintf(reply, sizeof(reply), "%s%s%s", commandName(command),
             query[0] != '\0' ? " " : "", query);
    setText(&Status::reply, reply);
    s_phase = Phase::kIdle;
    LOG_INFO("VOICE: \"%s\" done here, %lu ms after the speech ended", reply,
             static_cast<unsigned long>(millis() - s_speech_end_ms));
    return;
  }
  s_run_body = kTextBodyStart;
  ha::detail::appendJson(s_run_body, s_heard);
  s_run_body += "}";
  s_kind = RunKind::kText;
  s_want_run = true;
}

/** On the owner, every pass while the stream is open. */
void hook() {
  if (s_want_cancel.exchange(false)) {
    // The end of the speech, if any audio was owed: Home Assistant stops
    // listening now rather than waiting out its own timeout. Owed before
    // there is a channel to send it on, it waits for run-start.
    if (s_run_id != 0 && !s_audio_done && s_kind == RunKind::kListen) {
      if (s_handler >= 0) {
        ha::streamSendBinary(s_frame, 1);
        s_audio_done = true;
      } else {
        s_end_on_start = true;
      }
    }
    if (!s_end_on_start) {
      s_run_id = 0;  // anything more from it is ignored
    }
  }
  if (s_end_on_start) {
    // Waiting to end a cancelled request; no answer at all, and it is let go.
    if (millis() - s_run_sent_ms >= kStartTimeoutMs) {
      s_end_on_start = false;
      s_run_id = 0;
    }
    return;
  }
  // An apology owed, once the request it is for is over and its reply, if
  // it got as far as one, has been dropped. say() is the next request.
  if (s_sorry != nullptr && s_run_id == 0 && !s_want_run && !s_fetching &&
      !hw::audio::playing()) {
    const char* sorry = s_sorry.exchange(nullptr);
    if (sorry != nullptr && !say(sorry)) {
      hw::audio::cue(hw::audio::Cue::kFailed);
    }
  }
  if (s_want_run.exchange(false)) {
    s_handler = -1;
    s_audio_done = false;
    s_run_ended = false;
    s_fetch_queued = false;
    s_pcm_fill = 0;
    s_lag_logged = false;
    if (s_kind != RunKind::kText) {
      s_speech_end_ms = 0;  // a text run's speech is its listening run's
    }
    s_reply_url[0] = '\0';
    s_run_started = false;
    s_run_sent_ms = millis();
    s_run_id = ha::streamRequest(s_run_body.c_str());
    if (s_run_id == 0) {
      fail("could not reach Home Assistant", nullptr);
    }
    return;
  }
  if (s_run_id == 0) {
    return;
  }
  pumpAudio();

  const unsigned long now = millis();
  if (!s_run_started && now - s_run_sent_ms >= kStartTimeoutMs) {
    s_run_id = 0;
    fail("Home Assistant did not start the request", nullptr);
    return;
  }
  if (s_phase == Phase::kListening && hw::audio::capturing(kMic) &&
      now - s_run_sent_ms >= config::kVoiceMaxListenMs) {
    hw::audio::captureStop(kMic);
  }
  if (s_run_ended && !s_fetching && !hw::audio::playing()) {
    if (s_kind == RunKind::kListen && s_phase != Phase::kFailed) {
      actOnHeard();
      return;
    }
    s_run_id = 0;
    if (s_phase != Phase::kFailed) {
      s_phase = Phase::kIdle;
      if (s_speech_end_ms != 0) {
        LOG_INFO("VOICE: done, %lu ms after the speech ended",
                 static_cast<unsigned long>(now - s_speech_end_ms));
      } else {
        LOG_INFO("VOICE: done");
      }
    }
  }
}

/** The string at `path` -- keys, nullptr-terminated -- under `obj`. */
void readPath(const char* obj, const char* const* path, char* out,
              size_t out_len) {
  out[0] = '\0';
  const char* at = obj;
  for (; *path != nullptr && at != nullptr; ++path) {
    const char* next = nullptr;
    eachMember(at, [&](const char* key, const char* value) {
      if (next == nullptr && strcmp(key, *path) == 0) {
        next = value;
      }
    });
    at = next;
  }
  if (at != nullptr && *at == '"') {
    readJson(at, out, out_len);
  }
}

/** On the owner: the run's messages, and nullptr when the stream closes. */
void listener(const char* message) {
  if (message == nullptr) {
    if (s_end_on_start) {
      // A cancelled request: nothing to fail, and the connection that ends
      // it ends the run too.
      s_end_on_start = false;
      s_run_id = 0;
      return;
    }
    const Phase p = s_phase;
    if (s_run_id != 0 || p == Phase::kStarting) {
      s_run_id = 0;
      fail("lost the connection to Home Assistant", nullptr);
    }
    return;
  }
  if (s_run_id == 0) {
    return;
  }
  char type[16];
  static const char* const kType[] = {"type", nullptr};
  readPath(message, kType, type, sizeof(type));
  if (strcmp(type, "result") == 0) {
    bool ok = true;
    eachMember(message, [&](const char* key, const char* value) {
      if (strcmp(key, "success") == 0) {
        ok = *value == 't';
      }
    });
    if (!ok) {
      char why[96];
      static const char* const kMessage[] = {"error", "message", nullptr};
      readPath(message, kMessage, why, sizeof(why));
      s_run_id = 0;
      if (s_end_on_start) {
        s_end_on_start = false;  // refused, and already given up: no matter
        return;
      }
      fail(why[0] != '\0' ? why : "Home Assistant refused the request",
           kSorryFailed);
    }
    return;
  }
  if (strcmp(type, "event") != 0) {
    return;
  }
  const char* event = nullptr;
  eachMember(message, [&](const char* key, const char* value) {
    if (strcmp(key, "event") == 0) {
      event = value;
    }
  });
  char etype[24];
  static const char* const kEventType[] = {"type", nullptr};
  readPath(event, kEventType, etype, sizeof(etype));
  const char* data = nullptr;
  eachMember(event, [&](const char* key, const char* value) {
    if (strcmp(key, "data") == 0) {
      data = value;
    }
  });

  if (strcmp(etype, "run-start") == 0) {
    // The handler id is a number, not a string: read by hand.
    const char* runner = nullptr;
    eachMember(data, [&](const char* key, const char* value) {
      if (strcmp(key, "runner_data") == 0) {
        runner = value;
      }
    });
    s_handler = -1;
    eachMember(runner, [&](const char* key, const char* value) {
      if (strcmp(key, "stt_binary_handler_id") == 0) {
        s_handler = atoi(value);
      }
    });
    if (s_end_on_start) {
      // Cancelled while this was on its way: the end of the audio straight
      // away, and the request dropped.
      if (s_handler >= 0) {
        s_frame[0] = static_cast<uint8_t>(s_handler);
        ha::streamSendBinary(s_frame, 1);
      }
      s_end_on_start = false;
      s_run_id = 0;
      return;
    }
    static const char* const kUrl[] = {"tts_output", "url", nullptr};
    readPath(data, kUrl, s_reply_url, sizeof(s_reply_url));
    s_run_started = true;
    if (s_kind != RunKind::kListen) {
      // Nothing to hear, so the speech is asked for now; Home Assistant
      // answers the moment it is made.
      queueFetch();
      return;
    }
    s_frame[0] = static_cast<uint8_t>(s_handler);
    if (s_handler < 0) {
      s_run_id = 0;
      fail("Home Assistant gave no audio channel", kSorryFailed);
    }
  } else if (strcmp(etype, "stt-vad-end") == 0) {
    hw::audio::captureStop(kMic);  // it has stopped listening
  } else if (strcmp(etype, "stt-end") == 0) {
    char heard[sizeof(Status::heard)];
    static const char* const kText[] = {"stt_output", "text", nullptr};
    readPath(data, kText, heard, sizeof(heard));
    setText(&Status::heard, heard);
    snprintf(s_heard, sizeof(s_heard), "%s", heard);
    LOG_INFO("VOICE: heard \"%s\"", heard);
    hw::audio::captureStop(kMic);
  } else if (strcmp(etype, "intent-end") == 0) {
    char reply[sizeof(Status::reply)];
    static const char* const kSpeech[] = {"intent_output", "response",
                                          "speech",        "plain",
                                          "speech",        nullptr};
    readPath(data, kSpeech, reply, sizeof(reply));
    setText(&Status::reply, reply);
    LOG_INFO("VOICE: reply \"%s\"", reply);
  } else if (strcmp(etype, "tts-end") == 0) {
    queueFetch();  // in case run-start did not get to it
  } else if (strcmp(etype, "error") == 0) {
    char code[48];
    char why[96];
    static const char* const kCode[] = {"code", nullptr};
    static const char* const kMessage[] = {"message", nullptr};
    readPath(data, kCode, code, sizeof(code));
    readPath(data, kMessage, why, sizeof(why));
    LOG_WARN("VOICE: Home Assistant said %s", code);
    fail(why[0] != '\0' ? why : code,
         strcmp(code, "stt-no-text-recognized") == 0 ? kSorryNotHeard
                                                     : kSorryFailed);
  } else if (strcmp(etype, "run-end") == 0) {
    s_run_ended = true;
  }
}

/** One byte of the reply, waited for: -1 once the connection has closed,
 *  `deadline` has passed, or the request has been given up. */
int nextByte(NetworkClient& c, unsigned long deadline) {
  for (;;) {
    if (c.available() > 0) {
      return c.read();
    }
    if (s_fetch_abort || !c.connected() ||
        static_cast<long>(millis() - deadline) >= 0) {
      return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/** One line of the reply's head, or a chunk's size, without its CRLF and in
 *  lower case. False if it did not all come. */
bool nextLine(NetworkClient& c, char* out, size_t len, unsigned long deadline) {
  size_t n = 0;
  for (;;) {
    const int b = nextByte(c, deadline);
    if (b < 0) {
      return false;
    }
    if (b == '\n') {
      break;
    }
    if (b != '\r' && n + 1 < len) {
      out[n++] = static_cast<char>(tolower(b));
    }
  }
  out[n] = '\0';
  return true;
}

/** Up to `limit` bytes of the body, to the player as they come. Stops at
 *  `limit`, when the connection closes, when nothing has come for
 *  kBodyIdleMs, or when the request is given up; says how many it passed
 *  on. */
size_t bodyToPlayer(NetworkClient& c, size_t limit) {
  constexpr unsigned long kBodyIdleMs = 5000;
  uint8_t buf[512];
  size_t passed = 0;
  unsigned long heard = millis();
  while (passed < limit && !s_fetch_abort) {
    const int avail = c.available();
    if (avail > 0) {
      size_t want = limit - passed < sizeof(buf) ? limit - passed : sizeof(buf);
      want = static_cast<size_t>(avail) < want ? avail : want;
      const int got = c.read(buf, want);
      if (got > 0) {
        if (hw::audio::mp3Write(buf, got, config::kVoiceReplyTimeoutMs) <
            static_cast<size_t>(got)) {
          break;  // the player was stopped
        }
        passed += got;
        heard = millis();
        continue;
      }
    }
    if (!c.connected() || millis() - heard >= kBodyIdleMs) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return passed;
}

/** Internal RAM's free blocks, as heap_caps_walk() finds them: the total
 *  and the two largest. */
struct FreeBlocks {
  size_t total = 0;
  size_t first = 0;
  size_t second = 0;
};

// Nothing here may allocate or log: the heap is locked while it runs.
bool countFree(walker_heap_into_t, walker_block_info_t block, void* user) {
  if (!block.used) {
    auto* f = static_cast<FreeBlocks*>(user);
    f->total += block.size;
    if (block.size > f->first) {
      f->second = f->first;
      f->first = block.size;
    } else if (block.size > f->second) {
      f->second = block.size;
    }
  }
  return true;
}

/** Whether a TLS session would fit in internal RAM now. mbedTLS here keeps
 *  a 16 KB record buffer each way, about 17 KB with its overhead, and the
 *  handshake wants another 16 KB or so in pieces -- its big numbers and the
 *  server's certificates. The largest free block alone does not say: a
 *  34 KB block with nothing else as big failed, and so did a check that
 *  allowed only 6 KB for the handshake. Read off the heap's free blocks
 *  rather than found by allocating it all and giving it back, which took
 *  nearly all of internal RAM from everything else for the moment it
 *  lasted, ten times a second. */
bool tlsFits() {
  constexpr size_t kRecord = 16384 + 512;
  constexpr size_t kHandshake = 16 * 1024;
  FreeBlocks f;
  heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, countFree, &f);
  // Both record buffers placed: in the two largest blocks, or both in one.
  const bool records = f.second >= kRecord || f.first >= 2 * kRecord;
  return records && f.total >= 2 * kRecord + kHandshake;
}

/** Wait, up to config::kVoiceFetchMemoryWaitMs, for tlsFits(). */
bool waitForTlsRoom() {
  for (unsigned long waited = 0; !tlsFits(); waited += 100) {
    if (waited >= config::kVoiceFetchMemoryWaitMs) {
      LOG_WARN("VOICE: too little memory to download the reply (internal "
               "free %u, largest %u)",
               static_cast<unsigned>(heap_caps_get_free_size(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

/** The reply's audio, downloaded and played as it comes.
 *
 *  By hand rather than with HTTPClient, so that it can be given up: Home
 *  Assistant holds the GET until the speech is made, and when the request
 *  fails after the download has started -- the speech never made, or the
 *  stream lost -- it never answers. HTTPClient waited its whole timeout,
 *  15 s, and no request could start meanwhile; this checks s_fetch_abort
 *  while it waits. */
void fetchReply() {
  char host[config::kHaBaseUrlMaxLen + 1];
  char prefix[config::kHaBaseUrlMaxLen + 1];
  uint16_t port = 0;
  bool tls = false;
  if (!ha::detail::serverAddress(host, sizeof(host), port, tls, prefix,
                                 sizeof(prefix))) {
    LOG_WARN("VOICE: no server address for the reply");
    return;
  }
  String path = s_reply_url;
  if (path.startsWith("/")) {
    path = String(prefix) + path;
  } else {
    // An absolute URL: its path, from the same server.
    const int scheme = path.indexOf("://");
    const int slash = scheme < 0 ? -1 : path.indexOf('/', scheme + 3);
    if (slash < 0) {
      LOG_WARN("VOICE: bad reply URL");
      return;
    }
    path = path.substring(slash);
  }

  // The REST connection goes whether or not it is idle: its session is a
  // second one's worth of internal RAM, and the reply cannot wait for it.
  // The artwork worker holds off while a request runs (artwork.cpp); what
  // else is using the RAM may be about to give it back.
  ha::detail::dropRestConnection();

  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  NetworkClient* c = &plain;
  if (tls) {
    secure.setInsecure();  // as for the rest of Home Assistant: see REST
    secure.setHandshakeTimeout(config::kHaStreamHandshakeMs / 1000);
    c = &secure;
  }
  // A connection that could not be made is tried twice more: what failed
  // it is a handshake running out of internal RAM that something else gave
  // back moments later. Nothing has reached Home Assistant by then, so the
  // reply is still there to ask for.
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; ++attempt) {
    if (s_fetch_abort || !waitForTlsRoom()) {
      return;
    }
    connected = c->connect(host, port);
    if (!connected) {
      LOG_WARN("VOICE: reply download could not connect, %s",
               attempt < 2 ? "trying again" : "giving up");
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
  if (!connected) {
    return;
  }

  const unsigned long started = millis();
  String request("GET ");
  request += path;
  request += " HTTP/1.1\r\nHost: ";
  request += host;
  request += "\r\nConnection: close\r\n\r\n";
  c->write(reinterpret_cast<const uint8_t*>(request.c_str()),
           request.length());

  // The wait for the head is the wait for the speech to be made.
  const unsigned long deadline = started + config::kVoiceReplyTimeoutMs;
  char line[160];
  int status = 0;
  bool chunked = false;
  long length = -1;
  bool head = nextLine(*c, line, sizeof(line), deadline);
  if (head) {
    sscanf(line, "http/%*s %d", &status);
  }
  while (head) {
    head = nextLine(*c, line, sizeof(line), deadline);
    if (!head || line[0] == '\0') {
      break;
    }
    if (strncmp(line, "transfer-encoding:", 18) == 0 &&
        strstr(line + 18, "chunked") != nullptr) {
      chunked = true;
    } else if (strncmp(line, "content-length:", 15) == 0) {
      length = atol(line + 15);
    }
  }
  if (!head) {
    LOG_WARN("VOICE: reply %s", s_fetch_abort ? "dropped: the request failed"
                                              : "never came");
    c->stop();
    return;
  }
  if (status != 200) {
    LOG_WARN("VOICE: reply download: HTTP %d", status);
    c->stop();
    return;
  }
  LOG_INFO("VOICE: reply arriving, %lu ms after asking",
           static_cast<unsigned long>(millis() - started));

  if (hw::audio::mp3Begin()) {
    Phase thinking = Phase::kThinking;
    s_phase.compare_exchange_strong(thinking, Phase::kReplying);
    if (chunked) {
      // Each chunk as it comes, so the player starts on the first.
      for (;;) {
        if (!nextLine(*c, line, sizeof(line), millis() + 5000)) {
          break;
        }
        const size_t size = strtoul(line, nullptr, 16);
        if (size == 0 || bodyToPlayer(*c, size) < size) {
          break;
        }
        nextLine(*c, line, sizeof(line), millis() + 5000);  // its CRLF
      }
    } else {
      bodyToPlayer(*c, length >= 0 ? static_cast<size_t>(length) : SIZE_MAX);
    }
    hw::audio::mp3End();
  }
  c->stop();
}

void voiceTask(void*) {
  for (;;) {
    Job job;
    if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    // In both, the REST connection is closed at once, idle or not: its TLS
    // session is the internal RAM the reply's download will want in a few
    // seconds, and nothing in a request needs it. Here, not on the caller,
    // which may be the loop: closing it waits out a call in progress. Closed
    // again before the download, in case something reopened it.
    if (job.kind == Job::kListen && job.from_position) {
      // After a wake word the command is already being said: the
      // microphone from where the wake word ended, then the beep, which
      // goes up with it.
      hw::audio::stop();
      Phase starting = Phase::kStarting;
      if (s_phase.compare_exchange_strong(starting, Phase::kListening)) {
        hw::audio::captureStartAt(kMic, job.position);
        s_capture_began = true;
        LOG_INFO("VOICE: listening, from the wake word");
        // After the microphone, which is already being said to.
        ha::detail::dropRestConnection();
        hw::audio::beep(kBeepHz, kBeepMs);
      }
    } else if (job.kind == Job::kListen) {
      hw::audio::stop();
      ha::detail::dropRestConnection();
      hw::audio::beep(kBeepHz, kBeepMs);
      // Cancelled during the beep, or already failed: no listening.
      Phase starting = Phase::kStarting;
      if (s_phase.compare_exchange_strong(starting, Phase::kListening)) {
        hw::audio::captureStart(kMic);
        s_capture_began = true;
        LOG_INFO("VOICE: listening");
      }
    } else {
      fetchReply();
      s_fetching = false;
    }
  }
}

}  // namespace

void init() {
  if (s_jobs != nullptr) {
    return;
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    const uint8_t stored = prefs.getUChar(kPrefsWakeKey, 0);
    // A word this build no longer has is as none chosen.
    if (stored >= 1 && stored <= wake_word::kWordCount &&
        wake_word::has(static_cast<wake_word::Word>(stored - 1))) {
      s_setting = stored;
      wake_word::setWord(static_cast<wake_word::Word>(stored - 1));
    }
    prefs.end();
  }
  // The audio starts either way, so that the setting can be turned on
  // again without a restart.
  if (!hw::audio::begin()) {
    return;
  }
  hw::audio::setVolume(config::kVoiceVolume);
  s_pcm = static_cast<int16_t*>(
      heap_caps_malloc(config::kVoiceFrameSamples * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_frame = static_cast<uint8_t*>(
      heap_caps_malloc(1 + config::kVoiceFrameSamples * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_pcm == nullptr || s_frame == nullptr) {
    LOG_WARN("VOICE: no memory for the audio");
    return;
  }
  s_jobs = xQueueCreate(4, sizeof(Job));
  // In PSRAM, as hw::audio's are: it is TLS's internal RAM that runs short.
  TaskHandle_t task = nullptr;
  xTaskCreatePinnedToCoreWithCaps(voiceTask, "voice", 16384, nullptr, 2,
                                  &task, 0,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_jobs == nullptr || task == nullptr) {
    LOG_WARN("VOICE: could not start");
    return;
  }
  ha::setStreamHook(hook);
  ha::setStreamListener(listener);
  if (s_setting != 0) {
    LOG_INFO("VOICE: ready, listening for \"%s\"", wake_word::phrase());
  } else {
    LOG_INFO("VOICE: ready, but disabled");
  }
}

int setting() {
  return s_setting;
}

bool enabled() {
  return s_setting != 0;
}

bool saveSetting(int value) {
  if (value < 0 || value > wake_word::kWordCount ||
      (value > 0 && !wake_word::has(static_cast<wake_word::Word>(value - 1)))) {
    return false;
  }
  if (value > 0) {
    wake_word::setWord(static_cast<wake_word::Word>(value - 1));
  }
  s_setting = static_cast<uint8_t>(value);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsWakeKey, static_cast<uint8_t>(value));
    prefs.remove(kPrefsOldEnabledKey);
    prefs.end();
  }
  if (value == 0) {
    cancel();
  }
  // A no-op where the wake word never started.
  wake_word::setListening(value != 0);
  if (value != 0) {
    LOG_INFO("VOICE: listening for \"%s\"", wake_word::phrase());
  } else {
    LOG_INFO("VOICE: disabled");
  }
  return true;
}

void setCommandHandler(bool (*handler)(Command command, const char* query)) {
  s_command_handler = handler;
}

bool available() {
  return s_setting != 0 && s_jobs != nullptr && hw::audio::ready() &&
         ha::streamIsOpen();
}

namespace {

/** Start a run: `body` is what to send, and `job` what this module's task
 *  does about it, if anything. */
bool begin(const String& body, bool say_only, const Job* job) {
  if (!available() || s_end_on_start) {
    return false;
  }
  const Phase p = s_phase;
  if (p != Phase::kIdle && p != Phase::kFailed && p != Phase::kReplying) {
    return false;
  }
  if (p == Phase::kReplying) {
    // A reply cut off, and its download with it.
    s_fetch_abort = true;
    hw::audio::stop();
    s_want_cancel = true;
    for (int i = 0; i < 40 && s_fetching; ++i) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  if (s_fetching) {
    return false;
  }
  taskENTER_CRITICAL(&s_lock);
  s_status = Status();
  taskEXIT_CRITICAL(&s_lock);
  s_sorry = nullptr;  // asked again: the new request is the answer
  s_capture_began = false;
  s_run_body = body;
  s_kind = say_only ? RunKind::kSay : RunKind::kListen;
  s_heard[0] = '\0';
  s_phase = say_only ? Phase::kThinking : Phase::kStarting;
  s_want_run = true;
  if (job != nullptr) {
    xQueueSend(s_jobs, job, 0);
  }
  return true;
}

}  // namespace

bool start() {
  const Job job = {Job::kListen, false, 0};
  return begin(kRunBody, false, &job);
}

bool startAt(uint32_t position) {
  const Job job = {Job::kListen, true, position};
  return begin(kRunBody, false, &job);
}

bool say(const char* text) {
  String body(kSayBodyStart);
  ha::detail::appendJson(body, text);
  body += "}";
  LOG_INFO("VOICE: saying \"%s\"", text);
  return begin(body, true, nullptr);
}

void finish() {
  // Let go before it had begun to listen: a press that was not held long
  // enough, which asked for nothing.
  if (s_phase == Phase::kStarting) {
    LOG_INFO("VOICE: let go before listening -- cancelled");
    cancel();
    return;
  }
  hw::audio::captureStop(kMic);
}

void cancel() {
  hw::audio::captureStop(kMic);
  s_sorry = nullptr;
  s_fetch_abort = true;
  hw::audio::stop();
  s_want_cancel = true;
  s_want_run = false;
  s_phase = Phase::kIdle;
}

bool busy() {
  const Phase p = s_phase;
  return (p != Phase::kIdle && p != Phase::kFailed) || s_fetching ||
         s_sorry != nullptr;
}

uint16_t level() {
  return s_phase == Phase::kListening ? hw::audio::inputPeak() : 0;
}

void status(Status& out) {
  taskENTER_CRITICAL(&s_lock);
  out = s_status;
  taskEXIT_CRITICAL(&s_lock);
  out.phase = s_phase;
}

}  // namespace services::voice

#else  // a build without voice

namespace services::voice {

void init() {}
int setting() { return 0; }
bool enabled() { return false; }
bool saveSetting(int) { return false; }
void setCommandHandler(bool (*)(Command, const char*)) {}
bool available() { return false; }
bool start() { return false; }
bool startAt(uint32_t) { return false; }
bool say(const char*) { return false; }
void finish() {}
void cancel() {}
bool busy() { return false; }
uint16_t level() { return 0; }
void status(Status& out) { out = Status{}; }

}  // namespace services::voice

#endif  // VOICE_ASSIST
