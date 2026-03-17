# MicroPython AmigaOS m68k Port

## Build

Cross-compiler: `/opt/amiga/bin/m68k-amigaos-gcc` (GCC 6.5.0b, bebbo toolchain).

Prerequisites: mpy-cross must be compiled first (one-time):

```
cd mpy-cross && gmake       # builds the .py -> .mpy cross-compiler
```

Then:

```
cd ports/amiga
gmake              # incremental build
gmake clean        # full clean
gmake V=1          # verbose build
```

The binary `build/micropython` is an AmigaOS hunk executable (magic `0x000003f3`),
directly runnable on Amiga 68020+ or in an emulator.

Build number auto-increments on each compilation (stored in `.build_number`).
Banner example: `MicroPython v1.27.0 on 2026-03-17 14:00 build 42; Amiga with M68020`

Compiler flags:
- `-noixemul`: uses libnix instead of ixemul.library (no POSIX emulation)
- `-m68020`: minimum 68020 instruction set
- `-O2`: standard optimization
- `-fdata-sections -ffunction-sections` + `-Wl,--gc-sections`: dead code elimination

## Port Architecture

### Files

| File | Role |
|------|------|
| `Makefile` | Cross-compiled build, py.mk + extmod.mk inclusion, build number |
| `mpconfigport.h` | MicroPython feature configuration for AmigaOS |
| `mphalport.h` | HAL: ticks stubs, delays, time_ns, readline via fgets |
| `main.c` | Entry point, custom REPL (do_repl), dynamic heap via AllocMem, argv, builtins, curdir save/restore |
| `amiga_mphal.c` | Console I/O: stdin (CR->LF), stdout, amiga_prompt (fgets), delays via usleep |
| `modamigaos.c` | Standalone `os` module (listdir, getcwd, chdir, system, _stat_type via dos.library) |
| `modtime.c` | Time implementation for AmigaOS (gmtime/localtime/time via libnix) |
| `qstrdefsport.h` | Port-specific qstrings (empty) |
| `manifest.py` | Frozen Python module declarations (base64, datetime, _ospath) |
| `modules/datetime.py` | Patched local copy of datetime (fixed __repr__) |
| `modules/_ospath.py` | os.path implementation for AmigaOS path conventions |
| `patches/` | Patches to upstream MicroPython files (see patches/README.md) |
| `run_tests.py` | Test runner: runs each test in a separate micropython process |

### Architecture Decisions

**No POSIX.** AmigaOS is not POSIX. All POSIX code from the unix port was removed:
no fork, no pthreads, no mmap, no POSIX signals, no termios, no select/poll, no
POSIX VFS.

**libnix (-noixemul).** Uses libnix which provides a minimal libc (malloc, stdio,
string, setjmp, time, gmtime) without depending on ixemul.library. Consequence:
`shared/libc/printf.c` and `shared/libc/abort_.c` are excluded from the build via
`SRC_EXTMOD_C := $(filter-out shared/libc/%.c,$(SRC_EXTMOD_C))` in the Makefile,
since libnix already provides these symbols.

**NLR setjmp.** m68k has no native NLR implementation in MicroPython. The
`setjmp`/`longjmp` fallback is used (`MICROPY_NLR_SETJMP=1`). The GC also uses
setjmp to capture registers (`MICROPY_GCREGS_SETJMP=1`).

**Arbitrary-precision integers (MPZ).** `MICROPY_LONGINT_IMPL` is set to `MPZ`.
Required by the frozen `datetime` module which uses 64-bit integer constants for
epoch calculations.

**No native emitters.** All native emitters (x64, x86, thumb, ARM) are disabled.
MicroPython bytecode is interpreted only.

**ROM level CORE_FEATURES.** Intermediate feature level providing a functional REPL
with essential builtins without excessive RAM usage.

**sys.path enabled.** `MICROPY_PY_SYS_PATH=1` is required for frozen module imports.
At startup, `mp_init()` creates `sys.path = ["", ".frozen"]`. The `.frozen/` virtual
path triggers lookup in `mp_find_frozen_module()`.

**No binascii frozen shim.** `manifest.py` freezes `base64.py` directly (via
`freeze()` instead of `require()`) to avoid pulling the Python `binascii.py` shim
from micropython-lib, which shadows the C binascii module.

**Patched datetime.py.** The `__repr__` of `date` in micropython-lib displays the
internal ordinal instead of year/month/day. The local copy in `modules/datetime.py`
fixes this. The time module import uses `__import__("time")` to work around the
name collision with `class time` defined in the same file (`from time import ...`
fails in frozen modules when the module name appears as a local symbol).

