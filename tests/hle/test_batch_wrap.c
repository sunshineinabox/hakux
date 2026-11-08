/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Every command the back end records through its device table (T.vkCmd*)
 * goes through the strip batch's wrapper (vk/batch_wrap.h), which records
 * a held run of strips first: a command without one reaches the command
 * buffer before the draws queued ahead of it, and those draws take its
 * state. vkCmdSetLineWidth had none, and two line strips of different
 * widths merged into one draw at the second width. */
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s) {
        s[n] = 0;
    }
    fclose(f);
    return s;
}

static void test_every_command_wrapped(void)
{
    char *wrap = slurp(HLE_BATCH_WRAP);
    HT_CHECK(wrap != NULL);
    if (!wrap) {
        return;
    }
    char list[] = HLE_VK_SOURCES;
    unsigned files = 0, cmds = 0;
    for (char *save = NULL, *path = strtok_r(list, "|", &save); path;
         path = strtok_r(NULL, "|", &save)) {
        char *src = slurp(path);
        HT_CHECK(src != NULL);
        if (!src) {
            continue;
        }
        files++;
        for (const char *p = strstr(src, "T.vkCmd"); p; p = strstr(p + 1, "T.vkCmd")) {
            char name[64], fn[80], inst[200];
            size_t n = 0;
            for (const char *q = p + 2; n + 1 < sizeof(name) &&
                 ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9'));
                 q++) {
                name[n++] = *q;
            }
            name[n] = 0;
            snprintf(fn, sizeof(fn), "w_%s(", name);
            snprintf(inst, sizeof(inst), "t->%s = w_%s;", name, name);
            cmds++;
            if (!strstr(wrap, fn) || !strstr(wrap, inst)) {
                fprintf(stderr, "%s: %s has no batch wrapper\n", path, name);
                ht_failures++;
            }
        }
        free(src);
    }
    HT_CHECK(files > 4 && cmds > 20);           /* the sources were found and read */
    free(wrap);
}

int main(void)
{
    HT_RUN(test_every_command_wrapped);
    HT_DONE();
}
