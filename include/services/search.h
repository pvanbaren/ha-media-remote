#pragma once

#include "services/ha_client.h"

/**
 * The search: what it looks for -- artists, albums, tracks, playlists or
 * radio stations -- the query being typed, the results it found, their
 * artwork, and starting one playing.
 *
 * Single-threaded by construction: everything here runs on the Arduino loop,
 * because a search is a response to a keypress and there is nothing to
 * preload. That is why there is no mutex, where services::browse needs one.
 *
 * Artwork is named here and drawn elsewhere: a result carries its image URL,
 * and ui::artwork turns that into a sprite at whatever size the panel wants.
 * Nothing in this file includes a graphics library.
 */
namespace services::search {

enum class Status : uint8_t {
  kIdle,       // nothing searched yet
  kSearching,  // request in flight
  kFound,      // results available
  kNoMatches,  // the search worked and found nothing
  kFailed,     // the request failed; ha::lastError() says why
};

/** What the search looks for: its name on the selector ("Artist"), the
 *  prompt in the empty query ("Artist name"), and the media_type Music
 *  Assistant knows it by ("artist"). Artists until the selector is tapped,
 *  and kept from one opening of the search to the next. */
const char* typeLabel();
const char* typePrompt();
const char* mediaType();
/** On to the next kind, round from radio back to artists. Results found for
 *  the last kind are dropped; the query stays. */
void nextType();

/** The query being typed. Never null; empty until a key is pressed. */
const char* query();
/** Append a character, if there is room. False when the buffer is full. */
bool append(char c);
/** Remove the last character. False when already empty. */
bool backspace();
void clearQuery();

/** Run the search for the current query. Blocks on the network. Artwork is
 *  not fetched here -- see loadNextThumb(). */
void run();

Status status();
int count();
/** nullptr when `index` is out of range. */
const ha::LibraryItem* at(int index);
/** Whether result `index` is played as radio, an endless queue seeded from
 *  it: an artist, when config::kArtistsPlayRadio. Anything else -- an
 *  album, a track, a playlist, a station -- plays as itself. */
bool playsAsRadio(int index);

}  // namespace services::search
