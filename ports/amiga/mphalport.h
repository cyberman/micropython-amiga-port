#ifndef MICROPY_INCLUDED_AMIGA_MPHALPORT_H
#define MICROPY_INCLUDED_AMIGA_MPHALPORT_H

#include <stdint.h>

// Time stubs — AmigaOS has no POSIX clock; all return 0 for now.
static inline mp_uint_t mp_hal_ticks_ms(void) { return 0; }
static inline mp_uint_t mp_hal_ticks_us(void) { return 0; }
static inline mp_uint_t mp_hal_ticks_cpu(void) { return 0; }
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);

// time_ns() support — 20ms precision via AmigaOS DateStamp()
#include <proto/dos.h>
static inline uint64_t mp_hal_time_ns(void) {
    struct DateStamp ds;
    DateStamp(&ds);
    // Amiga epoch 1978-01-01 → Unix epoch 1970-01-01 (2922 days)
    uint64_t unix_secs = (uint64_t)ds.ds_Days * 86400ULL
                       + (uint64_t)ds.ds_Minute * 60ULL
                       + (uint64_t)ds.ds_Tick / 50ULL
                       + 252460800ULL;
    uint64_t frac_ns = ((uint64_t)(ds.ds_Tick % 50)) * 20000000ULL;
    return unix_secs * 1000000000ULL + frac_ns;
}

static inline void mp_hal_set_interrupt_char(char c) { (void)c; }

// No EINTR on AmigaOS, so just execute the syscall once.
#include <errno.h>
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
    (ret) = (syscall); \
    if ((ret) == -1) { \
        int err = errno; \
        (void)err; \
        raise; \
    } \
}

// Console raw/cooked mode switching for readline
void mp_hal_stdio_mode_raw(void);
void mp_hal_stdio_mode_orig(void);

#endif // MICROPY_INCLUDED_AMIGA_MPHALPORT_H
