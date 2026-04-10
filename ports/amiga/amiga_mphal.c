#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/locale.h>
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

// Write a string to stdout, converting UTF-8 to Latin-1 for the AmigaOS terminal.
// Fast path: pure ASCII is written directly without copying.
mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    // Fast path: check for non-ASCII bytes
    int needs_conversion = 0;
    for (mp_uint_t i = 0; i < len; i++) {
        if ((unsigned char)str[i] > 0x7F) {
            needs_conversion = 1;
            break;
        }
    }
    if (!needs_conversion) {
        return (mp_uint_t)Write(Output(), (APTR)str, len);
    }
    // Convert UTF-8 to Latin-1, flushing in chunks
    char buf[256];
    mp_uint_t total = 0;
    mp_uint_t j = 0;
    for (mp_uint_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c < 0x80) {
            buf[j++] = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len
                   && ((unsigned char)str[i + 1] & 0xC0) == 0x80) {
            unsigned int cp = ((c & 0x1F) << 6) | ((unsigned char)str[i + 1] & 0x3F);
            buf[j++] = (cp <= 0xFF) ? (char)cp : '?';
            i++;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            buf[j++] = '?';
            i += 2;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            buf[j++] = '?';
            i += 3;
        } else {
            buf[j++] = c;
        }
        if (j >= sizeof(buf) - 4) {
            total += (mp_uint_t)Write(Output(), (APTR)buf, j);
            j = 0;
        }
    }
    if (j > 0) {
        total += (mp_uint_t)Write(Output(), (APTR)buf, j);
    }
    return total;
}

// Convert a UTF-8 C string to Latin-1 for passing to AmigaOS.
// Returns s directly if pure ASCII (fast path, no copy).
const char *amiga_utf8_to_latin1(const char *s, char *buf, size_t bufsize) {
    const char *p = s;
    while (*p) {
        if ((unsigned char)*p > 0x7F) goto convert;
        p++;
    }
    return s;
convert:;
    size_t j = 0;
    for (size_t i = 0; s[i] && j < bufsize - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            buf[j++] = c;
        } else if ((c & 0xE0) == 0xC0 && s[i + 1]
                   && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
            unsigned int cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            buf[j++] = (cp <= 0xFF) ? (char)cp : '?';
            i++;
        } else if ((c & 0xF0) == 0xE0 && s[i + 1] && s[i + 2]) {
            buf[j++] = '?';
            i += 2;
        } else if ((c & 0xF8) == 0xF0 && s[i + 1] && s[i + 2] && s[i + 3]) {
            buf[j++] = '?';
            i += 3;
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    return buf;
}

// Switch console to raw mode (character-by-character, no buffering)
void mp_hal_stdio_mode_raw(void) {
    SetMode(Input(), 1);  // MODE_RAW
}

// Restore console to cooked mode (line-buffered)
void mp_hal_stdio_mode_orig(void) {
    SetMode(Input(), 0);  // MODE_CON
}

// Timezone offset in seconds (local = utc + offset).
// Uses AmigaOS locale.library: loc_GMTOffset is in minutes, negative = east.
int32_t mp_hal_timezone_offset_s(void) {
    struct Locale *locale = OpenLocale(NULL);
    if (locale) {
        int32_t offset = -(locale->loc_GMTOffset) * 60;
        CloseLocale(locale);
        return offset;
    }
    return 0;
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
            return;
        }
    }
}

// Microsecond delay (too short to need Ctrl-C support)
void mp_hal_delay_us(mp_uint_t us) {
    usleep(us);
}
