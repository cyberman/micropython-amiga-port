/*
 * modintuition.c - amiga.intuition native module.
 *
 * Phase 1: easy_request() + high-level wrappers auto_request() and message().
 * All three share an internal helper built around EasyRequestArgs() from
 * intuition.library.
 *
 * Usage from Python:
 *     import amiga.intuition as intuition
 *
 *     idx = intuition.easy_request(title="amigagames+",
 *                                  body="Install it?",
 *                                  buttons=["Install", "Cancel"])
 *     ok  = intuition.auto_request(body="Continue?",
 *                                  yes="Sure", no="Nope")   # bool
 *     intuition.message(body="Done.", button="OK")          # None
 *
 * IntuitionBase is auto-opened by libnix startup via libstubs' intuition.o
 * auto-open stub, so the module does not need its own OpenLibrary lifecycle.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mphal.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

// Maximum number of buttons accepted. EasyRequest has no hard limit but
// 8 is plenty for a dialog and keeps the on-screen layout reasonable.
#define INTUITION_MAX_BUTTONS 8

// Convert a UTF-8 C string to Latin-1 into a freshly-allocated m_new buffer.
// Caller must m_del the returned pointer with size (utf8_len + 1).
// `*out_len` receives the Latin-1 byte length (excluding trailing NUL).
static char *utf8_to_latin1_buf(const char *utf8, size_t utf8_len,
                                size_t *out_len) {
    char *buf = m_new(char, utf8_len + 1);
    const char *res = amiga_utf8_to_latin1(utf8, buf, utf8_len + 1);
    // amiga_utf8_to_latin1 returns `utf8` directly if pure ASCII; copy in
    // that case so the caller owns a single allocation of known size.
    if (res != buf) {
        memcpy(buf, utf8, utf8_len + 1);
    }
    *out_len = strlen(buf);
    return buf;
}

// Same, but extracts the UTF-8 source from an mp_obj_t (must be a str).
static char *str_obj_to_latin1(mp_obj_t obj, size_t *out_len) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected str"));
    }
    const char *utf8 = mp_obj_str_get_str(obj);
    return utf8_to_latin1_buf(utf8, strlen(utf8), out_len);
}

// Reject non-str / empty / pipe-containing string arguments. Uses the qstr
// name for a clean error message ("'yes' must be non-empty" etc.).
static void check_label_arg(qstr name, mp_obj_t obj) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_msg_varg(&mp_type_TypeError,
            MP_ERROR_TEXT("'%q' must be str"), name);
    }
    const char *s = mp_obj_str_get_str(obj);
    size_t len = strlen(s);
    if (len == 0) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("'%q' must be non-empty"), name);
    }
    if (memchr(s, '|', len) != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("'%q' must not contain '|'"), name);
    }
}

// Core engine shared by easy_request / auto_request / message.
//
// title_utf8, body_utf8: raw C strings (caller owns storage; must stay alive
//   for the duration of the call -- they are pointers into either a literal
//   "" or into a Python str object still rooted on the caller's stack).
//   title_utf8 may be "" (empty) but must not be NULL.
// buttons_list: a Python list or tuple of 1..INTUITION_MAX_BUTTONS str labels
//   (each non-empty, no '|'). Validated here.
// return_none: if true, returns mp_const_none regardless of which button
//   was clicked (used by message()).
// return_bool: if true and return_none is false, returns mp_const_true
//   when index == 0 else mp_const_false (used by auto_request()).
// Otherwise returns MP_OBJ_NEW_SMALL_INT(index) (used by easy_request()).
static mp_obj_t do_easy_request_impl(const char *title_utf8,
                                     const char *body_utf8,
                                     mp_obj_t buttons_list,
                                     bool return_bool,
                                     bool return_none) {
    // --- Validate buttons (list/tuple of 1..N non-empty str, no '|') ---
    mp_obj_t *buttons;
    size_t n_buttons;
    mp_obj_get_array(buttons_list, &n_buttons, &buttons);
    if (n_buttons < 1 || n_buttons > INTUITION_MAX_BUTTONS) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("buttons must have between 1 and 8 items"));
    }
    for (size_t i = 0; i < n_buttons; i++) {
        if (!mp_obj_is_str(buttons[i])) {
            mp_raise_TypeError(MP_ERROR_TEXT("button labels must be str"));
        }
        const char *lab = mp_obj_str_get_str(buttons[i]);
        size_t lab_len = strlen(lab);
        if (lab_len == 0) {
            mp_raise_ValueError(
                MP_ERROR_TEXT("button labels must be non-empty"));
        }
        if (memchr(lab, '|', lab_len) != NULL) {
            mp_raise_ValueError(
                MP_ERROR_TEXT("button labels must not contain '|'"));
        }
    }

    // --- Convert all strings to Latin-1 on the GC heap ---
    size_t title_len;
    char *title_l1 = utf8_to_latin1_buf(title_utf8, strlen(title_utf8),
                                        &title_len);
    size_t title_cap = strlen(title_utf8) + 1;

    size_t body_len;
    char *body_l1 = utf8_to_latin1_buf(body_utf8, strlen(body_utf8),
                                       &body_len);
    size_t body_cap = strlen(body_utf8) + 1;

    char *labels_l1[INTUITION_MAX_BUTTONS];
    size_t labels_len[INTUITION_MAX_BUTTONS];
    size_t labels_cap[INTUITION_MAX_BUTTONS];
    size_t gadgets_total = 0;
    for (size_t i = 0; i < n_buttons; i++) {
        labels_l1[i] = str_obj_to_latin1(buttons[i], &labels_len[i]);
        labels_cap[i] = strlen(mp_obj_str_get_str(buttons[i])) + 1;
        gadgets_total += labels_len[i];
    }
    // (n_buttons - 1) '|' separators + 1 NUL
    size_t gadgets_cap = gadgets_total + n_buttons;
    char *gadgets_l1 = m_new(char, gadgets_cap);
    size_t pos = 0;
    for (size_t i = 0; i < n_buttons; i++) {
        memcpy(gadgets_l1 + pos, labels_l1[i], labels_len[i]);
        pos += labels_len[i];
        if (i + 1 < n_buttons) {
            gadgets_l1[pos++] = '|';
        }
    }
    gadgets_l1[pos] = '\0';

    // --- Fire the requester ---
    // "%s" as the fixed format, body as the only vararg -> no printf-injection
    // even if the user's body contains %s, %d or %n.
    struct EasyStruct es = {
        sizeof(struct EasyStruct),
        0,
        (STRPTR)title_l1,
        (STRPTR)"%s",
        (STRPTR)gadgets_l1,
    };
    ULONG sigmask = SIGBREAKF_CTRL_C;
    APTR va_args[] = { (APTR)body_l1 };
    LONG raw = EasyRequestArgs(NULL, &es, &sigmask, va_args);

    // --- Decide outcome, then free buffers before raising / returning ---
    bool ctrl_c = (raw == -1);
    int idx = 0;
    if (!ctrl_c) {
        if (n_buttons == 1) {
            // EasyRequest forces an "OK" gadget when only one label is
            // given and always returns 0; our API exposes index 0 too.
            idx = 0;
        } else if (raw == 0) {
            // 0 means "right-most button".
            idx = (int)n_buttons - 1;
        } else {
            // 1..N-1 -> 0..N-2 (left to right, excluding right-most).
            idx = (int)raw - 1;
        }
    }

    // Free in reverse allocation order.
    m_del(char, gadgets_l1, gadgets_cap);
    for (size_t i = n_buttons; i > 0; i--) {
        m_del(char, labels_l1[i - 1], labels_cap[i - 1]);
    }
    m_del(char, body_l1, body_cap);
    m_del(char, title_l1, title_cap);

    if (ctrl_c) {
        // Consume the signal: EasyRequestArgs leaves SIGBREAKF_CTRL_C posted
        // on its sigmask outparam, same cascade risk as modsocket.c (see
        // socket_raise_io_error). Raise KeyboardInterrupt directly so the
        // VM hook doesn't double-fire a pending KbdInt at POP_EXCEPT_JUMP.
        // Note: in practice intuition.library does not break out of an open
        // EasyRequest on SIGBREAKF_CTRL_C, so this path is defensive only.
        SetSignal(0, SIGBREAKF_CTRL_C);
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }

    if (return_none) {
        return mp_const_none;
    }
    if (return_bool) {
        return (idx == 0) ? mp_const_true : mp_const_false;
    }
    return MP_OBJ_NEW_SMALL_INT(idx);
}

// --- easy_request(*, title, body, buttons) -> int --------------------- //

static mp_obj_t mod_intuition_easy_request(size_t n_args,
                                           const mp_obj_t *pos_args,
                                           mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_title,   MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
          {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_body,    MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
          {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_buttons, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
          {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // title: must be str. Empty string is allowed (Intuition renders a
    // requester without a window title).
    if (!mp_obj_is_str(args[0].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("title must be str"));
    }
    if (!mp_obj_is_str(args[1].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("body must be str"));
    }

    return do_easy_request_impl(mp_obj_str_get_str(args[0].u_obj),
                                mp_obj_str_get_str(args[1].u_obj),
                                args[2].u_obj,
                                /*return_bool=*/false,
                                /*return_none=*/false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_easy_request_obj, 0,
                                  mod_intuition_easy_request);

