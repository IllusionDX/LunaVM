/* stdlib_os.c — Built-in os module for Luna.
 *
 * Directory: getcwd(), chdir(), listdir(), mkdir()
 * Files:     rename(), remove(), stat()
 * System:    execute(), getpid(), hostname(), username(), tmpdir()
 * Env:       getenv(), setenv()
 * Paths:     path_join(...), sep, pathsep
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "stdlib_os.h"
#include "value.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include <io.h>
#define PATHSEP_CHAR ';'
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <pwd.h>
#define PATHSEP_CHAR ':'
#endif

#define SEP_CHAR '/'

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(mod->exports, key, make_obj((Object*)f));
}

static void require_string(VM *vm, Value v, const char *fn, int idx) {
    if (!IS_STRING(v)) {
        luna_throw(vm, vm->type_error_class, "%s() argument %d must be a string", fn, idx);
    }
}

static const char *as_cstring(Value v) {
    return ((ObjString*)AS_OBJ(v))->chars;
}

/* ==================================================================
 * Directory operations
 * ================================================================== */

static Value os_getcwd(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.getcwd() takes no arguments");
    }
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, sizeof(buf))) {
        luna_throw(vm, vm->runtime_error_class, "os.getcwd(): %s", strerror(errno));
    }
#else
    if (!getcwd(buf, sizeof(buf))) {
        luna_throw(vm, vm->runtime_error_class, "os.getcwd(): %s", strerror(errno));
    }
#endif
    return make_obj((Object*)new_string(buf, (int)strlen(buf)));
}

static Value os_chdir(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.chdir() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.chdir", 1);
#ifdef _WIN32
    if (_chdir(as_cstring(args[0])) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.chdir(): %s", strerror(errno));
    }
#else
    if (chdir(as_cstring(args[0])) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.chdir(): %s", strerror(errno));
    }
#endif
    return make_null();
}

static Value os_listdir(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.listdir() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.listdir", 1);
    const char *path = as_cstring(args[0]);
    ObjList *list = new_list(0);

#ifdef _WIN32
    size_t path_len = strlen(path);
    char *pattern = malloc(path_len + 3);
    if (!pattern) {
        luna_throw(vm, vm->runtime_error_class, "os.listdir(): out of memory");
    }
    memcpy(pattern, path, path_len);
    if (path_len > 0 && (path[path_len - 1] == '\\' || path[path_len - 1] == '/')) {
        pattern[path_len] = '*';
        pattern[path_len + 1] = '\0';
    } else {
        pattern[path_len] = '\\';
        pattern[path_len + 1] = '*';
        pattern[path_len + 2] = '\0';
    }

    struct _finddata_t fd;
    intptr_t handle = _findfirst(pattern, &fd);
    free(pattern);
    if (handle == -1) {
        luna_throw(vm, vm->runtime_error_class, "os.listdir(): %s", strerror(errno));
    }
    do {
        if (strcmp(fd.name, ".") != 0 && strcmp(fd.name, "..") != 0) {
            list_add(list, make_obj((Object*)new_string(fd.name, (int)strlen(fd.name))));
        }
    } while (_findnext(handle, &fd) == 0);
    _findclose(handle);
#else
    DIR *dp = opendir(path);
    if (!dp) {
        luna_throw(vm, vm->runtime_error_class, "os.listdir(): %s", strerror(errno));
    }
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            list_add(list, make_obj((Object*)new_string(entry->d_name, (int)strlen(entry->d_name))));
        }
    }
    closedir(dp);
#endif
    return make_obj((Object*)list);
}

static Value os_mkdir(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.mkdir() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.mkdir", 1);
    const char *path = as_cstring(args[0]);
#ifdef _WIN32
    if (_mkdir(path) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.mkdir(): %s", strerror(errno));
    }
#else
    if (mkdir(path, 0755) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.mkdir(): %s", strerror(errno));
    }
#endif
    return make_null();
}

/* ==================================================================
 * File operations
 * ================================================================== */

static Value os_rename(VM *vm, Value *args, int n) {
    if (n != 2) {
        luna_throw(vm, vm->argument_error_class, "os.rename() expects exactly 2 arguments");
    }
    require_string(vm, args[0], "os.rename", 1);
    require_string(vm, args[1], "os.rename", 2);
    if (rename(as_cstring(args[0]), as_cstring(args[1])) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.rename(): %s", strerror(errno));
    }
    return make_null();
}

static Value os_remove(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.remove() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.remove", 1);
    if (remove(as_cstring(args[0])) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.remove(): %s", strerror(errno));
    }
    return make_null();
}