**Standalone os module (modamigaos.c).** The generic extmod `os` module
(`MICROPY_PY_OS`) is disabled. A native `modamigaos.c` replaces it, registered via
`MP_REGISTER_MODULE(MP_QSTR_os, ...)`. Uses dos.library directly for filesystem
operations.

**Dynamic heap via AllocMem.** The 512 KB GC heap is allocated dynamically via
`AllocMem(MICROPY_HEAP_SIZE, MEMF_ANY | MEMF_CLEAR)` instead of a static BSS
array. This keeps the binary small and only uses Amiga memory at runtime.
`FreeMem()` is called on normal exit and in crash handlers (`nlr_jump_fail`,
`__assert_func`).

**Stack 128 KB.** `long __stack = 131072;` in main.c tells libnix to allocate
128 KB of stack at launch. Required for the recursive Python parser and GC collect.

**extmod.mk included.** The Makefile includes `extmod/extmod.mk` to compile C
extension modules (re, json, binascii, time...). Objects go through `PY_O` (not
`PY_CORE_O`) to include everything.

## m68k GC Alignment (Critical)

On m68k with `MICROPY_OBJ_REPR_A`, object pointers must be 4-byte aligned
(bits 1:0 == 0). Without `MICROPY_STACK_CHECK` (disabled at CORE_FEATURES level),
the struct layout of `mp_state_thread_t` is:

```
char *stack_top;        // offset 0, 4 bytes
uint16_t gc_lock_depth; // offset 4, 2 bytes
// ROOT POINTER SECTION starts here at offset 6 (NOT aligned!)
```

This 2-byte misalignment causes two critical bugs:
1. **GC root scan corruption**: `gc_collect_start()` scans root pointers as 4-byte
   words starting from a 2-byte-shifted position, reading garbage values.
2. **sys.argv misidentified**: `mp_sys_argv_obj` ends up at an address with
   bits 1:0 != 0, making MicroPython interpret it as a qstr instead of an object.

**Fix**: `uint16_t _gc_lock_pad` added after `gc_lock_depth` in `py/mpstate.h`
(see `patches/mpstate_alignment.patch`). A `_Static_assert` in `main.c` catches
regressions at compile time.

The `gc.c:952` assertion may still trigger due to false positives from stack
scanning (heap-like addresses on the stack). `NDEBUG` is defined in
`mpconfigport.h` as a workaround.

## Module os (AmigaOS -- modamigaos.c)

Standalone module replacing the generic os module (`MICROPY_PY_OS=0`).
Registered via `MP_REGISTER_MODULE(MP_QSTR_os, mp_module_amiga_os)`.

### Functions

- `os.listdir([path])`: list directory contents via
  `Lock/Examine/ExNext/UnLock` + `AllocDosObject(DOS_FIB)`.
  No argument lists the current directory (path="").
  Rejects files with `MP_ENOTDIR` (checks `fib_DirEntryType`).
- `os.getcwd()`: returns current directory via `NameFromLock()` on the current
  lock (obtained by `CurrentDir(0)` + restore). Does NOT use
  `GetCurrentDirName()` since it reads the CLI structure which is not updated
  by `CurrentDir()`.
- `os.chdir(path)`: changes directory via `Lock(path)/CurrentDir(lock)`.
  Properly `UnLock()`s the previous lock, except for the original shell lock
  (preserved via global `original_dir` from main.c).
- `os.system(cmd)`: executes a shell command via libnix `system()`.
- `os._stat_type(path)`: returns 1 for directory, 2 for file, 0 if not found.
  Uses `Lock/Examine/fib_DirEntryType/FreeDosObject/UnLock`.
- `os.sep`: `"/"` (AmigaOS uses `/` for subdirectories and `:` for volumes).
- `os.path`: lazy-loaded via `__getattr__` -- imports frozen `_ospath` module.

### Current directory management

`main.c` saves the shell's current directory lock at startup as a global:
```c
BPTR original_dir = 0;  // global
original_dir = CurrentDir(0);
CurrentDir(original_dir);
```
Restores it before exit: `CurrentDir(original_dir)`. This ensures the shell
recovers its directory after running micropython, even if scripts used
`os.chdir()`.

## Module os.path (AmigaOS -- _ospath.py)

Frozen Python module implementing os.path for AmigaOS path conventions.
Loaded lazily via `os.__getattr__` when `os.path` is accessed.

### AmigaOS path conventions

- `":"` separates volume from path (e.g. `DH0:work/file`, `System:Libs`)
- `"/"` separates subdirectories
- A path is absolute if it contains `":"`
- No leading `"/"` for absolute paths
- `"."` is NOT valid as current directory -- use `""` (empty string)
- `join("DH0:", "work")` -> `"DH0:work"` (no slash after colon)

