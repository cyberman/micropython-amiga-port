/*
 * modarexx.c - ARexx IPC module for MicroPython AmigaOS port
 *
 * Phase 1: arexx.send(), arexx.exists(), arexx.ports()
 * Phase 2: arexx.Port() - persistent client with context manager
 *
 * Usage from Python:
 *   import arexx
 *
 *   # --- Phase 1: One-shot functions ---
 *   if arexx.exists("IBROWSE"):
 *       rc = arexx.send("IBROWSE", "HIDE")
 *       rc, result = arexx.send("IBROWSE", "QUERY ITEM=URL", result=True)
 *
 *   ports = arexx.ports()
 *
 *   # --- Phase 2: Persistent client ---
 *   port = arexx.Port("IBROWSE")
 *   port.send("SHOW")
 *   rc, url = port.send("QUERY ITEM=URL", result=True)
 *   port.close()
 *
 *   # Or with context manager:
 *   with arexx.Port("IBROWSE") as ib:
 *       ib.send("SHOW")
 *       rc, url = ib.send("QUERY ITEM=URL", result=True)
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/nlr.h"

#include <proto/exec.h>
#include <proto/rexxsyslib.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <rexx/storage.h>
#include <rexx/errors.h>

#include <string.h>

/* ====================================================================== */
/* Global state                                                           */
/* ====================================================================== */

// RexxSysBase is declared as extern struct RxsLib * by proto/rexxsyslib.h.
struct RxsLib *RexxSysBase = NULL;

/*
 * Linked list of open Port objects for cleanup at exit.
 * Each arexx_port_obj_t that has a live replyPort is linked here.
 * This ensures mod_arexx_deinit() can close everything even if the
 * user forgot close() and the GC didn't run.
 */
typedef struct _arexx_port_node {
    struct _arexx_port_node *next;
    struct MsgPort *replyPort;
} arexx_port_node_t;

static arexx_port_node_t *open_ports_head = NULL;

static void register_open_port(arexx_port_node_t *node) {
    node->next = open_ports_head;
    open_ports_head = node;
}

static void unregister_open_port(arexx_port_node_t *node) {
    arexx_port_node_t **pp = &open_ports_head;
    while (*pp != NULL) {
        if (*pp == node) {
            *pp = node->next;
            node->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static bool ensure_rexxsys(void) {
    if (RexxSysBase == NULL) {
        RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
        if (RexxSysBase == NULL) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("cannot open rexxsyslib.library"));
            return false;
        }
    }
    return true;
}

/* ====================================================================== */
/* Internal helper: send a command using a given reply port               */
/* ====================================================================== */

/*
 * Core send logic shared by arexx.send() and Port.send().
 * Takes an existing replyPort (caller manages its lifetime).
 * Returns: mp_obj_new_int(rc) or tuple(rc, result_str) depending on want_result.
 */
static mp_obj_t arexx_send_internal(struct MsgPort *replyPort,
                                     const char *portname,
                                     const char *command,
                                     bool want_result) {
    /* Create the RexxMsg */
    struct RexxMsg *rexxMsg = CreateRexxMsg(replyPort, NULL, NULL);
    if (rexxMsg == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot create RexxMsg"));
        return mp_const_none;
    }

    /* Create the argument string (the command) */
    rexxMsg->rm_Args[0] = (STRPTR)CreateArgstring(
        (CONST_STRPTR)command, strlen(command));
    if (rexxMsg->rm_Args[0] == NULL) {
        DeleteRexxMsg(rexxMsg);
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot create argstring"));
        return mp_const_none;
    }

    /* Set action flags */
    rexxMsg->rm_Action = RXCOMM;
    if (want_result) {
        rexxMsg->rm_Action |= RXFF_RESULT;
    }

    /* Find the target port and send (under Forbid for atomicity) */
    Forbid();
    struct MsgPort *targetPort = FindPort((CONST_STRPTR)portname);
    if (targetPort == NULL) {
        Permit();
        DeleteArgstring((UBYTE *)rexxMsg->rm_Args[0]);
        DeleteRexxMsg(rexxMsg);
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("ARexx port '%s' not found"), portname);
        return mp_const_none;
    }
    PutMsg(targetPort, &rexxMsg->rm_Node);
    Permit();

    /* Wait for the reply */
    WaitPort(replyPort);
    GetMsg(replyPort);

    /* Extract results */
    LONG rc = rexxMsg->rm_Result1;
    mp_obj_t result_str = mp_const_none;

    if (want_result && rc == 0 && rexxMsg->rm_Result2 != 0) {
        const char *res_ptr = (const char *)rexxMsg->rm_Result2;
        size_t res_len = strlen(res_ptr);
        /* Try UTF-8 first, fall back to bytes for Latin-1 AmigaOS strings */
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            result_str = mp_obj_new_str(res_ptr, res_len);
            nlr_pop();
        } else {
            result_str = mp_obj_new_bytes((const byte *)res_ptr, res_len);
        }
        DeleteArgstring((UBYTE *)rexxMsg->rm_Result2);
    }

    /* Cleanup message (but NOT the replyPort - caller manages that) */
    DeleteArgstring((UBYTE *)rexxMsg->rm_Args[0]);
    DeleteRexxMsg(rexxMsg);

    /* Return value */
    if (want_result) {
        mp_obj_t tuple[2];
        tuple[0] = mp_obj_new_int(rc);
        tuple[1] = result_str;
        return mp_obj_new_tuple(2, tuple);
    } else {
        return mp_obj_new_int(rc);
    }
}

