#include "services/search.h"

#include <Arduino.h>

#include <cstring>

#include "config.h"

namespace services::search {
namespace {

char s_query[config::kSearchQueryMaxLen + 1] = {};
size_t s_len = 0;

ha::LibraryItem s_results[config::kMaxSearchResults];
int s_count = 0;
Status s_status = Status::kIdle;

}  // namespace

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
  const int found =
      ha::searchArtists(s_query, s_results, config::kMaxSearchResults);
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

bool play(int index) {
  const ha::LibraryItem* item = at(index);
  if (item == nullptr) {
    return false;
  }
  const char* entity = ha::selectedEntity();
  if (entity[0] == '\0') {
    return false;
  }
  return ha::playMedia(entity, item->uri, item->media_type,
                       config::kSearchPlaysRadio);
}

}  // namespace services::search