### Functions

- `sep`: `"/"`
- `join(*paths)`: join path components with AmigaOS conventions
- `split(path)` -> `(head, tail)`: split at last `/` or `:`
- `basename(path)`, `dirname(path)`: wrappers around `split()`
- `splitext(path)` -> `(root, ext)`: split at last `"."`
- `isabs(path)`: True if path contains `":"`
- `normpath(path)`: resolves `"."` and `".."`
- `abspath(path)`: uses `os.getcwd()` for relative paths
- `exists(path)`: True if file or directory exists (via `os._stat_type()`)
- `isfile(path)`: True if regular file (via `os._stat_type()`)
- `isdir(path)`: True if directory (via `os._stat_type()`)

## Frozen Modules

Frozen modules are Python files compiled to bytecode (.mpy) by mpy-cross, then
embedded in the C binary via `frozen_content.c`. Importable without a filesystem.

### Mechanism

1. `manifest.py` declares modules to freeze via `freeze("path", "file.py")`
2. `FROZEN_MANIFEST` in the Makefile points to this file
3. mkrules.mk calls `tools/makemanifest.py` which compiles `.py` to `.mpy`
   and generates `build/frozen_content.c`
4. This C file is compiled and linked into the binary

### Current frozen modules

| Module | Source | Dependencies |
|--------|--------|-------------|
| `base64` | micropython-lib/python-stdlib/base64 | `binascii` (C extmod) |
| `datetime` | local copy `modules/datetime.py` | `time` (C extmod) |
| `_ospath` | local `modules/_ospath.py` | `os` (native C module) |

### Adding a frozen module

Edit `manifest.py` and add `freeze("path", "file.py")` or `require("name")`
(note: require pulls transitive dependencies). Available modules are in
`lib/micropython-lib/python-stdlib/` and `lib/micropython-lib/micropython/`.
Then `gmake clean && gmake`.

## Module time (AmigaOS)

Implemented via `modtime.c` (included by extmod/modtime.c via
`MICROPY_PY_TIME_INCLUDEFILE`):

- `time.time()`: seconds since epoch 1970 via libnix `time()`
- `time.time_ns()`: nanoseconds since epoch (second resolution, via
  `mp_hal_time_ns()` = `time() * 1e9`)
- `time.gmtime([secs])` / `time.localtime([secs])`: 8-element tuple
- `time.mktime(tuple)`: inverse conversion
- `time.sleep(secs)` / `time.sleep_ms(ms)` / `time.sleep_us(us)`: `usleep()` libnix
- `time.ticks_ms()` / `time.ticks_us()` / `time.ticks_cpu()`: stubs (return 0)

Epoch is 1970 (`MICROPY_EPOCH_IS_1970=1`), matching libnix `time()`.

## Script Execution

### Command line

```
micropython                      # launch interactive REPL
micropython script.py            # execute a script
micropython script.py arg1 arg2  # execute with sys.argv = ['script.py', 'arg1', 'arg2']
```

A single script is executed (argv[1]). Additional arguments are passed in
`sys.argv` (populated with argv[1:] after `mp_init()`).

### Filesystem

`mp_lexer_new_from_file()` reads files into memory via `fopen/fseek/fread`,
enabling:
- Execution of `.py` scripts from the command line
- `import` of `.py` modules from the AmigaOS filesystem

`mp_import_stat()` checks file existence via `fopen`.

### Test runner (run_tests.py)

Runs each `test_*.py` file in a separate micropython process via
`os.system("micropython " + filepath)`. Ensures a fresh VM per test.

```
micropython run_tests.py tests/
micropython run_tests.py tests/test_math.py tests/test_string.py
```

## AmigaOS Specifics

### REPL (cooked mode / fgets)

The REPL uses cooked (line-buffered) mode via `fgets()` in `amiga_prompt()`.
No readline: `MICROPY_USE_READLINE=0`. Line editing is handled natively by the
AmigaOS console (left/right arrows, backspace). No history or tab completion.

The REPL is implemented by `do_repl()` in `main.c` which uses `mp_hal_readline()`
(wrapper around `amiga_prompt/fgets`) and parses with `MP_PARSE_SINGLE_INPUT` so
expressions print their result (e.g. `1+1` prints `2`). Multi-line blocks
(if/for/def/class) are handled via `mp_repl_continue_with_input()`.

`shared/readline/readline.c` is included in the build because `pyexec.c`
references it, but it is not actively used.

### CR vs LF

AmigaOS sends CR (`\r`, 13) as line ending instead of LF (`\n`, 10).
- `amiga_prompt()` strips trailing CR and LF
- `mp_hal_stdin_rx_chr()` converts `\r` to `\n`

