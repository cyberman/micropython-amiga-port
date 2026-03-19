// SSL/TLS module for MicroPython AmigaOS port via AmiSSL.
// Provides ssl.wrap_socket() for HTTPS support.
// Uses a custom BIO that routes I/O through libnix send()/recv().

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objstr.h"

#include <proto/exec.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <amissl/tags.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

// libnix errno is a macro (*__errno). AmiSSL needs a plain 'errno' symbol.
int amiga_errno_storage __asm("_errno");

// AmiSSL library bases — owned by us (no libamisslauto)
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;
// SocketBase is provided by libsocket
extern struct Library *SocketBase;

static SSL_CTX *global_ctx = NULL;
static int amissl_initialized = 0;
static BIO_METHOD *bio_amiga_method = NULL;

// --- Custom BIO using libnix send()/recv() ---
// __attribute__((saveds)) restores A4 register when called from AmiSSL library.

static int __attribute__((saveds)) bio_amiga_write(BIO *b, const char *buf, int len) {
    int fd = (int)BIO_get_data(b);
    if (fd < 0 || buf == NULL || len <= 0) return -1;
    int ret = send(fd, buf, len, 0);
    if (ret < 0) {
        BIO_clear_retry_flags(b);
        return -1;
    }
    return ret;
}

static int __attribute__((saveds)) bio_amiga_read(BIO *b, char *buf, int len) {
    int fd = (int)BIO_get_data(b);
    if (fd < 0 || buf == NULL || len <= 0) return 0;
    int ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        BIO_clear_retry_flags(b);
        return -1;
    }
    if (ret == 0) {
        BIO_clear_retry_flags(b);
    }
    return ret;
}

static int __attribute__((saveds)) bio_amiga_puts(BIO *b, const char *str) {
    return bio_amiga_write(b, str, strlen(str));
}

static long __attribute__((saveds)) bio_amiga_ctrl(BIO *b, int cmd, long num, void *ptr) {
    (void)b; (void)num; (void)ptr;
    if (cmd == BIO_CTRL_FLUSH) return 1;
    return 0;
}

static int __attribute__((saveds)) bio_amiga_create(BIO *b) {
    BIO_set_data(b, NULL);
    BIO_set_init(b, 1);
    return 1;
}

static int __attribute__((saveds)) bio_amiga_destroy(BIO *b) {
    if (b == NULL) return 0;
    BIO_set_data(b, NULL);
    return 1;
}

static BIO_METHOD *BIO_s_amiga(void) {
    if (bio_amiga_method == NULL) {
        bio_amiga_method = BIO_meth_new(BIO_TYPE_SOCKET, "amiga_socket");
        BIO_meth_set_write(bio_amiga_method, bio_amiga_write);
        BIO_meth_set_read(bio_amiga_method, bio_amiga_read);
        BIO_meth_set_puts(bio_amiga_method, bio_amiga_puts);
        BIO_meth_set_ctrl(bio_amiga_method, bio_amiga_ctrl);
        BIO_meth_set_create(bio_amiga_method, bio_amiga_create);
        BIO_meth_set_destroy(bio_amiga_method, bio_amiga_destroy);
    }
    return bio_amiga_method;
}

// --- AmiSSL initialization ---

static void ensure_amissl_init(void) {
    if (amissl_initialized) return;

    // Full manual AmiSSL initialization (no libamisslauto)
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Cannot open amisslmaster.library"));
    }

    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("InitAmiSSLMaster failed"));
    }

    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Cannot open AmiSSL"));
    }

    if (InitAmiSSL(AmiSSL_SocketBase, SocketBase,
                   AmiSSL_ErrNoPtr, &amiga_errno_storage,
                   TAG_DONE) != 0) {
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLBase = NULL;
        AmiSSLMasterBase = NULL;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("InitAmiSSL failed"));
    }

    global_ctx = SSL_CTX_new(TLS_client_method());
    if (!global_ctx) {
        CleanupAmiSSL(TAG_DONE);
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLBase = NULL;
        AmiSSLMasterBase = NULL;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Cannot create SSL context"));
    }

    SSL_CTX_set_default_verify_paths(global_ctx);
    amissl_initialized = 1;
}

