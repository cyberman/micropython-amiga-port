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
Banner example: `MicroPython v1.27.0 on 2026-03-18 14:00 build 42; Amiga with M68020`

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
| `mphalport.h` | HAL: ticks stubs, delays, time_ns, raw/cooked mode switch, MP_HAL_RETRY_SYSCALL |
| `main.c` | Entry point, custom REPL with readline, dynamic heap via AllocMem, -c/-m options, VFS mount, curdir save/restore |
| `amiga_mphal.c` | Console I/O: raw mode Read(Input())/Write(Output()), CSI-to-ANSI translation, delays via usleep |
| `modamigaos.c` | Low-level `uos` C module (listdir, getcwd, chdir, mkdir, rmdir, remove, rename, stat, system, _stat_type) |
| `modsocket.c` | BSD socket module (socket, connect, bind, send, recv, getaddrinfo) via libsocket/bsdsocket.library |
| `modssl.c` | SSL/TLS module via AmiSSL (wrap_socket, custom BIO with saveds callbacks) |
| `modarexx.c` | ARexx IPC module (send, exists, ports) via rexxsyslib.library |
| `modzlib.c` | Native _zlib module with CRC32 for the frozen zlib module |
| `modtime.c` | Time implementation for AmigaOS (gmtime/localtime/time via libnix) |
| `qstrdefsport.h` | Port-specific qstrings (empty) |
| `manifest.py` | Frozen Python module declarations (base64, datetime, _ospath, os) |
| `modules/datetime.py` | Patched local copy of datetime (fixed __repr__, added strftime) |
| `modules/_ospath.py` | os.path implementation for AmigaOS path conventions |
| `modules/os.py` | Frozen os module: re-exports uos, adds makedirs() and walk(), imports _ospath as path |
| `modules/platform.py` | Frozen platform module: CPU/FPU/chipset/Kickstart detection via uos C helpers |
| `modules/urequests.py` | Frozen HTTP/1.1 client (GET, POST, PUT, DELETE, HEAD, chunked TE) |
| `modules/gzip.py` | Frozen gzip module: CPython-compatible compress()/decompress() |
| `patches/` | Patches to upstream MicroPython files (see patches/README.md) |
| `run_tests.py` | Test runner: runs each test in a separate micropython process |

### Architecture Decisions

**No POSIX.** AmigaOS is not POSIX. All POSIX code from the unix port was removed:
no fork, no pthreads, no mmap, no POSIX signals, no termios, no select/poll.

**libnix (-noixemul).** Uses libnix which provides a minimal libc (malloc, stdio,
string, setjmp, time, gmtime, open, read, write, stat, opendir...) without
depending on ixemul.library. Consequence: `shared/libc/printf.c` and
`shared/libc/abort_.c` are excluded from the build via
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

**ROM level EVERYTHING.** Maximum Python compatibility: f-strings, set operations,
OrderedDict, advanced slicing, descriptors, async/await, generators, and more.
Features not available on AmigaOS (threading, networking stack, native emitters,
machine module, alternative VFS, etc.) are explicitly disabled in mpconfigport.h.
`mp_stack_set_limit(40000)` is called in `vm_init()` (required because EVERYTHING
enables `MICROPY_STACK_CHECK`).

**VFS_POSIX for file I/O.** `MICROPY_VFS=1` and `MICROPY_VFS_POSIX=1` enable
full `open()`/`read()`/`write()`/`close()` support. The POSIX VFS is mounted at
`/` during `vm_init()`. libnix provides all required POSIX symbols (open, close,
read, write, stat, lseek, mkdir, rmdir, unlink, opendir, readdir, etc.).
`mp_lexer_new_from_file()` and `mp_import_stat()` are provided by VFS.
`MICROPY_PERSISTENT_CODE_LOAD=1` enables importing precompiled `.mpy` bytecode
files (produced by `mpy-cross`), which load faster and use less RAM than `.py`.

**sys.path enabled.** `MICROPY_PY_SYS_PATH=1` is required for frozen module imports.
At startup, `mp_init()` creates `sys.path = ["", ".frozen"]`. The `.frozen/` virtual
path triggers lookup in `mp_find_frozen_module()`.

