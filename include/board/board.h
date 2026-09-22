#pragma once

/**
 * The board this firmware is being built for.
 *
 * Which header that is comes from platformio.ini, as
 * -DBOARD_HEADER='"board/<name>.h"', so adding a board is a header and an env
 * rather than a conditional threaded through every file. Only src/ui/ and
 * src/hardware/ include this; the application and the services are written
 * against no particular panel.
 */
#ifndef BOARD_HEADER
#error "No board selected. Set -DBOARD_HEADER='\"board/<name>.h\"' in platformio.ini."
#endif

#include BOARD_HEADER
