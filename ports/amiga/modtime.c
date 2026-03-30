// Port-specific time functions for AmigaOS.
// Included by extmod/modtime.c via MICROPY_PY_TIME_INCLUDEFILE.

#include <time.h>
#include <proto/locale.h>
#include "shared/timeutils/timeutils.h"

// Get timezone offset in seconds (local = utc + offset).
// Uses AmigaOS locale.library: loc_GMTOffset is in minutes, negative = east.
// Cached on first call to avoid opening/closing locale.library every time.
static int32_t amiga_tz_offset_s(void) {
    static int32_t cached_offset = 0;
    static int cached = 0;
    if (!cached) {
        struct Locale *locale = OpenLocale(NULL);
        if (locale) {
            cached_offset = -(locale->loc_GMTOffset) * 60;
            CloseLocale(locale);
        }
        cached = 1;
    }
    return cached_offset;
}

// Get the local time (UTC + timezone offset).
static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    time_t t = time(NULL) + amiga_tz_offset_s();
    struct tm *gt = gmtime(&t);
    if (gt == NULL) {
        memset(tm, 0, sizeof(*tm));
        tm->tm_year = 1970;
        tm->tm_mon = 1;
        tm->tm_mday = 1;
        return;
    }
    tm->tm_year = gt->tm_year + 1900;
    tm->tm_mon = gt->tm_mon + 1;
    tm->tm_mday = gt->tm_mday;
    tm->tm_hour = gt->tm_hour;
    tm->tm_min = gt->tm_min;
    tm->tm_sec = gt->tm_sec;
    tm->tm_wday = (gt->tm_wday + 6) % 7; // C: Sun=0, MicroPython: Mon=0
    tm->tm_yday = gt->tm_yday + 1;
}

// Return the number of seconds since the Epoch (UTC, no timezone).
static mp_obj_t mp_time_time_get(void) {
    time_t t = time(NULL);
    return mp_obj_new_int((mp_int_t)t);
}

// time.strftime(fmt[, t]) — format a time tuple as a string.
// If t is None or omitted, uses localtime().
static mp_obj_t mp_time_strftime(size_t n_args, const mp_obj_t *args) {
    const char *fmt = mp_obj_str_get_str(args[0]);

    int year, month, day, hour, minute, second, weekday, yearday;

    if (n_args < 2 || args[1] == mp_const_none) {
        // Use current local time
        timeutils_struct_time_t tm;
        mp_time_localtime_get(&tm);
        year = tm.tm_year; month = tm.tm_mon; day = tm.tm_mday;
        hour = tm.tm_hour; minute = tm.tm_min; second = tm.tm_sec;
        weekday = tm.tm_wday; yearday = tm.tm_yday;
    } else {
        // Extract from tuple
        mp_obj_t *items;
        size_t len;
        mp_obj_tuple_get(args[1], &len, &items);
        year = mp_obj_get_int(items[0]);
        month = mp_obj_get_int(items[1]);
        day = mp_obj_get_int(items[2]);
        hour = mp_obj_get_int(items[3]);
        minute = mp_obj_get_int(items[4]);
        second = mp_obj_get_int(items[5]);
        weekday = mp_obj_get_int(items[6]);
        yearday = (len > 7) ? mp_obj_get_int(items[7]) : 0;
    }

    static const char *month_names[] = {"", "January", "February", "March",
        "April", "May", "June", "July", "August", "September",
        "October", "November", "December"};
    static const char *month_abbr[] = {"", "Jan", "Feb", "Mar", "Apr", "May",
        "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *day_names[] = {"Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday", "Sunday"};
    static const char *day_abbr[] = {"Mon", "Tue", "Wed", "Thu", "Fri",
        "Sat", "Sun"};

    vstr_t vstr;
    vstr_init(&vstr, 32);

    size_t i = 0;
    size_t fmtlen = strlen(fmt);
    while (i < fmtlen) {
        if (fmt[i] == '%' && i + 1 < fmtlen) {
            char c = fmt[i + 1];
            char buf[16];
            switch (c) {
                case 'Y': snprintf(buf, sizeof(buf), "%04d", year); vstr_add_str(&vstr, buf); break;
                case 'y': snprintf(buf, sizeof(buf), "%02d", year % 100); vstr_add_str(&vstr, buf); break;
                case 'm': snprintf(buf, sizeof(buf), "%02d", month); vstr_add_str(&vstr, buf); break;
                case 'd': snprintf(buf, sizeof(buf), "%02d", day); vstr_add_str(&vstr, buf); break;
                case 'H': snprintf(buf, sizeof(buf), "%02d", hour); vstr_add_str(&vstr, buf); break;
                case 'M': snprintf(buf, sizeof(buf), "%02d", minute); vstr_add_str(&vstr, buf); break;
                case 'S': snprintf(buf, sizeof(buf), "%02d", second); vstr_add_str(&vstr, buf); break;
                case 'j': snprintf(buf, sizeof(buf), "%03d", yearday); vstr_add_str(&vstr, buf); break;
                case 'A': vstr_add_str(&vstr, day_names[weekday % 7]); break;
                case 'a': vstr_add_str(&vstr, day_abbr[weekday % 7]); break;
                case 'B': if (month >= 1 && month <= 12) vstr_add_str(&vstr, month_names[month]); break;
                case 'b': if (month >= 1 && month <= 12) vstr_add_str(&vstr, month_abbr[month]); break;
                case 'p': vstr_add_str(&vstr, hour < 12 ? "AM" : "PM"); break;
                case 'I': { int h = hour % 12; snprintf(buf, sizeof(buf), "%02d", h ? h : 12); vstr_add_str(&vstr, buf); break; }
                case '%': vstr_add_char(&vstr, '%'); break;
                default: vstr_add_char(&vstr, '%'); vstr_add_char(&vstr, c); break;
            }
            i += 2;
        } else {
            vstr_add_char(&vstr, fmt[i]);
            i++;
        }
    }

    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_time_strftime_obj, 1, 2, mp_time_strftime);
