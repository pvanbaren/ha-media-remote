#pragma once

#include <cstdint>

#include "ui/ui.h"

/**
 * The settings page on the device, and the list of choices behind each of
 * its rows: one scrolling list of rows under a title, built a row at a time
 * by the app (see ui::settingsBegin()).
 *
 * A row with a detail shows two lines, the text small and grey over the
 * detail -- a setting's name over its value. One without shows its text
 * alone. The current choice in a list of choices is marked, and the list
 * opens scrolled to it.
 */
namespace ui::settings_list {

/** Start an empty list titled `title`, or `note` in its place when that is
 *  not empty. False when there is no memory for the rows. */
bool begin(const char* title, const char* note);
/** Add a row; ignored once the list is full. */
void add(const char* text, const char* detail, SettingStyle style);

/** Scroll row `focus` to the middle of the panel -- or when that is -1, the
 *  current choice if there is one -- as far as the list allows, or to the
 *  top. */
void open(int focus);

/** Compose and present the list. */
void draw();

/** Light row `row` as pressed -- or put it back -- on the glass at once,
 *  redrawing that row alone: the answer to a tap whose work may take a
 *  second or two. The next draw(), or whatever screen comes next, replaces
 *  it. */
void showPressed(int row, bool pressed);

/** Scroll by pixels; positive moves further down. False when the clamp
 *  meant nothing moved. */
bool scrollByPx(int delta);
/** Whether the list is as far as it goes; `direction` negative for the top. */
bool atScrollLimit(int direction);

/** The row under (x, y), or -1. */
int rowAt(int x, int y);

}  // namespace ui::settings_list