// Full AmiSSL cleanup — we own everything (no libamisslauto).
void amissl_cleanup(void) {
    if (global_ctx) {
        SSL_CTX_free(global_ctx);
        global_ctx = NULL;
    }
    if (bio_amiga_method) {
        BIO_meth_free(bio_amiga_method);
        bio_amiga_method = NULL;
    }
    if (amissl_initialized && AmiSSLBase) {
        CleanupAmiSSL(TAG_DONE);
    }
    if (AmiSSLBase) {
        CloseAmiSSL();
        AmiSSLBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    AmiSSLExtBase = NULL;
    amissl_initialized = 0;
}

// --- SSLSocket type ---

typedef struct _mp_obj_ssl_socket_t {
    mp_obj_base_t base;
    mp_obj_t sock;
    int fd;
    SSL *ssl;
} mp_obj_ssl_socket_t;

static const mp_obj_type_t ssl_socket_type;

static mp_obj_t ssl_socket_recv(mp_obj_t self_in, mp_obj_t len_in) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ssl) {
        mp_raise_OSError(MP_EBADF);
    }
    int len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);
    int n = SSL_read(self->ssl, vstr.buf, len);
    if (n < 0) {
        vstr_clear(&vstr);
        mp_raise_OSError(MP_EIO);
    }
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssl_socket_recv_obj, ssl_socket_recv);

static mp_obj_t ssl_socket_send(mp_obj_t self_in, mp_obj_t data_in) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->ssl) {
        mp_raise_OSError(MP_EBADF);
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    int n = SSL_write(self->ssl, bufinfo.buf, bufinfo.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssl_socket_send_obj, ssl_socket_send);

// Explicit close() — safe to call from Python code.
// Does SSL_shutdown (sends close_notify) then closes underlying socket.
static mp_obj_t ssl_socket_close(mp_obj_t self_in) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl) {
        SSL_shutdown(self->ssl);
        SSL_free(self->ssl);  // also frees the BIO attached via SSL_set_bio
        self->ssl = NULL;
    }
    if (self->fd >= 0 && self->sock != mp_const_none) {
        mp_obj_t close_method[2];
        mp_load_method(self->sock, MP_QSTR_close, close_method);
        mp_call_method_n_kw(0, 0, close_method);
        self->sock = mp_const_none;
        self->fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssl_socket_close_obj, ssl_socket_close);

// GC-safe __del__ — called during gc_sweep, must NOT do I/O or call Python.
// SSL_shutdown is skipped (it does network I/O via BIO send, unsafe if
// the underlying socket fd was already closed by its own __del__).
// mp_load_method/mp_call are skipped (unsafe during GC sweep).
static mp_obj_t ssl_socket___del__(mp_obj_t self_in) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl) {
        SSL_free(self->ssl);  // frees SSL + BIO memory, no I/O
        self->ssl = NULL;
    }
    if (self->fd >= 0) {
        close(self->fd);      // close fd directly, no Python calls
        self->fd = -1;
    }
    self->sock = mp_const_none;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssl_socket___del___obj, ssl_socket___del__);

static const mp_rom_map_elem_t ssl_socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ssl_socket___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ssl_socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&ssl_socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ssl_socket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&ssl_socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&ssl_socket_send_obj) },
};
static MP_DEFINE_CONST_DICT(ssl_socket_locals_dict, ssl_socket_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ssl_socket_type,
    MP_QSTR_SSLSocket,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ssl_socket_locals_dict
);

// --- ssl.wrap_socket() ---

static mp_obj_t mod_ssl_wrap_socket(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_obj_t sock = pos_args[0];
    mp_arg_val_t args[1];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, 1, allowed_args, args);

    ensure_amissl_init();

    // Get the fd from the Python socket via fileno()
    mp_obj_t fileno_method = mp_load_attr(sock, MP_QSTR_fileno);
    int fd = mp_obj_get_int(mp_call_function_0(fileno_method));

    SSL *ssl = SSL_new(global_ctx);
    if (!ssl) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SSL_new failed"));
    }

    // Create custom BIO that uses libnix send()/recv() with the libnix fd
    BIO *bio = BIO_new(BIO_s_amiga());
    BIO_set_data(bio, (void *)fd);
    SSL_set_bio(ssl, bio, bio);

    // Set SNI hostname if provided
    if (args[0].u_obj != mp_const_none) {
        const char *hostname = mp_obj_str_get_str(args[0].u_obj);
        SSL_set_tlsext_host_name(ssl, hostname);
    }

    int ret = SSL_connect(ssl);
    if (ret != 1) {
        SSL_free(ssl);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SSL handshake failed"));
    }

    mp_obj_ssl_socket_t *o = mp_obj_malloc(mp_obj_ssl_socket_t, &ssl_socket_type);
    o->sock = sock;
    o->fd = fd;
    o->ssl = ssl;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_ssl_wrap_socket_obj, 1, mod_ssl_wrap_socket);

// Module globals
static const mp_rom_map_elem_t ssl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ssl) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket), MP_ROM_PTR(&mod_ssl_wrap_socket_obj) },
    { MP_ROM_QSTR(MP_QSTR_SSLSocket), MP_ROM_PTR(&ssl_socket_type) },
};
static MP_DEFINE_CONST_DICT(ssl_module_globals, ssl_module_globals_table);

const mp_obj_module_t mp_module_ssl = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ssl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ssl, mp_module_ssl);