// --- auto_request(*, body, yes="Yes", no="No") -> bool ---------------- //

static mp_obj_t mod_intuition_auto_request(size_t n_args,
                                           const mp_obj_t *pos_args,
                                           mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_body, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
          {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_yes,  MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_Yes)} },
        { MP_QSTR_no,   MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_No)} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_str(args[0].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("body must be str"));
    }
    check_label_arg(MP_QSTR_yes, args[1].u_obj);
    check_label_arg(MP_QSTR_no,  args[2].u_obj);

    mp_obj_t items[2] = { args[1].u_obj, args[2].u_obj };
    mp_obj_t buttons = mp_obj_new_tuple(2, items);

    return do_easy_request_impl("",
                                mp_obj_str_get_str(args[0].u_obj),
                                buttons,
                                /*return_bool=*/true,
                                /*return_none=*/false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_auto_request_obj, 0,
                                  mod_intuition_auto_request);

// --- message(*, body, button="OK") -> None ---------------------------- //

static mp_obj_t mod_intuition_message(size_t n_args,
                                      const mp_obj_t *pos_args,
                                      mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_body,   MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ,
          {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_button, MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_OK)} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_str(args[0].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("body must be str"));
    }
    check_label_arg(MP_QSTR_button, args[1].u_obj);

    mp_obj_t items[1] = { args[1].u_obj };
    mp_obj_t buttons = mp_obj_new_tuple(1, items);

    return do_easy_request_impl("",
                                mp_obj_str_get_str(args[0].u_obj),
                                buttons,
                                /*return_bool=*/false,
                                /*return_none=*/true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_message_obj, 0,
                                  mod_intuition_message);

