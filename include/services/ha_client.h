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

/** One Music Assistant library item, as shown in the swipe-up list. */
struct LibraryItem {
  /** Music Assistant URI, e.g. "library://artist/79". What play_media takes. */
  char uri[96] = {};
  char name[config::kFriendlyNameMaxLen] = {};
  /** Absolute image URL, on whatever CDN the provider uses. May be empty. */
  char image[160] = {};
  /** "artist", "album", "playlist", "radio", "track". */
  char media_type[12] = {};
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
  /** The control entity's input that carries this player: the one chosen
   *  in the portal, or one named after the player -- empty when neither,
   *  which is anything that is not an input-switching amplifier. */
  char player_source[64] = {};
  /** The source the control entity is on now; empty when none. */
  char control_source[64] = {};
  /** media_artist alone -- none of the subtitle's fallbacks to album or app
   *  name. Empty when the player reports no artist. */
  char artist[64] = {};
  /** media_content_id: for a Music Assistant player, the track's URI. */
  char track[96] = {};
  /** When the room is hearing something other than the player -- the
   *  volume/power entity is on another input -- the entity it is hearing:
   *  the player that input is named after, or the volume device itself.
   *  Everything above about what is playing (state, title, art, progress,
   *  features, artist) is that entity's, and play/pause and skip go to it.
   *  Empty when it is the player. */
  char media_entity[config::kEntityIdMaxLen] = {};
  /** The friendly names of the player and of the volume/power entity -- the
   *  configured ones, whatever the room is hearing -- for the status page. */
  char player_name[config::kFriendlyNameMaxLen] = {};
  char control_name[config::kFriendlyNameMaxLen] = {};
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

/** Music Assistant config entry id, for the library calls. Empty when none is
 *  stored, which simply means no station list. */
const char* maConfigEntry();
void saveMaConfigEntry(const char* entry_id);
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
/** The player's input on the volume/power entity, as chosen in the portal;
 *  empty means the input named after the player, where there is one. */
const char* storedControlInput();
void selectControlInput(const char* input);
/** The player's own volume, 0..1, set when the remote switches a separate
 *  volume and power entity on; negative when that is off. What the portal
 *  stored, or config::kPlayerWakeVolume until it has stored one. The portal
 *  calls it Player volume at switch-on; "wake" here is the amplifier's. */
float playerWakeVolume();
/** The same as a whole percent for the portal: 0..100, or -1 for off. */
int playerWakeVolumePercent();
/** Store `percent`, 0..100, or -1 to turn the pin off. */
void savePlayerWakeVolumePercent(int percent);
/** Every input `entity_id` lists (source_list), for the portal. Returns the
 *  count, or -1 on failure. */
int fetchSources(const char* entity_id,
                 char (*out)[config::kSourceNameMaxLen], int capacity);

/** Fetch every media_player entity, sorted by friendly name.
 *  Writes at most config::kMaxPlayers entries; returns the count, or -1 on a
 *  transport/auth failure (leaving `out` untouched). */
int fetchPlayers(PlayerEntry* out, size_t capacity);

/** Fetch the selected player's now-playing snapshot. False on failure. */
bool fetchState(const char* entity_id, PlayerState& out);

/**
 * Live state over Home Assistant's WebSocket API, instead of polling.
 *
 * The stream renders the same template fetchState() posts, but as a
 * subscription: Home Assistant tracks every entity the template reads and
 * sends a fresh rendering whenever one of them changes -- the player, and the
 * volume/power entity where that is a different one. So a volume change or a
 * track starting arrives in a round trip rather than at the next poll, and
 * nothing is asked while nothing happens. The template reads now(), which
 * re-renders it once a minute as well; that keeps the position honest and
 * doubles as proof the connection is alive.
 *
 * Service calls ride the same socket while it is open (callService,
 * setVolume, playMedia, and the library and search, which ask for the
 * service's response), so steady state is one connection and one TLS
 * session. Only the player list stays on REST.
 *
 * One task owns the stream: the one that calls streamOpen() and
 * streamService(). Any task may call the services above; from elsewhere they
 * are handed to the owner, and fall back to REST if it cannot take them
 * promptly.
 */
/** Connect, authenticate and subscribe to `entity_id`'s state. False on any
 *  failure, with the reason in lastError(). */
bool streamOpen(const char* entity_id);
/** From any task. */
bool streamIsOpen();
/** Service the stream for up to `wait_ms`: answer pings, run services handed
 *  over by other tasks, and read state. True when `out` holds a fresh state.
 *  Closes the stream when it has died, or when the settings it was opened
 *  against have changed -- check streamIsOpen() after a false. */
bool streamService(PlayerState& out, uint32_t wait_ms);
void streamClose();

/** Fetch one slice of the Music Assistant library, via
 *  `music_assistant.get_library?return_response`.
 *
 *  `media_type` is "artist", "album", "playlist", "radio" or "track";
 *  `order_by` is one of the integration's sort keys ("last_played_desc",
 *  "random", "play_count_desc", "name", ...) or nullptr for its default.
 *  Writes at most `capacity` entries; returns the count, or -1 on failure
 *  (including "no config entry id stored", which is the ordinary un-set-up
 *  case rather than an error).
 *
 *  Unlike everything else here this parses JSON, because a service response is
 *  JSON and there is no template that can call a service. It is parsed by hand
 *  rather than with a library, by walking each item's own members and stepping
 *  over every value whole -- see readLibraryItem in the implementation. The
 *  walk is not optional: an album nests an "artists" array whose objects carry
 *  their own "name" and "image", so a flat scan reads the wrong ones. */
int fetchLibrary(const char* media_type, const char* order_by, int limit,
                 LibraryItem* out, size_t capacity);

/** Search Music Assistant for artists by name, via `music_assistant.search`.
 *
 *  Reaches past the local library into whatever providers are configured, and
 *  is forgiving about partial names -- "radioh" finds Radiohead -- which is
 *  what makes typing on a 240 px circle worth doing at all. Returns the count,
 *  or -1 on failure.
 *
 *  Same flat item shape as fetchLibrary(), under an "artists" key rather than
 *  "items", so it shares the parser. */
int searchArtists(const char* name, LibraryItem* out, size_t capacity);

/** Play a library item on `entity_id`, via `music_assistant.play_media`.
 *  `uri` and `media_type` come from a LibraryItem.
 *
 *  `radio_mode` asks Music Assistant to seed an endless queue from the item
 *  rather than play just it -- an artist becomes artist radio. */
bool playMedia(const char* entity_id, const char* uri, const char* media_type,
               bool radio_mode = false);

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
