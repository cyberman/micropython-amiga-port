# launch_ibrowse.py - Launch IBrowse and tell it to open the Aminet
#                     MicroPython page via its ARexx port.
#
# Usage:
#   micropython launch_ibrowse.py <path_to_ibrowse>
#
# Example:
#   micropython launch_ibrowse.py Work:IBrowse/IBrowse
#
# Author: written for Fabrice's MicroPython Amiga port (build 227+)

import sys
import os
import time
import arexx

URL = "https://aminet.net/package/dev/lang/micropython"
AREXX_PORT = "IBROWSE"
POLL_INTERVAL = 1     # seconds between port-existence checks
MAX_WAIT = 60         # seconds before giving up


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help"):
        print("Usage: launch_ibrowse.py <path_to_ibrowse>")
        print("Example: launch_ibrowse.py Work:IBrowse/IBrowse")
        return

    ibrowse_path = argv[0]

    # If IBrowse is already running, skip the launch step
    if arexx.exists(AREXX_PORT):
        print("IBrowse is already running (port %s found)." % AREXX_PORT)
    else:
        print("Launching IBrowse: %s" % ibrowse_path)
        # Run >NIL: <NIL: detaches the process so we don't block here.
        # Quotes around the path handle Amiga paths with spaces.
        cmd = 'Run >NIL: <NIL: "%s"' % ibrowse_path
        rc = os.system(cmd)
        if rc != 0:
            print("Error: failed to launch IBrowse (rc=%d)" % rc)
            return

        # Wait for the ARexx port to appear
        print("Waiting for ARexx port '%s'" % AREXX_PORT, end="")
        waited = 0
        while not arexx.exists(AREXX_PORT):
            if waited >= MAX_WAIT:
                print()
                print("Error: IBrowse did not register its ARexx port "
                      "within %d seconds." % MAX_WAIT)
                return
            time.sleep(POLL_INTERVAL)
            waited += POLL_INTERVAL
            sys.stdout.write(".")
        print(" ready (%ds)" % waited)

    # Send the GOTOURL command
    print("Opening URL: %s" % URL)
    try:
        with arexx.Port(AREXX_PORT) as ib:
            rc = ib.send('GOTOURL URL="%s"' % URL)
            if rc == 0:
                print("Done.")
            else:
                print("IBrowse returned rc=%d" % rc)
    except OSError as e:
        print("ARexx error: %s" % e)


main()
