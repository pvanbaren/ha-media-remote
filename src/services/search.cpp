#include "services/search.h"

#include <Arduino.h>

#include <cstring>

#include "config.h"
#include "services/history.h"

namespace services::search {
namespace {

/** The kinds the selector steps through, in order. */
struct Type {
  const char* label;
  const char* prompt;
  const char* media_type;
};
constexpr Type kTypes[] = {
    {"Artist", "Artist name", "artist"},
    {"Album", "Album name", "album"},
    {"Track", "Track name", "track"},
    {"Playlist", "Playlist name", "playlist"},
    {"Radio", "Station name", "radio"},
};
constexpr int kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);
int s_type = 0;

char s_query[config::kSearchQueryMaxLen + 1] = {};
size_t s_len = 0;

ha::LibraryItem s_results[config::kMaxSearchResults];
int s_count = 0;
Status s_status = Status::kIdle;

}  // namespace

const char* typeLabel() { return kTypes[s_type].label; }
const char* typePrompt() { return kTypes[s_type].prompt; }
const char* mediaType() { return kTypes[s_type].media_type; }

void nextType() {
  s_type = (s_type + 1) % kTypeCount;
  s_count = 0;
  s_status = Status::kIdle;
}

const char* query() { return s_query; }

bool append(char c) {
  if (s_len >= config::kSearchQueryMaxLen) {
    return false;
  }
  s_query[s_len++] = c;
  s_query[s_len] = '\0';
  return true;
}

bool backspace() {
  if (s_len == 0) {
    return false;
  }
  s_query[--s_len] = '\0';
  return true;
}

void clearQuery() {
  s_len = 0;
  s_query[0] = '\0';
  s_count = 0;
  s_status = Status::kIdle;
}

void run() {
  if (s_len == 0) {
    s_status = Status::kIdle;
    s_count = 0;
    return;
  }

  s_status = Status::kSearching;
  const int found = ha::search(s_query, mediaType(), s_results,
                               config::kMaxSearchResults);
  if (found < 0) {
    s_count = 0;
    s_status = Status::kFailed;
    return;
  }
  s_count = found;
  s_status = found > 0 ? Status::kFound : Status::kNoMatches;
}

Status status() { return s_status; }

int count() { return s_count; }

const ha::LibraryItem* at(int index) {
  if (index < 0 || index >= s_count) {
    return nullptr;
  }
  return &s_results[index];
}

bool playsAsRadio(int index) {
  const ha::LibraryItem* item = at(index);
  return config::kSearchPlaysRadio && item != nullptr &&
         strcmp(item->media_type, "artist") == 0;
}

}  // namespace services::search
