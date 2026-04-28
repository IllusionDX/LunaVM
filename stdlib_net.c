/* stdlib_net.c — Built-in net module for Luna.
 *
 * Cross-platform TCP/UDP sockets using OBJ_USERDATA finalizers.
 *
 *   net.tcp()              -> Socket(AF_INET, SOCK_STREAM)
 *   net.udp()              -> Socket(AF_INET, SOCK_DGRAM)
 *   net.Socket(family, type) -> generic socket
 *   net.resolve(hostname)  -> IP string
 *
 * Socket methods:
 *   s.connect(host, port)
 *   s.bind(port, host?)    -- host defaults to "0.0.0.0"
 *   s.listen(backlog?)
 *   s.accept()             -> [socket, client_ip] or null
 *   s.send(data)           -- string or buffer
 *   s.recv(max_size)       -> ObjBuffer
 *   s.close()
 *   s.set_blocking(bool)
 *   s.is_connected()       -> bool
 *   s.get_address()        -> "ip:port" string
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "stdlib_net.h"
#include "value.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

/* ==================================================================
 * Cross-platform socket handle
 * ================================================================== */

typedef struct LunaSocket {
#ifdef _WIN32
    SOCKET fd;
#else
    int fd;
#endif
    int family;
    int type;
    int connected;
    int bound;
    int listening;
    int blocking;
} LunaSocket;

static ObjClass *socket_class = NULL;

#ifdef _WIN32
static int winsock_initialized = 0;

static int ensure_winsock(void) {
    if (winsock_initialized) return 0;
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) return -1;
    winsock_initialized = 1;
    return 0;
}

static int socket_error(void) {
    return WSAGetLastError();
}

static int is_wouldblock(int err) {
    return err == WSAEWOULDBLOCK;
}
#else
static int ensure_winsock(void) { return 0; }

static int socket_error(void) {
    return errno;
}

static int is_wouldblock(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}
#endif

static int is_invalid_fd(LunaSocket *s) {
#ifdef _WIN32
    return s->fd == INVALID_SOCKET;
#else
    return s->fd < 0;
#endif
}

static void socket_close_fd(LunaSocket *s) {
    if (!s) return;
    if (is_invalid_fd(s)) return;
#ifdef _WIN32
    closesocket(s->fd);
    s->fd = INVALID_SOCKET;
#else
    close(s->fd);
    s->fd = -1;
#endif
    s->connected = 0;
    s->bound = 0;
    s->listening = 0;
}

static void socket_finalizer(void *data) {
    LunaSocket *s = (LunaSocket*)data;
    socket_close_fd(s);
    free(s);
}

/* ==================================================================
 * Helpers
 * ================================================================== */

static void require_string(VM *vm, Value v, const char *fn, int idx) {
    if (!IS_STRING(v)) {
        luna_throw(vm, vm->type_error_class,
            "%s() argument %d must be a string", fn, idx);
    }
}

static const char *as_cstring(Value v) {
    return ((ObjString*)AS_OBJ(v))->chars;
}

static int as_int_checked(VM *vm, Value v, const char *fn, int idx) {
    if (!IS_NUMBER(v)) {
        luna_throw(vm, vm->type_error_class,
            "%s() argument %d must be numeric", fn, idx);
    }
    return (int)value_to_double(v);
}

static ObjUserdata *socket_userdata_from_instance(VM *vm, Value self, const char *fn) {
    if (!IS_INSTANCE(self) || !AS_OBJ(self)) {
        luna_throw(vm, vm->type_error_class, "%s() expects a Socket instance", fn);
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(self);
    Value handle = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(handle) || !AS_OBJ(handle)) {
        luna_throw(vm, vm->runtime_error_class, "%s() called on closed socket", fn);
    }
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(handle);
    if (!ud->data) {
        luna_throw(vm, vm->runtime_error_class, "%s() called on closed socket", fn);
    }
    if (!ud->tag || strcmp(ud->tag, "net.Socket") != 0) {
        luna_throw(vm, vm->type_error_class, "%s() invalid socket handle", fn);
    }
    return ud;
}

static LunaSocket *socket_handle_from_instance(VM *vm, Value self, const char *fn) {
    ObjUserdata *ud = socket_userdata_from_instance(vm, self, fn);
    return (LunaSocket*)ud->data;
}

static void clear_socket_handle(ObjInstance *inst) {
    instance_set_field(inst, "_handle", make_null());
}

/* ==================================================================
 * Socket instance creation
 * ================================================================== */

