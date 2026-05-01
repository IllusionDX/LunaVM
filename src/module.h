/* module.h — Module resolution and file loading for Luna imports. */

#ifndef LUNA_MODULE_H
#define LUNA_MODULE_H

/* Resolve a module name to a file path.
 * If from_dir is non-NULL, searches there first:
 *   {from_dir}/lib/{module}.luna, then {from_dir}/{module}.luna
 * Then falls back to: ./lib/{module}.luna, ./{module}.luna
 * Returns heap-allocated path, or NULL if not found.
 */
char *module_resolve_path(const char *module_name, const char *from_dir);

/* Read the entire contents of a file into a heap-allocated string.
 * Returns NULL on error.
 */
char *module_read_source(const char *filepath);

#endif /* LUNA_MODULE_H */