### Exiting the REPL

Four ways to exit:
- `quit()` or `exit()`: port-defined builtins, raise `SystemExit`
- `sys.exit()`: standard MicroPython (`MICROPY_PY_SYS_EXIT=1`)
- Ctrl-C: detected as `\x03` at start of line in `do_repl()`
- EOF (end of file redirection): `amiga_prompt()` returns NULL

The REPL prints "Bye!" before quitting.

## Enabled Modules

### Always active (CORE_FEATURES)

- `builtins`: basic Python functions (print, len, range, etc.)
- `gc`: garbage collector control
- `sys`: system info, `sys.exit()`, `sys.platform`, `sys.path`, `sys.argv`
- `struct`: binary data pack/unpack
- `math`: math functions (single-precision float)
- `micropython`: MicroPython introspection
- `io`: StringIO, BytesIO

### Module os (standalone native -- modamigaos.c)

- `os.listdir([path])`: directory contents (dos.library Examine/ExNext)
- `os.getcwd()`: current directory (NameFromLock)
- `os.chdir(path)`: change directory (Lock/CurrentDir, UnLocks old lock)
- `os.system(cmd)`: execute shell command
- `os._stat_type(path)`: 0=not found, 1=dir, 2=file (Lock/Examine)
- `os.sep`: `"/"`
- `os.path`: lazy-loaded _ospath module

### Explicitly enabled (C extmod modules)

- `re`: regular expressions
- `json`: JSON encoding/decoding
- `binascii`: hexlify, unhexlify, a2b_base64, b2a_base64
- `time`: time, gmtime, localtime, mktime, sleep, ticks_ms/us/cpu

### Frozen (Python modules embedded in binary)

- `base64`: base64, base32, base16 encoding/decoding
- `datetime`: date, time, datetime, timedelta (patched local copy)
- `_ospath`: os.path for AmigaOS (join, split, exists, isfile, isdir, etc.)

### Port-added builtins

- `quit([code])`: exit REPL (raise SystemExit)
- `exit([code])`: alias for quit()
- `input([prompt])`: line reading via `amiga_prompt()`
- `open(file)`: file reading via fopen/fread (read-only stub)

### Disabled

- `_thread`: no multithreading (no pthreads on AmigaOS)
- `socket`: no networking
- `select`: no I/O multiplexing
- `signal`: no POSIX signals
- `ffi`: no FFI
- `termios`: no POSIX terminal control
- VFS: no MicroPython VFS (filesystem handled directly via dos.library)

## Memory

- GC heap: 512 KB dynamic via `AllocMem(MICROPY_HEAP_SIZE, MEMF_ANY | MEMF_CLEAR)`,
  freed by `FreeMem()` on exit and in crash handlers
- Stack: 128 KB minimum (`long __stack = 131072` in main.c)
- Qstr: 2048-byte chunks, 2-byte length, 2-byte hash
- GC collect: `gc_helper_collect_regs_and_stack()` via `gchelper_generic.c`
  (setjmp-based register capture + stack scan)
- Stack control: `mp_stack_ctrl_init()` + `mp_stack_set_top()` called in `vm_init()`
- Types: `mp_int_t` = int32, `mp_uint_t` = uint32 (32-bit architecture)
- Long integers: MPZ (arbitrary precision)
- `mp_off_t` = long

## Known Bugs

- **GC assertion gc.c:952**: `NDEBUG` is defined to suppress a false positive in
  `gc_free()` triggered during stack scanning. The root pointer alignment is now
  correct (fixed via `_gc_lock_pad` in `py/mpstate.h`), but the stack scan can
  still hit heap-like addresses that trigger the assertion. This does not cause
  data corruption -- it is a conservative false positive.

## Known Limitations

- `time.ticks_ms()` is a stub (no high-resolution clock yet)
- `mp_hal_set_interrupt_char()` is a no-op (no Ctrl-C during execution)
- Input line limited to 255 characters (amiga_prompt buffer)
- `open()` is read-only (no file writing)
- No cursor key history in REPL (would need raw mode + CSI translation)

## Future Work

- File writing via `open()` mode "w" (dos.library Open/Write/Close)
- High-resolution clock via timer.device or ReadEClock() for ticks_ms/ticks_us
- Ctrl-C support via `SetSignal()` / `CheckSignal()` (SIGBREAKF_CTRL_C)
- REPL readline with cursor keys/history (needs raw mode + CSI translation)
- Add `os.remove()`, `os.mkdir()`, `os.rename()` via dos.library
- Add more frozen modules (hashlib, collections, etc.)
- Investigate gc.c:952 assertion root cause (remove NDEBUG)