static Value create_socket_instance(VM *vm, int family, int type) {
    if (ensure_winsock() != 0) {
        luna_throw(vm, vm->runtime_error_class,
            "net: failed to initialize networking");
    }

#ifdef _WIN32
    SOCKET fd = socket(family, type, 0);
    if (fd == INVALID_SOCKET) {
        luna_throw(vm, vm->runtime_error_class,
            "net: socket() failed: %d", socket_error());
    }
#else
    int fd = socket(family, type, 0);
    if (fd < 0) {
        luna_throw(vm, vm->runtime_error_class,
            "net: socket() failed: %s", strerror(errno));
    }
#endif

    LunaSocket *sock = (LunaSocket*)malloc(sizeof(LunaSocket));
    if (!sock) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        luna_throw(vm, vm->runtime_error_class, "net: out of memory");
    }

    sock->fd = fd;
    sock->family = family;
    sock->type = type;
    sock->connected = 0;
    sock->bound = 0;
    sock->listening = 0;
    sock->blocking = 1;

    ObjUserdata *ud = new_userdata_tagged("net.Socket", sock, socket_finalizer);

    ObjInstance *inst = new_instance(socket_class, 4);
    instance_set_field(inst, "_handle", make_obj((Object*)ud));

    return make_obj((Object*)inst);
}

/* ==================================================================
 * Socket methods
 * ================================================================== */

static Value socket_connect(VM *vm, Value *args, int n) {
    if (n < 3) {
        luna_throw(vm, vm->argument_error_class,
            "connect() expects host and port arguments");
    }
    require_string(vm, args[1], "connect", 1);
    int port = as_int_checked(vm, args[2], "connect", 2);

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "connect");
    if (s->connected) {
        luna_throw(vm, vm->runtime_error_class, "connect(): socket already connected");
    }

    const char *host = as_cstring(args[1]);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = (short)s->family;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(s->family, host, &addr.sin_addr) <= 0) {
        /* Try DNS resolution */
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            luna_throw(vm, vm->runtime_error_class,
                "connect(): could not resolve host '%s'", host);
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    int rc = connect(s->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) {
        int err = socket_error();
        if (!is_wouldblock(err)) {
            luna_throw(vm, vm->runtime_error_class,
                "connect() failed: %d", err);
        }
    }

    s->connected = 1;
    return make_null();
}

static Value socket_bind(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "bind() expects at least a port argument");
    }
    int port = as_int_checked(vm, args[1], "bind", 1);
    const char *host = "0.0.0.0";
    if (n >= 3 && !IS_NIL(args[2])) {
        require_string(vm, args[2], "bind", 2);
        host = as_cstring(args[2]);
    }

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "bind");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = (short)s->family;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(s->family, host, &addr.sin_addr) <= 0) {
        luna_throw(vm, vm->runtime_error_class,
            "bind(): invalid address '%s'", host);
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (bind(s->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        luna_throw(vm, vm->runtime_error_class,
            "bind() failed: %d", socket_error());
    }

    s->bound = 1;
    return make_null();
}

static Value socket_listen(VM *vm, Value *args, int n) {
    int backlog = 5;
    if (n >= 2 && !IS_NIL(args[1])) {
        backlog = as_int_checked(vm, args[1], "listen", 1);
    }

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "listen");

    if (listen(s->fd, backlog) != 0) {
        luna_throw(vm, vm->runtime_error_class,
            "listen() failed: %d", socket_error());
    }

    s->listening = 1;
    return make_null();
}

static Value socket_accept(VM *vm, Value *args, int n) {
    (void)n;
    LunaSocket *s = socket_handle_from_instance(vm, args[0], "accept");

    struct sockaddr_in client_addr;
#ifdef _WIN32
    int addr_len = sizeof(client_addr);
#else
    socklen_t addr_len = sizeof(client_addr);
#endif

#ifdef _WIN32
    SOCKET client_fd = accept(s->fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd == INVALID_SOCKET) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return make_null();
        }
        luna_throw(vm, vm->runtime_error_class,
            "accept() failed: %d", err);
    }
#else
    int client_fd = accept(s->fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return make_null();
        }
        luna_throw(vm, vm->runtime_error_class,
            "accept() failed: %s", strerror(err));
    }
#endif

    /* Create client socket instance */
    LunaSocket *client_sock = (LunaSocket*)malloc(sizeof(LunaSocket));
    if (!client_sock) {
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        luna_throw(vm, vm->runtime_error_class, "accept(): out of memory");
    }

    client_sock->fd = client_fd;
    client_sock->family = s->family;
    client_sock->type = s->type;
    client_sock->connected = 1;
    client_sock->bound = 0;
    client_sock->listening = 0;
    client_sock->blocking = s->blocking;

    ObjUserdata *ud = new_userdata_tagged("net.Socket", client_sock, socket_finalizer);
    ObjInstance *inst = new_instance(socket_class, 4);
    instance_set_field(inst, "_handle", make_obj((Object*)ud));

    /* Client IP string */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));

    /* Return [socket, ip] as a list */
    ObjList *result = new_list(2);
    list_add(result, make_obj((Object*)inst));
    list_add(result, make_obj((Object*)new_string(ip_str, (int)strlen(ip_str))));

    return make_obj((Object*)result);
}

