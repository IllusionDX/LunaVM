/* module.c — Module resolution and file loading for Luna imports. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"

char *module_resolve_path(const char *module_name, const char *from_dir) {
    char path[1024];

    if (from_dir && from_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s.luna", from_dir, module_name);
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); return strdup(path); }
    }

    snprintf(path, sizeof(path), "./%s.luna", module_name);
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return strdup(path); }

    return NULL;
}

char *module_read_source(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[n] = '\0';
    return buf;
}
