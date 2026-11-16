/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS kernel input events (input.c). */
#pragma once
#include <stdint.h>

enum {
    KIN_NONE = 0,
    KIN_UP, KIN_DOWN,               /* Volume: move the focus */
    KIN_SELECT,                     /* Bixby short */
    KIN_BACK,                       /* Bixby held */
    KIN_POWER,                      /* Power short: screen off / on */
    KIN_POWER_LONG,                 /* Power held 1 s: the power menu */
    KIN_PRESS, KIN_MOVE, KIN_RELEASE,  /* touch, with the position */
    KIN_SHOT,                       /* 8w: Power + Volume Up together (iOS's screenshot); neither key's own action */
    KIN_FIRE                        /* 1.2-alpha30: Power held 1 s on the fire starter's page (kin_fire_keys) */
};
struct kin {
    int type, x, y;
    int id;                         /* 8a (Track A): the finger (the touch IC's TID); 0 the first. Keys: 0 */
};
void kin_push(int type, int x, int y);
void kin_push_id(int type, int id, int x, int y);
/* 1.2-alpha30: the fire starter's key mode (survival.h): while the app in front refreshes it (SET_SURV_FS_KEYS, at
 * least every 2 s) Power short is Select and Power held 1 s sends KIN_FIRE (the app's hold kin_fire_keys_id), and the
 * output then runs only while Power stays down; no screen-off or power menu from Power meanwhile. */
extern int kin_fire_keys_id;
extern uint64_t kin_fire_keys_t;
int kin_fire_trigger(void);          /* 1: Power is held past its 1 s in the fire key mode */   /* a touch of finger id (KIN_PRESS / MOVE / RELEASE) */
int kin_pop(struct kin *e);
uint64_t kin_idle_ms(void);
void kin_touch_held(int *down);
void input_reset(void);