static Value socket_send(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "send() requires a data argument");
    }

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "send");

    const uint8_t *data = NULL;
    size_t len = 0;

    if (IS_STRING(args[1])) {
        ObjString *str = (ObjString*)AS_OBJ(args[1]);
        data = (const uint8_t*)str->chars;
        len = (size_t)str->length;
    } else if (IS_BUFFER(args[1])) {
        ObjBuffer *buf = (ObjBuffer*)AS_OBJ(args[1]);
        data = buf->data;
        len = buf->size;
    } else {
        luna_throw(vm, vm->type_error_class,
            "send() data must be a string or buffer");
    }

    size_t total_sent = 0;
    while (total_sent < len) {
#ifdef _WIN32
        int sent = send(s->fd, (const char*)(data + total_sent),
                        (int)(len - total_sent), 0);
#else
        ssize_t sent = send(s->fd, data + total_sent, len - total_sent, 0);
#endif
        if (sent < 0) {
            int err = socket_error();
            if (is_wouldblock(err)) {
                break;
            }
            luna_throw(vm, vm->runtime_error_class,
                "send() failed: %d", err);
        }
        if (sent == 0) break;
        total_sent += (size_t)sent;
    }

    return make_int((int64_t)total_sent);
}

static Value socket_recv(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "recv() requires a max_size argument");
    }
    int max_size = as_int_checked(vm, args[1], "recv", 1);
    if (max_size <= 0 || max_size > 65536) {
        luna_throw(vm, vm->argument_error_class,
            "recv() max_size must be between 1 and 65536");
    }

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "recv");

    uint8_t *buf = (uint8_t*)malloc((size_t)max_size);
    if (!buf) {
        luna_throw(vm, vm->runtime_error_class, "recv(): out of memory");
    }

#ifdef _WIN32
    int received = recv(s->fd, (char*)buf, max_size, 0);
#else
    ssize_t received = recv(s->fd, buf, (size_t)max_size, 0);
#endif

    if (received < 0) {
        int err = socket_error();
        free(buf);
        if (is_wouldblock(err)) {
            /* Return empty buffer on would-block */
            ObjBuffer *empty = new_buffer(0);
            return make_obj((Object*)empty);
        }
        luna_throw(vm, vm->runtime_error_class,
            "recv() failed: %d", err);
    }

    if (received == 0) {
        /* Peer closed connection */
        free(buf);
        ObjBuffer *empty = new_buffer(0);
        return make_obj((Object*)empty);
    }

    ObjBuffer *out = new_buffer((size_t)received);
    buffer_append_data(out, buf, (size_t)received);
    free(buf);

    return make_obj((Object*)out);
}

static Value socket_close(VM *vm, Value *args, int n) {
    (void)vm;
    (void)n;
    if (!IS_INSTANCE(args[0]) || !AS_OBJ(args[0])) {
        return make_bool(false);
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value handle = instance_get_field(inst, "_handle");
    if (!IS_USERDATA(handle) || !AS_OBJ(handle)) {
        return make_bool(false);
    }
    ObjUserdata *ud = (ObjUserdata*)AS_OBJ(handle);
    if (ud->data) {
        socket_finalizer(ud->data);
        ud->data = NULL;
    }
    clear_socket_handle(inst);
    return make_bool(true);
}

static Value socket_set_blocking(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "set_blocking() requires a boolean argument");
    }
    if (!IS_BOOL(args[1])) {
        luna_throw(vm, vm->type_error_class,
            "set_blocking() argument must be a boolean");
    }

    LunaSocket *s = socket_handle_from_instance(vm, args[0], "set_blocking");
    int blocking = AS_BOOL(args[1]) ? 1 : 0;

#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    if (ioctlsocket(s->fd, FIONBIO, &mode) != 0) {
        luna_throw(vm, vm->runtime_error_class,
            "set_blocking() failed: %d", WSAGetLastError());
    }
#else
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags < 0) {
        luna_throw(vm, vm->runtime_error_class,
            "set_blocking() failed: %s", strerror(errno));
    }
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    if (fcntl(s->fd, F_SETFL, flags) < 0) {
        luna_throw(vm, vm->runtime_error_class,
            "set_blocking() failed: %s", strerror(errno));
    }
#endif

    s->blocking = blocking;
    return make_null();
}

