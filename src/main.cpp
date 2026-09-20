/**
 * Arduino entry points, and nothing else.
 *
 * The remote lives in src/app/; the panel lives in src/ui/<shape>/. Keeping
 * this file empty of both is what lets either be swapped without touching the
 * other.
 */
#include "app/app.h"

void setup() { app::setup(); }

void loop() { app::loop(); }
