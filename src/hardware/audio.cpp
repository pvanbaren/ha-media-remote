#include "hardware/audio.h"

// The headless build has no board, and no audio: it gets the no-ops below,
// as does any build without voice (-DVOICE_ASSIST, services/voice.h), whose
// audio this is.
#if defined(BOARD_HEADER)
#include "board/board.h"
#endif

#if defined(BOARD_HAS_MIC) && defined(VOICE_ASSIST)

#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "hardware/audio_codec.h"
#include "log.h"

// minimp3 (lib/minimp3): MPEG layer 3 only, in plain C -- it knows SSE and
// NEON, not the S3's vector unit. Decoding takes about 6% of the time the
// reply takes to play.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include <minimp3.h>

/**
 * One I2S port, both ways, at 16 kHz in stereo: the codecs' native frame.
 * Two tasks serve it, both with their stacks in PSRAM, since internal RAM is
 * what TLS runs short of:
 *
 *  - The capture task reads the microphones for as long as the port runs,
 *    which keeps the receive DMA drained, and while any reader is capturing
 *    keeps the left channel (MIC1), 12 dB up, in a ring. It never waits for
 *    a reader: one that falls a whole ring behind skips ahead.
 *  - The player task decodes a reply from a stream buffer that mp3Write()
 *    fills, a frame at a time, and resamples it to 16 kHz for the speaker.
 *    minimp3 keeps 16 KB of scratch on the stack, which is why this one is
 *    as big as it is.
 *
 * The speaker side is shared by beep() and the player, under s_tx_lock. The
 * DMA plays silence whenever nothing is written (ESP_I2S sets auto_clear),
 * so a reply that arrives more slowly than it plays pauses rather than
 * stutters.
 */