/* ====================================================================== */
/* Phase 1: Module-level functions                                        */
/* ====================================================================== */

/* ---- arexx.exists(portname) ------------------------------------------ */

STATIC mp_obj_t mod_arexx_exists(mp_obj_t portname_obj) {
    const char *portname = mp_obj_str_get_str(portname_obj);
    struct MsgPort *port;

    Forbid();
    port = FindPort((CONST_STRPTR)portname);
    Permit();

    return mp_obj_new_bool(port != NULL);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_arexx_exists_obj, mod_arexx_exists);

/* ---- arexx.send(portname, command, result=False) --------------------- */

STATIC mp_obj_t mod_arexx_send(size_t n_args, const mp_obj_t *pos_args,
                                mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_portname, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_command,  MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_result,   MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const char *portname = mp_obj_str_get_str(args[0].u_obj);
    const char *command  = mp_obj_str_get_str(args[1].u_obj);
    bool want_result     = args[2].u_bool;

    if (!ensure_rexxsys()) {
        return mp_const_none;
    }

    /* Phase 1: ephemeral reply port for each call */
    struct MsgPort *replyPort = CreateMsgPort();
    if (replyPort == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot create reply port"));
        return mp_const_none;
    }

    mp_obj_t result = arexx_send_internal(replyPort, portname, command,
                                           want_result);

    DeleteMsgPort(replyPort);
    return result;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_arexx_send_obj, 2, mod_arexx_send);

/* ---- arexx.ports() --------------------------------------------------- */

STATIC mp_obj_t mod_arexx_ports(void) {
    mp_obj_t port_list = mp_obj_new_list(0, NULL);
    struct Node *node;

    Forbid();
    struct List *pubPortList = &SysBase->PortList;
    for (node = pubPortList->lh_Head; node->ln_Succ; node = node->ln_Succ) {
        if (node->ln_Name != NULL) {
            mp_obj_t name = mp_obj_new_str(node->ln_Name,
                                           strlen(node->ln_Name));
            mp_obj_list_append(port_list, name);
        }
    }
    Permit();

    return port_list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_arexx_ports_obj, mod_arexx_ports);

/* ====================================================================== */
/* Phase 2: arexx.Port class - persistent client                          */
/* ====================================================================== */

typedef struct _arexx_port_obj_t {
    mp_obj_base_t base;
    char portname[128];          /* target port name */
    struct MsgPort *replyPort;   /* persistent reply port (NULL = closed) */
    arexx_port_node_t node;      /* linked list node for global cleanup */
} arexx_port_obj_t;

/* Forward declaration of the type */
extern const mp_obj_type_t arexx_port_type;

/* ---- Port.__init__(portname) ----------------------------------------- */

STATIC mp_obj_t arexx_port_make_new(const mp_obj_type_t *type,
                                     size_t n_args, size_t n_kw,
                                     const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    if (!ensure_rexxsys()) {
        return mp_const_none;
    }

    const char *portname = mp_obj_str_get_str(args[0]);

    arexx_port_obj_t *self = mp_obj_malloc(arexx_port_obj_t, type);
    strncpy(self->portname, portname, sizeof(self->portname) - 1);
    self->portname[sizeof(self->portname) - 1] = '\0';

    /* Create the persistent reply port */
    self->replyPort = CreateMsgPort();
    if (self->replyPort == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("cannot create reply port"));
        return mp_const_none;
    }

    /* Register for global cleanup */
    self->node.replyPort = self->replyPort;
    register_open_port(&self->node);

    return MP_OBJ_FROM_PTR(self);
}

/* ---- Port.send(command, result=False) -------------------------------- */

