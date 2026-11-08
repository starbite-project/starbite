/* SPDX-License-Identifier: GPL-2.0-only
 * sbdisplay (stage 2, the display-only driver) built into sbwddm as its display-only mode: guard.c calls
 * SdDriverEntry when full mode is not wanted or not safe. Same code as sbdisplay.sys, same Parameters
 * (Modes, DefaultMode, Rotation, Scale..., read from sbwddm's service key).
 */
#define SB_SBDISPLAY_EMBEDDED 1
#include "../sbdisplay/sbdisplay.c"
