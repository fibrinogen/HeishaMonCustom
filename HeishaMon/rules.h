/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef __RULES_H_
#define __RULES_H_

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <Arduino.h>

#include "src/common/mem.h"

#ifdef HEISHAMON_DISABLE_RULES

// Keep the public interface available to the unchanged HeishaMon modules,
// while omitting the rules engine itself from the firmware build.
static uint8_t nrrules = 0;

static inline void rules_boot(void) {}
static inline void rules_deinitialize(void) {}
static inline int rules_parse(char *) { return -1; }
static inline void rules_setup(void) {}
static inline void rules_timer_cb(int) {}
static inline void rules_event_cb(const char *, const char *) {}
static inline void rules_execute(void) {}

#else

extern uint8_t nrrules;

void rules_boot(void);
void rules_deinitialize(void);
int rules_parse(char *file);
void rules_setup(void);
void rules_timer_cb(int nr);
void rules_event_cb(const char *prefix, const char *name);
void rules_execute(void);

#endif

#endif