**Two-layer os module (uos + os.py).** The C module `modamigaos.c` is registered
as `uos` (via `MP_REGISTER_MODULE(MP_QSTR_uos, ...)`). A frozen `os.py` does
`from uos import *` and adds `makedirs()`, `walk()`, and `import _ospath as path`.
This allows extending os with Python functions while keeping C functions fast.

**No binascii frozen shim.** `manifest.py` freezes `base64.py` directly (via
`freeze()` instead of `require()`) to avoid pulling the Python `binascii.py` shim
from micropython-lib, which shadows the C binascii module.

**Patched datetime.py.** The `__repr__` of `date` in micropython-lib displays the
internal ordinal instead of year/month/day. The local copy in `modules/datetime.py`
fixes this. The time module import uses `__import__("time")` to work around the
name collision with `class time` defined in the same file.

**Dynamic heap via AllocMem.** The GC heap (default 128 KB) is allocated dynamically
via `AllocMem(heap_size, MEMF_ANY | MEMF_CLEAR)`. Size is configurable via the
`-m` command line option. `FreeMem()` is called on normal exit and in crash handlers
(`nlr_jump_fail`, `__assert_func`). Available RAM is checked before allocation.

**Stack 128 KB + StackSwap.** `long __stack = 131072;` in main.c tells libnix to
allocate 128 KB of stack. Additionally, `ensure_stack()` checks the actual stack
at the start of `main()` and uses `StackSwap()` to allocate a 64 KB stack if the
shell didn't provide enough. After the swap, `stack_top` is set to the top of the
new stack (not main's frame on the old shell stack) — critical for GC stack
scanning. At exit, `restore_stack()` frees the allocation before `_exit()`.

**extmod.mk included.** The Makefile includes `extmod/extmod.mk` to compile C
extension modules (re, json, binascii, time, random, hashlib, errno...). Objects
go through `PY_O` (not `PY_CORE_O`) to include everything.

## m68k GC Alignment (Critical)

On m68k with `MICROPY_OBJ_REPR_A`, object pointers must be 4-byte aligned
(bits 1:0 == 0). Without the `_gc_lock_pad` fix,
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

The `gc.c:952` assertion was caused by `stack_top` pointing to the wrong stack
after `StackSwap()`. This is now fixed — `stack_top` is set to the top of the
swapped stack. Assertions are fully enabled (no `NDEBUG`).

## Command Line

```
micropython                          # launch interactive REPL
micropython script.py                # execute a script
micropython script.py arg1 arg2      # execute with sys.argv = ['script.py', 'arg1', 'arg2']
micropython -c "print('hello')"      # execute inline code
micropython -m 1024 script.py        # execute with 1024 KB heap
micropython -m 512                   # launch REPL with 512 KB heap
micropython script.mpy               # execute precompiled bytecode
```

The `-m <size_kb>` option must come first. Default heap is 128 KB.
The `-c <code>` option executes Python code and exits.

## Module uos / os (AmigaOS)

The C module `uos` (`modamigaos.c`) provides low-level filesystem operations
using dos.library. The frozen `os.py` re-exports everything from `uos` and adds
`makedirs()`, `walk()`, and `os.path`.

### C functions (uos / modamigaos.c)

- `listdir([path])`: directory contents via Lock/Examine/ExNext. Rejects files (ENOTDIR).
- `getcwd()`: current directory via NameFromLock on CurrentDir lock.
- `chdir(path)`: change directory, UnLocks old lock (preserves shell original_dir).
- `mkdir(path)`: create directory via CreateDir/UnLock.
- `rmdir(path)`: remove empty directory (verifies is dir, then DeleteFile).
- `remove(path)`: remove file (verifies is NOT dir, then DeleteFile).
- `rename(old, new)`: rename via dos.library Rename().
- `stat(path)`: 10-element tuple (mode, ino, dev, nlink, uid, gid, size, atime, mtime, ctime).
  DateStamp converted to Unix epoch (+252460800s offset).
- `system(cmd)`: execute shell command via libnix system().
- `_stat_type(path)`: 0=not found, 1=dir, 2=file (used by _ospath).
- `_cpu()`: CPU string from SysBase->AttnFlags (68000/68020/68030/68040/68060).
- `_fpu()`: FPU string from AttnFlags (none/68881/68882/68040-60 internal).
- `_chipset()`: "OCS", "ECS" or "AGA" from GfxBase->ChipRevBits0.
- `_kickstart()`: "version.revision" from SysBase (lib_Version + SoftVer).
- `_chipmem()`: available chip RAM in bytes (AvailMem).
- `_fastmem()`: available fast RAM in bytes (AvailMem).
- `getenv(key[, default])`: read environment variable via GetVar (GVF_GLOBAL_ONLY).
- `putenv(key, value)`: set environment variable via SetVar.
- `unsetenv(key)`: delete environment variable via DeleteVar.
- `chmod(path, flags)`: set protection bits via SetProtection (raw AmigaOS flags).
- `getprotect(path)`: read protection bits via Lock/Examine/fib_Protection.
- Constants: `FIBF_DELETE`, `FIBF_EXECUTE`, `FIBF_WRITE`, `FIBF_READ`,
  `FIBF_ARCHIVE`, `FIBF_PURE`, `FIBF_SCRIPT`, `FIBF_HOLD`.
  Note: RWED bits (0-3) are INVERTED on AmigaOS (0=allowed, 1=denied).
- `sep`: `"/"`.

### Python extensions (os.py)

- `makedirs(name, exist_ok=False)`: recursive directory creation with AmigaOS path support.
- `walk(top, topdown=True)`: directory tree generator yielding (dirpath, dirnames, filenames).
- `path`: the `_ospath` module (join, split, basename, dirname, exists, isfile, isdir, etc.).

### Current directory management

`main.c` saves the shell's current directory lock at startup as a global:
```c
BPTR original_dir = 0;  // global
original_dir = CurrentDir(0);
CurrentDir(original_dir);
```
Restores it before exit: `CurrentDir(original_dir)`.

## Module os.path (AmigaOS -- _ospath.py)

Frozen Python module implementing os.path for AmigaOS path conventions.
Loaded via `import _ospath as path` in the frozen `os.py`.

### AmigaOS path conventions

- `":"` separates volume from path (e.g. `DH0:work/file`, `System:Libs`)
- `"/"` separates subdirectories
- A path is absolute if it contains `":"`
- No leading `"/"` for absolute paths
- `"."` is NOT valid as current directory -- use `""` (empty string)
- `join("DH0:", "work")` -> `"DH0:work"` (no slash after colon)

### Functions

- `sep`, `join`, `split`, `basename`, `dirname`, `splitext`
- `isabs`, `normpath`, `abspath`
- `exists`, `isfile`, `isdir` (via `uos._stat_type()`)

## Frozen Modules

Frozen modules are Python files compiled to bytecode (.mpy) by mpy-cross, then
embedded in the C binary via `frozen_content.c`. Importable without a filesystem.

### Current frozen modules

| Module | Source | Dependencies |
|--------|--------|-------------|
| `base64` | micropython-lib/python-stdlib/base64 | `binascii` (C extmod) |
| `datetime` | local copy `modules/datetime.py` | `time` (C extmod) |
| `_ospath` | local `modules/_ospath.py` | `uos` (C module) |
| `os` | local `modules/os.py` | `uos` (C module), `_ospath` |
| `platform` | local `modules/platform.py` | `uos` (C module), `sys` |
| `urequests` | local `modules/urequests.py` | `socket`, `ssl` (C modules), `json` |
| `gzip` | local `modules/gzip.py` | `deflate`, `io` |
| `zlib` | local `modules/zlib.py` | `_zlib` (C module), `deflate`, `io` |
| `zipfile` | local `modules/zipfile.py` | `zlib`, `deflate`, `struct`, `os`, `io` |

### Adding a frozen module

Edit `manifest.py` and add `freeze("path", "file.py")` or `require("name")`
(note: require pulls transitive dependencies). Available modules are in
`lib/micropython-lib/python-stdlib/` and `lib/micropython-lib/micropython/`.
Then `gmake clean && gmake`.

## Module socket (AmigaOS -- modsocket.c)

Native C module providing BSD sockets via libnix `libsocket.a`, which wraps
AmigaOS `bsdsocket.library`. Requires a TCP/IP stack (e.g. Roadshow, Miami,
AmiTCP) to be running.

### Functions and classes

- `socket.socket(family, type, proto)`: create a socket (defaults: AF_INET, SOCK_STREAM, 0)
- `socket.getaddrinfo(host, port)`: DNS resolution, returns list of 5-tuples
- Constants: `AF_INET`, `AF_INET6`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOL_SOCKET`, `SO_REUSEADDR`

### Socket methods

- `connect(addr)`, `bind(addr)`, `listen([backlog])`, `accept()`
- `send(data)`, `recv(bufsize)`, `sendto(data, addr)`, `recvfrom(bufsize)`
- `setsockopt(level, optname, value)`, `setblocking(flag)` (no-op)
- `close()`, `fileno()`

### Cleanup

- Socket objects have a `__del__` finalizer that calls `close()` on the fd.
- `gc_sweep_all()` is called before `mp_deinit()` on all exit paths to ensure
  orphaned sockets are closed before shutdown.
- On crash (`nlr_jump_fail`, `__assert_func`), libnix closes bsdsocket.library
  via `exit()`, which releases all associated sockets.

### Limitations

- Sockets are always blocking (no non-blocking/timeout support)
- Linked with `-lsocket` in the Makefile

## Module arexx (AmigaOS -- modarexx.c)

Native C module providing ARexx inter-process communication via
`rexxsyslib.library`. Allows MicroPython scripts to send commands to any
ARexx-capable AmigaOS application.

### Functions

- `arexx.exists(portname)`: returns True if the named ARexx port exists
  (uses Forbid/FindPort/Permit for safe access)
- `arexx.send(portname, command, result=False)`: sends an ARexx command.
  Returns RC (int) if result=False, or (RC, result_string) tuple if result=True.
  Creates a private reply port, sends via PutMsg, waits with WaitPort.
  Result strings that aren't valid UTF-8 are returned as bytes (Latin-1 fallback).
- `arexx.ports()`: returns a list of all public port names (snapshot under Forbid)
- `arexx.Port(portname)`: persistent client class with reusable reply port.
  Methods: `send(command, result=False)`, `close()`. Properties: `portname`, `closed`.
  Context manager: `with arexx.Port("APP") as p: p.send("CMD")`
  `__del__` finalizer for GC cleanup. Open ports tracked in a linked list for
  `mod_arexx_deinit()` cleanup at exit.

### Cleanup

`mod_arexx_deinit()` is called before exit to close rexxsyslib.library.
The library is opened lazily on first `arexx.send()` call.

## Module ssl (AmigaOS -- modssl.c)

Native C module providing TLS via AmiSSL (AmigaOS OpenSSL wrapper). Requires
`amisslmaster.library` and AmiSSL v4+ installed on the Amiga.

### Architecture

- Uses a custom BIO that routes I/O through libnix `send()`/`recv()` with
  `__attribute__((saveds))` callbacks (required for A4 register restoration
  when called from AmiSSL shared library).
- AmiSSL initialization is fully manual (`OpenLibrary` → `InitAmiSSLMaster` →
  `OpenAmiSSL` → `InitAmiSSL` with `SocketBase` and `errno` pointer).
  No `-lamisslauto` to avoid destructor conflicts at exit.
- A single global `SSL_CTX` is shared across all connections.
- `amissl_cleanup()` does full teardown: `SSL_CTX_free` → `CleanupAmiSSL` →
  `CloseAmiSSL` → `CloseLibrary`.
- SSLSocket `__del__` is GC-safe: calls `SSL_free` + `close(fd)` directly,
  skips `SSL_shutdown` (I/O unsafe during GC) and Python method calls.

### Functions

- `ssl.wrap_socket(sock, server_hostname=None)`: wraps a connected socket with TLS.
  Returns an `SSLSocket` with `send()`, `recv()`, `read()`, `write()`, `close()`.
- SSLSocket has a `__del__` finalizer for GC cleanup.

### Link flags

`-lamisslstubs` in the Makefile (after `-lsocket`). Note: `-lamisslauto` is NOT
used — it caused exit crashes due to its destructor double-closing AmiSSL
libraries. All AmiSSL open/close is done manually in `ensure_amissl_init()` and
`amissl_cleanup()`.

## Module time (AmigaOS)

Implemented via `modtime.c` (included by extmod/modtime.c via
`MICROPY_PY_TIME_INCLUDEFILE`):

- `time.time()`: seconds since epoch 1970 via libnix `time()`
- `time.time_ns()`: nanoseconds since epoch (second resolution)
- `time.gmtime([secs])` / `time.localtime([secs])`: 8-element tuple
- `time.mktime(tuple)`: inverse conversion
- `time.sleep(secs)` / `time.sleep_ms(ms)` / `time.sleep_us(us)`: `usleep()` libnix
- `time.gmtime([secs])` / `time.localtime([secs])`: 8-element time tuple
- `time.mktime(tuple)`: inverse conversion
- `time.ticks_ms()` / `time.ticks_us()` / `time.ticks_cpu()`: stubs (return 0)

Epoch is 1970 (`MICROPY_EPOCH_IS_1970=1`), matching libnix `time()`.

## AmigaOS Specifics

### REPL (readline with raw mode)

The REPL uses `shared/readline/readline.c` (`MICROPY_USE_READLINE=1`) with
50-entry command history. The AmigaOS console is switched to raw mode via
`SetMode(Input(), 1)` for character-by-character input, and restored to cooked
mode via `SetMode(Input(), 0)` after each command execution and on exit.

Console I/O uses `Read(Input())` and `Write(Output())` from dos.library
(not stdio fgets/fwrite) for raw mode compatibility.

### CSI Translation

AmigaOS sends CSI (0x9B / 155) + letter for cursor keys, but MicroPython
readline expects ANSI ESC (27) + '[' (91) + letter. `mp_hal_stdin_rx_chr()`
translates on the fly: when it receives 155, it returns 27 (ESC) and queues
'[' for the next call.

### Exiting the REPL

- `quit()` or `exit()`: port-defined builtins, raise `SystemExit`
- `sys.exit()`: standard MicroPython (`MICROPY_PY_SYS_EXIT=1`)
- Ctrl-D: exit REPL cleanly
- Ctrl-C: cancel current input
- Ctrl-E: enter paste mode

Console is restored to cooked mode in crash handlers (`nlr_jump_fail`,
`__assert_func`) to avoid leaving the shell in raw mode.

## Enabled Modules

### Always active (EVERYTHING)

- `builtins`: basic Python functions (print, len, range, etc.)
- `gc`: garbage collector control
- `sys`: system info, `sys.exit()`, `sys.platform`, `sys.path`, `sys.argv`
- `struct`: binary data pack/unpack
- `math`: math functions (single-precision float)
- `micropython`: MicroPython introspection
- `io`: StringIO, BytesIO

### Module os (uos C + frozen os.py)

- `os.listdir`, `os.getcwd`, `os.chdir`, `os.system`, `os.sep`
- `os.mkdir`, `os.rmdir`, `os.remove`, `os.rename`, `os.stat`
- `os.makedirs(name, exist_ok=False)`: recursive directory creation
- `os.walk(top, topdown=True)`: directory tree generator
- `os.path`: join, split, basename, dirname, exists, isfile, isdir, etc.

### Explicitly enabled (C extmod modules)

- `re`: regular expressions
- `json`: JSON encoding/decoding
- `binascii`: hexlify, unhexlify, a2b_base64, b2a_base64
- `time`: time, gmtime, localtime, mktime, sleep, ticks_ms/us/cpu
- `random`: random, randint, randrange, choice, uniform
- `hashlib`: sha256 (built-in, no TLS needed; MD5/SHA1 not available)
- `errno`: POSIX error constants
- `deflate`: compression/decompression (raw deflate, zlib, gzip via DeflateIO)
- `platform`: system/CPU/FPU/chipset/Kickstart detection (frozen, uses uos C helpers)
- `socket`: TCP/UDP sockets, DNS resolution (native C module via libsocket/bsdsocket.library)
- `ssl`: TLS via AmiSSL (native C module, custom BIO with saveds callbacks)
- `arexx`: ARexx IPC (send commands to AmigaOS apps, list ports, check existence)

### Frozen (Python modules embedded in binary)

- `base64`: base64, base32, base16 encoding/decoding
- `datetime`: date, time, datetime, timedelta (patched: fixed __repr__, added strftime)
- `_ospath`: os.path for AmigaOS
- `os`: os module wrapper (makedirs, walk, path)
- `platform`: system, machine, processor, version, fpu, chipset, amiga_info
- `urequests`: HTTP/1.1 HTTPS client with chunked TE, gzip decompression, buffered I/O
- `deflate`: compression/decompression (raw deflate, zlib, gzip formats)
- `zlib`: CPython-compatible zlib API (compress, decompress, crc32) — native C crc32 + frozen Python
- `zipfile`: read/write ZIP archives (stored + deflated, CRC32 verification, extractall)
- `gzip`: CPython-compatible gzip.compress()/gzip.decompress() (frozen, wraps deflate)

### Port-added builtins

- `quit([code])`: exit REPL (raise SystemExit)
- `exit([code])`: alias for quit()
- `input([prompt])`: line reading
- `open(file, mode)`: full file I/O via VFS_POSIX (read and write)
- `help()`, `help('modules')`: built-in help

### Disabled

- `_thread`: no multithreading (no pthreads on AmigaOS)
- `socket`: available (see below)
- `select`: no I/O multiplexing
- `signal`: no POSIX signals
- `ffi`: no FFI
- `termios`: no POSIX terminal control
- `hashlib.md5`, `hashlib.sha1`: require mbedTLS/axTLS (not available)

## Memory

- GC heap: 128 KB default, configurable via `-m <kb>` (e.g. `-m 1024` for 1 MB)
  Dynamic allocation via `AllocMem(heap_size, MEMF_ANY | MEMF_CLEAR)`,
  freed by `FreeMem()` on exit and in crash handlers
- Available RAM checked before allocation; warning if >80% requested
- Heap size and available RAM displayed in REPL banner
- Stack: 128 KB via libnix (`long __stack = 131072`), with StackSwap fallback to 64 KB
- Qstr: 2048-byte chunks, 2-byte length, 2-byte hash
- GC collect: `gc_helper_collect_regs_and_stack()` via `gchelper_generic.c`
  (setjmp-based register capture + stack scan)
- Stack control: `mp_stack_ctrl_init()` + `mp_stack_set_top()` called in `vm_init()`
- Types: `mp_int_t` = int32, `mp_uint_t` = uint32 (32-bit architecture)
- Long integers: MPZ (arbitrary precision)
- `mp_off_t` = long

## Known Bugs

- **GC assertion gc.c:952** (FIXED): was caused by two bugs — misaligned root
  pointers (fixed via `_gc_lock_pad` in `py/mpstate.h`) and `stack_top` pointing
  to the wrong stack after `StackSwap()` (fixed by setting `stack_top` to the top
  of the swapped stack). Assertions are now fully enabled.

## Known Limitations

- `time.ticks_ms()` is a stub (no high-resolution clock yet)
- Ctrl-C (KeyboardInterrupt) works during execution via MICROPY_VM_HOOK_POLL
  polling SetSignal(SIGBREAKF_CTRL_C) every 64 bytecodes. time.sleep() is
  interruptible in 100ms chunks via Delay().
- `hashlib` only supports SHA256 (MD5/SHA1 require TLS library)
- `hashlib.sha256().digest()` can only be called once (MicroPython limitation)

## Future Work

- High-resolution clock via timer.device or ReadEClock() for ticks_ms/ticks_us
- Add more frozen modules (collections, etc.)
