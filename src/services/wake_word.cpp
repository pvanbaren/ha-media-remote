/**
 * microWakeWord, as Home Assistant's Voice PE runs it, here on the remote.
 *
 * The microphone's 16 kHz audio goes through TensorFlow Lite Micro's audio
 * front end (lib/microfrontend) 10 ms at a time, which makes 40 spectrogram
 * channels of each 30 ms window. The model is a streaming one: it keeps its
 * own memory of what it has heard in variables between runs, and takes three
 * of those 10 ms steps per run. Each run gives a confidence, 0..255; the
 * wake word is heard when the average of the last five reaches the model's
 * cutoff.
 *
 * The front end's settings and the scaling of its output to the model's
 * int8 input are microWakeWord's, as ESPHome's micro_wake_word applies them:
 * the models were trained on features made exactly this way.
 *
 * The models are esphome/micro-wake-word-models' four, v2 (Apache License
 * 2.0), at commit 40ff33f5 -- hey_jarvis, okay_nabu, hey_mycroft and alexa
 * -- each with the cutoff its manifest gives, embedded by platformio.ini on
 * a board with a microphone. One is loaded at a time, the one chosen, into
 * the same arenas. Where a model is not embedded its symbol is not there,
 * and it is not offered.
 */

#include "services/wake_word.h"

#include <Arduino.h>

#include <atomic>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>

#include "config.h"
#include "hardware/audio.h"
#include "log.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

// The models, where the board's env embeds them; weak, so a build without
// one still links and finds nullptr for it.
extern const uint8_t kHeyJarvisModel[] asm(
    "_binary_data_hey_jarvis_tflite_start") __attribute__((weak));
extern const uint8_t kOkayNabuModel[] asm(
    "_binary_data_okay_nabu_tflite_start") __attribute__((weak));
extern const uint8_t kHeyMycroftModel[] asm(
    "_binary_data_hey_mycroft_tflite_start") __attribute__((weak));
extern const uint8_t kAlexaModel[] asm("_binary_data_alexa_tflite_start")
    __attribute__((weak));

