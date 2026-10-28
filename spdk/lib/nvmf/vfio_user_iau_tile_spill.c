/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.10b: the IAU tile engine core with the A19 ctx-spill emulation (IAU_TILE_CTX_SPILL=1). The same source as
 * vfio_user_iau_tile.c compiled with IAU_ENG6_SPILL: the descriptor fetch adds one 64 B store of the slot line into
 * the route's ctx block, the completion one 64 B load of it before the record. vfio_user.c installs
 * iau_eng6_ring_poll_spill as the engine thread's poll at bind time when the knob is on; with it off this object is
 * never called and the knob-off engine (vfio_user_iau_tile_lnk.c) is the unchanged source, so its instructions do not
 * change (A12 precedent). The object pads its .text to 64 B (vfio_user_text_pad.h).
 */
#include "vfio_user_text_pad.h"

VU_TEXT_PAD64();

#pragma GCC diagnostic ignored "-Wunused-function"
#define IAU_ENG6_SPILL 1
#include "vfio_user_iau_tile_spill_names.h"
#include "vfio_user_iau_tile.c"
