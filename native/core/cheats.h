/*
 * hakuX native core — cheats: memory codes the app turns on and off
 * (patches/mods/README.md has the format). The engine is pure C: the guest's memory
 * through callbacks (mods.c gives it the VM's, the host tests a fake one).
 *
 * The cheats and their lines come in at boot, before the guest runs;
 * nc_cheat_compile from one thread while it runs; nc_cheat_set and the
 * frontend's (nc_cheat_front_*) may come from any thread; nc_cheats_run runs
 * on the guest CPU's thread with the guest stopped.
 */
#ifndef NATIVE_CORE_CHEATS_H
#define NATIVE_CORE_CHEATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_CHEATS_MAX 256

typedef struct NcCheatMem {
    void *opaque;
    /* false: the address isn't mapped */
    bool (*read)(void *opaque, uint32_t va, void *buf, unsigned n);
    bool (*write)(void *opaque, uint32_t va, const void *buf, unsigned n);
} NcCheatMem;

/* A new cheat, its code lines next (nc_cheat_line): its index, or -1 when
 * there is no room. once: written once each time it is turned on. */
int nc_cheat_add(const char *name, bool on, bool once);
/* One code line of the cheat added last. False, with why in err, when it
 * isn't one (the cheat keeps its other lines). */
bool nc_cheat_line(const char *line, char *err, size_t errlen);
/* A whole cheat at once (text: its code lines, one per line), from the app
 * while the game runs: built past the cheats there are and published last,
 * so the guest's thread never sees it half made. dry: only checked. Returns
 * its index (0 when dry), or -1 with why in err ("line 2: bad address"). */
int nc_cheat_compile(const char *name, bool on, bool once, const char *text, bool dry, char *err,
                     size_t errlen);
void nc_cheat_set(int index, bool on);
bool nc_cheat_on(int index);
unsigned nc_cheat_count(void);
const char *nc_cheat_name(int index);
/* Lines skipped so far for an unmapped address or pointer. */
uint64_t nc_cheat_misses(int index);
/* The frontend's cheats (libretro's retro_cheat_set): a set apart from the
 * ones above, by the frontend's index, kept over nc_cheats_reset (a restart)
 * until nc_cheat_front_reset. text: its codes as a cheat file writes them,
 * joined by +, blanks or newlines, # comments: code lines (w32 0x0034A5C8 99,
 * if16 ... end) or AAAAAAAA VVVVVVVV (8 hex digits each: V's 32 bits at A).
 * Off, or on with the text; false, and off, with why in err when the text
 * isn't one ("code 2: bad address"). */
bool nc_cheat_front_set(unsigned index, bool on, const char *text, char *err, size_t errlen);
void nc_cheat_front_reset(void);
/* The guest's thread, after a run: a frontend cheat whose code was first
 * skipped for an unmapped address since it was set (or the boot), each
 * once; -1 when none. */
int nc_cheat_front_missed(void);
/* Runs every cheat that is on: returns the bytes written. */
unsigned nc_cheats_run(const NcCheatMem *m);
/* The cheats above gone (the core stopped, the host tests); the frontend's
 * stay, their misses counted again. */
void nc_cheats_reset(void);

#ifdef __cplusplus
}
#endif

#endif
