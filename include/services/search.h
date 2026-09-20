#pragma once

#include "services/ha_client.h"

/**
 * Artist search: the query being typed, the results it found, their artwork,
 * and starting one playing.
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

/** Play result `index` on the selected player, as radio when
 *  config::kSearchPlaysRadio. Blocks on the service call. */
bool play(int index);

}  // namespace services::search
