#!/usr/bin/env python3
"""
Patch script for VFS POSIX Latin-1 filename conversion hook.

On AmigaOS, filenames use Latin-1 encoding but MicroPython strings are
UTF-8. This script adds MICROPY_VFS_POSIX_CONVERT_PATH hooks in the
VFS POSIX layer so that open(), import, stat, rename, mkdir, rmdir etc.
convert UTF-8 filenames to Latin-1 before passing them to libnix/AmigaOS.

The hooks are guarded with #ifdef so they have zero impact on other ports.

Usage (from MicroPython root):
    python3 ports/amiga/patches/patch_vfs_posix_latin1.py

The script is idempotent — safe to run multiple times.
"""

import os
import sys

PATCHES = [
    # --- extmod/vfs_posix_file.c: mp_vfs_posix_file_open() ---
    (
        "extmod/vfs_posix_file.c",
        [
            "    const char *fname = mp_obj_str_get_str(fid);\n",
            "    int fd;\n",
        ],
        [
            "    const char *fname = mp_obj_str_get_str(fid);\n",
            "    // Allow port to convert filename encoding (e.g., UTF-8 → Latin-1 for AmigaOS)\n",
            "    #ifdef MICROPY_VFS_POSIX_CONVERT_PATH\n",
            "    char fname_conv_buf_[256];\n",
            "    fname = MICROPY_VFS_POSIX_CONVERT_PATH(fname, fname_conv_buf_, sizeof(fname_conv_buf_));\n",
            "    #endif\n",
            "    int fd;\n",
        ],
    ),

    # --- extmod/vfs_posix.c: vfs_posix_get_path_str() ---
    (
        "extmod/vfs_posix.c",
        [
            "    const char *path_str = mp_obj_str_get_str(path);\n",
            "    if (self->root_len == 0 || path_str[0] != '/') {\n",
            "        return path_str;\n",
        ],
        [
            "    const char *path_str = mp_obj_str_get_str(path);\n",
            "    // Allow port to convert filename encoding (e.g., UTF-8 → Latin-1 for AmigaOS)\n",
            "    #ifdef MICROPY_VFS_POSIX_CONVERT_PATH\n",
            "    char path_conv_buf_[256];\n",
            "    path_str = MICROPY_VFS_POSIX_CONVERT_PATH(path_str, path_conv_buf_, sizeof(path_conv_buf_));\n",
            "    #endif\n",
            "    if (self->root_len == 0 || path_str[0] != '/') {\n",
            "        return path_str;\n",
        ],
    ),

    # --- extmod/vfs_posix.c: mp_vfs_posix_import_stat() ---
    (
        "extmod/vfs_posix.c",
        [
            "static mp_import_stat_t mp_vfs_posix_import_stat(void *self_in, const char *path) {\n",
            "    mp_obj_vfs_posix_t *self = self_in;\n",
            "    if (self->root_len != 0) {\n",
        ],
        [
            "static mp_import_stat_t mp_vfs_posix_import_stat(void *self_in, const char *path) {\n",
            "    mp_obj_vfs_posix_t *self = self_in;\n",
            "    // Allow port to convert filename encoding (e.g., UTF-8 → Latin-1 for AmigaOS)\n",
            "    #ifdef MICROPY_VFS_POSIX_CONVERT_PATH\n",
            "    char import_path_conv_buf_[256];\n",
            "    path = MICROPY_VFS_POSIX_CONVERT_PATH(path, import_path_conv_buf_, sizeof(import_path_conv_buf_));\n",
            "    #endif\n",
            "    if (self->root_len != 0) {\n",
        ],
    ),
]


def find_block(lines, pattern):
    """Find the starting index of a multi-line pattern in lines."""
    for i in range(len(lines) - len(pattern) + 1):
        if lines[i:i + len(pattern)] == pattern:
            return i
    return -1


def patch_file(filepath, original, patched):
    """Apply one patch to a file. Returns True if modified."""
    if not os.path.exists(filepath):
        print(f"  SKIP {filepath} (not found)")
        return False

    with open(filepath, "r") as f:
        lines = f.readlines()

    # Check if already patched
    if find_block(lines, patched) >= 0:
        print(f"  OK   {filepath} (already patched)")
        return False

    # Find original block
    idx = find_block(lines, original)
    if idx < 0:
        print(f"  WARN {filepath} (pattern not found — manually check)")
        return False

    # Apply patch
    lines[idx:idx + len(original)] = patched
    with open(filepath, "w") as f:
        f.writelines(lines)
    print(f"  PATCH {filepath} line {idx + 1}")
    return True


def main():
    # Determine repo root (script is in ports/amiga/patches/)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, "..", "..", ".."))
    os.chdir(root)

    print("Patching VFS POSIX Latin-1 filename conversion hooks...")
    print(f"Root: {root}")

    modified = 0
    for filepath, original, patched in PATCHES:
        if patch_file(filepath, original, patched):
            modified += 1

    print(f"\nDone: {modified} file(s) modified, "
          f"{len(PATCHES) - modified} already patched or skipped.")


if __name__ == "__main__":
    main()
