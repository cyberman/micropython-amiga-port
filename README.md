# MicroPython for AmigaOS (m68k)

A port of [MicroPython](https://micropython.org/) v1.27 to AmigaOS, targeting
Motorola 68020+ processors. Runs on classic Amiga hardware (A1200, A3000, A4000)
and emulators (WinUAE, FS-UAE).

## About This Port

This port is developed by **Fabrice** with coding assistance from **Claude**
(Anthropic's AI). Claude writes and modifies the code under Fabrice's direct
supervision: Fabrice defines the architecture, chooses the implementation
strategy, tests every build on real Amiga hardware or emulator, and provides
detailed feedback and bug reports that guide the development. Every change is
reviewed and validated by a human before being committed.

## Features

- **Interactive REPL** with readline support (cursor keys, command history)
- **Script execution**: `micropython script.py [args...]`
- **Inline execution**: `micropython -c "print('hello')"`
- **Configurable heap**: `micropython -m 1024` for 1 MB heap (default 128 KB)
- **File I/O**: full `open()`/`read()`/`write()`/`close()` via VFS_POSIX
- **AmigaOS filesystem**: `os.listdir`, `os.getcwd`, `os.chdir`, `os.mkdir`,
  `os.rmdir`, `os.remove`, `os.rename`, `os.stat`, `os.makedirs`, `os.walk`
- **os.path**: join, split, basename, dirname, exists, isfile, isdir, abspath,
  normpath (with AmigaOS volume:path conventions)
- **Modules**: re, json, math, struct, binascii, base64, time, datetime,
  random, hashlib (sha256), errno, platform, socket, ssl, urequests, deflate, gzip, gc, sys, io
- **Networking**: TCP/UDP sockets, DNS resolution via bsdsocket.library
- **TLS/SSL**: HTTPS support via AmiSSL (requires amissl.library on Amiga)
- **HTTP client**: `urequests.get()`, `post()`, `put()`, `delete()` with HTTP/1.1,
  chunked transfer encoding, gzip decompression, HTTP and HTTPS
- **Platform detection**: `platform.amiga_info()` shows CPU, FPU, chipset,
  Kickstart version, and available memory

## Quick Start

### Requirements

- [bebbo's m68k-amigaos-gcc toolchain](https://franke.ms/git/bebbo/amiga-gcc)
  installed at `/opt/amiga/`
- GNU Make (`gmake`)

### Build

```sh
cd mpy-cross && gmake          # one-time: build the bytecode cross-compiler
cd ../ports/amiga && gmake     # build the Amiga binary
```

The binary `build/micropython` is ready to copy to your Amiga.

### Run

```
micropython                          ; interactive REPL
micropython script.py                ; run a script
micropython -c "print(2**32)"        ; run inline code
micropython -m 512 script.py         ; run with 512 KB heap
```

## AmigaOS Path Conventions

AmigaOS uses `:` to separate volumes from paths and `/` for subdirectories:

```python
>>> import os
>>> os.getcwd()
'DH0:Work'
>>> os.path.join("DH0:", "work", "scripts")
'DH0:work/scripts'
>>> os.path.isabs("DH0:file.py")
True
```

## Known Limitations

- `time.ticks_ms()` returns 0 (no high-resolution timer yet)
- No Ctrl-C during script execution (only in REPL input)
- `hashlib` only supports SHA256 (MD5/SHA1 require TLS library)
- Sockets are always blocking (no non-blocking mode)
- No multithreading

## License

MicroPython is licensed under the MIT License. See the [LICENSE](LICENSE)
file in the repository root.
