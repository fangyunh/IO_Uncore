/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.4 (review I1): the IAU tile engine core as libspdk_nvmf links it. vfio_user_iau_tile.c is the one engine
 * source (the unit tests build it alone, tests/realcore/run_c_tests.sh engine6); this translation unit adds only
 * the 64 B padding of the object's .text (vfio_user_text_pad.h), so the objects and libraries linked after it keep
 * their 64 B phase. The engine's code is unchanged.
 */
#include "vfio_user_text_pad.h"

VU_TEXT_PAD64();

#include "vfio_user_iau_tile.c"
