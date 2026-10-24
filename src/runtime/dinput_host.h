/* The keyboard: dinput.c. docs/architecture.md (input). */
#pragma once
#include "native32.h"

void dih_init(int headless);
void dih_key(int vk, int down);  /* a --key went down or up: queue it, wake the game's keyboard read */
void dih_shim_DirectInputCreateA(void);
int  host_key_held(int vk);      /* host.c: a --key is down */
#define DIH_SHIMS { "DirectInputCreateA", dih_shim_DirectInputCreateA }