namespace services::wake_word {
namespace {

constexpr hw::audio::Reader kMic = hw::audio::Reader::kWakeWord;

struct Model {
  const char* phrase;
  const uint8_t* data;
  /** The manifest's probability_cutoff, of 255. */
  uint8_t cutoff;
};
/** In Word's order. */
const Model kModels[kWordCount] = {
    {"Hey Jarvis", kHeyJarvisModel, 247},    // 0.97
    {"Okay Nabu", kOkayNabuModel, 247},      // 0.97
    {"Hey Mycroft", kHeyMycroftModel, 242},  // 0.95
    {"Alexa", kAlexaModel, 230},             // 0.90
};

const Model& model(Word word) {
  return kModels[static_cast<int>(word)];
}
/** The manifests' 22 to 26 KB are ESPHome's TensorFlow Lite Micro's
 *  figures; this framework's would not fit the models in them. */
constexpr size_t kTensorArenaBytes = 64 * 1024;
/** For the model's variables, which live outside the tensor arena:
 *  ESPHome's figure. */
constexpr size_t kVariableArenaBytes = 1024;
constexpr int kMaxVariables = 20;
/** The front end's step, and the model's: 10 ms. */
constexpr size_t kStepSamples = 160;
constexpr int kFeatures = 40;

std::atomic<bool> s_started{false};
std::atomic<bool> s_listening{false};
std::atomic<bool> s_detected{false};
std::atomic<uint32_t> s_position{0};
/** The Word wanted, setWord()'s. */
std::atomic<uint8_t> s_word{static_cast<uint8_t>(Word::kHeyJarvis)};
/** The loaded model's cutoff, for the stats. */
std::atomic<uint8_t> s_cutoff{0};

// Made once, in prepare().
tflite::MicroMutableOpResolver<20> s_ops;
uint8_t* s_arena = nullptr;
uint8_t* s_var_arena = nullptr;
void* s_interpreter_place = nullptr;

// The task's own, after init().
FrontendState s_frontend;
tflite::MicroInterpreter* s_interpreter = nullptr;
/** The Word whose model was last loaded, or tried: -1 for none. */
int s_loaded = -1;
TfLiteTensor* s_input = nullptr;
TfLiteTensor* s_output = nullptr;
int s_stride = 0;  // 10 ms steps per run
int s_slot = 0;    // of them filled
uint8_t s_window[config::kWakeWordWindow] = {};
int s_window_at = 0;
/** Runs still to make before it may fire. */
int s_settle = 0;
constexpr int kSettleRuns = static_cast<int>(config::kWakeWordSettleMs / 30);

/** The stats, under s_stats_lock. */
Stats s_stats;
uint64_t s_total_us = 0;
portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

void* psram(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** microWakeWord's front end, as it was trained with. */
bool frontendInit() {
  FrontendConfig config;
  FrontendFillConfigWithDefaults(&config);
  config.window.size_ms = 30;
  config.window.step_size_ms = 10;
  config.filterbank.num_channels = kFeatures;
  config.filterbank.lower_band_limit = 125.0f;
  config.filterbank.upper_band_limit = 7500.0f;
  config.noise_reduction.smoothing_bits = 10;
  config.noise_reduction.even_smoothing = 0.025f;
  config.noise_reduction.odd_smoothing = 0.06f;
  config.noise_reduction.min_signal_remaining = 0.05f;
  config.pcan_gain_control.enable_pcan = 1;
  config.pcan_gain_control.strength = 0.95f;
  config.pcan_gain_control.offset = 80.0f;
  config.pcan_gain_control.gain_bits = 21;
  config.log_scale.enable_log = 1;
  config.log_scale.scale_shift = 6;
  return FrontendPopulateState(&config, &s_frontend,
                               hw::audio::kSampleRate) != 0;
}

/** What every model needs, once: its operations and its memory. */
bool prepare() {
  // The operations microWakeWord's streaming models use.
  if (s_ops.AddCallOnce() != kTfLiteOk || s_ops.AddVarHandle() != kTfLiteOk ||
      s_ops.AddReshape() != kTfLiteOk ||
      s_ops.AddReadVariable() != kTfLiteOk ||
      s_ops.AddStridedSlice() != kTfLiteOk ||
      s_ops.AddConcatenation() != kTfLiteOk ||
      s_ops.AddAssignVariable() != kTfLiteOk ||
      s_ops.AddConv2D() != kTfLiteOk || s_ops.AddMul() != kTfLiteOk ||
      s_ops.AddAdd() != kTfLiteOk || s_ops.AddMean() != kTfLiteOk ||
      s_ops.AddFullyConnected() != kTfLiteOk ||
      s_ops.AddLogistic() != kTfLiteOk || s_ops.AddQuantize() != kTfLiteOk ||
      s_ops.AddDepthwiseConv2D() != kTfLiteOk ||
      s_ops.AddAveragePool2D() != kTfLiteOk ||
      s_ops.AddMaxPool2D() != kTfLiteOk || s_ops.AddPad() != kTfLiteOk ||
      s_ops.AddPack() != kTfLiteOk || s_ops.AddSplitV() != kTfLiteOk) {
    LOG_WARN("WAKE: could not register the models' operations");
    return false;
  }
  // In PSRAM, both arenas: internal RAM is TLS's. The interpreter itself in
  // PSRAM too: a plain new would put its 1 KB or so in internal RAM.
  s_arena = static_cast<uint8_t*>(psram(kTensorArenaBytes));
  s_var_arena = static_cast<uint8_t*>(psram(kVariableArenaBytes));
  s_interpreter_place = psram(sizeof(tflite::MicroInterpreter));
  if (s_arena == nullptr || s_var_arena == nullptr ||
      s_interpreter_place == nullptr) {
    LOG_WARN("WAKE: no memory for the model");
    return false;
  }
  return true;
}

void unload() {
  if (s_interpreter != nullptr) {
    s_interpreter->~MicroInterpreter();
    s_interpreter = nullptr;
  }
}

/** `word`'s model into the arenas, in place of the one there: on the task,
 *  or in init() before it starts. False, with none loaded, where it cannot
 *  be; not tried again until another word is chosen. */
bool load(Word word) {
  unload();
  s_loaded = static_cast<int>(word);
  const Model& m = model(word);
  if (m.data == nullptr) {
    LOG_WARN("WAKE: no \"%s\" model in this build", m.phrase);
    return false;
  }
  const tflite::Model* tfl = tflite::GetModel(m.data);
  if (tfl->version() != TFLITE_SCHEMA_VERSION) {
    LOG_WARN("WAKE: model is schema %lu, not %d",
             static_cast<unsigned long>(tfl->version()),
             TFLITE_SCHEMA_VERSION);
    return false;
  }
  // Both made afresh over their arenas, which the last model's interpreter,
  // now gone, had.
  tflite::MicroAllocator* allocator =
      tflite::MicroAllocator::Create(s_var_arena, kVariableArenaBytes);
  tflite::MicroResourceVariables* variables =
      tflite::MicroResourceVariables::Create(allocator, kMaxVariables);
  s_interpreter = new (s_interpreter_place) tflite::MicroInterpreter(
      tfl, s_ops, s_arena, kTensorArenaBytes, variables);
  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    LOG_WARN("WAKE: the \"%s\" model does not fit its arena", m.phrase);
    unload();
    return false;
  }
  s_input = s_interpreter->input(0);
  s_output = s_interpreter->output(0);
  if (s_input->type != kTfLiteInt8 || s_input->dims->size != 3 ||
      s_input->dims->data[2] != kFeatures || s_output->type != kTfLiteUInt8) {
    LOG_WARN("WAKE: the \"%s\" model's input or output is not what was "
             "expected",
             m.phrase);
    unload();
    return false;
  }
  s_stride = s_input->dims->data[1];
  s_cutoff = m.cutoff;
  LOG_INFO("WAKE: \"%s\" ready, %d steps a run, arena %u of %u bytes",
           m.phrase, s_stride,
           static_cast<unsigned>(s_interpreter->arena_used_bytes()),
           static_cast<unsigned>(kTensorArenaBytes));
  return true;
}

/** Afresh: the front end's smoothing, the model's memory, the scores. */
void restart() {
  FrontendReset(&s_frontend);
  s_interpreter->Reset();
  s_slot = 0;
  memset(s_window, 0, sizeof(s_window));
  s_settle = kSettleRuns;
}

void run() {
  const uint32_t t0 = micros();
  if (s_interpreter->Invoke() != kTfLiteOk) {
    LOG_WARN("WAKE: the model failed to run");
    return;
  }
  const uint32_t us = micros() - t0;
  s_window[s_window_at] = s_output->data.uint8[0];
  s_window_at = (s_window_at + 1) % config::kWakeWordWindow;
  int sum = 0;
  for (uint8_t p : s_window) {
    sum += p;
  }
  const uint8_t score = static_cast<uint8_t>(sum / config::kWakeWordWindow);

  bool fired = false;
  if (s_settle > 0) {
    --s_settle;
  } else if (score >= s_cutoff) {
    fired = true;
    s_position = hw::audio::capturePosition(kMic) -
                 hw::audio::kSampleRate * config::kWakeWordPreRollMs / 1000;
    s_detected = true;
    memset(s_window, 0, sizeof(s_window));
    s_settle = kSettleRuns;
  }

  taskENTER_CRITICAL(&s_stats_lock);
  ++s_stats.runs;
  s_total_us += us;
  s_stats.max_us = us > s_stats.max_us ? us : s_stats.max_us;
  s_stats.max_score = score > s_stats.max_score ? score : s_stats.max_score;
  s_stats.detections += fired ? 1 : 0;
  taskEXIT_CRITICAL(&s_stats_lock);
  if (fired) {
    LOG_INFO("WAKE: heard \"%s\" (score %u)",
             model(static_cast<Word>(s_loaded)).phrase, score);
  }
}

/** One 10 ms step of audio into the front end, and its features into the
 *  model's input, scaled as microWakeWord scales them. */
void step(const int16_t* samples) {
  size_t left = kStepSamples;
  while (left > 0) {
    size_t read = 0;
    const FrontendOutput out =
        FrontendProcessSamples(&s_frontend, samples, left, &read);
    samples += read;
    left -= read;
    if (out.size != kFeatures) {
      if (read == 0) {
        return;
      }
      continue;
    }
    int8_t* in = s_input->data.int8 + s_slot * kFeatures;
    for (int i = 0; i < kFeatures; ++i) {
      // 256 / 666: the front end's log scale to the model's int8 range.
      int32_t v = (static_cast<int32_t>(out.values[i]) * 256 + 333) / 666;
      v -= 128;
      in[i] = static_cast<int8_t>(v < -128 ? -128 : v > 127 ? 127 : v);
    }
    if (++s_slot == s_stride) {
      s_slot = 0;
      run();
    }
  }
}

void wakeTask(void*) {
  int16_t samples[kStepSamples];
  size_t fill = 0;
  bool live = false;
  bool held = false;  // quiet while the remote listens or speaks
  for (;;) {
    // Another word chosen: its model, and afresh.
    if (s_word != s_loaded) {
      load(static_cast<Word>(s_word.load()));
      if (live && s_interpreter != nullptr) {
        restart();
      }
    }
    if (!s_listening || s_interpreter == nullptr) {
      if (live) {
        hw::audio::captureStop(kMic);
        live = false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!live) {
      hw::audio::captureStart(kMic);
      restart();
      live = true;
      fill = 0;
    }
    fill += hw::audio::captureRead(kMic, samples + fill, kStepSamples - fill);
    if (fill < kStepSamples) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    fill = 0;
    // Read and thrown away while a request listens or a reply plays, so as
    // to be current when it is over; then afresh.
    if (hw::audio::playing() ||
        hw::audio::capturing(hw::audio::Reader::kRequest)) {
      held = true;
      continue;
    }
    if (held) {
      held = false;
      restart();
    }
    step(samples);
  }
}

}  // namespace

bool has(Word word) {
  return static_cast<int>(word) < kWordCount && model(word).data != nullptr;
}

const char* phraseOf(Word word) {
  return static_cast<int>(word) < kWordCount ? model(word).phrase : "";
}

bool init() {
  if (s_started) {
    return true;
  }
  bool any = false;
  for (const Model& m : kModels) {
    any = any || m.data != nullptr;
  }
  if (!any || !hw::audio::ready()) {
    return false;
  }
  // Both in PSRAM, which internal RAM -- what the reply's TLS download is
  // short of -- would otherwise give 12 KB to: the front end through its
  // library's own malloc (lib/microfrontend/src/microfrontend_psram.h), the
  // interpreter in modelInit().
  const bool frontend = frontendInit();
  if (!frontend) {
    LOG_WARN("WAKE: no memory for the front end");
  }
  if (!frontend || !prepare()) {
    return false;
  }
  // The word chosen so far, now, so that a model that does not load says
  // so as the remote starts. The task loads another when one is chosen.
  load(static_cast<Word>(s_word.load()));
  // Its stack in PSRAM, as the audio tasks' are. On the loop's core, so
  // the network's core keeps the stream and the reply moving.
  TaskHandle_t task = nullptr;
  xTaskCreatePinnedToCoreWithCaps(wakeTask, "wake", 12288, nullptr, 3, &task,
                                  1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (task == nullptr) {
    LOG_WARN("WAKE: could not start");
    return false;
  }
  s_started = true;
  return true;
}

void setWord(Word word) {
  if (static_cast<int>(word) < kWordCount) {
    s_word = static_cast<uint8_t>(word);
  }
}

Word word() {
  return static_cast<Word>(s_word.load());
}

void setListening(bool on) {
  s_listening = on && s_started;
}

bool listening() {
  return s_listening;
}

const char* phrase() {
  return model(word()).phrase;
}

bool takeDetection(uint32_t& position) {
  if (!s_detected.exchange(false)) {
    return false;
  }
  position = s_position;
  return true;
}

void takeStats(Stats& out) {
  taskENTER_CRITICAL(&s_stats_lock);
  out = s_stats;
  out.cutoff = s_cutoff;
  out.mean_us = s_stats.runs > 0
                    ? static_cast<uint32_t>(s_total_us / s_stats.runs)
                    : 0;
  s_stats = Stats();
  s_total_us = 0;
  taskEXIT_CRITICAL(&s_stats_lock);
  out.dropped = hw::audio::captureDropped(kMic);
}

}  // namespace services::wake_word
