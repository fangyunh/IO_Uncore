/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.10b: the engine core compiled a second time with the ctx-spill emulation (vfio_user_iau_tile_spill.c).
 * Every external symbol of vfio_user_iau_tile.c is renamed here, so the second copy never clashes with the knob-off
 * copy in vfio_user_iau_tile_lnk.o; only iau_eng6_ring_poll becomes the exported iau_eng6_ring_poll_spill (declared in
 * vfio_user_iau_tile.h). The copies of the control-plane functions are never called.
 */
#ifndef SPDK_VFIO_USER_IAU_TILE_SPILL_NAMES_H
#define SPDK_VFIO_USER_IAU_TILE_SPILL_NAMES_H

#define iau_eng6_ring_poll		iau_eng6_ring_poll_spill
#define iau_eng6_member_stat_keys	iau_eng6s_member_stat_keys
#define iau_eng6_ring_stat_keys		iau_eng6s_ring_stat_keys
#define iau_eng6_member_stat		iau_eng6s_member_stat
#define iau_eng6_ring_stat		iau_eng6s_ring_stat
#define iau_eng6_ring_init		iau_eng6s_ring_init
#define iau_eng6_ring_remap		iau_eng6s_ring_remap
#define iau_eng6_member_init		iau_eng6s_member_init
#define iau_eng6_member_fini		iau_eng6s_member_fini
#define iau_eng6_member_add		iau_eng6s_member_add
#define iau_eng6_member_stop		iau_eng6s_member_stop
#define iau_eng6_member_foreign_db	iau_eng6s_member_foreign_db
#define iau_eng6_member_detach		iau_eng6s_member_detach
#define iau_eng6_route_release		iau_eng6s_route_release
#define iau_eng6_fmt_ring		iau_eng6s_fmt_ring
#define iau_eng6_fmt_bind		iau_eng6s_fmt_bind
#define iau_eng6_fmt_stats		iau_eng6s_fmt_stats
#define iau_eng6_fmt_ring_end		iau_eng6s_fmt_ring_end
#define iau_eng6_fmt_spill		iau_eng6s_fmt_spill
#define iau_eng6_member_set_ctx		iau_eng6s_member_set_ctx

#endif