static Value os_stat(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.stat() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.stat", 1);
    const char *path = as_cstring(args[0]);

#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.stat(): %s", strerror(errno));
    }
    int is_dir = (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.stat(): %s", strerror(errno));
    }
    int is_dir = S_ISDIR(st.st_mode);
#endif

    ObjDict *d = new_dict();

    /* size */
    dict_set(d, make_obj((Object*)new_string("size", 4)),
             make_int((int64_t)st.st_size));

    /* is_dir */
    dict_set(d, make_obj((Object*)new_string("is_dir", 6)),
             make_bool(is_dir));

    /* mtime */
    dict_set(d, make_obj((Object*)new_string("mtime", 5)),
             make_double((double)st.st_mtime));

    return make_obj((Object*)d);
}

/* ==================================================================
 * System operations
 * ================================================================== */

static Value os_execute(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.execute() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.execute", 1);
    const char *cmd = as_cstring(args[0]);
    int ret = system(cmd);
#ifdef _WIN32
    return make_int((int64_t)ret);
#else
    if (ret == -1) {
        luna_throw(vm, vm->runtime_error_class, "os.execute(): %s", strerror(errno));
    }
    if (WIFEXITED(ret)) {
        return make_int((int64_t)WEXITSTATUS(ret));
    }
    return make_int(-1);
#endif
}

static Value os_args(VM *vm, Value *args_val, int n) {
    (void)args_val;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.args() takes no arguments");
    }
    ObjList *list = new_list(vm->process_argc);
    for (int i = 0; i < vm->process_argc; i++) {
        const char *s = vm->process_argv && vm->process_argv[i] ? vm->process_argv[i] : "";
        list_add(list, make_obj((Object*)new_string(s, (int)strlen(s))));
    }
    return make_obj((Object*)list);
}

static Value os_exit(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.exit() expects exactly 1 argument");
    }
    if (!IS_NUMBER(args[0])) {
        luna_throw(vm, vm->type_error_class, "os.exit() argument must be numeric");
    }
    int code = (int)value_to_double(args[0]);
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return make_null();
}

static Value os_platform(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.platform() takes no arguments");
    }
#ifdef _WIN32
    return make_obj((Object*)new_string("win32", 5));
#else
    return make_obj((Object*)new_string("posix", 5));
#endif
}

static Value os_getpid(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.getpid() takes no arguments");
    }
#ifdef _WIN32
    return make_int((int64_t)_getpid());
#else
    return make_int((int64_t)getpid());
#endif
}

static Value os_hostname(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.hostname() takes no arguments");
    }
    char buf[256];
#ifdef _WIN32
    DWORD size = sizeof(buf);
    if (!GetComputerNameA(buf, &size)) {
        luna_throw(vm, vm->runtime_error_class, "os.hostname(): system call failed");
    }
#else
    if (gethostname(buf, sizeof(buf)) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.hostname(): %s", strerror(errno));
    }
#endif
    buf[sizeof(buf) - 1] = '\0';
    return make_obj((Object*)new_string(buf, (int)strlen(buf)));
}

static Value os_username(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.username() takes no arguments");
    }
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (!GetUserNameA(buf, &size)) {
        luna_throw(vm, vm->runtime_error_class, "os.username(): system call failed");
    }
    return make_obj((Object*)new_string(buf, (int)strlen(buf)));
#else
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_name) {
        luna_throw(vm, vm->runtime_error_class, "os.username(): unable to determine user");
    }
    return make_obj((Object*)new_string(pw->pw_name, (int)strlen(pw->pw_name)));
#endif
}

static Value os_tmpdir(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "os.tmpdir() takes no arguments");
    }
#ifdef _WIN32
    char buf[MAX_PATH + 1];
    DWORD len = GetTempPathA(sizeof(buf), buf);
    if (len == 0 || len > sizeof(buf)) {
        luna_throw(vm, vm->runtime_error_class, "os.tmpdir(): system call failed");
    }
    /* Remove trailing backslash if present */
    size_t slen = strlen(buf);
    if (slen > 0 && (buf[slen - 1] == '\\' || buf[slen - 1] == '/')) {
        buf[slen - 1] = '\0';
    }
#else
    const char *env = getenv("TMPDIR");
    const char *tmp = "/tmp";
    const char *dir = env ? env : tmp;
    return make_obj((Object*)new_string(dir, (int)strlen(dir)));
#endif
    return make_obj((Object*)new_string(buf, (int)strlen(buf)));
}

/* ==================================================================
 * Environment operations
 * ================================================================== */

