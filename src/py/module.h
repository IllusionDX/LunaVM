/* module.h — Module resolution and file loading for Luna imports. */

#ifndef LUNA_MODULE_H
#define LUNA_MODULE_H

/* Populate the sys.path search list: ".", PYTHONPATH, per-OS standard
 * site-packages/dist-packages dirs, host-probed paths last. */
void module_init_global_paths(void);

/* Probe the host CPython (python / python3) for its real site-packages and
 * append any existing ones. Safe to call when no Python is installed. */
void module_probe_host_python_paths(void);

/* Resolve a module name to a .py file: {from_dir}/{module}.py first, then
 * each search path in order. Only bare identifiers are accepted.
 * Returns heap-allocated path, or NULL if not found.
 */
char *module_resolve_path(const char *module_name, const char *from_dir);

/* Read the entire contents of a file into a heap-allocated string.
 * Returns NULL on error.
 */
char *module_read_source(const char *filepath);

#endif /* LUNA_MODULE_H */