STATIC mp_obj_t arexx_port_send(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_command, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_result,  MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    arexx_port_obj_t *self = MP_OBJ_TO_PTR(args[0].u_obj);
    const char *command    = mp_obj_str_get_str(args[1].u_obj);
    bool want_result       = args[2].u_bool;

    if (self->replyPort == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("Port is closed"));
        return mp_const_none;
    }

    return arexx_send_internal(self->replyPort, self->portname,
                                command, want_result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(arexx_port_send_obj, 2, arexx_port_send);

/* ---- Port.close() ---------------------------------------------------- */

STATIC mp_obj_t arexx_port_close(mp_obj_t self_in) {
    arexx_port_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->replyPort != NULL) {
        unregister_open_port(&self->node);
        DeleteMsgPort(self->replyPort);
        self->replyPort = NULL;
        self->node.replyPort = NULL;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(arexx_port_close_obj, arexx_port_close);

/* ---- Port.__enter__ / __exit__ (context manager) --------------------- */

STATIC mp_obj_t arexx_port_enter(mp_obj_t self_in) {
    arexx_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->replyPort == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("Port is closed"));
    }
    return self_in;  /* return self for 'as' binding */
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(arexx_port_enter_obj, arexx_port_enter);

STATIC mp_obj_t arexx_port_exit(size_t n_args, const mp_obj_t *args) {
    /* args[0] = self, args[1..3] = exc_type, exc_val, exc_tb (ignored) */
    arexx_port_close(args[0]);
    return mp_const_false;  /* don't suppress exceptions */
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(arexx_port_exit_obj, 4, 4,
                                             arexx_port_exit);

/* ---- Port.__del__ (GC safety net) ------------------------------------ */

STATIC mp_obj_t arexx_port_del(mp_obj_t self_in) {
    arexx_port_close(self_in);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(arexx_port_del_obj, arexx_port_del);

/* ---- Port attr handler (properties + fallback to locals_dict) --------- */
/*
 * Handles read-only properties (portname, closed) without parentheses.
 * For unknown attributes, sets dest[1] = MP_OBJ_SENTINEL to tell
 * MicroPython to continue lookup in locals_dict (for methods).
 * Pattern from extmod/modtls_mbedtls.c ssl_context_attr().
 */
STATIC void arexx_port_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    arexx_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_portname) {
            dest[0] = mp_obj_new_str(self->portname, strlen(self->portname));
        } else if (attr == MP_QSTR_closed) {
            dest[0] = mp_obj_new_bool(self->replyPort == NULL);
        } else {
            // Continue lookup in locals_dict for methods.
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

/* ---- Port locals dict (methods) -------------------------------------- */

STATIC const mp_rom_map_elem_t arexx_port_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send),      MP_ROM_PTR(&arexx_port_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&arexx_port_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&arexx_port_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&arexx_port_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),   MP_ROM_PTR(&arexx_port_del_obj) },
};
STATIC MP_DEFINE_CONST_DICT(arexx_port_locals_dict,
                             arexx_port_locals_dict_table);

/* ---- Port type definition -------------------------------------------- */

MP_DEFINE_CONST_OBJ_TYPE(
    arexx_port_type,
    MP_QSTR_Port,
    MP_TYPE_FLAG_NONE,
    make_new, arexx_port_make_new,
    attr, arexx_port_attr,
    locals_dict, &arexx_port_locals_dict
);

/* ====================================================================== */
/* Module cleanup                                                          */
/* ====================================================================== */

void mod_arexx_deinit(void) {
    /* Close any Port objects that were not properly closed */
    while (open_ports_head != NULL) {
        arexx_port_node_t *node = open_ports_head;
        open_ports_head = node->next;
        if (node->replyPort != NULL) {
            DeleteMsgPort(node->replyPort);
            node->replyPort = NULL;
        }
    }

    /* Close the library */
    if (RexxSysBase != NULL) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
    }
}

/* ====================================================================== */
/* Module definition                                                       */
/* ====================================================================== */

STATIC const mp_rom_map_elem_t arexx_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_arexx) },
    /* Phase 1 functions */
    { MP_ROM_QSTR(MP_QSTR_send),     MP_ROM_PTR(&mod_arexx_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_exists),   MP_ROM_PTR(&mod_arexx_exists_obj) },
    { MP_ROM_QSTR(MP_QSTR_ports),    MP_ROM_PTR(&mod_arexx_ports_obj) },
    /* Phase 2 class */
    { MP_ROM_QSTR(MP_QSTR_Port),     MP_ROM_PTR(&arexx_port_type) },
};
STATIC MP_DEFINE_CONST_DICT(arexx_module_globals, arexx_module_globals_table);

const mp_obj_module_t mp_module_arexx = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&arexx_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_arexx, mp_module_arexx);
