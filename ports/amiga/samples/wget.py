#
# wget.py - Minimal wget clone for MicroPython (AmigaOS m68k port).
#
# Usage:
#   micropython wget.py <url> [-O output_file]
#
# Supports http:// and https:// (https requires AmiSSL).
# Follows redirects (up to 5), shows a text progress bar with speed & ETA.
#
# This script is a thin wrapper around http_util.download_to_file().
# All the real work lives in http_util.py, which is shared with
# aminet_tools.py and any other sample that needs HTTP.
#
# Author: written for Fabrice's MicroPython Amiga port (build 230+).
#

import sys
import http_util


def print_usage():
    print("Usage: wget.py <url> [-O output_file]")
    print("  Supports http:// and https:// (https needs AmiSSL).")


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help"):
        print_usage()
        return

    url = None
    output = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-O":
            i += 1
            if i >= len(argv):
                print("wget: -O requires a filename")
                return
            output = argv[i]
        elif a.startswith("-"):
            print("wget: unknown option %s" % a)
            return
        else:
            url = a
        i += 1

    if url is None:
        print("wget: missing URL")
        print_usage()
        return

    try:
        http_util.download_to_file(url, output, show_progress=True)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    except Exception as e:
        print("\nError: %s" % e)


main()
