// Low-level 'uos' module for the AmigaOS port.
// The frozen 'os' module (os.py) re-exports everything from uos
// and adds makedirs() and walk().
// Uses dos.library for filesystem operations.
// Latin-1 ↔ UTF-8 conversion: AmigaOS uses Latin-1 for filenames,
// MicroPython stores strings as UTF-8. All path-taking functions convert
// UTF-8 → Latin-1 before calling AmigaOS, and listdir/getcwd convert
// Latin-1 → UTF-8 when returning filenames to Python.

#include <stdlib.h>
#include <string.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include <stdio.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <graphics/gfxbase.h>

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;

// Chipset detection bits from ChipRevBits0
#define GFXB_AA_ALICE  2
#define GFXB_AA_LISA   3
#define GFXB_HR_AGNUS  0
#define GFXB_HR_DENISE 1

// Create a MicroPython str from AmigaOS Latin-1 bytes with proper UTF-8 encoding.
// Latin-1 codepoints 0x80-0xFF become 2-byte UTF-8 sequences.
static mp_obj_t mp_obj_new_str_from_latin1(const char *s, size_t len) {
    // Fast path: pure ASCII (common case)
    int needs_conversion = 0;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)s[i] > 0x7F) {
            needs_conversion = 1;
            break;
        }
    }
    if (!needs_conversion) {
        return mp_obj_new_str(s, len);
    }
    // Worst case: every byte becomes 2 bytes (AmigaOS paths max ~256 chars)
    char utf8_buf[512];
    size_t j = 0;
    for (size_t i = 0; i < len && j < sizeof(utf8_buf) - 2; i++) {
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

// Extract a Latin-1 C string from a MicroPython str for passing to AmigaOS.
// Uses the shared amiga_utf8_to_latin1() from amiga_mphal.c.
extern const char *amiga_utf8_to_latin1(const char *s, char *buf, size_t bufsize);
static const char *get_latin1_path(mp_obj_t str_obj, char *buf, size_t bufsize) {
    const char *s = mp_obj_str_get_str(str_obj);
    return amiga_utf8_to_latin1(s, buf, bufsize);
}

// os.listdir([path]) — list directory contents using Examine/ExNext.
static mp_obj_t mod_os_listdir(size_t n_args, const mp_obj_t *args) {
    char path_buf[256];
    const char *path = (n_args == 0) ? "" : get_latin1_path(args[0], path_buf, sizeof(path_buf));

    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }

    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);

    if (Examine(lock, fib)) {
        // fib_DirEntryType > 0 means directory on AmigaOS
        if (fib->fib_DirEntryType < 0) {
            FreeDosObject(DOS_FIB, fib);
            UnLock(lock);
            mp_raise_OSError(MP_ENOTDIR);
        }
        while (ExNext(lock, fib)) {
            const char *name = (const char *)fib->fib_FileName;
            mp_obj_list_append(list, mp_obj_new_str_from_latin1(name, strlen(name)));
        }
    }

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_os_listdir_obj, 0, 1, mod_os_listdir);

// os.getcwd() — return current directory name.
static mp_obj_t mod_os_getcwd(void) {
    char buf[256];
    // Use NameFromLock on the actual current dir lock set by CurrentDir().
    // GetCurrentDirName() reads the CLI structure which is NOT updated
    // by CurrentDir(), so it would return a stale value after chdir().
    BPTR lock = CurrentDir(0);
    CurrentDir(lock); // restore
    if (lock && NameFromLock(lock, (STRPTR)buf, sizeof(buf))) {
        return mp_obj_new_str_from_latin1(buf, strlen(buf));
    }
    // lock==0 means boot volume root
    if (lock == 0) {
        return mp_obj_new_str("SYS:", 4);
    }
    mp_raise_OSError(MP_ENOENT);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_getcwd_obj, mod_os_getcwd);

// os.chdir(path) — change current directory.
// original_dir is saved in main.c at startup; we must not UnLock it.
extern BPTR original_dir;

