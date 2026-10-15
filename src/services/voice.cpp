/**
 * A voice request, end to end. The protocol, as much of it as is used:
 *
 *   client  {"id":9,"type":"assist_pipeline/run","start_stage":"stt",
 *            "end_stage":"tts","input":{"sample_rate":16000}}
 *   server  {"id":9,"type":"result","success":true,"result":null}
 *   server  event run-start: runner_data.stt_binary_handler_id, and the
 *           reply's audio URL already, as tts_output.url
 *   client  binary: the handler id, then 16-bit 16 kHz mono audio, again and
 *           again; the handler id alone for the end of the speech
 *   server  events stt-vad-start, stt-vad-end (it heard the speech stop),
 *           stt-end (stt_output.text), intent-end (the reply's text, at
 *           intent_output.response.speech.plain.speech), tts-end, run-end;
 *           or error, with a code and a message, and then run-end
 *
 * Home Assistant holds a GET of the reply's URL until the speech has been
 * made, so the download is started once the words are recognised and is
 * connected and waiting by the time the reply exists. Not sooner: a request
 * whose speech was not understood never makes the reply, and its download
 * would wait out its whole timeout.
 *
 * Three tasks take part. Any task calls start() and the rest. The stream's
 * owner runs hook() and listener(): the request, the audio and the events,
 * as the socket is that task's alone. This module's own task plays the beep
 * and downloads the reply, which would hold the owner up for seconds.
 */

#include "services/voice.h"

#include <Arduino.h>

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
#include "services/yielding_client.h"