// --- amiga.intuition sub-module ---------------------------------------- //

static const mp_rom_map_elem_t mp_module_amiga_intuition_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),
      MP_ROM_QSTR(MP_QSTR_amiga_dot_intuition) },
    { MP_ROM_QSTR(MP_QSTR_easy_request),
      MP_ROM_PTR(&mod_intuition_easy_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_auto_request),
      MP_ROM_PTR(&mod_intuition_auto_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_message),
      MP_ROM_PTR(&mod_intuition_message_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_amiga_intuition_globals,
                            mp_module_amiga_intuition_globals_table);

static const mp_obj_module_t mp_module_amiga_intuition = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_amiga_intuition_globals,
};

// --- amiga top-level package ------------------------------------------- //
// Sub-packages are exposed by placing them in the parent's globals dict
// (MICROPY_MODULE_BUILTIN_SUBPACKAGES, enabled by EVERYTHING level).
// Each sub-package is defined in its own translation unit and wired in
// here via an extern declaration.

extern const mp_obj_module_t mp_module_amiga_asl;

static const mp_rom_map_elem_t mp_module_amiga_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_amiga) },
    { MP_ROM_QSTR(MP_QSTR_intuition), MP_ROM_PTR(&mp_module_amiga_intuition) },
    { MP_ROM_QSTR(MP_QSTR_asl),       MP_ROM_PTR(&mp_module_amiga_asl) },
};
static MP_DEFINE_CONST_DICT(mp_module_amiga_globals,
                            mp_module_amiga_globals_table);

const mp_obj_module_t mp_module_amiga = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_amiga_globals,
};

MP_REGISTER_MODULE(MP_QSTR_amiga, mp_module_amiga);