static mp_obj_t mod_os_chdir(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    BPTR new_lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (new_lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    BPTR old_lock = CurrentDir(new_lock);
    if (old_lock && old_lock != original_dir) {
        UnLock(old_lock);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_chdir_obj, mod_os_chdir);

// os.system(cmd) — execute a shell command.
static mp_obj_t mod_os_system(mp_obj_t cmd_in) {
    const char *cmd = mp_obj_str_get_str(cmd_in);
    int r = system(cmd);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_system_obj, mod_os_system);

// os._stat_type(path) — return 1 for dir, 2 for file, 0 if not found.
static mp_obj_t mod_os_stat_type(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    int result = 0;
    if (Examine(lock, fib)) {
        result = (fib->fib_DirEntryType > 0) ? 1 : 2;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return MP_OBJ_NEW_SMALL_INT(result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_stat_type_obj, mod_os_stat_type);

// AmigaOS epoch is 1 Jan 1978, Unix epoch is 1 Jan 1970.
// Difference in seconds: 8 years (2 leap) = 2922 days * 86400 = 252460800.
#define AMIGA_EPOCH_OFFSET 252460800

// Convert AmigaOS DateStamp to Unix timestamp.
static mp_int_t datestamp_to_unix(const struct DateStamp *ds) {
    return (mp_int_t)((long)ds->ds_Days * 86400L
        + (long)ds->ds_Minute * 60L
        + (long)ds->ds_Tick / TICKS_PER_SECOND)
        + AMIGA_EPOCH_OFFSET;
}

// os.mkdir(path) — create a directory.
static mp_obj_t mod_os_mkdir(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    BPTR lock = CreateDir((CONST_STRPTR)path);
    if (lock == 0) {
        mp_raise_OSError(MP_EIO);
    }
    UnLock(lock);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_mkdir_obj, mod_os_mkdir);

// os.rmdir(path) — remove an empty directory.
static mp_obj_t mod_os_rmdir(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    // Verify it is a directory
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    int is_dir = 0;
    if (Examine(lock, fib)) {
        is_dir = (fib->fib_DirEntryType > 0);
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    if (!is_dir) {
        mp_raise_OSError(MP_ENOTDIR);
    }
    if (!DeleteFile((CONST_STRPTR)path)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_rmdir_obj, mod_os_rmdir);

// os.remove(path) — remove a file (not a directory).
static mp_obj_t mod_os_remove(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    // Verify it is NOT a directory
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    int is_dir = 0;
    if (Examine(lock, fib)) {
        is_dir = (fib->fib_DirEntryType > 0);
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    if (is_dir) {
        mp_raise_OSError(MP_EISDIR);
    }
    if (!DeleteFile((CONST_STRPTR)path)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_remove_obj, mod_os_remove);

// os.rename(old, new) — rename a file or directory.
static mp_obj_t mod_os_rename(mp_obj_t old_in, mp_obj_t new_in) {
    char old_buf[256], new_buf[256];
    const char *old_path = get_latin1_path(old_in, old_buf, sizeof(old_buf));
    const char *new_path = get_latin1_path(new_in, new_buf, sizeof(new_buf));
    if (!Rename((CONST_STRPTR)old_path, (CONST_STRPTR)new_path)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_os_rename_obj, mod_os_rename);

// os.stat(path) — return a 10-element stat_result tuple.
// Convert Unix permission mode (0o755, 0o666, etc.) to AmigaOS protection bits.
// Owner RWED bits are INVERTED on AmigaOS (set = denied).
// Group/Other bits are NOT inverted (set = allowed).
static ULONG unix_mode_to_amiga(mp_int_t mode) {
    ULONG prot = 0;
    // Owner bits (inverted: set bit = DENIED)
    if (!(mode & 0400)) prot |= FIBF_READ;
    if (!(mode & 0200)) { prot |= FIBF_WRITE; prot |= FIBF_DELETE; }
    if (!(mode & 0100)) prot |= FIBF_EXECUTE;
    // Group bits (not inverted: set bit = ALLOWED)
    if (mode & 0040) prot |= FIBF_GRP_READ;
    if (mode & 0020) prot |= FIBF_GRP_WRITE;
    if (mode & 0010) prot |= FIBF_GRP_EXECUTE;
    // Other bits (not inverted: set bit = ALLOWED)
    if (mode & 0004) prot |= FIBF_OTR_READ;
    if (mode & 0002) prot |= FIBF_OTR_WRITE;
    if (mode & 0001) prot |= FIBF_OTR_EXECUTE;
    return prot;
}

// Convert AmigaOS protection bits to Unix permission mode.
static mp_int_t amiga_to_unix_mode(ULONG prot) {
    mp_int_t mode = 0;
    // Owner bits (inverted)
    if (!(prot & FIBF_READ))    mode |= 0400;
    if (!(prot & FIBF_WRITE))   mode |= 0200;
    if (!(prot & FIBF_EXECUTE)) mode |= 0100;
    // Group bits (not inverted)
    if (prot & FIBF_GRP_READ)    mode |= 0040;
    if (prot & FIBF_GRP_WRITE)   mode |= 0020;
    if (prot & FIBF_GRP_EXECUTE) mode |= 0010;
    // Other bits (not inverted)
    if (prot & FIBF_OTR_READ)    mode |= 0004;
    if (prot & FIBF_OTR_WRITE)   mode |= 0002;
    if (prot & FIBF_OTR_EXECUTE) mode |= 0001;
    return mode;
}

static mp_obj_t mod_os_stat(mp_obj_t path_in) {
    char path_buf[256];
    const char *path = get_latin1_path(path_in, path_buf, sizeof(path_buf));
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        mp_raise_OSError(MP_EIO);
    }
    // st_mode: file type + real permissions from fib_Protection
    mp_int_t st_type = (fib->fib_DirEntryType > 0) ? 0040000 : 0100000;
    mp_int_t st_mode = st_type | amiga_to_unix_mode(fib->fib_Protection);
    mp_int_t st_size = fib->fib_Size;
    mp_int_t st_time = datestamp_to_unix(&fib->fib_Date);
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);

    mp_obj_t items[10];
    items[0] = MP_OBJ_NEW_SMALL_INT(st_mode);  // st_mode
    items[1] = MP_OBJ_NEW_SMALL_INT(0);         // st_ino
    items[2] = MP_OBJ_NEW_SMALL_INT(0);         // st_dev
    items[3] = MP_OBJ_NEW_SMALL_INT(1);         // st_nlink
    items[4] = MP_OBJ_NEW_SMALL_INT(0);         // st_uid
    items[5] = MP_OBJ_NEW_SMALL_INT(0);         // st_gid
    items[6] = MP_OBJ_NEW_SMALL_INT(st_size);   // st_size
    items[7] = mp_obj_new_int(st_time);          // st_atime
    items[8] = mp_obj_new_int(st_time);          // st_mtime
    items[9] = mp_obj_new_int(st_time);          // st_ctime
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_stat_obj, mod_os_stat);

// os.chmod(path, mode) — set file permissions using Unix mode (0o755, 0o666, etc.).
// Converts Unix permission bits to AmigaOS protection bits automatically.
static mp_obj_t mod_os_chmod(mp_obj_t path_obj, mp_obj_t mode_obj) {
    char path_buf[256];
    const char *path = get_latin1_path(path_obj, path_buf, sizeof(path_buf));
    mp_int_t mode = mp_obj_get_int(mode_obj);
    ULONG prot = unix_mode_to_amiga(mode);
    if (!SetProtection((CONST_STRPTR)path, prot)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_os_chmod_obj, mod_os_chmod);

// os.getprotect(path) — read file protection bits via Lock()/Examine().
static mp_obj_t mod_os_getprotect(mp_obj_t path_obj) {
    char path_buf[256];
    const char *path = get_latin1_path(path_obj, path_buf, sizeof(path_buf));
    BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib == NULL) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    BOOL ok = Examine(lock, fib);
    LONG prot = fib->fib_Protection;
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    if (!ok) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_int(prot);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_getprotect_obj, mod_os_getprotect);

// os.getenv(key[, default]) — read an environment variable via GetVar().
static mp_obj_t mod_os_getenv(size_t n_args, const mp_obj_t *args) {
    const char *key = mp_obj_str_get_str(args[0]);
    char buf[256];
    LONG len = GetVar((CONST_STRPTR)key, (STRPTR)buf, sizeof(buf),
                      GVF_GLOBAL_ONLY | LV_VAR);
    if (len < 0) {
        return (n_args > 1) ? args[1] : mp_const_none;
    }
    return mp_obj_new_str(buf, (size_t)len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_os_getenv_obj, 1, 2, mod_os_getenv);

// os.putenv(key, value) — set an environment variable via SetVar().
static mp_obj_t mod_os_putenv(mp_obj_t key_obj, mp_obj_t val_obj) {
    const char *key = mp_obj_str_get_str(key_obj);
    size_t val_len;
    const char *val = mp_obj_str_get_data(val_obj, &val_len);
    if (!SetVar((CONST_STRPTR)key, (CONST_STRPTR)val, val_len,
                GVF_GLOBAL_ONLY | LV_VAR)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_os_putenv_obj, mod_os_putenv);

// os.unsetenv(key) — delete an environment variable via DeleteVar().
static mp_obj_t mod_os_unsetenv(mp_obj_t key_obj) {
    const char *key = mp_obj_str_get_str(key_obj);
    DeleteVar((CONST_STRPTR)key, GVF_GLOBAL_ONLY | LV_VAR);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_os_unsetenv_obj, mod_os_unsetenv);

// uos._cpu() — detect CPU from SysBase->AttnFlags.
static mp_obj_t mod_os_cpu(void) {
    UWORD flags = SysBase->AttnFlags;
    const char *cpu;
    if (flags & AFF_68060)      cpu = "68060";
    else if (flags & AFF_68040) cpu = "68040";
    else if (flags & AFF_68030) cpu = "68030";
    else if (flags & AFF_68020) cpu = "68020";
    else                        cpu = "68000";
    return mp_obj_new_str(cpu, strlen(cpu));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_cpu_obj, mod_os_cpu);

// uos._fpu() — detect FPU from SysBase->AttnFlags.
static mp_obj_t mod_os_fpu(void) {
    UWORD flags = SysBase->AttnFlags;
    const char *fpu;
    if (flags & AFF_FPU40)       fpu = "68040/68060 internal";
    else if (flags & AFF_68882)  fpu = "68882";
    else if (flags & AFF_68881)  fpu = "68881";
    else                         fpu = "none";
    return mp_obj_new_str(fpu, strlen(fpu));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_fpu_obj, mod_os_fpu);

// uos._chipset() — detect chipset from GfxBase->ChipRevBits0.
static mp_obj_t mod_os_chipset(void) {
    const char *cs;
    if (GfxBase == NULL) {
        cs = "unknown";
    } else {
        UBYTE bits = GfxBase->ChipRevBits0;
        if ((bits & (1 << GFXB_AA_ALICE)) && (bits & (1 << GFXB_AA_LISA)))
            cs = "AGA";
        else if ((bits & (1 << GFXB_HR_AGNUS)) || (bits & (1 << GFXB_HR_DENISE)))
            cs = "ECS";
        else
            cs = "OCS";
    }
    return mp_obj_new_str(cs, strlen(cs));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_chipset_obj, mod_os_chipset);

// uos._kickstart() — return "version.revision" string.
static mp_obj_t mod_os_kickstart(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d",
             (int)SysBase->LibNode.lib_Version,
             (int)SysBase->SoftVer);
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_kickstart_obj, mod_os_kickstart);

// uos._chipmem() — return available chip memory in bytes.
static mp_obj_t mod_os_chipmem(void) {
    return mp_obj_new_int((mp_int_t)AvailMem(MEMF_CHIP));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_chipmem_obj, mod_os_chipmem);

// uos._fastmem() — return available fast memory in bytes.
static mp_obj_t mod_os_fastmem(void) {
    return mp_obj_new_int((mp_int_t)AvailMem(MEMF_FAST));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_os_fastmem_obj, mod_os_fastmem);

static const mp_rom_map_elem_t os_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_uos) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&mod_os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&mod_os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&mod_os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_system), MP_ROM_PTR(&mod_os_system_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&mod_os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&mod_os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&mod_os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&mod_os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&mod_os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
    { MP_ROM_QSTR(MP_QSTR__stat_type), MP_ROM_PTR(&mod_os_stat_type_obj) },
    { MP_ROM_QSTR(MP_QSTR__cpu), MP_ROM_PTR(&mod_os_cpu_obj) },
    { MP_ROM_QSTR(MP_QSTR__fpu), MP_ROM_PTR(&mod_os_fpu_obj) },
    { MP_ROM_QSTR(MP_QSTR__chipset), MP_ROM_PTR(&mod_os_chipset_obj) },
    { MP_ROM_QSTR(MP_QSTR__kickstart), MP_ROM_PTR(&mod_os_kickstart_obj) },
    { MP_ROM_QSTR(MP_QSTR__chipmem), MP_ROM_PTR(&mod_os_chipmem_obj) },
    { MP_ROM_QSTR(MP_QSTR__fastmem), MP_ROM_PTR(&mod_os_fastmem_obj) },
    { MP_ROM_QSTR(MP_QSTR_chmod), MP_ROM_PTR(&mod_os_chmod_obj) },
    { MP_ROM_QSTR(MP_QSTR_getprotect), MP_ROM_PTR(&mod_os_getprotect_obj) },
    { MP_ROM_QSTR(MP_QSTR_getenv), MP_ROM_PTR(&mod_os_getenv_obj) },
    { MP_ROM_QSTR(MP_QSTR_putenv), MP_ROM_PTR(&mod_os_putenv_obj) },
    { MP_ROM_QSTR(MP_QSTR_unsetenv), MP_ROM_PTR(&mod_os_unsetenv_obj) },
    // AmigaOS protection bit constants (RWED bits 0-3 are INVERTED: 0=allowed)
    { MP_ROM_QSTR(MP_QSTR_FIBF_DELETE), MP_ROM_INT(FIBF_DELETE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_EXECUTE), MP_ROM_INT(FIBF_EXECUTE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_WRITE), MP_ROM_INT(FIBF_WRITE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_READ), MP_ROM_INT(FIBF_READ) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_ARCHIVE), MP_ROM_INT(FIBF_ARCHIVE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_PURE), MP_ROM_INT(FIBF_PURE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_SCRIPT), MP_ROM_INT(FIBF_SCRIPT) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_HOLD), MP_ROM_INT(FIBF_HOLD) },
};
static MP_DEFINE_CONST_DICT(os_module_globals, os_module_globals_table);

const mp_obj_module_t mp_module_amiga_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_uos, mp_module_amiga_os);
