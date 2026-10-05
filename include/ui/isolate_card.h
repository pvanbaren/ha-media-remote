#pragma once

#include <LovyanGFX.hpp>

#include "ui/ui.h"

/**
 * Isolate's card, from the "+N rooms" chip on now playing: a question or a
 * failure under the title, the rooms it concerns one to a line, and buttons
 * along the bottom -- Isolate and Cancel to ask, OK under a failure, none
 * while the rooms are being turned off. A swipe right cancels too.
 */
namespace ui::isolate_card {

enum class Result : uint8_t {
  kNone,
  kConfirm,  // Isolate
  kCancel,   // Cancel or OK
};

/** Compose and present `card`, and remember its buttons for hit(). */
void draw(const IsolateCard& card);

/** What a tap at (x, y) pressed on the card as last drawn. */
Result hit(int x, int y);

}  // namespace ui::isolate_card