namespace services::voice {
namespace {

using ha::detail::eachMember;
using ha::detail::readJson;

constexpr char kRunBody[] =
    "\"type\":\"assist_pipeline/run\",\"start_stage\":\"stt\","
    "\"end_stage\":\"tts\",\"input\":{\"sample_rate\":16000}";
/** How long Home Assistant has to start the run before it counts as lost. */
constexpr unsigned long kStartTimeoutMs = 5000;
/** The beep that says it is listening. */
constexpr uint16_t kBeepHz = 880;
constexpr uint16_t kBeepMs = 120;

enum class Job : uint8_t { kListen, kFetch };

std::atomic<Phase> s_phase{Phase::kIdle};
/** start() and cancel(), for the owner's next pass. */
std::atomic<bool> s_want_run{false};
std::atomic<bool> s_want_cancel{false};
/** Set by this module's task once the microphone is being kept. */
std::atomic<bool> s_capture_began{false};
std::atomic<bool> s_fetching{false};
/** Set when the request a download belongs to has failed or been dropped:
 *  Home Assistant will never make its speech, and the download stops
 *  waiting for it. Cleared as a download is queued. */
std::atomic<bool> s_fetch_abort{false};

/** The texts, for status(); under s_lock. */
Status s_status;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t s_jobs = nullptr;
/** The reply's URL, written by the owner before it queues kFetch. */
char s_reply_url[256] = {};

// The owner's alone.
int s_run_id = 0;
int s_handler = -1;
bool s_audio_done = false;
bool s_run_ended = false;
bool s_fetch_queued = false;
unsigned long s_run_sent_ms = 0;
unsigned long s_speech_end_ms = 0;
/** One message of audio being gathered, and the message itself: in PSRAM,
 *  taken by init() on a board with a microphone and nowhere else. */
int16_t* s_pcm = nullptr;
size_t s_pcm_fill = 0;
uint8_t* s_frame = nullptr;

template <size_t N>
void setText(char (Status::*field)[N], const char* text) {
  taskENTER_CRITICAL(&s_lock);
  snprintf(s_status.*field, N, "%s", text);
  taskEXIT_CRITICAL(&s_lock);
}

void fail(const char* why) {
  hw::audio::captureStop();
  s_fetch_abort = true;
  setText(&Status::error, why);
  s_phase = Phase::kFailed;
  LOG_WARN("VOICE: %s", why);
}

/** The microphone, as much as there is, out in whole messages; the last,
 *  short one and the end marker once it has stopped. */
void pumpAudio() {
  if (s_handler < 0 || s_audio_done || !s_capture_began) {
    return;
  }
  for (;;) {
    // Whether it was still capturing, asked before reading: stopped then
    // and nothing read now means everything has been read.
    const bool capturing = hw::audio::capturing();
    const size_t n = hw::audio::captureRead(
        s_pcm + s_pcm_fill, config::kVoiceFrameSamples - s_pcm_fill);
    s_pcm_fill += n;
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
      if (hw::audio::captureDropped() > 0) {
        LOG_WARN("VOICE: %lu samples of speech lost",
                 static_cast<unsigned long>(hw::audio::captureDropped()));
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
  const Job job = Job::kFetch;
  xQueueSend(s_jobs, &job, 0);
}

/** On the owner, every pass while the stream is open. */
void hook() {
  if (s_want_cancel.exchange(false)) {
    s_run_id = 0;  // anything more from it is ignored
  }
  if (s_want_run.exchange(false)) {
    s_handler = -1;
    s_audio_done = false;
    s_run_ended = false;
    s_fetch_queued = false;
    s_pcm_fill = 0;
    s_reply_url[0] = '\0';
    s_run_sent_ms = millis();
    s_run_id = ha::streamRequest(kRunBody);
    if (s_run_id == 0) {
      fail("could not reach Home Assistant");
    }
    return;
  }
  if (s_run_id == 0) {
    return;
  }
  pumpAudio();

  const unsigned long now = millis();
  if (s_handler < 0 && now - s_run_sent_ms >= kStartTimeoutMs) {
    s_run_id = 0;
    fail("Home Assistant did not start listening");
    return;
  }
  if (s_phase == Phase::kListening && hw::audio::capturing() &&
      now - s_run_sent_ms >= config::kVoiceMaxListenMs) {
    hw::audio::captureStop();
  }
  if (s_run_ended && !s_fetching && !hw::audio::playing()) {
    s_run_id = 0;
    if (s_phase != Phase::kFailed) {
      s_phase = Phase::kIdle;
      LOG_INFO("VOICE: done, %lu ms after the speech ended",
               static_cast<unsigned long>(now - s_speech_end_ms));
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
    const Phase p = s_phase;
    if (s_run_id != 0 || p == Phase::kStarting) {
      s_run_id = 0;
      fail("lost the connection to Home Assistant");
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
      fail(why[0] != '\0' ? why : "Home Assistant refused the request");
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
    static const char* const kUrl[] = {"tts_output", "url", nullptr};
    readPath(data, kUrl, s_reply_url, sizeof(s_reply_url));
    s_frame[0] = static_cast<uint8_t>(s_handler);
    if (s_handler < 0) {
      s_run_id = 0;
      fail("Home Assistant gave no audio channel");
    }
  } else if (strcmp(etype, "stt-vad-end") == 0) {
    hw::audio::captureStop();  // it has stopped listening
  } else if (strcmp(etype, "stt-end") == 0) {
    char heard[sizeof(Status::heard)];
    static const char* const kText[] = {"stt_output", "text", nullptr};
    readPath(data, kText, heard, sizeof(heard));
    setText(&Status::heard, heard);
    LOG_INFO("VOICE: heard \"%s\"", heard);
    hw::audio::captureStop();
    queueFetch();
  } else if (strcmp(etype, "intent-end") == 0) {
    char reply[sizeof(Status::reply)];
    static const char* const kSpeech[] = {"intent_output", "response",
                                          "speech",        "plain",
                                          "speech",        nullptr};
    readPath(data, kSpeech, reply, sizeof(reply));
    setText(&Status::reply, reply);
    LOG_INFO("VOICE: reply \"%s\"", reply);
  } else if (strcmp(etype, "tts-end") == 0) {
    queueFetch();  // in case stt-end did not get to it
  } else if (strcmp(etype, "error") == 0) {
    char code[48];
    char why[96];
    static const char* const kCode[] = {"code", nullptr};
    static const char* const kMessage[] = {"message", nullptr};
    readPath(data, kCode, code, sizeof(code));
    readPath(data, kMessage, why, sizeof(why));
    LOG_WARN("VOICE: Home Assistant said %s", code);
    fail(why[0] != '\0' ? why : code);
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

  auto largest = [] {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                            MALLOC_CAP_8BIT);
  };
  if (largest() < config::kVoiceFetchMinBlockBytes) {
    ha::detail::dropRestConnection();
  }
  if (largest() < config::kVoiceFetchMinBlockBytes) {
    LOG_WARN("VOICE: too little memory to download the reply (largest "
             "block %u)",
             static_cast<unsigned>(largest()));
    return;
  }

  services::YieldingClient plain;
  services::YieldingClientSecure secure;
  NetworkClient* c = &plain;
  if (tls) {
    secure.setInsecure();  // as for the rest of Home Assistant: see REST
    secure.setHandshakeTimeout(config::kHaStreamHandshakeMs / 1000);
    c = &secure;
  }
  if (!c->connect(host, port)) {
    LOG_WARN("VOICE: reply download could not connect");
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
    if (job == Job::kListen) {
      hw::audio::stop();
      hw::audio::beep(kBeepHz, kBeepMs);
      // Cancelled during the beep, or already failed: no listening.
      Phase starting = Phase::kStarting;
      if (s_phase.compare_exchange_strong(starting, Phase::kListening)) {
        hw::audio::captureStart();
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
  if (s_jobs != nullptr || !hw::audio::begin()) {
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
  LOG_INFO("VOICE: ready");
}

bool available() {
  return s_jobs != nullptr && hw::audio::ready() && ha::streamIsOpen();
}

bool start() {
  if (!available()) {
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
  s_capture_began = false;
  s_phase = Phase::kStarting;
  s_want_run = true;
  const Job job = Job::kListen;
  xQueueSend(s_jobs, &job, 0);
  return true;
}

void finish() {
  hw::audio::captureStop();
}

void cancel() {
  hw::audio::captureStop();
  s_fetch_abort = true;
  hw::audio::stop();
  s_want_cancel = true;
  s_want_run = false;
  s_phase = Phase::kIdle;
}

bool busy() {
  const Phase p = s_phase;
  return (p != Phase::kIdle && p != Phase::kFailed) || s_fetching;
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