static Value socket_is_connected(VM *vm, Value *args, int n) {
    (void)n;
    LunaSocket *s = socket_handle_from_instance(vm, args[0], "is_connected");
    return make_bool(s->connected && !is_invalid_fd(s));
}

static Value socket_get_address(VM *vm, Value *args, int n) {
    (void)n;
    LunaSocket *s = socket_handle_from_instance(vm, args[0], "get_address");

    struct sockaddr_in addr;
#ifdef _WIN32
    int addr_len = sizeof(addr);
#else
    socklen_t addr_len = sizeof(addr);
#endif

    if (getsockname(s->fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        luna_throw(vm, vm->runtime_error_class,
            "get_address() failed: %d", socket_error());
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));

    char result[INET_ADDRSTRLEN + 16];
    snprintf(result, sizeof(result), "%s:%d", ip_str, ntohs(addr.sin_port));

    return make_obj((Object*)new_string(result, (int)strlen(result)));
}

/* ==================================================================
 * Module-level functions
 * ================================================================== */

static Value net_tcp(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "net.tcp() takes no arguments");
    }
    return create_socket_instance(vm, AF_INET, SOCK_STREAM);
}

static Value net_udp(VM *vm, Value *args, int n) {
    (void)args;
    if (n != 0) {
        luna_throw(vm, vm->argument_error_class, "net.udp() takes no arguments");
    }
    return create_socket_instance(vm, AF_INET, SOCK_DGRAM);
}

static Value net_socket(VM *vm, Value *args, int n) {
    if (n < 2) {
        luna_throw(vm, vm->argument_error_class,
            "net.Socket() requires family and type arguments");
    }
    int family = as_int_checked(vm, args[0], "Socket", 1);
    int type   = as_int_checked(vm, args[1], "Socket", 2);
    return create_socket_instance(vm, family, type);
}

static Value net_resolve(VM *vm, Value *args, int n) {
    if (n < 1) {
        luna_throw(vm, vm->argument_error_class,
            "net.resolve() requires a hostname argument");
    }
    require_string(vm, args[0], "resolve", 1);
    const char *host = as_cstring(args[0]);

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) {
        luna_throw(vm, vm->runtime_error_class,
            "resolve(): could not resolve '%s'", host);
    }

    struct in_addr addr;
    memcpy(&addr, he->h_addr_list[0], (size_t)he->h_length);
    char *ip = inet_ntoa(addr);

    return make_obj((Object*)new_string(ip, (int)strlen(ip)));
}

/* ==================================================================
 * Module registration
 * ================================================================== */

static void module_add_native(ObjModule *mod, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    Value key = make_obj((Object*)new_string(name, (int)strlen(name)));
    dict_set(mod->exports, key, make_obj((Object*)f));
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
    cls->method_names[cls->method_count] = strdup(name);
    cls->method_count++;
}

void vm_register_net_module(VM *vm) {
    /* Socket class */
    socket_class = new_class("Socket", NULL);
    retain_obj((Object*)socket_class);

    class_add_native_method(socket_class, "connect",       socket_connect);
    class_add_native_method(socket_class, "bind",          socket_bind);
    class_add_native_method(socket_class, "listen",        socket_listen);
    class_add_native_method(socket_class, "accept",        socket_accept);
    class_add_native_method(socket_class, "send",          socket_send);
    class_add_native_method(socket_class, "recv",          socket_recv);
    class_add_native_method(socket_class, "close",         socket_close);
    class_add_native_method(socket_class, "set_blocking",  socket_set_blocking);
    class_add_native_method(socket_class, "is_connected",  socket_is_connected);
    class_add_native_method(socket_class, "get_address",   socket_get_address);

    /* Module */
    ObjModule *mod = new_module("net");

    module_add_native(mod, "tcp",     net_tcp);
    module_add_native(mod, "udp",     net_udp);
    module_add_native(mod, "Socket",  net_socket);
    module_add_native(mod, "resolve", net_resolve);

    /* Constants */
    dict_set(mod->exports,
             make_obj((Object*)new_string("AF_INET", 7)),
             make_int(AF_INET));
    dict_set(mod->exports,
             make_obj((Object*)new_string("AF_INET6", 8)),
             make_int(AF_INET6));
    dict_set(mod->exports,
             make_obj((Object*)new_string("SOCK_STREAM", 11)),
             make_int(SOCK_STREAM));
    dict_set(mod->exports,
             make_obj((Object*)new_string("SOCK_DGRAM", 10)),
             make_int(SOCK_DGRAM));

    /* Export Socket class constructor — net.Socket(family, type) */
    module_add_native(mod, "Socket", net_socket);

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("net", 3)),
             make_obj((Object*)mod));
}
