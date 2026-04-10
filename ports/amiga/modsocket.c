// BSD socket module for the AmigaOS port.
// Uses libnix libsocket.a which wraps bsdsocket.library.

#include <string.h>
#include <errno.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/objstr.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include <proto/exec.h>
#include <dos/dos.h>

typedef struct _mp_obj_socket_t {
    mp_obj_base_t base;
    int fd;
} mp_obj_socket_t;

static const mp_obj_type_t socket_type;

//
// Check and consume SIGBREAKF_CTRL_C after a bsdsocket blocking call.
//
// WinUAE (and presumably Roadshow/AmiTCP) wake up bsdsocket syscalls
// when SIGBREAKF_CTRL_C is posted, but do NOT consume the signal.
// If we just raise OSError here, the caller's Python code might catch
// it (except OSError:) and then at the MP_BC_POP_EXCEPT_JUMP at the end
// of the except block, our VM hook (mp_amiga_check_signals) would
// consume SIGBREAKF_CTRL_C and queue a KeyboardInterrupt. The KbdInt
// would then be raised by mp_handle_pending() at the worst possible
// moment (mid-POP_EXCEPT_JUMP), crashing the VM with vm.c:1144
// assertion.
//
// To avoid the cascade, we consume SIGBREAKF_CTRL_C here and raise
// KeyboardInterrupt DIRECTLY instead of OSError when we detect it.
// This way only one exception is in flight and there's no pending
// signal for the VM hook to trip on.
//
static MP_NORETURN void socket_raise_io_error(int err) {
    ULONG sig = SetSignal(0, SIGBREAKF_CTRL_C);
    if (sig & SIGBREAKF_CTRL_C) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    mp_raise_OSError(err);
}

// socket.close()
static mp_obj_t socket_close(mp_obj_t self_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_close_obj, socket_close);

