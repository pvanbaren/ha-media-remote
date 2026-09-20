#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace services::ha {

/** One media_player entity as offered in the portal dropdown. */
struct PlayerEntry {
  char entity_id[config::kEntityIdMaxLen] = {};
  char name[config::kFriendlyNameMaxLen] = {};
  bool available = false;
};

enum class PlaybackState : uint8_t {
  kUnknown,
  kPlaying,
  kPaused,
  kIdle,
  kOff,
  kUnavailable,
};

/** Snapshot of the selected player, as rendered by the now-playing screen. */
struct PlayerState {
  PlaybackState playback = PlaybackState::kUnknown;
  char name[config::kFriendlyNameMaxLen] = {};
  char title[96] = {};
  /** Artist, falling back to album then app name — whatever HA has. */
  char subtitle[64] = {};
  /** entity_picture: a HA-relative path, or an absolute URL from some
   *  integrations. Empty when the player has no art. */
  char picture[192] = {};
  uint32_t supported_features = 0;
  /** supported_features of the *control* entity -- the same number when
   *  nothing separate is configured. What the volume slider tests, since it
   *  is the control entity the slider drives. */
  uint32_t control_features = 0;
  /** 0..1, or negative when the volume source reports no volume_level.
   *  Read from the control entity, which is the one that makes the room
   *  louder when the two differ. */
  float volume = -1.0f;
  bool muted = false;
  /** Control entity is off or in standby, so a tap should power it on. */
  bool control_off = false;
  /** The control entity's source that is this player, by the player's own
   *  name -- empty when it lists no such source, which is anything that is
   *  not an input-switching amplifier. */
  char player_source[64] = {};
  /** The source the control entity is on now; empty when none. */
  char control_source[64] = {};
  /** Set by the app, not the server: there has been no title for less than
   *  config::kUntitledLabelDelayMs, so the screen leaves the title area blank
   *  instead of labelling a gap between tracks as nothing playing. */
  bool hold_label = false;
  /** Seconds. duration <= 0 means the player reports no progress. */
  float duration_s = 0.0f;
  /** Position already advanced by the age of HA's last position update. */
  float position_s = 0.0f;
  /** millis() when this snapshot was taken, for local progress interpolation. */
  unsigned long sampled_ms = 0;
};

// MediaPlayerEntityFeature bits used here (homeassistant/components/media_player).
constexpr uint32_t kFeaturePause = 1u << 0;
constexpr uint32_t kFeatureVolumeSet = 1u << 2;
constexpr uint32_t kFeatureVolumeMute = 1u << 3;
constexpr uint32_t kFeaturePreviousTrack = 1u << 4;
constexpr uint32_t kFeatureNextTrack = 1u << 5;
constexpr uint32_t kFeatureTurnOn = 1u << 7;
constexpr uint32_t kFeatureTurnOff = 1u << 8;
constexpr uint32_t kFeatureSelectSource = 1u << 11;
constexpr uint32_t kFeaturePlay = 1u << 14;

/** Load base URL / token / last selection from NVS. Call once in setup(). */
void init();

/** True once a base URL and a token are both stored. */
bool configured();
const char* baseUrl();
/** True when the base URL is https, i.e. every request pays for a TLS
 *  session. Callers budgeting heap need to know. */
bool usesTls();

/** Persist portal input. Either may be empty to leave that field untouched. */
void saveCredentials(const char* base_url, const char* token);

/** Long-lived token, for callers that have to set their own
 *  Authorization header (cover art fetches the image URL directly).
 *  Empty string when none is stored. */
const char* token();

/** Current values, for pre-filling the portal fields. */
const char* storedBaseUrl();
bool hasStoredToken();
void clearCredentials();

/** entity_id of the player being controlled; empty until one is chosen. */
const char* selectedEntity();
void selectEntity(const char* entity_id);

/** The half of an entity_id worth showing someone: everything past the dot.
 *  "media_player." is thirteen characters that say nothing on a list of media
 *  players, and rather more than that on a 240 px circle. Returns a pointer
 *  into `entity_id` -- no copy, and it lives exactly as long as that does.
 *  An id with no dot comes back whole. */
const char* entityLabel(const char* entity_id);

/** entity_id that carries volume and power.
 *
 *  Usually the same thing that plays, and then this just returns
 *  selectedEntity(). They come apart on the setup worth having the setting
 *  for: a Music Assistant player streaming into an amplifier or receiver,
 *  where the player's own volume_level is missing or a software gain nobody
 *  wants, the real knob belongs to the receiver, and the receiver is also the
 *  thing that has to be switched on before any of it makes a sound. */
const char* controlEntity();
/** The raw stored value, where empty means "follow the player". The portal
 *  needs to tell that apart from a deliberate choice of the same entity. */
const char* storedControlEntity();
/** True when a different entity carries volume and power. */
bool controlIsSeparate();
/** Copies of the player and of controlEntity(), safe from any task. The
 *  portal rewrites both from the Arduino loop, so a task reading the live
 *  buffers can catch one half-written -- and a half-written entity id can
 *  still be a valid one, just of the wrong entity. */
void copySelectedEntity(char* out, size_t out_len);
void copyControlEntity(char* out, size_t out_len);
/** Empty string is a real choice here: it stores "follow the player". */
void selectControlEntity(const char* entity_id);

/** Fetch every media_player entity, sorted by friendly name.
 *  Writes at most config::kMaxPlayers entries; returns the count, or -1 on a
 *  transport/auth failure (leaving `out` untouched). */
int fetchPlayers(PlayerEntry* out, size_t capacity);

/** Fetch the selected player's now-playing snapshot. False on failure. */
bool fetchState(const char* entity_id, PlayerState& out);

/** POST /api/services/media_player/<service> for one entity. */
bool callService(const char* service, const char* entity_id);

/** media_player.select_source for one entity. */
bool selectSource(const char* entity_id, const char* source);

/** media_player.volume_set. `level` is clamped to 0..1. */
bool setVolume(const char* entity_id, float level);

/** Progress interpolated from the snapshot to now, clamped to duration.
 *  Returns -1 when the player reports no usable duration. */
float interpolatedPosition(const PlayerState& state);

/** Human-readable reason the last request failed, for the error screen. */
const char* lastError();

/** Log one outbound HTTP request: at debug when it worked, at warn when it
 *  did not (no answer, or a 4xx/5xx). Shared rather than private so the cover
 *  art fetches read the same as the API calls in the console. Pass -1 for a
 *  byte count that is not known.  */
void logHttpRequest(const char* method, const char* url, int status,
                    int request_bytes, int response_bytes,
                    unsigned long elapsed_ms);

}  // namespace services::ha
