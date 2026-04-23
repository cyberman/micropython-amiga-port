/*
 * modintuition.c - amiga.intuition native module.
 *
 * Phase 1: easy_request() -- wraps EasyRequestArgs() from intuition.library.
 *
 * Usage from Python:
 *     import amiga.intuition as intuition
 *     idx = intuition.easy_request(
 *         title="amigagames+",
 *         body="Update package found.\nInstall it?",
 *         buttons=["Install", "Cancel"])
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

// Convert a MicroPython str to Latin-1 into a freshly-allocated m_new buffer.
// Caller must m_del the returned pointer. `*out_len` receives the Latin-1
// byte length (excluding trailing NUL). Raises TypeError if obj is not str.
static char *str_obj_to_latin1(mp_obj_t obj, size_t *out_len) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected str"));
    }
    const char *utf8 = mp_obj_str_get_str(obj);
    size_t utf8_len = strlen(utf8);
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

// easy_request(*, title, body, buttons) -> int
//
// Displays a modal EasyRequest() dialog on the Workbench screen. Returns
// the 0-based index of the clicked button (left-to-right). Raises
// KeyboardInterrupt if the user hits Ctrl-C while the requester is open.
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

    mp_obj_t title_obj   = args[0].u_obj;
    mp_obj_t body_obj    = args[1].u_obj;
    mp_obj_t buttons_obj = args[2].u_obj;

    // --- Validate title / body (type + basic constraints) ---
    if (!mp_obj_is_str(title_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("title must be str"));
    }
    if (!mp_obj_is_str(body_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("body must be str"));
    }
    if (strlen(mp_obj_str_get_str(title_obj)) == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("title must be non-empty"));
    }

    // --- Validate buttons (list/tuple of 1..8 non-empty str, no '|') ---
    mp_obj_t *buttons;
    size_t n_buttons;
    mp_obj_get_array(buttons_obj, &n_buttons, &buttons);
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
    char *title_l1 = str_obj_to_latin1(title_obj, &title_len);

    size_t body_len;
    char *body_l1 = str_obj_to_latin1(body_obj, &body_len);

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

    // --- Decide outcome, then free buffers before raising ---
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
    m_del(char, body_l1, body_len + 1);
    m_del(char, title_l1, title_len + 1);

    if (ctrl_c) {
        // Consume the signal: EasyRequestArgs leaves SIGBREAKF_CTRL_C posted
        // on its sigmask outparam, same cascade risk as modsocket.c (see
        // socket_raise_io_error). Raise KeyboardInterrupt directly so the
        // VM hook doesn't double-fire a pending KbdInt at POP_EXCEPT_JUMP.
        SetSignal(0, SIGBREAKF_CTRL_C);
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    return MP_OBJ_NEW_SMALL_INT(idx);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_easy_request_obj, 0,
                                  mod_intuition_easy_request);

// --- amiga.intuition sub-module ---------------------------------------- //

static const mp_rom_map_elem_t mp_module_amiga_intuition_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),
      MP_ROM_QSTR(MP_QSTR_amiga_dot_intuition) },
    { MP_ROM_QSTR(MP_QSTR_easy_request),
      MP_ROM_PTR(&mod_intuition_easy_request_obj) },
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

static const mp_rom_map_elem_t mp_module_amiga_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_amiga) },
    { MP_ROM_QSTR(MP_QSTR_intuition), MP_ROM_PTR(&mp_module_amiga_intuition) },
};
static MP_DEFINE_CONST_DICT(mp_module_amiga_globals,
                            mp_module_amiga_globals_table);

const mp_obj_module_t mp_module_amiga = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_amiga_globals,
};

MP_REGISTER_MODULE(MP_QSTR_amiga, mp_module_amiga);