namespace hw::audio {
namespace {

constexpr int kDefaultVolume = 70;
/** The microphone ring: 2.048 s. A power of two, for the index mask. */
constexpr uint32_t kRingSamples = 32768;
/** Software gain on the microphone, as a shift: 12 dB. */
constexpr int kMicGainShift = 2;
/** Frames per read from the port: 16 ms. */
constexpr size_t kReadFrames = 256;
/** MP3 held between the download and the player: 4 s at 64 kbps. */
constexpr size_t kMp3BufferBytes = 32 * 1024;
/** The player's window on the stream: the largest MP3 frame (1,441 bytes)
 *  more than twice over. */
constexpr size_t kWindowBytes = 4096;
/** ESP_I2S's DMA, 6 x 240 frames, 90 ms. That much silence is written after
 *  a sound, so the amplifier goes off only once the sound has left. */
constexpr size_t kDmaFrames = 6 * 240;
/** How loud beep() is, of full scale. It is heard over a room, not at the
 *  ear; at 0.1 it was loud. */
constexpr float kBeepAmplitude = 0.05f;
/** Silence played after the amplifier is switched on, before any sound: it
 *  stays muted for a while after its enable rises, and a short beep played
 *  at once was not heard at all. */
constexpr size_t kWarmUpFrames = kSampleRate * 150 / 1000;
/** The amplifier stays on this long after the last sound, so a run of cues
 *  -- volume ticks under a moving finger -- pays the warm-up once. */
constexpr unsigned long kAmpHoldMs = 2000;
/** How loud the cues are, of full scale: under the beep. */
constexpr float kCueAmplitude = 0.04f;

I2SClass s_i2s;
bool s_tried = false;
std::atomic<bool> s_ready{false};
SemaphoreHandle_t s_tx_lock = nullptr;

// --- Microphone: the capture task writes s_head, each reader its own tail.
int16_t* s_ring = nullptr;
std::atomic<uint32_t> s_head{0};
struct ReaderState {
  std::atomic<uint32_t> tail{0};
  std::atomic<bool> on{false};
  /** Where it was stopped: a stopped reader reads up to here and no
   *  further, though the ring goes on filling for another. */
  std::atomic<uint32_t> end{0};
  std::atomic<uint32_t> dropped{0};
};
ReaderState s_readers[2];
std::atomic<uint16_t> s_peak{0};

ReaderState& readerState(Reader reader) {
  return s_readers[static_cast<size_t>(reader)];
}

bool anyCapturing() {
  return s_readers[0].on.load(std::memory_order_relaxed) ||
         s_readers[1].on.load(std::memory_order_relaxed);
}

// --- Speaker
StreamBufferHandle_t s_mp3 = nullptr;
TaskHandle_t s_player = nullptr;
uint8_t* s_window = nullptr;
mp3dec_t* s_dec = nullptr;
std::atomic<bool> s_playing{false};
std::atomic<bool> s_ending{false};
std::atomic<bool> s_stopping{false};
QueueHandle_t s_cues = nullptr;
/** The amplifier, and when a sound last went out through it. Under
 *  s_tx_lock. */
bool s_amp_on = false;
unsigned long s_amp_used_ms = 0;

void* psram(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void writeSilence(size_t frames) {
  int16_t zero[2 * 240] = {};
  while (frames > 0) {
    const size_t n = frames < 240 ? frames : 240;
    s_i2s.write(reinterpret_cast<uint8_t*>(zero), n * 4);
    frames -= n;
  }
}

/** The amplifier on, and given time to unmute, unless it is on already.
 *  Under s_tx_lock. */
void speakerOn() {
  if (!s_amp_on) {
    codec::speaker(true);
    writeSilence(kWarmUpFrames);
    s_amp_on = true;
  }
}

/** A sound has gone out: the amplifier stays on for kAmpHoldMs from now.
 *  Under s_tx_lock. */
void speakerUsed() {
  s_amp_used_ms = millis();
}

/** The amplifier off, once what was written has left the DMA, if nothing
 *  has used it for kAmpHoldMs. Under s_tx_lock. */
void speakerOffIfIdle() {
  if (s_amp_on && millis() - s_amp_used_ms >= kAmpHoldMs) {
    writeSilence(kDmaFrames);
    codec::speaker(false);
    s_amp_on = false;
  }
}

/** A tone of `ms` at `hz`, eased in and out over 5 ms so it does not click.
 *  Under s_tx_lock, with the amplifier on. */
void tone(uint16_t hz, uint16_t ms, float amplitude) {
  constexpr int kChunk = 240;
  int16_t buf[2 * kChunk];
  const int frames = kSampleRate * ms / 1000;
  const int ramp = kSampleRate * 5 / 1000 < frames / 2
                       ? kSampleRate * 5 / 1000
                       : frames / 2;
  const float step = 2.0f * static_cast<float>(M_PI) * hz / kSampleRate;
  for (int done = 0; done < frames; done += kChunk) {
    const int n = frames - done < kChunk ? frames - done : kChunk;
    for (int i = 0; i < n; ++i) {
      const int k = done + i;
      const int edge = k < frames - k ? k : frames - k;
      const float env =
          ramp > 0 && edge < ramp ? static_cast<float>(edge) / ramp : 1.0f;
      const int16_t s =
          static_cast<int16_t>(sinf(step * k) * env * amplitude * 32767);
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    s_i2s.write(reinterpret_cast<uint8_t*>(buf), n * 4);
  }
}

void playCue(Cue c) {
  switch (c) {
    case Cue::kPlay:
      tone(660, 60, kCueAmplitude);
      tone(880, 80, kCueAmplitude);
      break;
    case Cue::kPause:
      tone(880, 60, kCueAmplitude);
      tone(660, 80, kCueAmplitude);
      break;
    case Cue::kNext:
      tone(660, 40, kCueAmplitude);
      tone(880, 40, kCueAmplitude);
      tone(1100, 50, kCueAmplitude);
      break;
    case Cue::kPrevious:
      tone(1100, 40, kCueAmplitude);
      tone(880, 40, kCueAmplitude);
      tone(660, 50, kCueAmplitude);
      break;
    case Cue::kTick:
      tone(1500, 12, kCueAmplitude);
      break;
    case Cue::kLimit:
      tone(1500, 12, kCueAmplitude);
      writeSilence(kSampleRate * 40 / 1000);
      tone(1500, 12, kCueAmplitude);
      break;
    case Cue::kFailed:
      // Louder than the others: it answers a request, not a touch, and is
      // heard from across the room.
      tone(440, 120, kCueAmplitude * 4);
      tone(330, 200, kCueAmplitude * 4);
      break;
    case Cue::kWarm:
      break;  // speakerOn() has done it
  }
}

void captureTask(void*) {
  int16_t stereo[2 * kReadFrames];
  for (;;) {
    const size_t got =
        s_i2s.readBytes(reinterpret_cast<char*>(stereo), sizeof(stereo)) / 4;
    if (got == 0) {
      vTaskDelay(1);
      continue;
    }
    if (!anyCapturing()) {
      s_peak.store(0, std::memory_order_relaxed);
      continue;
    }
    uint32_t head = s_head.load(std::memory_order_relaxed);
    int peak = 0;
    for (size_t i = 0; i < got; ++i) {
      int v = stereo[2 * i] * (1 << kMicGainShift);
      v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
      peak = abs(v) > peak ? abs(v) : peak;
      s_ring[head & (kRingSamples - 1)] = static_cast<int16_t>(v);
      ++head;
    }
    s_head.store(head, std::memory_order_release);
    s_peak.store(static_cast<uint16_t>(peak), std::memory_order_relaxed);
  }
}

/** Mono samples at any rate, out to the speaker at kSampleRate on both
 *  channels, by linear interpolation: plenty for speech. */
class Output {
 public:
  explicit Output(int in_hz)
      : step_(static_cast<float>(in_hz) / kSampleRate) {}

  void push(const int16_t* x, int n) {
    while (pos_ < n - 1) {
      const int i = static_cast<int>(floorf(pos_));
      const float f = pos_ - i;
      const float a = i < 0 ? last_ : x[i];
      const float b = x[i + 1];
      const int16_t s = static_cast<int16_t>(a + (b - a) * f);
      buf_[2 * used_] = s;
      buf_[2 * used_ + 1] = s;
      if (++used_ == kFrames) {
        flush();
      }
      pos_ += step_;
    }
    last_ = x[n - 1];
    pos_ -= n;
  }

  void flush() {
    if (used_ > 0) {
      s_i2s.write(reinterpret_cast<uint8_t*>(buf_), used_ * 4);
      written_ += used_;
      used_ = 0;
    }
  }

  size_t written() const { return written_; }

 private:
  /** 16 ms: small, so the first frame is heard as soon as it is decoded. */
  static constexpr int kFrames = 256;
  float step_;
  float pos_ = 0.0f;
  float last_ = 0.0f;
  int16_t buf_[2 * kFrames];
  int used_ = 0;
  size_t written_ = 0;
};

/** Play the reply mp3Begin() started, as it arrives: each frame decoded as
 *  soon as all of it and the next frame's header are in, as minimp3 checks
 *  a frame against the next and, given a partial one, resets and skips. */
void playOne() {
  const uint32_t t_begin = millis();
  uint8_t* win = s_window;
  size_t fill = 0;  // bytes in the window
  size_t off = 0;   // of which decoded or skipped
  size_t skip = 0;  // of an ID3 tag, still to come
  bool id3_checked = false;
  bool need_more = false;
  Output* out = nullptr;
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  mp3dec_frame_info_t info = {};
  int hz = 0;
  int kbps = 0;
  int waits = 0;
  uint32_t t_first_sound = 0;
  uint32_t decode_us = 0;
  // Cleared whole, not just mp3dec_init()'s first byte: its free-format
  // size is read above before minimp3 has set it.
  memset(s_dec, 0, sizeof(*s_dec));

  for (;;) {
    if (s_stopping) {
      break;
    }
    if (off > 0) {
      memmove(win, win + off, fill - off);
      fill -= off;
      off = 0;
    }
    // Read before the receive: if it was set then and the stream is empty
    // after, every byte has been taken.
    const bool ending = s_ending;
    if (fill < kWindowBytes) {
      fill += xStreamBufferReceive(s_mp3, win + fill, kWindowBytes - fill,
                                   need_more ? pdMS_TO_TICKS(10) : 0);
    }
    if (need_more && out != nullptr) {
      ++waits;
    }
    need_more = false;
    const bool all_in = ending && xStreamBufferIsEmpty(s_mp3) == pdTRUE;
    size_t avail = fill - off;

    if (skip > 0) {
      const size_t n = avail < skip ? avail : skip;
      off += n;
      skip -= n;
      if (skip > 0) {
        if (all_in) {
          break;
        }
        need_more = true;
        continue;
      }
      avail = fill - off;
    }
    if (!id3_checked) {
      if (avail < 10 && !all_in) {
        need_more = true;
        continue;
      }
      id3_checked = true;
      // An ID3v2 tag, which Home Assistant's replies begin with: its size
      // is syncsafe, 7 bits a byte.
      const uint8_t* h = win + off;
      if (avail >= 10 && memcmp(h, "ID3", 3) == 0) {
        skip = 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 |
                     (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
        continue;
      }
    }
    if (avail == 0) {
      if (all_in) {
        break;
      }
      need_more = true;
      continue;
    }
    // How much has to be in before the frame at `off` can be decoded. A
    // header it cannot size: the whole window, and let minimp3 search.
    const uint8_t* h = win + off;
    size_t need = kWindowBytes;
    if (avail >= HDR_SIZE && hdr_valid(h)) {
      const int fb = hdr_frame_bytes(h, s_dec->free_format_bytes);
      if (fb > 0) {
        need = fb + hdr_padding(h) + HDR_SIZE;
      }
    }
    if (avail < need && !all_in) {
      need_more = true;
      continue;
    }
    const uint32_t d0 = micros();
    const int n = mp3dec_decode_frame(s_dec, h, static_cast<int>(avail), pcm,
                                      &info);
    decode_us += micros() - d0;
    if (info.frame_bytes == 0) {
      // A frame at the start of the window that is not all there.
      if (all_in) {
        break;
      }
      if (avail >= kWindowBytes) {
        off += 1;  // and never will be: step past its header
      }
      need_more = true;
      continue;
    }
    off += info.frame_bytes;
    if (n == 0) {
      continue;  // skipped data, or a frame that leans on a missing reservoir
    }
    if (out == nullptr) {
      out = new Output(info.hz);
      hz = info.hz;
      kbps = info.bitrate_kbps;
    }
    if (info.channels == 2) {
      for (int i = 0; i < n; ++i) {
        pcm[i] = static_cast<int16_t>((pcm[2 * i] + pcm[2 * i + 1]) / 2);
      }
    }
    out->push(pcm, n);
    if (t_first_sound == 0) {
      out->flush();  // the first frame out now, not when the buffer fills
      t_first_sound = millis();
    }
  }

  if (out != nullptr) {
    out->flush();
    LOG_INFO("AUDIO: reply, %d Hz MP3 at %d kbps: %lu ms of sound, first "
             "%lu ms after it began; decoding took %lu ms; waited for data "
             "%d times%s",
             hz, kbps,
             static_cast<unsigned long>(out->written() * 1000 / kSampleRate),
             static_cast<unsigned long>(t_first_sound - t_begin),
             static_cast<unsigned long>(decode_us / 1000), waits,
             s_stopping ? "; stopped" : "");
    delete out;
  } else if (!s_stopping) {
    LOG_WARN("AUDIO: reply had no MP3 in it");
  }
}

/** A reply when mp3Begin() says so, the cues as they come, and the
 *  amplifier off once it has been idle for kAmpHoldMs. */
void playerTask(void*) {
  for (;;) {
    const uint32_t reply = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (reply > 0 && s_playing) {
      xSemaphoreTake(s_tx_lock, portMAX_DELAY);
      speakerOn();
      playOne();
      speakerUsed();
      xSemaphoreGive(s_tx_lock);
      s_playing = false;
    }
    Cue c;
    while (xQueueReceive(s_cues, &c, 0) == pdTRUE) {
      if (s_playing) {
        continue;  // a reply is on its way: no cue across it
      }
      xSemaphoreTake(s_tx_lock, portMAX_DELAY);
      speakerOn();
      playCue(c);
      speakerUsed();
      xSemaphoreGive(s_tx_lock);
    }
    // Not waited for: beep() holds the lock for half a second, and this
    // can be done on the next pass.
    if (xSemaphoreTake(s_tx_lock, 0) == pdTRUE) {
      speakerOffIfIdle();
      xSemaphoreGive(s_tx_lock);
    }
  }
}

}  // namespace

bool begin() {
  if (s_ready) {
    return true;
  }
  if (s_tried) {
    return false;
  }
  s_tried = true;

  s_ring = static_cast<int16_t*>(psram(kRingSamples * sizeof(int16_t)));
  s_window = static_cast<uint8_t*>(psram(kWindowBytes));
  s_dec = static_cast<mp3dec_t*>(psram(sizeof(mp3dec_t)));
  s_mp3 = xStreamBufferCreateWithCaps(kMp3BufferBytes, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_tx_lock = xSemaphoreCreateMutex();
  s_cues = xQueueCreate(8, sizeof(Cue));
  if (s_ring == nullptr || s_window == nullptr || s_dec == nullptr ||
      s_mp3 == nullptr || s_tx_lock == nullptr || s_cues == nullptr) {
    LOG_WARN("AUDIO: no memory for the buffers");
    return false;
  }

  s_i2s.setPins(board::kAudioPinBclk, board::kAudioPinLrck,
                board::kAudioPinDout, board::kAudioPinDin,
                board::kAudioPinMclk);
  if (!s_i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO)) {
    LOG_WARN("AUDIO: I2S would not start");
    return false;
  }
  delay(10);  // MCLK running before the codecs are spoken to
  if (!codec::init(kDefaultVolume)) {
    s_i2s.end();
    return false;
  }

  // The capture task on the loop's core, above it, so a long draw does not
  // let the receive DMA overrun; the player beside the network.
  TaskHandle_t capture = nullptr;
  xTaskCreatePinnedToCoreWithCaps(captureTask, "audio_in", 6144, nullptr, 5,
                                  &capture, 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  xTaskCreatePinnedToCoreWithCaps(playerTask, "audio_out", 28672, nullptr, 3,
                                  &s_player, 0,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (capture == nullptr || s_player == nullptr) {
    LOG_WARN("AUDIO: tasks would not start");
    return false;
  }
  s_ready = true;
  LOG_INFO("AUDIO: ready, %lu Hz", static_cast<unsigned long>(kSampleRate));
  return true;
}

bool ready() {
  return s_ready;
}

bool captureStart(Reader reader) {
  return captureStartAt(reader, s_head.load(std::memory_order_acquire));
}

bool captureStartAt(Reader reader, uint32_t position) {
  if (!s_ready) {
    return false;
  }
  ReaderState& r = readerState(reader);
  r.dropped = 0;
  r.tail.store(position, std::memory_order_release);
  r.on = true;
  return true;
}

float levelBefore(uint32_t position, uint32_t samples) {
  if (!s_ready) {
    return -96.0f;
  }
  // Only what the ring still holds, short of the block being written.
  const uint32_t held = kRingSamples - kReadFrames;
  const uint32_t ago = s_head.load(std::memory_order_acquire) - position;
  if (ago > held) {
    return -96.0f;
  }
  if (samples > held - ago) {
    samples = held - ago;
  }
  if (samples == 0) {
    return -96.0f;
  }
  double sum = 0.0;
  for (uint32_t i = 0; i < samples; ++i) {
    const double v = s_ring[(position - samples + i) & (kRingSamples - 1)];
    sum += v * v;
  }
  const double rms = sqrt(sum / samples);
  return rms < 1.0 ? -96.0f
                   : static_cast<float>(20.0 * log10(rms / 32768.0));
}

void captureStop(Reader reader) {
  ReaderState& r = readerState(reader);
  if (r.on) {
    r.end.store(s_head.load(std::memory_order_acquire),
                std::memory_order_release);
    r.on = false;
  }
}

bool capturing(Reader reader) {
  return readerState(reader).on;
}

size_t captureRead(Reader reader, int16_t* out, size_t max) {
  if (!s_ready) {
    return 0;
  }
  ReaderState& r = readerState(reader);
  uint32_t tail = r.tail.load(std::memory_order_relaxed);
  // A stopped reader reads to where it stopped; a live one to the latest.
  const bool on = r.on.load(std::memory_order_acquire);
  const uint32_t head = on ? s_head.load(std::memory_order_acquire)
                           : r.end.load(std::memory_order_acquire);
  if (!on && head - tail > kRingSamples) {
    return 0;  // past its stop, which a stopped reader cannot be
  }
  // Nearly a whole ring behind: the oldest of it is being written over.
  // Skip to a block's room short of that, so nothing is read as it is
  // written.
  if (head - tail > kRingSamples - kReadFrames) {
    const uint32_t skip = head - tail - (kRingSamples - kReadFrames);
    r.dropped.fetch_add(skip, std::memory_order_relaxed);
    tail += skip;
  }
  const size_t n = (head - tail) < max ? (head - tail) : max;
  for (size_t i = 0; i < n; ++i) {
    out[i] = s_ring[(tail + i) & (kRingSamples - 1)];
  }
  r.tail.store(tail + n, std::memory_order_release);
  return n;
}

uint32_t capturePosition(Reader reader) {
  return readerState(reader).tail.load(std::memory_order_acquire);
}

uint32_t captureDropped(Reader reader) {
  return readerState(reader).dropped;
}

uint16_t inputPeak() {
  return s_peak;
}

void setVolume(int percent) {
  if (s_ready) {
    codec::setVolume(percent);
  }
}

void beep(uint16_t hz, uint16_t ms) {
  if (!s_ready) {
    return;
  }
  xSemaphoreTake(s_tx_lock, portMAX_DELAY);
  speakerOn();
  tone(hz, ms, kBeepAmplitude);
  // Out of the DMA before this returns: a request starts listening next,
  // and should not hear the end of its own beep.
  writeSilence(kDmaFrames);
  speakerUsed();
  xSemaphoreGive(s_tx_lock);
}

void cue(Cue c) {
  if (s_ready && !s_playing) {
    xQueueSend(s_cues, &c, 0);
  }
}

bool mp3Begin() {
  if (!s_ready || s_playing) {
    return false;
  }
  // Nothing waits on the stream now: the player is between replies.
  xStreamBufferReset(s_mp3);
  s_ending = false;
  s_stopping = false;
  s_playing = true;
  xTaskNotifyGive(s_player);
  return true;
}

size_t mp3Write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
  size_t done = 0;
  const uint32_t start = millis();
  while (done < len && s_playing && !s_stopping) {
    done += xStreamBufferSend(s_mp3, data + done, len - done,
                              pdMS_TO_TICKS(20));
    if (millis() - start >= timeout_ms) {
      break;
    }
  }
  return done;
}

void mp3End() {
  s_ending = true;
}

void stop() {
  if (!s_playing) {
    return;
  }
  s_stopping = true;
  for (int i = 0; i < 50 && s_playing; ++i) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool playing() {
  return s_playing;
}

}  // namespace hw::audio

#else  // no microphone or speaker on this board, or no voice in this build

namespace hw::audio {

bool begin() { return false; }
bool ready() { return false; }
bool captureStart(Reader) { return false; }
bool captureStartAt(Reader, uint32_t) { return false; }
float levelBefore(uint32_t, uint32_t) { return -96.0f; }
void captureStop(Reader) {}
bool capturing(Reader) { return false; }
size_t captureRead(Reader, int16_t*, size_t) { return 0; }
uint32_t capturePosition(Reader) { return 0; }
uint32_t captureDropped(Reader) { return 0; }
uint16_t inputPeak() { return 0; }
void setVolume(int) {}
void beep(uint16_t, uint16_t) {}
void cue(Cue) {}
bool mp3Begin() { return false; }
size_t mp3Write(const uint8_t*, size_t, uint32_t) { return 0; }
void mp3End() {}
void stop() {}
bool playing() { return false; }

}  // namespace hw::audio

#endif
