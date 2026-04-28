/* stdlib_io.c â€” Basic file I/O module for Luna.
 *
 * Public surface:
 *   - io.File(path, mode) / io.open(path, mode) -> File instance
 *   - File methods: read_line(), read_all(), write(text), flush(), close()
 *   - module helpers: read_file(path), write_file(path, text), append_file(path, text),
 *     exists(path), remove(path)
 *
 * Internally the instance stores a userdata handle so the GC can close
 * forgotten files through a finalizer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "stdlib_io.h"
#include "value.h"

typedef struct LunaFile {
    FILE *fp;
    char *path;
    char *mode;
} LunaFile;

static ObjClass *file_class = NULL;

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(out, s, len + 1);
    return out;
}

static char *value_to_cstring(VM *vm, Value v, const char *fn, int arg_idx) {
    if (!IS_STRING(v)) {
        luna_throw(vm, vm->type_error_class,
            "%s() argument %d must be a string", fn, arg_idx);
    }
    ObjString *s = (ObjString*)AS_OBJ(v);
    char *buf = malloc((size_t)s->length + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    memcpy(buf, s->chars, (size_t)s->length);
    buf[s->length] = '\0';
    return buf;
}

static void file_finalizer(void *data) {
    LunaFile *file = (LunaFile*)data;
    if (!file) return;
    if (file->fp) {
        fclose(file->fp);
        file->fp = NULL;
    }
    free(file->path);
    free(file->mode);
    free(file);
}

static ObjUserdata *file_userdata_from_instance(VM *vm, Value self, const char *fn) {
    if (!IS_INSTANCE(self) || !AS_OBJ(self)) {
        luna_throw(vm, vm->type_error_class, "%s() expects a File instance", fn);
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value handle = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(handle) || !AS_OBJ(handle)) {
        luna_throw(vm, vm->runtime_error_class, "%s() called on closed file handle", fn);
    }
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(handle);
    if (!ud->data) {
        luna_throw(vm, vm->runtime_error_class, "%s() called on closed file handle", fn);
    }
    if (!ud->tag || strcmp(ud->tag, "io.File") != 0) {
        luna_throw(vm, vm->type_error_class, "%s() invalid file handle", fn);
    }
    return ud;
}

static LunaFile *file_handle_from_instance(VM *vm, Value self, const char *fn) {
    ObjUserdata *ud = file_userdata_from_instance(vm, self, fn);
    return (LunaFile*)ud->data;
}

static void set_file_handle(ObjInstance *inst, FILE *fp, const char *path, const char *mode) {
    LunaFile *file = malloc(sizeof(LunaFile));
    if (!file) { fprintf(stderr, "OOM\n"); exit(1); }
    file->fp = fp;
    file->path = dup_cstr(path);
    file->mode = dup_cstr(mode);

    ObjUserdata *ud = new_userdata_tagged("io.File", file, file_finalizer);
    instance_set_field(inst, "_handle", make_obj((Object*)ud));
}

static void clear_file_handle(ObjInstance *inst) {
    instance_set_field(inst, "_handle", make_null());
}

static Value io_file_close(VM *vm, Value *args, int n) {
    (void)vm;
    (void)n;
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value handle = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(handle) || !AS_OBJ(handle)) {
        return make_bool(false);
    }
    clear_file_handle(inst);
    return make_bool(true);
}

static Value io_file_flush(VM *vm, Value *args, int n) {
    (void)n;
    LunaFile *file = file_handle_from_instance(vm, args[0], "flush");
    if (fflush(file->fp) != 0) {
        luna_throw(vm, vm->runtime_error_class, "flush() failed");
    }
    return make_null();
}

static Value io_file_write(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class, "write() requires a value argument");
    }
    LunaFile *file = file_handle_from_instance(vm, args[0], "write");
    char *text = value_to_string(args[1]);
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, file->fp);
    free(text);
    if (written != len) {
        luna_throw(vm, vm->runtime_error_class, "write() failed");
    }
    return make_int((int32_t)written);
}

static Value io_file_read_line(VM *vm, Value *args, int n) {
    (void)n;
    LunaFile *file = file_handle_from_instance(vm, args[0], "read_line");
    char buf[1024];
    if (!fgets(buf, sizeof(buf), file->fp)) {
        return make_null();
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        len--;
    }
    return make_obj((Object*)new_string(buf, (int)len));
}

static Value io_file_read_all(VM *vm, Value *args, int n) {
    (void)n;
    LunaFile *file = file_handle_from_instance(vm, args[0], "read_all");
    long start = ftell(file->fp);
    if (start < 0) start = 0;
    if (fseek(file->fp, 0, SEEK_END) != 0) {
        luna_throw(vm, vm->runtime_error_class, "read_all() failed");
    }
    long size = ftell(file->fp);
    if (size < 0) {
        luna_throw(vm, vm->runtime_error_class, "read_all() failed");
    }
    if (fseek(file->fp, start, SEEK_SET) != 0) {
        luna_throw(vm, vm->runtime_error_class, "read_all() failed");
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fprintf(stderr, "OOM\n"); exit(1); }
    size_t read = fread(buf, 1, (size_t)size, file->fp);
    if (read == 0 && ferror(file->fp)) {
        free(buf);
        luna_throw(vm, vm->runtime_error_class, "read_all() failed");
    }
    buf[read] = '\0';
    Value out = make_obj((Object*)new_string(buf, (int)read));
    free(buf);
    return out;
}

static Value io_exists(VM *vm, Value *args, int n) {
    if (n < 1) {
        luna_throw(vm, vm->argument_error_class, "exists() requires a path");
    }
    char *path = value_to_cstring(vm, args[0], "exists", 1);
    FILE *fp = fopen(path, "rb");
    bool ok = fp != NULL;
    if (fp) fclose(fp);
    free(path);
    return make_bool(ok);
}

static Value io_remove(VM *vm, Value *args, int n) {
    if (n < 1) {
        luna_throw(vm, vm->argument_error_class, "remove() requires a path");
    }
    char *path = value_to_cstring(vm, args[0], "remove", 1);
    int rc = remove(path);
    free(path);
    return make_bool(rc == 0);
}

static Value io_open(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class, "open() requires path and mode");
    }
    char *path = value_to_cstring(vm, args[0], "open", 1);
    char *mode = value_to_cstring(vm, args[1], "open", 2);
    FILE *fp = fopen(path, mode);
    if (!fp) {
        free(path);
        free(mode);
        luna_throw(vm, vm->runtime_error_class, "io.open: failed to open file");
    }

    ObjInstance *inst = new_instance(file_class, 4);
    set_file_handle(inst, fp, path, mode);
    free(path);
    free(mode);
    return make_obj((Object*)inst);
}

static Value io_read_file(VM *vm, Value *args, int n) {
    if (n < 1) {
        luna_throw(vm, vm->argument_error_class, "read_file() requires a path");
    }
    char *path = value_to_cstring(vm, args[0], "read_file", 1);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        free(path);
        luna_throw(vm, vm->runtime_error_class, "io.read_file: failed to open file");
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        free(path);
        luna_throw(vm, vm->runtime_error_class, "read_file() failed");
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        free(path);
        luna_throw(vm, vm->runtime_error_class, "read_file() failed");
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        free(path);
        luna_throw(vm, vm->runtime_error_class, "read_file() failed");
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); free(path); fprintf(stderr, "OOM\n"); exit(1); }
    size_t read = fread(buf, 1, (size_t)size, fp);
    bool had_error = ferror(fp);
    fclose(fp);
    free(path);
    if (had_error) {
        free(buf);
        luna_throw(vm, vm->runtime_error_class, "read_file() failed");
    }
    buf[read] = '\0';
    Value out = make_obj((Object*)new_string(buf, (int)read));
    free(buf);
    return out;
}

static Value io_write_file(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class, "write_file() requires path and data");
    }
    char *path = value_to_cstring(vm, args[0], "write_file", 1);
    char *data = value_to_string(args[1]);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(path);
        free(data);
        luna_throw(vm, vm->runtime_error_class, "io.write_file: failed to open file");
    }
    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, fp);
    int ok = fclose(fp);
    free(path);
    free(data);
    return make_bool(written == len && ok == 0);
}

static Value io_append_file(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class, "append_file() requires path and data");
    }
    char *path = value_to_cstring(vm, args[0], "append_file", 1);
    char *data = value_to_string(args[1]);
    FILE *fp = fopen(path, "ab");
    if (!fp) {
        free(path);
        free(data);
        luna_throw(vm, vm->runtime_error_class, "io.append_file: failed to open file");
    }
    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, fp);
    int ok = fclose(fp);
    free(path);
    free(data);
    return make_bool(written == len && ok == 0);
}

static void class_add_native_method(ObjClass *cls, const char *name, NativeFn fn) {
    if (cls->method_count >= cls->method_capacity) {
        int new_cap = cls->method_capacity < 4 ? 4 : cls->method_capacity * 2;
        cls->methods = realloc(cls->methods, sizeof(ObjFunction*) * new_cap);
        cls->method_names = realloc(cls->method_names, sizeof(char*) * new_cap);
        cls->method_capacity = new_cap;
    }
    ObjFunction *f = new_native_function(name, fn);
    cls->methods[cls->method_count] = f;
    cls->method_names[cls->method_count] = dup_cstr(name);
    cls->method_count++;
}

static void io_add_fn(ObjDict *exports, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(exports, key, make_obj((Object*)f));
}

static Value io_file_ctor(VM *vm, Value *args, int n) {
    return io_open(vm, args, n);
}

void vm_register_io_module(VM *vm) {
    file_class = new_class("File", NULL);
    retain_obj((Object*)file_class);

    class_add_native_method(file_class, "read_line", io_file_read_line);
    class_add_native_method(file_class, "read_all",  io_file_read_all);
    class_add_native_method(file_class, "write",     io_file_write);
    class_add_native_method(file_class, "flush",     io_file_flush);
    class_add_native_method(file_class, "close",     io_file_close);

    ObjModule *mod = new_module("io");
    io_add_fn(mod->exports, "File",       io_file_ctor);
    io_add_fn(mod->exports, "open",       io_open);
    io_add_fn(mod->exports, "read_file",  io_read_file);
    io_add_fn(mod->exports, "write_file", io_write_file);
    io_add_fn(mod->exports, "append_file", io_append_file);
    io_add_fn(mod->exports, "exists",     io_exists);
    io_add_fn(mod->exports, "remove",     io_remove);

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("io", 2)),
             make_obj((Object*)mod));
}
