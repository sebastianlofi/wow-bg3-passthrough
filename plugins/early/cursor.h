/* wowbg3-early, part 2 (cursor.c). */
#ifndef WOWBG3_EARLY_CURSOR_H
#define WOWBG3_EARLY_CURSOR_H
#include "bg3le_plugin.h"
void cursor_init(const bg3le_host* host, bg3le_plugin* self, const char* dir);
unsigned cursor_created_after_load(void);
unsigned cursor_swaps(void);
#endif
