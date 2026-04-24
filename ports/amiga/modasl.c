/*
 * modasl.c - amiga.asl native module.
 *
 * Phase 1: file_request() wrapper around asl.library's FileRequester.
 * Validation, UTF-8 <-> Latin-1 conversion, and module exposure follow
 * modintuition.c patterns.
 *
 * Usage from Python:
 *     import amiga.asl as asl
 *
 *     # Simple open dialog
 *     path = asl.file_request(title="Open file",
 *                             initial_drawer="PROGDIR:")
 *     # -> "PROGDIR:myfile.txt" or None if cancelled
 *
 *     # Save dialog
 *     path = asl.file_request(title="Save as",
 *                             initial_file="untitled.txt",
 *                             save_mode=True)
 *
 *     # Multi-select
 *     paths = asl.file_request(multi_select=True)   # -> list[str] | None
 *
 *     # Pick a directory
 *     drawer = asl.file_request(drawers_only=True)
 *
 * AslBase is auto-opened by libnix startup via libstubs' asl.o stub
 * (an entry in ___LIB_LIST__ walked at process start, then unwound at
 * exit by ___exitlibraries). No OpenLibrary/CloseLibrary in this file.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <workbench/startup.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>

// Tag list capacity: at most 9 tags + TAG_END. 12 gives comfortable headroom.
#define ASL_MAX_TAGS  12

// AmigaOS practical full-path limit is ~255; 512 leaves room for UTF-8
// expansion on the way in and the AddPart() join on the way out.
#define ASL_PATH_MAX  512

// Convert a Python str (UTF-8) into a caller-provided Latin-1 buffer.
// `obj` must be a Python str; the empty string yields "". Always NUL-terminates.
static void str_obj_to_latin1_buf(mp_obj_t obj, char *buf, size_t bufsize) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected str"));
    }
    const char *utf8 = mp_obj_str_get_str(obj);
    const char *res = amiga_utf8_to_latin1(utf8, buf, bufsize);
    // amiga_utf8_to_latin1 may return `utf8` directly when pure ASCII; in
    // that case we still want our own copy in `buf` so the caller owns the
    // storage. Clamp to (bufsize - 1) to guarantee NUL termination.
    if (res != buf) {
        size_t n = strlen(utf8);
        if (n >= bufsize) {
            n = bufsize - 1;
        }
        memcpy(buf, utf8, n);
        buf[n] = '\0';
    }
}

// Convert Latin-1 bytes (from AmigaOS) back to a Python str (UTF-8).
// Same logic as the static helper in modamigaos.c; duplicated here to keep
// modasl.c self-contained. Latin-1 codepoints 0x80..0xFF become 2-byte UTF-8.
static mp_obj_t latin1_to_str_obj(const char *s, size_t len) {
    bool needs_conv = false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)s[i] > 0x7F) {
            needs_conv = true;
            break;
        }
    }
    if (!needs_conv) {
        return mp_obj_new_str(s, len);
    }
    // Worst case: every byte becomes 2 bytes.
    char utf8_buf[ASL_PATH_MAX * 2];
    size_t j = 0;
    for (size_t i = 0; i < len && j + 2 < sizeof(utf8_buf); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            utf8_buf[j++] = c;
        } else {
            utf8_buf[j++] = (char)(0xC0 | (c >> 6));
            utf8_buf[j++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return mp_obj_new_str(utf8_buf, j);
}

// Join drawer + filename via dos.library AddPart(). Raises OSError if the
// resulting path would exceed the local buffer (should not happen with
// ASL_PATH_MAX=512 on AmigaOS, but we check honestly). Returns a fresh
// Python str (Latin-1 -> UTF-8).
static mp_obj_t build_joined_path(const char *drawer, const char *filename) {
    char buf[ASL_PATH_MAX];
    size_t dn = strlen(drawer);
    if (dn >= sizeof(buf)) {
        dn = sizeof(buf) - 1;
    }
    memcpy(buf, drawer, dn);
    buf[dn] = '\0';
    if (!AddPart((STRPTR)buf, (CONST_STRPTR)filename, (ULONG)sizeof(buf))) {
        mp_raise_msg(&mp_type_OSError,
                     MP_ERROR_TEXT("joined path too long"));
    }
    return latin1_to_str_obj(buf, strlen(buf));
}

// --- file_request(**) -------------------------------------------------- //

static mp_obj_t mod_asl_file_request(size_t n_args,
                                     const mp_obj_t *pos_args,
                                     mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_title,           MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_initial_drawer,  MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_initial_file,    MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_initial_pattern, MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_save_mode,       MP_ARG_KW_ONLY | MP_ARG_BOOL,
          {.u_bool = false} },
        { MP_QSTR_multi_select,    MP_ARG_KW_ONLY | MP_ARG_BOOL,
          {.u_bool = false} },
        { MP_QSTR_drawers_only,    MP_ARG_KW_ONLY | MP_ARG_BOOL,
          {.u_bool = false} },
        { MP_QSTR_reject_icons,    MP_ARG_KW_ONLY | MP_ARG_BOOL,
          {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t title_obj   = args[0].u_obj;
    mp_obj_t drawer_obj  = args[1].u_obj;
    mp_obj_t file_obj    = args[2].u_obj;
    mp_obj_t pattern_obj = args[3].u_obj;
    bool save_mode    = args[4].u_bool;
    bool multi_select = args[5].u_bool;
    bool drawers_only = args[6].u_bool;
    bool reject_icons = args[7].u_bool;

    // --- Validate mutually-exclusive flag combinations ---
    // Raised BEFORE any Amiga call so a bad kwargs combination never
    // touches asl.library.
    if (save_mode && multi_select) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("save_mode and multi_select are mutually exclusive"));
    }
    if (save_mode && drawers_only) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("save_mode and drawers_only are mutually exclusive"));
    }

    // --- Prepare Latin-1 stack buffers for the string tags ---
    // Each buffer is only considered "present" when the caller passed a
    // non-None AND non-empty str. Empty strings are skipped so we don't
    // clear a sensible default inside asl.library.
    char title_buf[ASL_PATH_MAX];    bool has_title   = false;
    char drawer_buf[ASL_PATH_MAX];   bool has_drawer  = false;
    char file_buf[ASL_PATH_MAX];     bool has_file    = false;
    char pattern_buf[ASL_PATH_MAX];  bool has_pattern = false;

    if (title_obj != mp_const_none) {
        str_obj_to_latin1_buf(title_obj, title_buf, sizeof(title_buf));
        has_title = (title_buf[0] != '\0');
    }
    if (drawer_obj != mp_const_none) {
        str_obj_to_latin1_buf(drawer_obj, drawer_buf, sizeof(drawer_buf));
        has_drawer = (drawer_buf[0] != '\0');
    }
    if (file_obj != mp_const_none) {
        str_obj_to_latin1_buf(file_obj, file_buf, sizeof(file_buf));
        has_file = (file_buf[0] != '\0');
    }
    if (pattern_obj != mp_const_none) {
        str_obj_to_latin1_buf(pattern_obj, pattern_buf, sizeof(pattern_buf));
        has_pattern = (pattern_buf[0] != '\0');
    }

    // --- Build the tag list ---
    // Only the tags actually needed are appended; empty/default values are
    // omitted so asl.library keeps its own defaults.
    struct TagItem tags[ASL_MAX_TAGS];
    int t = 0;
    if (has_title) {
        tags[t].ti_Tag = ASLFR_TitleText;
        tags[t++].ti_Data = (ULONG)title_buf;
    }
    if (has_drawer) {
        tags[t].ti_Tag = ASLFR_InitialDrawer;
        tags[t++].ti_Data = (ULONG)drawer_buf;
    }
    if (has_file) {
        tags[t].ti_Tag = ASLFR_InitialFile;
        tags[t++].ti_Data = (ULONG)file_buf;
    }
    if (has_pattern) {
        tags[t].ti_Tag = ASLFR_InitialPattern;
        tags[t++].ti_Data = (ULONG)pattern_buf;
        // Pattern gadget must be made visible for the filter to be usable.
        tags[t].ti_Tag = ASLFR_DoPatterns;
        tags[t++].ti_Data = TRUE;
    }
    if (save_mode) {
        tags[t].ti_Tag = ASLFR_DoSaveMode;
        tags[t++].ti_Data = TRUE;
    }
    if (multi_select) {
        tags[t].ti_Tag = ASLFR_DoMultiSelect;
        tags[t++].ti_Data = TRUE;
    }
    if (drawers_only) {
        tags[t].ti_Tag = ASLFR_DrawersOnly;
        tags[t++].ti_Data = TRUE;
    }
    if (reject_icons) {
        tags[t].ti_Tag = ASLFR_RejectIcons;
        tags[t++].ti_Data = TRUE;
    }
    tags[t].ti_Tag = TAG_END;

    // --- Allocate and fire the requester ---
    struct FileRequester *req =
        (struct FileRequester *)AllocAslRequest(ASL_FileRequest, tags);
    if (req == NULL) {
        mp_raise_type(&mp_type_MemoryError);
    }

    BOOL ok = AslRequest((APTR)req, tags);

    if (!ok) {
        // User clicked Cancel (or a system error opened the requester).
        FreeAslRequest((APTR)req);
        return mp_const_none;
    }

    // --- Decode the result ---
    // CRITICAL: every Python string must be built BEFORE FreeAslRequest(),
    // because the requester owns rf_Dir, rf_File, and every wa_Name in
    // rf_ArgList. After Free those pointers are dangling.
    mp_obj_t result;

    if (multi_select) {
        LONG n = req->rf_NumArgs;
        result = mp_obj_new_list(0, NULL);
        for (LONG i = 0; i < n; i++) {
            const char *name = (const char *)req->rf_ArgList[i].wa_Name;
            mp_obj_list_append(result,
                build_joined_path((const char *)req->rf_Dir,
                                  name != NULL ? name : ""));
        }
    } else if (drawers_only) {
        // AddPart() with an empty filename returns the drawer untouched —
        // matching AmigaOS conventions for "this is the directory itself".
        result = build_joined_path((const char *)req->rf_Dir, "");
    } else {
        result = build_joined_path((const char *)req->rf_Dir,
                                   (const char *)req->rf_File);
    }

    FreeAslRequest((APTR)req);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_asl_file_request_obj, 0,
                                  mod_asl_file_request);

// --- amiga.asl sub-module --------------------------------------------- //
// The parent `amiga` package is registered in modintuition.c; it imports
// `mp_module_amiga_asl` via an extern declaration and adds it to its
// globals dict, which the MICROPY_MODULE_BUILTIN_SUBPACKAGES mechanism
// then exposes as `amiga.asl` to user code.

static const mp_rom_map_elem_t mp_module_amiga_asl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),
      MP_ROM_QSTR(MP_QSTR_amiga_dot_asl) },
    { MP_ROM_QSTR(MP_QSTR_file_request),
      MP_ROM_PTR(&mod_asl_file_request_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_amiga_asl_globals,
                            mp_module_amiga_asl_globals_table);

const mp_obj_module_t mp_module_amiga_asl = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_amiga_asl_globals,
};
