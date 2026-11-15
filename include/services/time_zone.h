#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The clock face's time zone: the Time zone setting.
 *
 * Home Assistant's, the default, is the zone Home Assistant is set to, asked
 * for once Home Assistant answers and kept in NVS so the next boot has it
 * before it answers again. Or one of a fixed list, chosen on the device or
 * in the portal.
 *
 * The board's C library takes a POSIX TZ rule, not a zone name, so the list
 * carries each zone's rule, and Home Assistant's zone is looked up in it by
 * name -- among the names each entry stands for. A zone not in the list gets
 * its present UTC offset, without summer time, as Home Assistant gave it.
 *
 * The poll task notes Home Assistant's zone; the loop applies the rule
 * (takeChange()), and the loop and the portal, which runs from it, choose.
 */
namespace services::time_zone {

/** One zone on the list: `id` its IANA name, as stored and as the portal
 *  sends it; `name` what the list shows; `posix` its rule; `also` the
 *  other IANA names it stands for, space-separated. */
struct Zone {
  const char* id;
  const char* name;
  const char* posix;
  const char* also;
};

/** Long enough for any id on the list, and for any IANA name. */
constexpr size_t kIdMaxLen = 40;
/** Long enough for any rule on the list, or an offset made into one. */
constexpr size_t kPosixMaxLen = 48;

int count();
/** The list's zone `i`, west to east; nullptr out of range. */
const Zone* at(int i);
/** The zone on the list `id` names, as its id or among its others, or
 *  nullptr. */
const Zone* find(const char* id);

/** Read what is stored. Before the loop starts. */
void init();

/** The chosen zone's id, or "" for Home Assistant's. Loop only. */
const char* chosen();
/** Choose `id`, a zone on the list, or "" for Home Assistant's, and store
 *  it. False, changing nothing, for an id not on the list. Loop only. */
bool choose(const char* id);

/** Home Assistant's zone, `iana` and its present UTC offset as "%z" gives
 *  it ("-0500"). Poll task. */
void noteHomeAssistant(const char* iana, const char* offset);
/** Home Assistant's zone as the list names it, or the last part of its
 *  IANA name; "" when it has never been heard. */
void homeAssistantName(char* out, size_t out_len);

/** The rule in force: the chosen zone's, or Home Assistant's, or UTC when
 *  neither is known. */
void posix(char* out, size_t out_len);
/** True once after the rule in force has changed, for the loop to apply. */
bool takeChange();

}  // namespace services::time_zone
