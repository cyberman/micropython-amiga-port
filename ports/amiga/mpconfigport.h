#include <stdint.h>

// MicroPython port configuration for AmigaOS m68k

// Use EVERYTHING level for maximum Python compatibility (f-strings, advanced
// builtins, OrderedDict, set operations, etc). Features not available on
// AmigaOS are explicitly disabled below.
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_EVERYTHING)

// Compiler and REPL
#define MICROPY_ENABLE_COMPILER         (1)
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_HELPER_REPL             (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT  (1)
#define MICROPY_PY_BUILTINS_INPUT       (1)
#define MICROPY_PY_BUILTINS_HELP        (1)
#define MICROPY_PY_BUILTINS_HELP_MODULES (1)

// Readline with history and cursor key support
#define MICROPY_USE_READLINE            (1)
#define MICROPY_USE_READLINE_HISTORY    (1)
#define MICROPY_READLINE_HISTORY_SIZE   (50)
#define MICROPY_HAL_HAS_STDIO_MODE_SWITCH (1)

// Use setjmp-based NLR (no native m68k NLR implementation)
#define MICROPY_NLR_SETJMP              (1)

// Memory
#define MICROPY_ALLOC_PATH_MAX          (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT  (16)
#define MICROPY_ALLOC_QSTR_CHUNK_INIT   (2048)
#define MICROPY_QSTR_BYTES_IN_LEN       (2)
#define MICROPY_QSTR_BYTES_IN_HASH      (2)
#define MICROPY_HEAP_SIZE               (128 * 1024) // 128 KB

// --- Features NOT available on AmigaOS ---

// No native code emitters (not m68k)
#define MICROPY_EMIT_X64                (0)
#define MICROPY_EMIT_X86                (0)
#define MICROPY_EMIT_THUMB              (0)
#define MICROPY_EMIT_ARM                (0)
#define MICROPY_EMIT_INLINE_THUMB       (0)
#define MICROPY_EMIT_INLINE_THUMB_FLOAT (0)
#define MICROPY_EMIT_INLINE_RV32        (0)

// No threading (no pthreads on AmigaOS)
#define MICROPY_PY_THREAD               (0)

// No POSIX-specific modules
#define MICROPY_PY_FFI                  (0)
#define MICROPY_PY_TERMIOS              (0)
#define MICROPY_PY_SIGNAL               (0)
#define MICROPY_PY_SELECT               (0)

// No network stack / hardware modules (we have our own modsocket.c/modssl.c)
#define MICROPY_PY_SOCKET               (0)
#define MICROPY_PY_NETWORK              (0)
#define MICROPY_PY_BLUETOOTH            (0)
#define MICROPY_PY_LWIP                 (0)
#define MICROPY_PY_OPENAMP              (0)
#define MICROPY_PY_MACHINE              (0)
#define MICROPY_PY_ONEWIRE             (0)

// We have our own os module (modamigaos.c)
#define MICROPY_PY_OS                   (0)

// We have our own ssl module (modssl.c)
#define MICROPY_PY_SSL                  (0)

// EVERYTHING enables sys std files but we don't provide stream objects
#define MICROPY_PY_SYS_STDFILES         (1)
#define MICROPY_PY_SYS_STDIO_BUFFER     (1)

// No atexit / executable
#define MICROPY_PY_SYS_ATEXIT           (0)
#define MICROPY_PY_SYS_EXECUTABLE       (0)
// DUPTERM redirects VFS POSIX stdout/stderr writes through mp_hal_stdout_tx_strn(),
// which does UTF-8 → Latin-1 conversion for the AmigaOS terminal.
#define MICROPY_PY_OS_DUPTERM           (1)

// No .mpy saving or marshal
#define MICROPY_PERSISTENT_CODE_SAVE    (0)
#define MICROPY_PERSISTENT_CODE_SAVE_FILE (0)
#define MICROPY_PERSISTENT_CODE_SAVE_FUN (0)
#define MICROPY_PERSISTENT_CODE_LOAD    (1)
#define MICROPY_PY_MARSHAL              (0)

// No alternative VFS implementations
#define MICROPY_VFS_FAT                 (0)
#define MICROPY_VFS_LFS1                (0)
#define MICROPY_VFS_LFS2                (0)
#define MICROPY_VFS_ROM                 (0)

// No raw file I/O (we use POSIX VFS)
#define MICROPY_PY_IO_FILEIO            (0)

// libnix missing math functions
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS (0)

// MD5/SHA1 require mbedTLS/axTLS (not available)
#define MICROPY_PY_HASHLIB_MD5          (0)
#define MICROPY_PY_HASHLIB_SHA1         (0)

// --- Features explicitly ENABLED ---

#define MICROPY_ENABLE_FINALISER        (1)
#define MICROPY_VFS                     (1)
#define MICROPY_VFS_POSIX               (1)
#define MICROPY_READER_VFS              (1)
#define MICROPY_PY_SYS_EXIT             (1)
#define MICROPY_PY_SYS_PATH             (1)
#define MICROPY_PY_SYS_ARGV             (1)
#define MICROPY_PY_SYS_PLATFORM         (1)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_TIME_INCLUDEFILE     "ports/amiga/modtime.c"
#define MICROPY_PY_TIME_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_strftime), MP_ROM_PTR(&mp_time_strftime_obj) },
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS    (1)
#define MICROPY_EPOCH_IS_1970           (1)
#define MICROPY_HAL_HAS_TIMEZONE        (1)
#define MICROPY_PY_DEFLATE_COMPRESS     (1)

// VFS POSIX filename conversion: UTF-8 → Latin-1 for AmigaOS
extern const char *amiga_utf8_to_latin1(const char *s, char *buf, size_t bufsize);
#define MICROPY_VFS_POSIX_CONVERT_PATH(path, buf, sz) amiga_utf8_to_latin1(path, buf, sz)
#define MICROPY_GCREGS_SETJMP           (1)

// Ctrl-C support: poll AmigaOS SIGBREAKF_CTRL_C every N bytecode operations.
// This enables KeyboardInterrupt during Python execution (loops, calls).
extern void mp_amiga_check_signals(void);
#define MICROPY_VM_HOOK_COUNT   (64)
#define MICROPY_VM_HOOK_INIT    static unsigned int vm_hook_div = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL    if (--vm_hook_div == 0) { vm_hook_div = MICROPY_VM_HOOK_COUNT; mp_amiga_check_signals(); }
#define MICROPY_VM_HOOK_LOOP    MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN  MICROPY_VM_HOOK_POLL

// quit() and exit() builtins
extern const struct _mp_obj_fun_builtin_var_t mp_builtin_quit_obj;
extern const struct _mp_obj_fun_builtin_var_t mp_builtin_exit_obj;
#define MICROPY_PORT_BUILTINS \
    { MP_ROM_QSTR(MP_QSTR_quit), MP_ROM_PTR(&mp_builtin_quit_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&mp_builtin_exit_obj) },

// Type definitions for m68k
typedef int32_t mp_int_t;
typedef uint32_t mp_uint_t;
typedef long mp_off_t;

#define MP_SSIZE_MAX INT32_MAX

#include <alloca.h>

#define MICROPY_HW_BOARD_NAME           "Amiga"
#define MICROPY_HW_MCU_NAME             "M68020"

// Override sys.version to show MicroPython version + build instead of git hash
#include "genhdr/amigaversion.h"
#define MICROPY_BANNER_NAME_AND_VERSION \
    "MicroPython v" MICROPY_VERSION_STRING " on " AMIGA_BUILD_TIMESTAMP \
    " build " AMIGA_BUILD_NUM

#define MP_STATE_PORT MP_STATE_VM
