#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Which VLW faces this board carries.
 *
 * The faces are embedded by the linker, so their symbol names are spelled out
 * per board -- a 240 px panel wants 15/17/20/24 px and the 720 px Qualia
 * 34/38/45/54, and nothing can choose between those at run time. Everything
 * that *uses* a face is shared; this is only the list.
 */
namespace hw {

/** Pointer to the start of an embedded .vlw blob. */
const uint8_t* const* fontTable(size_t& count);

}  // namespace hw
