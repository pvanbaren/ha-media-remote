#include "ui/ui.h"

#include <Arduino.h>

#include <cmath>
#include <cstdlib>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display.h"
#include "hardware/touch.h"
#include "services/browse.h"
#include "services/search.h"
#include "ui/artwork.h"
#include "ui/browse_list.h"
#include "ui/canvas.h"
#include "ui/cover_art.h"
#include "ui/now_playing.h"
#include "ui/player_pick.h"
#include "ui/search.h"
#include "ui/status_screens.h"
#include "ui/theme.h"
#include "ui/wifi_join.h"

/**
 * The round panel's half of ui/ui.h.
 *
 * Everything here is about a circle: where the transport buttons sit, that a
 * volume swipe runs sideways across the top because the arc follows the
 * bezel, that a list is clipped to the chord at its own y. A square panel
 * would answer the same calls differently and src/app/ would not notice.
 */
namespace ui {
namespace {

using services::ha::PlayerState;
using Action = now_playing::Action;

// --- Volume drag -----------------------------------------------------------
/** The finger owns the level while it is down. */
bool s_vol_dragging = false;
float s_vol_level = 0.0f;
float s_vol_drawn = -1.0f;
int s_vol_last_x = 0;
unsigned long s_vol_sent_ms = 0;
/** Snapshot taken when the slider was grabbed, reused for the whole drag:
 *  mute and the feature bits cannot change under a finger. */
PlayerState s_drag_state;

/** Smallest level change worth a repaint: one step of the 252 degree arc that
 *  moves the knob about a pixel at the 240 px baseline. */
constexpr float kVolumeRedrawEpsilon = 0.004f;

// --- List scrolling --------------------------------------------------------
/** One set of state: the browse list and the search results are never on
 *  screen at the same time. */
bool s_list_dragging = false;
int s_list_last_y = 0;
/** Display px per millisecond, signed with the finger. */
float s_list_velocity = 0.0f;
unsigned long s_list_move_ms = 0;
unsigned long s_list_glide_ms = 0;
/** When the list last moved, by drag or by glide, for kListTapGuardMs. */
unsigned long s_list_moved_ms = 0;
/** When the list was last actually repainted, for board::kListRedrawMinMs. */
unsigned long s_list_draw_ms = 0;
/** The finger has moved the list somewhere the glass has not caught up with
 *  yet. Held until the ration allows a repaint, and flushed on release so the
 *  list never settles a few pixels from where it was left. */
bool s_list_redraw_pending = false;

/** What a scrollable screen offers the drag handler. Function pointers rather
 *  than virtuals: there are two, both known at compile time, and neither
 *  wants a vtable. */
struct ScrollableList {
  bool (*scrollByPx)(int delta);
  bool (*atScrollLimit)(int direction);
  void (*redraw)();
};

// redrawRows() rather than draw(): a drag only moves the rows and the
// indicator, and repainting the rest costs more than the scroll can afford.
constexpr ScrollableList kBrowseList{browse_list::scrollByPx,
                                     browse_list::atScrollLimit,
                                     browse_list::redrawRows};
constexpr ScrollableList kSearchResults{search::scrollByPx,
                                        search::atScrollLimit,
                                        search::redrawResults};
constexpr ScrollableList kWifiNetworks{wifi_join::scrollByPx,
                                       wifi_join::atScrollLimit,
                                       wifi_join::draw};
constexpr ScrollableList kPlayerList{player_pick::scrollByPx,
                                     player_pick::atScrollLimit,
                                     player_pick::draw};

constexpr int kDragSlopPx =
    static_cast<int>(board::kTouchTapSlopPx * board::kUiScale + 0.5f);

/** Drive the volume arc from a horizontal swipe across the top of the panel.
 *
 *  The touch driver only reports gestures once a press completes, which is no
 *  use to a slider, so this tracks the live position itself and cancels the
 *  press so its release is not also read as a swipe.
 *
 *  The press may start anywhere in the top half rather than on the 7 px
 *  track, because landing a fingertip on that track is a precision task on a
 *  1.28" circle and this is the control most used without looking. And the
 *  movement is *relative*: the level shifts by however far the finger went,
 *  so a swipe adjusts from wherever the volume already was instead of jumping
 *  it to whatever angle the finger points at.
 *
 *  Only a predominantly horizontal press is claimed. A vertical one is left
 *  alone so swipe-up still opens the browse list.
 *
 *  Returns true when the caller should be told the level changed. */
bool driveVolume(Screen screen, const PlayerState* state, Input& out) {
  if (screen != Screen::kNowPlaying) {
    s_vol_dragging = false;
    return false;
  }

  if (!hw::touchIsDown()) {
    if (!s_vol_dragging) {
      return false;
    }
    // Always close with an authoritative set: the throttle below may have
    // skipped the last position the finger actually rested on. The app is
    // told before the flag drops, because between the two there is no guard
    // on the volume at all and a poll landing in that window is exactly the
    // one carrying the pre-change level.
    s_vol_dragging = false;
    out.intent = Intent::kSetVolume;
    out.level = s_vol_level;
    return true;
  }

  int x = 0;
  int y = 0;
  hw::touchPosition(x, y);

  if (!s_vol_dragging) {
    const int travel_x = hw::touchDragDx();
    const int travel_y = hw::touchDragDy();
    if (abs(travel_x) < kDragSlopPx || abs(travel_x) <= abs(travel_y)) {
      return false;  // not committed sideways yet, or it is a vertical swipe
    }
    // Where the finger landed, not where it is now: a swipe that began in the
    // top half stays a volume swipe even once it has wandered below centre.
    if (!now_playing::volumeSwipeRegion(x - travel_x, y - travel_y)) {
      return false;
    }
    if (state == nullptr || !now_playing::volumeAvailable(*state)) {
      return false;
    }

    s_drag_state = *state;
    hw::touchCancel();
    s_vol_dragging = true;
    s_vol_sent_ms = 0;
    // Start from where the volume actually is. The slop travelled getting
    // here is deliberately not applied, so the arc does not jump on grab.
    s_vol_level = state->volume;
    s_vol_drawn = -1.0f;
    s_vol_last_x = x;
  } else {
    const int moved = x - s_vol_last_x;
    s_vol_last_x = x;
    s_vol_level += moved * now_playing::volumeLevelPerPx();
    if (s_vol_level < 0.0f) {
      s_vol_level = 0.0f;
    } else if (s_vol_level > 1.0f) {
      s_vol_level = 1.0f;
    }
  }

  // This runs on every loop pass, far faster than the panel or the eye needs.
  // Repaint only when the knob would actually move by a pixel's worth.
  if (fabsf(s_vol_level - s_vol_drawn) >= kVolumeRedrawEpsilon) {
    s_vol_drawn = s_vol_level;
    s_drag_state.volume = s_vol_level;
    now_playing::refreshVolume(s_drag_state, s_vol_level);
  }

  if (millis() - s_vol_sent_ms >= config::kVolumeSendIntervalMs) {
    s_vol_sent_ms = millis();
    out.intent = Intent::kSetVolume;
    out.level = s_vol_level;
    return true;
  }
  return false;
}

/** Scroll a list under the finger, and let it glide when released.
 *
 *  Only a predominantly *vertical* press is taken. A horizontal one is left
 *  alone so it can still resolve as the swipe-right that leaves the list. */
void driveList(const ScrollableList& list, bool active) {
  /** Below this the glide is over; above it, a flick keeps going. px/ms. */
  constexpr float kGlideStopSpeed = 0.02f;
  /** Fraction of speed kept per frame. Lower stops sooner. */
  constexpr float kGlideDecay = 0.88f;
  constexpr unsigned long kGlideFrameMs = 16;

  if (!active) {
    s_list_dragging = false;
    s_list_velocity = 0.0f;
    return;
  }

  if (hw::touchIsDown()) {
    int x = 0;
    int y = 0;
    hw::touchPosition(x, y);

    if (!s_list_dragging) {
      // A finger on a gliding list catches it. Only stops it: the press that
      // did so is refused as a tap (see listTapAllowed()), and leaving the
      // velocity set would let the glide carry on once it lifts.
      if (s_list_velocity != 0.0f) {
        s_list_velocity = 0.0f;
        s_list_moved_ms = millis();
      }
      const int travel_x = hw::touchDragDx();
      const int travel_y = hw::touchDragDy();
      if (abs(travel_y) < kDragSlopPx || abs(travel_y) <= abs(travel_x)) {
        return;  // not committed vertically yet, or it is a sideways swipe
      }
      hw::touchCancel();
      s_list_dragging = true;
      s_list_velocity = 0.0f;
      s_list_last_y = y;
      s_list_move_ms = millis();
      return;
    }

    const int dy = y - s_list_last_y;
    if (dy == 0) {
      return;
    }
    const unsigned long now = millis();
    const unsigned long dt = now - s_list_move_ms;
    if (dt > 0) {
      // Smoothed, so one jittery sample cannot fling the list.
      const float sample = static_cast<float>(dy) / static_cast<float>(dt);
      s_list_velocity = s_list_velocity * 0.6f + sample * 0.4f;
    }
    s_list_last_y = y;
    s_list_move_ms = now;

    // Dragging down moves the content down, which is scrolling *up* the list.
    // Scrolled on every sample so the position follows the finger exactly;
    // repainted only as often as the board can finish one, because asking for
    // repaints faster than that does not make the list smoother -- it queues
    // them, and on a panel scanned continuously out of PSRAM it starves the
    // scan-out of the bus and breaks up the picture instead.
    if (list.scrollByPx(-dy)) {
      s_list_moved_ms = now;
      s_list_redraw_pending = true;
    }
    if (s_list_redraw_pending &&
        now - s_list_draw_ms >= board::kListRedrawMinMs) {
      s_list_draw_ms = now;
      s_list_redraw_pending = false;
      list.redraw();
    }
    return;
  }

  if (s_list_dragging) {
    s_list_dragging = false;
    s_list_glide_ms = millis();
    // Whatever the ration held back, now that there is no finger to keep up
    // with -- otherwise the list rests wherever the last repaint happened to
    // land rather than where it was released.
    if (s_list_redraw_pending) {
      s_list_redraw_pending = false;
      s_list_draw_ms = millis();
      list.redraw();
    }
    // A finger that stopped before lifting meant to stop.
    if (millis() - s_list_move_ms > 120) {
      s_list_velocity = 0.0f;
    }
  }

  if (s_list_velocity == 0.0f) {
    return;
  }
  // The glide is rationed by the same ceiling as the drag: it repaints the
  // whole list per step, so on a board where that costs more than a frame it
  // would starve the scan-out exactly as a drag does.
  constexpr unsigned long kFrameMs = board::kListRedrawMinMs > kGlideFrameMs
                                         ? board::kListRedrawMinMs
                                         : kGlideFrameMs;
  const unsigned long now = millis();
  const unsigned long elapsed = now - s_list_glide_ms;
  if (elapsed < kFrameMs) {
    return;
  }
  s_list_glide_ms = now;

  // Distance and decay both taken from the time that actually passed rather
  // than from a nominal frame, so a slower repaint makes the glide coarser
  // without also making it travel further or last longer.
  //
  // Capped, though, because that reasoning holds for a slow repaint and not
  // for a stall. Nothing here bounds `elapsed`, and the loop can lose most of
  // a second to a blocking fetch; at a typical fling speed that multiplied out
  // to a single step of more than a screen height, after which the decay below
  // -- 0.88 to the power of the same number -- killed the velocity outright.
  // The list teleported and stopped dead. Half a screen is the most a step may
  // cover, so a fling always lands somewhere you watched it go.
  constexpr int kMaxGlideStepPx = board::kDisplayHeight / 2;
  int step = static_cast<int>(-s_list_velocity * elapsed);
  if (step > kMaxGlideStepPx) {
    step = kMaxGlideStepPx;
  } else if (step < -kMaxGlideStepPx) {
    step = -kMaxGlideStepPx;
  }
  if (step != 0 && list.scrollByPx(step)) {
    s_list_moved_ms = now;
    s_list_draw_ms = now;
    list.redraw();
  } else if (list.atScrollLimit(step)) {
    s_list_velocity = 0.0f;
    return;
  }

  s_list_velocity *=
      powf(kGlideDecay, static_cast<float>(elapsed) / kGlideFrameMs);
  if (fabsf(s_list_velocity) < kGlideStopSpeed) {
    s_list_velocity = 0.0f;
  }
}

void driveListScrolling(Screen screen) {
  if (screen == Screen::kBrowse) {
    driveList(kBrowseList, true);
  } else if (screen == Screen::kSearch) {
    // Only the results scroll; the keyboard has nothing to move, and
    // scrollByPx() says so.
    driveList(kSearchResults, search::showingResults());
  } else if (screen == Screen::kWifi) {
    driveList(kWifiNetworks, wifi_join::onList());
  } else if (screen == Screen::kPlayers) {
    driveList(kPlayerList, true);
  } else {
    driveList(kBrowseList, false);
  }
}

/** Whether a tap on a scrolling list is a tap, rather than a press that
 *  caught a moving list or a swipe that was never seen to move. */
bool listTapAllowed(const hw::TouchReport& report) {
  if (report.unwatched_ms > config::kTouchWatchGapMs) {
    LOG_DEBUG("UI: list tap ignored, unwatched for %lu ms",
                  report.unwatched_ms);
    return false;
  }
  if (s_list_moved_ms != 0 &&
      static_cast<long>(report.down_ms -
                        (s_list_moved_ms + config::kListTapGuardMs)) < 0) {
    LOG_DEBUG("UI: list tap ignored, the list was moving");
    return false;
  }
  return true;
}

Input browseTouch(const hw::TouchReport& report) {
  Input out;
  if (report.event == hw::TouchEvent::kTap && !listTapAllowed(report)) {
    return out;
  }
  switch (report.event) {
    case hw::TouchEvent::kTap: {
      int row = -1;
      const auto result = browse_list::handleTap(report.x, report.y, row);
      if (result == browse_list::Result::kSelected) {
        // Acknowledge first. play_media on an artist has taken four seconds
        // and more on a live instance, and a screen that simply stops for
        // that long reads as a device that missed the tap.
        browse_list::showStarting(row);
        out.intent = Intent::kPlayBrowseRow;
        // Resolved here: a row is a layout idea, an item is not.
        out.index = browse_list::itemForRow(row);
      } else if (result == browse_list::Result::kDismissed) {
        out.intent = Intent::kBackToNowPlaying;
      }
      break;
    }
    case hw::TouchEvent::kSwipeLeft:
      out.intent = Intent::kOpenSearch;
      break;
    // Vertical swipes never arrive here: driveList() claims the press as soon
    // as it commits to an axis and cancels the gesture, so the list has
    // already moved with the finger.
    case hw::TouchEvent::kSwipeRight:
      out.intent = Intent::kBackToNowPlaying;
      break;
    default:
      break;
  }
  return out;
}

Input searchTouch(const hw::TouchReport& report) {
  Input out;
  // Only the results scroll. The keyboard is typed on, and a key press must
  // not be second-guessed.
  if (report.event == hw::TouchEvent::kTap && search::showingResults() &&
      !listTapAllowed(report)) {
    return out;
  }
  switch (report.event) {
    case hw::TouchEvent::kTap: {
      int index = -1;
      switch (search::handleTap(report.x, report.y, index)) {
        case search::Result::kChanged:
          search::draw();
          break;
        case search::Result::kSearch:
          // This frame has to reach the panel before the blocking call, not
          // after, or the screen simply stops for the length of the request.
          search::showSearching();
          out.intent = Intent::kRunSearch;
          break;
        case search::Result::kSelected:
          // Acknowledge before blocking. play_media on an artist takes
          // seconds, and in radio mode Music Assistant also has to build a
          // queue, so this is the longest wait anywhere in the firmware.
          search::showStarting(index);
          out.intent = Intent::kPlaySearchResult;
          out.index = index;
          break;
        case search::Result::kBack:
          if (search::backToKeyboard()) {
            search::draw();
          }
          break;
        default:
          break;
      }
      break;
    }
    case hw::TouchEvent::kSwipeRight:
      // Back out one step at a time: results to keyboard, keyboard to list.
      if (search::backToKeyboard()) {
        search::draw();
      } else {
        out.intent = Intent::kOpenBrowse;
      }
      break;
    default:
      break;
  }
  return out;
}

Input wifiTouch(const hw::TouchReport& report) {
  Input out;
  if (report.event == hw::TouchEvent::kTap && wifi_join::onList() &&
      !listTapAllowed(report)) {
    return out;
  }
  switch (report.event) {
    case hw::TouchEvent::kTap:
      switch (wifi_join::handleTap(report.x, report.y)) {
        case wifi_join::Result::kChanged:
          wifi_join::draw();
          break;
        case wifi_join::Result::kJoin:
          out.intent = Intent::kWifiJoin;
          break;
        case wifi_join::Result::kRescan:
          out.intent = Intent::kWifiRescan;
          break;
        default:
          break;
      }
      break;
    case hw::TouchEvent::kSwipeRight:
      // Back out one page at a time: password to list, list to wherever the
      // list was opened from.
      if (wifi_join::back()) {
        wifi_join::draw();
      } else {
        out.intent = Intent::kWifiLeave;
      }
      break;
    default:
      break;
  }
  return out;
}

Input playersTouch(const hw::TouchReport& report) {
  Input out;
  // Nothing to back out to: with no player there is nothing else to show.
  if (report.event != hw::TouchEvent::kTap || !listTapAllowed(report)) {
    return out;
  }
  switch (player_pick::handleTap(report.x, report.y)) {
    case player_pick::Result::kChosen:
      out.intent = Intent::kChoosePlayer;
      break;
    case player_pick::Result::kRefresh:
      out.intent = Intent::kPlayersRefresh;
      break;
    default:
      break;
  }
  return out;
}

}  // namespace

bool init(uint8_t rotation) {
  displayInit(rotation);
  canvasInit();
  artwork::init();
  hw::touchInit();
  return true;
}

Input poll(Screen screen, const PlayerState* state) {
  Input out;

  // A touch on a blanked panel only wakes it. Handled before anything else
  // reads the touch state, so the same press cannot also press a button.
  if (displayIsBlanked()) {
    // A whole tap can now arrive in one poll -- down and up queued behind a
    // slow pass -- so a completed gesture wakes the panel too, not only a
    // finger seen resting on it.
    hw::TouchReport discard;
    if (hw::touchIsDown() || hw::touchPoll(discard)) {
      wake();
      out.intent = Intent::kWokeFromTouch;
    }
    return out;
  }

  if (driveVolume(screen, state, out)) {
    return out;
  }
  driveListScrolling(screen);

  hw::TouchReport report;
  if (!hw::touchPoll(report)) {
    return out;
  }

  if (screen == Screen::kSearch) {
    return searchTouch(report);
  }
  if (screen == Screen::kBrowse) {
    return browseTouch(report);
  }
  if (screen == Screen::kWifi) {
    return wifiTouch(report);
  }
  if (screen == Screen::kPlayers) {
    return playersTouch(report);
  }
  if (screen == Screen::kMessage) {
    // Most of what the cards complain about is fixed in the portal, and the
    // app notices on its own. A tap is passed up all the same: the Setup and
    // No Wi-Fi cards open the network list on it.
    if (report.event == hw::TouchEvent::kTap) {
      out.intent = Intent::kTapCard;
    }
    return out;
  }
  if (screen == Screen::kStatus) {
    // A swipe left goes on to the Wi-Fi networks, as one does from the list
    // to the search; any other tap or swipe goes back.
    if (report.event == hw::TouchEvent::kSwipeLeft) {
      out.intent = Intent::kOpenWifi;
    } else if (report.event != hw::TouchEvent::kNone) {
      out.intent = Intent::kBackToNowPlaying;
    }
    return out;
  }

  if (report.event == hw::TouchEvent::kSwipeUp) {
    out.intent = Intent::kOpenBrowse;
    return out;
  }
  if (report.event == hw::TouchEvent::kSwipeDown) {
    out.intent = Intent::kOpenStatus;
    return out;
  }
  if (report.event != hw::TouchEvent::kTap) {
    return out;
  }

  switch (now_playing::hitTest(report.x, report.y)) {
    case Action::kPrevious:
      out.intent = Intent::kPrevious;
      break;
    case Action::kPlayPause:
      out.intent = Intent::kPlayPause;
      break;
    case Action::kNext:
      out.intent = Intent::kNext;
      break;
    default:
      break;
  }
  return out;
}

// --- What to show ----------------------------------------------------------

void showNowPlaying(const PlayerState& state) { now_playing::draw(state); }

void showBrowse() {
  hw::touchCancel();
  browse_list::opened();
  s_list_dragging = false;
  s_list_velocity = 0.0f;
  browse_list::draw();
}

void showStatus(const DeviceStatus& status) { statusScreenDevice(status); }

void showWifi(const char* note, const char* current) {
  hw::touchCancel();
  wifi_join::open(note, current);
  s_list_dragging = false;
  s_list_velocity = 0.0f;
  wifi_join::draw();
}

void showWifiNetworks(const WifiNetwork* networks, int count) {
  wifi_join::setNetworks(networks, count);
  if (wifi_join::onList()) {
    wifi_join::draw();
  }
}

void showPlayers(const char* note) {
  hw::touchCancel();
  player_pick::open(note);
  s_list_dragging = false;
  s_list_velocity = 0.0f;
  player_pick::draw();
}

const char* playerChosen() { return player_pick::chosenEntity(); }

const char* wifiChosenSsid() { return wifi_join::chosenSsid(); }

const char* wifiPassword() { return wifi_join::password(); }

void showSearch() {
  hw::touchCancel();
  search::reset();
  s_list_dragging = false;
  s_list_velocity = 0.0f;
  search::draw();
}

void showSearchResults() { search::draw(); }

void showSearching() { search::showSearching(); }

void showLoading(const char* what) {
  statusScreenMessage("Loading", what, "", theme::kStatusPlain,
                      theme::kStatusPlainText);
}

void showCommandPending(Intent intent, bool pending) {
  switch (intent) {
    case Intent::kPrevious:
      now_playing::showButtonPressed(Action::kPrevious, pending);
      break;
    case Intent::kPlayPause:
      now_playing::showButtonPressed(Action::kPlayPause, pending);
      break;
    case Intent::kNext:
      now_playing::showButtonPressed(Action::kNext, pending);
      break;
    default:
      break;
  }
}

void showLoadingList() {
  statusScreenMessage("Loading", "music", "", theme::kStatusPlain,
                      theme::kStatusPlainText);
}

void showNeedsHaSetup() {
  hw::touchCancel();
  statusScreenNeedsHaSetup();
}

void showHaSignIn(const char* qr_text, const char* url, const char* name) {
  hw::touchCancel();
  statusScreenHaSignIn(qr_text, url, name);
}

void showNoPlayer() {
  hw::touchCancel();
  statusScreenNoPlayer();
}

void showHaUnreachable(const char* detail) { statusScreenHaUnreachable(detail); }

void showPortal() { statusScreenPortal(); }

void showConnecting(const char* ssid) { statusScreenConnectingBegin(ssid); }

void tickConnecting() { statusScreenConnectingTick(); }

void showConnectFailed() { statusScreenConnectFailed(); }

void showSettingsCleared() { statusScreenWifiReset(); }

void refreshElapsed(const PlayerState& state) {
  now_playing::refreshElapsed(state);
}

// --- Panel power -----------------------------------------------------------

bool isBlanked() { return displayIsBlanked(); }

void blank() { displayBlank(); }

void wake() {
  if (!displayIsBlanked()) {
    return;
  }
  displayWake();
  // The touch that woke the screen must not also land on a control.
  hw::touchCancel();
}

// --- Artwork ---------------------------------------------------------------

void requestArtwork(const char* picture) { artwork::requestCover(picture); }

bool artworkPending(const char* picture) {
  return artwork::coverPending(picture);
}

bool takeArtworkFinished() { return artwork::takeCoverFinished(); }

void clearArtwork() {
  cover::clear();
  artwork::forgetCover();
}

bool idleWork(Screen screen) {
  // Thumbnails are fetched by artwork's own worker now, so nothing here
  // blocks: this queues what the screen wants, collects what has arrived, and
  // decides whether that is worth a repaint.
  //
  // Arrivals are gathered per screen and repainted together. The worker can
  // land several a second once its connection is warm, and a full list
  // repaint is the better part of 150 ms on the Qualia -- one per image
  // would put back a slice of the stall the worker exists to remove.
  static Screen s_pending_screen = Screen::kMessage;
  static uint32_t s_pending = 0;
  static unsigned long s_repainted_ms = 0;

  if (screen != s_pending_screen) {
    s_pending_screen = screen;
    s_pending = 0;  // a new screen is composed whole, pictures and all
  }

  if (screen == Screen::kSearch) {
    if (!search::showingResults()) {
      return false;
    }
    s_pending |= search::updateThumbs();
  } else {
    // The list's own artwork is filled in from wherever the user happens to
    // be, so a swipe up lands on a finished list rather than one loading.
    //
    // Except while the poll task is reloading it: that holds the list's lock
    // across the network, and asking for the item count would wait out the
    // whole load on the loop. The load only runs off the list screens.
    if (screen != Screen::kBrowse && services::browse::busy()) {
      return false;
    }
    const uint32_t finished = browse_list::updateThumbs();
    if (screen == Screen::kBrowse) {
      s_pending |= finished;
    }
  }

  if (s_pending == 0) {
    return false;
  }
  // Never under a finger or a glide: those repaint the rows themselves, and
  // with the pictures in, since artwork::draw() shows whatever has arrived.
  if (hw::touchIsDown() || s_list_velocity != 0.0f ||
      millis() - s_repainted_ms < config::kThumbRepaintMinMs) {
    return false;
  }

  const bool visible = screen == Screen::kSearch
                           ? search::anyResultVisible(s_pending)
                           : browse_list::anyItemVisible(s_pending);
  s_pending = 0;
  if (!visible) {
    return false;  // they will be drawn when scrolled to
  }
  s_repainted_ms = millis();
  if (screen == Screen::kSearch) {
    search::draw();
  } else {
    // Not redrawRows(): that may move the rows already on the panel, and
    // those are the ones without the new pictures.
    browse_list::repaintRows();
  }
  return true;
}

unsigned long lastInteractionMs() { return hw::touchLastActivityMs(); }

bool volumeDragging() { return s_vol_dragging; }

}  // namespace ui