static Value os_getenv(VM *vm, Value *args, int n) {
    if (n != 1) {
        luna_throw(vm, vm->argument_error_class, "os.getenv() expects exactly 1 argument");
    }
    require_string(vm, args[0], "os.getenv", 1);
    const char *key = as_cstring(args[0]);
    const char *val = getenv(key);
    if (!val) return make_null();
    return make_obj((Object*)new_string(val, (int)strlen(val)));
}

static Value os_setenv(VM *vm, Value *args, int n) {
    if (n != 2) {
        luna_throw(vm, vm->argument_error_class, "os.setenv() expects exactly 2 arguments");
    }
    require_string(vm, args[0], "os.setenv", 1);
    require_string(vm, args[1], "os.setenv", 2);
    const char *key = as_cstring(args[0]);
    const char *val = as_cstring(args[1]);

#ifdef _WIN32
    if (_putenv_s(key, val) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.setenv(): %s", strerror(errno));
    }
#else
    if (setenv(key, val, 1) != 0) {
        luna_throw(vm, vm->runtime_error_class, "os.setenv(): %s", strerror(errno));
    }
#endif
    return make_null();
}

/* ==================================================================
 * Path operations
 * ================================================================== */

static Value os_path_join(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class, "os.path_join() expects at least 2 arguments");
    }
    for (int i = 0; i < n; i++) {
        require_string(vm, args[i], "os.path_join", i + 1);
    }

    size_t capacity = 1;
    for (int i = 0; i < n; i++) {
        capacity += ((ObjString*)AS_OBJ(args[i]))->length;
    }
    capacity += n;

    char *buf = (char*)malloc(capacity);
    if (!buf) {
        luna_throw(vm, vm->runtime_error_class, "os.path_join(): out of memory");
    }

    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        ObjString *s = (ObjString*)AS_OBJ(args[i]);
        const char *chars = s->chars;
        size_t len = s->length;
        if (len == 0) continue;

        if (pos == 0) {
            memcpy(buf + pos, chars, len);
            pos += len;
        } else {
            while (pos > 0 && (buf[pos - 1] == '/' || buf[pos - 1] == '\\')) pos--;
            size_t start = 0;
            size_t end = len;
            while (start < end && (chars[start] == '/' || chars[start] == '\\')) start++;
            while (end > start && (chars[end - 1] == '/' || chars[end - 1] == '\\')) end--;
            size_t seg_len = end - start;
            if (seg_len > 0) {
                buf[pos++] = SEP_CHAR;
                memcpy(buf + pos, chars + start, seg_len);
                pos += seg_len;
            }
        }
    }
    if (pos > 1) {
        while (pos > 1 && (buf[pos - 1] == '/' || buf[pos - 1] == '\\')) pos--;
    }
    buf[pos] = '\0';

    ObjString *result = new_string(buf, (int)pos);
    free(buf);
    return make_obj((Object*)result);
}

/* ==================================================================
 * Module registration
 * ================================================================== */

void vm_register_os_module(VM *vm) {
    ObjModule *mod = new_module("os");

    /* Directory */
    module_add_native(mod, "getcwd",  os_getcwd);
    module_add_native(mod, "chdir",   os_chdir);
    module_add_native(mod, "listdir", os_listdir);
    module_add_native(mod, "mkdir",   os_mkdir);

    /* Files */
    module_add_native(mod, "rename", os_rename);
    module_add_native(mod, "remove", os_remove);
    module_add_native(mod, "stat",   os_stat);

    /* System */
    module_add_native(mod, "args",     os_args);
    module_add_native(mod, "exit",     os_exit);
    module_add_native(mod, "platform", os_platform);
    module_add_native(mod, "execute",  os_execute);
    module_add_native(mod, "getpid",   os_getpid);
    module_add_native(mod, "hostname", os_hostname);
    module_add_native(mod, "username", os_username);
    module_add_native(mod, "tmpdir",   os_tmpdir);

    /* Environment */
    module_add_native(mod, "getenv", os_getenv);
    module_add_native(mod, "setenv", os_setenv);

    /* Paths */
    module_add_native(mod, "path_join", os_path_join);

    /* Constants */
    const char sep_str[] = {SEP_CHAR, '\0'};
    const char pathsep_str[] = {PATHSEP_CHAR, '\0'};
    dict_set(mod->exports,
             make_obj((Object*)new_string("sep", 3)),
             make_obj((Object*)new_string(sep_str, 1)));
    dict_set(mod->exports,
             make_obj((Object*)new_string("pathsep", 7)),
             make_obj((Object*)new_string(pathsep_str, 1)));

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("os", 2)),
             make_obj((Object*)mod));
}