// socket.bind(address)
static mp_obj_t socket_bind(mp_obj_t self_in, mp_obj_t addr_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(addr_in, 2, &items);
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (strlen(host) == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
    }

    if (bind(self->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_bind_obj, socket_bind);

// socket.listen([backlog])
static mp_obj_t socket_listen(size_t n_args, const mp_obj_t *args) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(args[0]);
    int backlog = (n_args > 1) ? mp_obj_get_int(args[1]) : 2;
    if (listen(self->fd, backlog) < 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_listen_obj, 1, 2, socket_listen);

// socket.accept() -> (new_socket, address)
static mp_obj_t socket_accept(mp_obj_t self_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int new_fd = accept(self->fd, (struct sockaddr *)&addr, &addr_len);
    // WinUAE bsdsocket may return a fake fd on Ctrl-C without setting
    // errno. Check the signal independently of the return value.
    ULONG sig = SetSignal(0, SIGBREAKF_CTRL_C);
    if (sig & SIGBREAKF_CTRL_C) {
        if (new_fd >= 0) {
            close(new_fd);  // close the fake fd to avoid a leak
        }
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    if (new_fd < 0) {
        socket_raise_io_error(errno);
    }

    mp_obj_socket_t *o = mp_obj_malloc(mp_obj_socket_t, &socket_type);
    o->fd = new_fd;

    const char *addr_str = inet_ntoa(addr.sin_addr);
    mp_obj_t tuple[2] = {
        MP_OBJ_FROM_PTR(o),
        mp_obj_new_tuple(2, (mp_obj_t[]){
            mp_obj_new_str(addr_str, strlen(addr_str)),
            MP_OBJ_NEW_SMALL_INT(ntohs(addr.sin_port)),
        }),
    };
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_accept_obj, socket_accept);

// socket.connect(address)
static mp_obj_t socket_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(addr_in, 2, &items);
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Try as IP address first, then DNS resolve
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (he == NULL) {
            mp_raise_OSError(MP_ENOENT);
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(self->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        socket_raise_io_error(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_connect_obj, socket_connect);

// socket.send(data)
static mp_obj_t socket_send(mp_obj_t self_in, mp_obj_t data_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int n = send(self->fd, bufinfo.buf, bufinfo.len, 0);
    if (n < 0) {
        socket_raise_io_error(errno);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_send_obj, socket_send);

// socket.recv(bufsize)
static mp_obj_t socket_recv(mp_obj_t self_in, mp_obj_t len_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    int len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);
    int n = recv(self->fd, vstr.buf, len, 0);
    if (n < 0) {
        vstr_clear(&vstr);
        socket_raise_io_error(errno);
    }
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_recv_obj, socket_recv);

// socket.sendto(data, address)
static mp_obj_t socket_sendto(mp_obj_t self_in, mp_obj_t data_in, mp_obj_t addr_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    mp_obj_t *items;
    mp_obj_get_array_fixed_n(addr_in, 2, &items);
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (he == NULL) {
            mp_raise_OSError(MP_ENOENT);
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    int n = sendto(self->fd, bufinfo.buf, bufinfo.len, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) {
        socket_raise_io_error(errno);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_3(socket_sendto_obj, socket_sendto);

// socket.recvfrom(bufsize)
static mp_obj_t socket_recvfrom(mp_obj_t self_in, mp_obj_t len_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    int len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int n = recvfrom(self->fd, vstr.buf, len, 0,
                     (struct sockaddr *)&addr, &addr_len);
    if (n < 0) {
        vstr_clear(&vstr);
        socket_raise_io_error(errno);
    }
    vstr.len = n;

    const char *addr_str = inet_ntoa(addr.sin_addr);
    mp_obj_t tuple[2] = {
        mp_obj_new_bytes_from_vstr(&vstr),
        mp_obj_new_tuple(2, (mp_obj_t[]){
            mp_obj_new_str(addr_str, strlen(addr_str)),
            MP_OBJ_NEW_SMALL_INT(ntohs(addr.sin_port)),
        }),
    };
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_recvfrom_obj, socket_recvfrom);

// socket.setsockopt(level, optname, value)
static mp_obj_t socket_setsockopt(size_t n_args, const mp_obj_t *args) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(args[0]);
    int level = mp_obj_get_int(args[1]);
    int optname = mp_obj_get_int(args[2]);
    int optval = mp_obj_get_int(args[3]);
    if (setsockopt(self->fd, level, optname, &optval, sizeof(optval)) < 0) {
        mp_raise_OSError(errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_setsockopt_obj, 4, 4, socket_setsockopt);

// socket.setblocking(flag)
static mp_obj_t socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    (void)self_in;
    (void)flag_in;
    // Not implemented — sockets are always blocking on AmigaOS libnix
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_setblocking_obj, socket_setblocking);

// socket.fileno()
static mp_obj_t socket_fileno(mp_obj_t self_in) {
    mp_obj_socket_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_fileno_obj, socket_fileno);

// socket.__del__() — ensure fd is closed when GC collects the object
static mp_obj_t socket___del__(mp_obj_t self_in) {
    return socket_close(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket___del___obj, socket___del__);

// Socket methods table
static const mp_rom_map_elem_t socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&socket___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&socket_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&socket_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&socket_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&socket_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&socket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&socket_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&socket_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&socket_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&socket_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&socket_fileno_obj) },
};
static MP_DEFINE_CONST_DICT(socket_locals_dict, socket_locals_dict_table);

// socket(family, type, proto) constructor
static mp_obj_t socket_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 3, false);
    int family = (n_args > 0) ? mp_obj_get_int(args[0]) : AF_INET;
    int stype = (n_args > 1) ? mp_obj_get_int(args[1]) : SOCK_STREAM;
    int proto = (n_args > 2) ? mp_obj_get_int(args[2]) : 0;

    int fd = socket(family, stype, proto);
    if (fd < 0) {
        mp_raise_OSError(errno);
    }

    mp_obj_socket_t *o = mp_obj_malloc(mp_obj_socket_t, type);
    o->fd = fd;
    return MP_OBJ_FROM_PTR(o);
}

static MP_DEFINE_CONST_OBJ_TYPE(
    socket_type,
    MP_QSTR_socket,
    MP_TYPE_FLAG_NONE,
    make_new, socket_make_new,
    locals_dict, &socket_locals_dict
);

// Module-level getaddrinfo(host, port)
static mp_obj_t mod_getaddrinfo(mp_obj_t host_in, mp_obj_t port_in) {
    const char *host = mp_obj_str_get_str(host_in);
    int port = mp_obj_get_int(port_in);

    struct hostent *he = gethostbyname(host);
    if (he == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; he->h_addr_list[i] != NULL; i++) {
        struct in_addr *addr = (struct in_addr *)he->h_addr_list[i];
        const char *addr_str = inet_ntoa(*addr);
        mp_obj_t tuple_items[5] = {
            MP_OBJ_NEW_SMALL_INT(AF_INET),
            MP_OBJ_NEW_SMALL_INT(SOCK_STREAM),
            MP_OBJ_NEW_SMALL_INT(0),
            mp_obj_new_str("", 0),
            mp_obj_new_tuple(2, (mp_obj_t[]){
                mp_obj_new_str(addr_str, strlen(addr_str)),
                MP_OBJ_NEW_SMALL_INT(port),
            }),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(5, tuple_items));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_getaddrinfo_obj, mod_getaddrinfo);

// Module globals
static const mp_rom_map_elem_t socket_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&socket_type) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&mod_getaddrinfo_obj) },

    // Address families
    { MP_ROM_QSTR(MP_QSTR_AF_INET), MP_ROM_INT(AF_INET) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET6), MP_ROM_INT(AF_INET6) },

    // Socket types
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(SOCK_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_DGRAM), MP_ROM_INT(SOCK_DGRAM) },

    // Socket options
    { MP_ROM_QSTR(MP_QSTR_SOL_SOCKET), MP_ROM_INT(SOL_SOCKET) },
    { MP_ROM_QSTR(MP_QSTR_SO_REUSEADDR), MP_ROM_INT(SO_REUSEADDR) },
};
static MP_DEFINE_CONST_DICT(socket_module_globals, socket_module_globals_table);

const mp_obj_module_t mp_module_socket = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&socket_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);
