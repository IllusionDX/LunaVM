/* module.c — sys.path search and .py module loading for the py frontend. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "module.h"

#ifdef _WIN32
#include <io.h>
#define POPEN  _popen
#define PCLOSE _pclose
#else
#include <dirent.h>
#define POPEN  popen
#define PCLOSE pclose
#endif

/* Ordered search list: ".", PYTHONPATH, per-OS standard dirs, host-probed
 * site-packages. The importing file's dir is tried before this list. */
static char **g_paths;
static int g_path_count;
static int g_path_cap;

static bool dir_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void add_path(const char *path) {
    if (!path || !path[0]) return;
    for (int i = 0; i < g_path_count; i++)
        if (strcmp(g_paths[i], path) == 0) return;
    char *dup = strdup(path);
    if (!dup) return;
    if (g_path_count == g_path_cap) {
        int cap = g_path_cap ? g_path_cap * 2 : 8;
        char **grown = realloc(g_paths, (size_t)cap * sizeof(char *));
        if (!grown) { free(dup); return; }
        g_paths = grown;
        g_path_cap = cap;
    }
    g_paths[g_path_count++] = dup;
}

/* Split an env var into paths (Windows ';', POSIX ':') and keep existing dirs. */
static void add_env_paths(const char *var) {
    const char *value = getenv(var);
    if (!value || !value[0]) return;
    char *buf = strdup(value);
    if (!buf) return;
#ifdef _WIN32
    const char *delim = ";";
#else
    const char *delim = ":";
#endif
    for (char *token = strtok(buf, delim); token; token = strtok(NULL, delim))
        if (dir_exists(token)) add_path(token);
    free(buf);
}

/* Add {parent}/{prefix...}/{leaf} for every child dir of parent whose name
 * starts with prefix (resolves Python312, python3.11, ... without hardcoding). */
static void add_versioned_dirs(const char *parent, const char *prefix, const char *leaf) {
    if (!parent || !parent[0]) return;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s/%s*", parent, prefix);
    struct _finddata_t data;
    intptr_t handle = _findfirst(pattern, &data);
    if (handle == -1) return;
    do {
        if (!(data.attrib & _A_SUBDIR)) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s%s", parent, data.name, leaf);
        if (dir_exists(full)) add_path(full);
    } while (_findnext(handle, &data) == 0);
    _findclose(handle);
#else
    DIR *dir = opendir(parent);
    if (!dir) return;
    size_t plen = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, plen) != 0 || entry->d_name[plen] == '\0') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s%s", parent, entry->d_name, leaf);
        if (dir_exists(full)) add_path(full);
    }
    closedir(dir);
#endif
}

void module_init_global_paths(void) {
    /* Index 0: cwd, so local modules shadow installed ones. */
    add_path(".");

    /* PYTHONPATH: standard injection point for tools and environments. */
    add_env_paths("PYTHONPATH");

#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s/Python", appdata);
        add_versioned_dirs(parent, "Python3", "/site-packages");   /* pip --user */
    }
    const char *localappdata = getenv("LOCALAPPDATA");
    if (localappdata) {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s/Programs/Python", localappdata);
        add_versioned_dirs(parent, "Python3", "/Lib/site-packages");
    }
    add_versioned_dirs("C:", "Python3", "/Lib/site-packages");
#else
    const char *home = getenv("HOME");
    if (home) {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s/.local/lib", home);
        add_versioned_dirs(parent, "python3.", "/site-packages");  /* pip --user */
    }
    add_versioned_dirs("/usr/local/lib", "python3.", "/dist-packages");
    add_path("/usr/lib/python3/dist-packages");
#endif
}

/* Ask the host CPython where its packages actually live; silently skips
 * stubs/misconfigurations by keeping only existing directories. */
void module_probe_host_python_paths(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s -c \"import site, sys; "
             "print(';'.join(site.getsitepackages() + [site.getusersitepackages()]))\"",
             "python");
    FILE *fp = POPEN(cmd, "r");
#ifndef _WIN32
    if (!fp) {
        snprintf(cmd, sizeof(cmd),
                 "%s -c \"import site, sys; "
                 "print(';'.join(site.getsitepackages() + [site.getusersitepackages()]))\"",
                 "python3");
        fp = POPEN(cmd, "r");
    }
#endif
    if (!fp) return;

    /* Read one line; consume it fully even if larger than the buffer. */
    char buffer[4096];
    size_t n = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF && ch != '\n') {
        if (n + 1 < sizeof(buffer)) buffer[n++] = (char)ch;
    }
    PCLOSE(fp);
    if (n == 0) return;
    buffer[n] = '\0';
    buffer[strcspn(buffer, "\r")] = 0;

    for (char *token = strtok(buffer, ";"); token; token = strtok(NULL, ";"))
        if (dir_exists(token)) add_path(token);
}

/* Resolve a module name to a real .py file. Only .py sources are ever opened:
 * compiled .pyc/.pyd/.so files in site-packages are never touched. */
char *module_resolve_path(const char *module_name, const char *from_dir) {
    if (!module_name || !module_name[0]) return NULL;
    /* Bare identifiers only: no separators, traversal or drive letters. */
    for (const char *p = module_name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '.') return NULL;

    char path[1024];
    if (from_dir && from_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s.py", from_dir, module_name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return strdup(path); }
    }
    for (int i = 0; i < g_path_count; i++) {
        snprintf(path, sizeof(path), "%s/%s.py", g_paths[i], module_name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return strdup(path); }
    }
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
