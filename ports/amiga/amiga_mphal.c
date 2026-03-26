#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>

// usleep() is provided by libnix but not declared under -std=c99
extern int usleep(unsigned long);

#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/runtime.h"

// CSI translation state: AmigaOS sends CSI (155) + letter for cursor keys,
// but MicroPython readline expects ESC (27) + '[' (91) + letter.
static int csi_pending = 0;

// Read a single character from stdin (raw mode).
// Translates AmigaOS CSI sequences to ANSI escape sequences.
int mp_hal_stdin_rx_chr(void) {
    // If we have a pending '[' from a CSI translation, return it now
    if (csi_pending) {
        csi_pending = 0;
        return '[';
    }

    unsigned char c;
    LONG n = Read(Input(), &c, 1);
    if (n <= 0) {
        return 0;
    }

    // In raw mode, AmigaOS sends CR (13) for Enter.
    // readline.c expects CR, so do NOT convert to LF here.

    // AmigaOS CSI (155) → translate to ESC (27), queue '[' for next call
    if (c == 155) {
        csi_pending = 1;
        return 27;
    }

    return c;
}

// Write a string to stdout using dos.library Write() for raw mode compatibility.
mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    return (mp_uint_t)Write(Output(), (APTR)str, len);
}

// Switch console to raw mode (character-by-character, no buffering)
void mp_hal_stdio_mode_raw(void) {
    SetMode(Input(), 1);  // MODE_RAW
}

// Restore console to cooked mode (line-buffered)
void mp_hal_stdio_mode_orig(void) {
    SetMode(Input(), 0);  // MODE_CON
}

// Check for Ctrl-C (SIGBREAKF_CTRL_C) and raise KeyboardInterrupt.
// Called periodically from the VM bytecode loop via MICROPY_VM_HOOK_POLL.
void mp_amiga_check_signals(void) {
    ULONG signals = SetSignal(0, SIGBREAKF_CTRL_C);
    if (signals & SIGBREAKF_CTRL_C) {
        mp_sched_keyboard_interrupt();
    }
}

// Interruptible delay using AmigaOS Delay() (1 tick = 20ms).
// Sleeps in 100ms chunks, checking Ctrl-C between each.
void mp_hal_delay_ms(mp_uint_t ms) {
    while (ms > 0) {
        mp_uint_t chunk = (ms > 100) ? 100 : ms;
        LONG ticks = (chunk + 19) / 20;
        if (ticks > 0) {
            Delay(ticks);
        }
        ms -= chunk;
        ULONG sig = SetSignal(0, SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) {
            mp_sched_keyboard_interrupt();
            mp_handle_pending(true);
            return;
        }
    }
}

// Microsecond delay (too short to need Ctrl-C support)
void mp_hal_delay_us(mp_uint_t us) {
    usleep(us);
}
