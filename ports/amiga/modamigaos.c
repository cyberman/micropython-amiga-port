// Low-level 'uos' module for the AmigaOS port.
// The frozen 'os' module (os.py) re-exports everything from uos
// and adds makedirs() and walk().
// Uses dos.library for filesystem operations.

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

// os.listdir([path]) — list directory contents using Examine/ExNext.
static mp_obj_t mod_os_listdir(size_t n_args, const mp_obj_t *args) {
    const char *path = (n_args == 0) ? "" : mp_obj_str_get_str(args[0]);

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
            mp_obj_list_append(list, mp_obj_new_str(name, strlen(name)));
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
        return mp_obj_new_str(buf, strlen(buf));
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
    const char *path = mp_obj_str_get_str(path_in);
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
    const char *path = mp_obj_str_get_str(path_in);
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
    const char *path = mp_obj_str_get_str(path_in);
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
    const char *path = mp_obj_str_get_str(path_in);
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
    const char *path = mp_obj_str_get_str(path_in);
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
    const char *old_path = mp_obj_str_get_str(old_in);
    const char *new_path = mp_obj_str_get_str(new_in);
    if (!Rename((CONST_STRPTR)old_path, (CONST_STRPTR)new_path)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_os_rename_obj, mod_os_rename);

// os.stat(path) — return a 10-element stat_result tuple.
static mp_obj_t mod_os_stat(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
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
    // st_mode: directory = 0040755, file = 0100644
    mp_int_t st_mode = (fib->fib_DirEntryType > 0) ? 0040755 : 0100644;
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
};
static MP_DEFINE_CONST_DICT(os_module_globals, os_module_globals_table);

const mp_obj_module_t mp_module_amiga_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_uos, mp_module_amiga_os);
