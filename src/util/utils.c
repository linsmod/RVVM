/*
utils.с - Util functions
Copyright (C) 2021  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Must be included before <stdio.h>
#include <util/feature_test.h>

#include <util/blk_io.h>
#include <util/locking.h>
#include <util/rvtimer.h>
#include <util/stacktrace.h>
#include <util/threading.h>
#include <util/utils.h>
#include <util/vector.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h> // clock_gettime(), CLOCK_MONOTONIC

#if defined(ANDROID)
#include <android/log.h>
#endif

PUSH_OPTIMIZATION_SIZE

/*
 * String/Integer conversions
 */

static inline char digit_symbol(uint32_t val)
{
    if (val < 10) {
        return '0' + val;
    }
    if (val < 36) {
        return 'a' + val - 10;
    }
    return '?';
}

static inline uint32_t digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'A' && digit <= 'Z') {
        return digit - 'A' + 10;
    }
    if (digit >= 'a' && digit <= 'z') {
        return digit - 'a' + 10;
    }
    return -1;
}

size_t uint_to_str_base(char* str, size_t size, uint64_t val, uint8_t base)
{
    size_t len = 0;
    if (base >= 2 && base <= 36) {
        do {
            if (str) {
                if (len + 1 >= size) {
                    len = 0;
                    break;
                }
                str[len] = digit_symbol(val % base);
            }
            val /= base;
            len++;
        } while (val);
    }
    if (str) {
        // Reverse the string
        for (size_t i = 0; i < len / 2; ++i) {
            char tmp         = str[i];
            str[i]           = str[len - i - 1];
            str[len - i - 1] = tmp;
        }
        if (size) {
            str[len] = 0;
        }
    }
    return len;
}

uint64_t str_to_uint_base(const char* str, size_t* len, uint8_t base)
{
    uint64_t val  = 0;
    size_t   size = 0;
    if (base == 0) {
        base = 10;
        if (str[0] == '0') {
            base = 8; // Octal literal
            if (str[1] == 'o' || str[1] == 'O') {
                size = 2;
            } else if (str[1] == 'x' || str[1] == 'X') {
                base = 16; // Hex literal
                size = 2;
            } else if (str[1] == 'b' || str[1] == 'B') {
                base = 2; // Binary literal
                size = 2;
            }
        }
    }
    if (len) {
        len[0] = 0;
    }
    if (base >= 2 && base <= 36) {
        while (digit_value(str[size]) < base) {
            val *= base;
            val += digit_value(str[size++]);
            if (len) {
                len[0] = size;
            }
        }
    }
    return val;
}

size_t int_to_str_base(char* str, size_t size, int64_t val, uint8_t base)
{
    size_t sign  = val < 0;
    size_t usize = size ? (size - sign) : 0;
    char*  ustr  = str ? (str + sign) : NULL;
    size_t len   = uint_to_str_base(ustr, usize, sign ? -val : val, base);
    if (sign && str) {
        str[0] = 0;
        if (usize) {
            str[0] = '-';
        }
    }
    return len + sign;
}

int64_t str_to_int_base(const char* str, size_t* len, uint8_t base)
{
    bool     neg = (str[0] == '-');
    uint64_t val = str_to_uint_base(str + neg, len, base);
    if (neg && len && len[0]) {
        len[0]++;
    }
    return neg ? -val : val;
}

size_t int_to_str_dec(char* str, size_t size, int64_t val)
{
    return int_to_str_base(str, size, val, 10);
}

int64_t str_to_int_dec(const char* str)
{
    return str_to_int_base(str, NULL, 0);
}

/*
 * String functions
 */

size_t rvvm_strlen(const char* string)
{
    size_t i = 0;
    while (string[i]) {
        i++;
    }
    return i;
}

size_t rvvm_strnlen(const char* string, size_t size)
{
    size_t i = 0;
    while (i < size && string[i]) {
        i++;
    }
    return i;
}

bool rvvm_strcmp(const char* s1, const char* s2)
{
    size_t i = 0;
    while (s1[i] && s1[i] == s2[i]) {
        i++;
    }
    return s1[i] == s2[i];
}

const char* rvvm_strfind(const char* string, const char* pattern)
{
    while (*string) {
        const char* tmp = string;
        const char* pat = pattern;
        while (*tmp && *tmp == *pat) {
            tmp++;
            pat++;
        }
        if (!(*pat)) {
            return string;
        }
        string++;
    }
    return NULL;
}

size_t rvvm_strlcpy(char* dst, const char* src, size_t size)
{
    size_t i = 0;
    while (i + 1 < size && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (size) {
        dst[i] = 0;
    }
    return i;
}

/*
 * String formating
 */

#if !defined(USE_LIBC_PRINTF)

static size_t rvvm_strpad_internal(char* str, size_t size, char pad, bool left, size_t new_len, size_t old_len)
{
    if (new_len > old_len && size) {
        size_t diff = EVAL_MIN(size - 1, new_len - old_len);
        if (left && diff) {
            memset(str + old_len, pad, diff);
        } else if (diff) {
            memmove(str + diff, str, old_len);
            memset(str, pad, diff);
        }
        str[EVAL_MIN(size - 1, new_len)] = 0;
        return diff;
    }
    return 0;
}

#endif

size_t rvvm_vsnprintf(char* buffer, size_t size, const char* fmt, const void* argv)
{
    va_list args;
    memcpy(&args, argv, sizeof(args));
#if defined(USE_LIBC_PRINTF)
    return vsnprintf(buffer, size, fmt, args);
#else
    size_t ret = 0, len = 0;
    do {
        // Raw format string part length
        size_t str_len = 0;
        // Search for % format symbol
        const char* delim = rvvm_strfind(fmt, "%");
        if (delim) {
            str_len = ((size_t)delim) - ((size_t)fmt);
            delim++;
        } else {
            str_len = rvvm_strlen(fmt);
        }
        // Append raw format string part
        len += rvvm_strlcpy(buffer + len, fmt, EVAL_MIN(size - len, str_len + 1));
        ret += str_len;
        // Append formatted input
        if (delim) {
            size_t fmt_len = 0;
            // Flags
            bool pad_left = false, num_pfx = false, pad_zero = false;
            char num_plus = 0;
            // Width / Precision
            size_t width = 0, precs = 0, skip = 0;
            // Data type length
            const char* dlen = "";
            // Flags (May be omitted)
            if (delim[0] == '-') {
                // Left-justify the width-padded field
                pad_left = true;
                delim++;
            }
            if (delim[0] == '+' || delim[0] == ' ') {
                // Prefix the result with a plus/minus/space
                num_plus = delim[0];
                delim++;
            } else if (delim[0] == '#') {
                // Prefix the result with 0, 0x or 0X respectively
                num_pfx = true;
                delim++;
            }
            if (delim[0] == '0') {
                // Left-pad number with zeroes instead of spaces
                pad_zero = true;
                delim++;
            }
            // Width sub-specifier (May be omitted)
            if (delim[0] == '*') {
                // Width sub-specifier passed in args
                width = va_arg(args, unsigned int);
                delim++;
            } else {
                // Width sub-specifier encoded in format string or omitted
                width  = str_to_uint_base(delim, &skip, 10);
                delim += skip;
            }
            // Prevision sub-specifier (May be omitted)
            if (delim[0] == '.') {
                delim++;
                if (delim[0] == '*') {
                    // Precision sub-specifier passed in args
                    precs = va_arg(args, unsigned int);
                    delim++;
                } else {
                    // Precision sub-specifier encoded in format string
                    precs  = str_to_uint_base(delim, &skip, 10);
                    delim += skip;
                }
            }
            // Data type length sub-specifier
            if (delim[0] == 'l' || delim[0] == 'h' || delim[0] == 'j' || delim[0] == 't' || delim[0] == 'z') {
                dlen = delim;
                if (delim[1] == 'l' || delim[1] == 'h') {
                    delim++;
                }
                delim++;
            }
            switch (delim[0]) {
                case 's': {
                    // Append a string
                    const char* str = va_arg(args, const char*);
                    if (!str) {
                        str = "(null)";
                    }
                    fmt_len  = rvvm_strlen(str);
                    len     += rvvm_strlcpy(buffer + len, str, EVAL_MIN(size - len, fmt_len + 1));
                    break;
                }
                case 'd':
                case 'i':
                case 'u':
                case 'o':
                case 'x':
                case 'X': {
                    size_t  ser_len   = 0;
                    bool    signval   = delim[0] == 'd' || delim[0] == 'i';
                    char    prefix[3] = ZERO_INIT;
                    uint8_t base      = 10;
                    int64_t val       = 0;
                    // Extract data type from va_arg
                    if ((dlen[0] == 'l' && dlen[1] == 'l') || dlen[0] == 'j') {
                        val = signval ? (int64_t)va_arg(args, long long) : (int64_t)va_arg(args, unsigned long long);
                    } else if (dlen[0] == 'l') {
                        val = signval ? (int64_t)va_arg(args, long) : (int64_t)va_arg(args, unsigned long);
                    } else if (dlen[0] == 'h' && dlen[1] == 'h') {
                        val = signval ? (int64_t)(int8_t)va_arg(args, unsigned)
                                      : (int64_t)(uint8_t)va_arg(args, unsigned);
                    } else if (dlen[0] == 'h') {
                        val = signval ? (int64_t)(int16_t)va_arg(args, unsigned)
                                      : (int64_t)(uint16_t)va_arg(args, unsigned);
                    } else if (dlen[0] == 'z' || dlen[0] == 't') {
                        val = va_arg(args, size_t);
                    } else {
                        val = signval ? (int64_t)va_arg(args, int32_t) : (int64_t)va_arg(args, uint32_t);
                    }
                    // Detect number base and prefix if needed
                    if (delim[0] == 'x' || delim[0] == 'X') {
                        base = 16;
                        if (num_pfx) {
                            rvvm_strlcpy(prefix, "0x", sizeof(prefix));
                        }
                    } else if (delim[0] == 'o') {
                        base = 8;
                        if (num_pfx) {
                            rvvm_strlcpy(prefix, "0", sizeof(prefix));
                        }
                    } else if (signval && val < 0) {
                        prefix[0] = '-';
                        val       = -val;
                    } else if (signval && num_plus) {
                        prefix[0] = num_plus;
                    }
                    // Append prefix
                    len += rvvm_strlcpy(buffer + len, prefix, size - len);
                    // Calculate unpadded number length, append number
                    fmt_len = uint_to_str_base(NULL, 0, (uint64_t)val, base);
                    ser_len = uint_to_str_base(buffer + len, size - len, (uint64_t)val, base);
                    if (delim[0] == 'X') {
                        // Uppercase the hex string
                        for (char* s = buffer + len - !!len; *s; ++s) {
                            if (s[0] >= 'a' && s[1] <= 'f') {
                                s[0] += 'A' - 'a';
                            }
                        }
                    }
                    if (precs > fmt_len) {
                        // Pad the number with zeroes ignoring the prefix length
                        ser_len += rvvm_strpad_internal(buffer + len, size - len, '0', false, precs, fmt_len);
                        fmt_len  = precs;
                    }
                    fmt_len += rvvm_strlen(prefix);
                    if (width > fmt_len && pad_zero) {
                        // Pad the number with zeroes
                        ser_len += rvvm_strpad_internal(buffer + len, size - len, '0', false, width, fmt_len);
                        fmt_len  = width;
                    }
                    len += ser_len;
                    break;
                }
                case 'p': {
                    // Append pointer
                    const void* ptr = va_arg(args, const void*);
                    // Append 0x
                    len += rvvm_strlcpy(buffer + len, "0x", EVAL_MIN(size - len, 3));
                    // Append hex pointer
                    len += uint_to_str_base(buffer + len, size - len, (size_t)ptr, 16);
                    // Calculate result length
                    fmt_len = 2 + uint_to_str_base(NULL, 0, (size_t)ptr, 16);
                    break;
                }
                case 'c':
                case '%': {
                    // Append character (Or %)
                    char character[2] = "%";
                    if (delim[0] == 'c') {
                        character[0] = va_arg(args, unsigned);
                    }
                    fmt_len  = 1;
                    len     += rvvm_strlcpy(buffer + len, character, EVAL_MIN(size - len, 2));
                    break;
                }
                case 'n': {
                    // Nothing printed; Store current output length to int*
                    int* ptr = va_arg(args, int*);
                    if (ptr) {
                        *ptr = ret;
                    }
                    break;
                }
            }
            if (width > fmt_len) {
                // Pad the field width
                len     += rvvm_strpad_internal(buffer + len - fmt_len, size - len + fmt_len, //
                                                ' ', pad_left, width, fmt_len);
                fmt_len  = width;
            }
            ret += fmt_len;
            delim++;
            str_len = ((size_t)delim) - ((size_t)fmt);
        }
        fmt += str_len;
    } while (*fmt);
    return ret;
#endif
}

size_t rvvm_snprintf(char* buffer, size_t size, const char* fmt, ...)
{
    size_t  ret = 0;
    va_list args;
    va_start(args, fmt);
    ret = rvvm_vsnprintf(buffer, size, fmt, &args);
    va_end(args);
    return ret;
}

/*
 * Random generation
 */

static uint64_t rvvm_rng_seed = 0;

void rvvm_randombytes(void* buffer, size_t size)
{
    // Xorshift RNG seeded by precise timer
    uint64_t seed  = atomic_load_uint64_relax(&rvvm_rng_seed);
    uint8_t* dest  = buffer;
    size_t   rem   = size & 0x7;
    size          -= rem;
    if (!seed) {
        seed = rvtimer_clocksource(1000000000ULL);
    }
    for (size_t i = 0; i < size; i += 8) {
        seed ^= (seed >> 17);
        seed ^= (seed << 21);
        seed ^= (seed << 28);
        seed ^= (seed >> 49);
        memcpy(dest + i, &seed, 8);
    }
    seed ^= (seed >> 17);
    seed ^= (seed << 21);
    seed ^= (seed << 28);
    seed ^= (seed >> 49);
    memcpy(dest + size, &seed, rem);
    atomic_store_uint64_relax(&rvvm_rng_seed, seed);
}

void rvvm_randomserial(char* serial, size_t size)
{
    rvvm_randombytes(serial, size);
    for (size_t i = 0; i < size; ++i) {
        size_t c = ((uint8_t*)serial)[i] % ('Z' - 'A' + 10);
        if (c <= 9) {
            serial[i] = '0' + c;
        } else {
            serial[i] = 'A' + c - 10;
        }
    }
}

/*
 * Safe memory allocation
 */

SAFE_MALLOC void* safe_malloc(size_t size)
{
    void* ret = malloc(size);
    if (unlikely(!size)) {
        rvvm_debug("Suspicious 0-byte allocation");
    }
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
        must_never_reach();
    }
    return ret;
}

SAFE_CALLOC void* safe_calloc(size_t size, size_t n)
{
    void* ret = calloc(size, n);
    if (unlikely(!size || !n)) {
        rvvm_debug("Suspicious 0-byte allocation");
    }
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
        must_never_reach();
    }
    // Fence zeroing of allocated memory
    atomic_fence_ex(ATOMIC_RELEASE);
    return ret;
}

SAFE_REALLOC void* safe_realloc(void* ptr, size_t size)
{
    void* ret = realloc(ptr, size);
    if (unlikely(!size)) {
        rvvm_debug("Suspicious 0-byte allocation");
    }
    if (unlikely(ret == NULL)) {
        rvvm_fatal("Out of memory!");
        must_never_reach();
    }
    return ret;
}

/*
 * Command line & config parsing
 */

static int    argc_internal = 0;
static char** argv_internal = NULL;

static void rvvm_set_args_internal(int argc, char** argv)
{
    if (argc > 1) {
        argc_internal = argc;
        argv_internal = argv;
    }
}

void rvvm_set_args(int argc, char** argv)
{
    rvvm_set_args_internal(argc, argv);
    if (rvvm_has_arg("v") || rvvm_has_arg("verbose")) {
        rvvm_set_loglevel(LOG_INFO);
    }
}

static inline bool rvvm_config_is_newline(char c)
{
    return c == '\n' || c == '\r';
}

static inline bool rvvm_config_is_space(char c)
{
    return c == ' ' || c == '\t';
}

static inline bool rvvm_config_is_delim(char c)
{
    return rvvm_config_is_space(c) || rvvm_config_is_newline(c);
}

static inline bool rvvm_config_is_comment(char c)
{
    return c == '#';
}

static inline bool rvvm_config_is_quot(char c)
{
    return c == '\"';
}

static int rvvm_split_config(const char* str, int argc, char** argv)
{
    bool arg = true;
    int  ret = 1;
    if (argc && argv) {
        argv[0] = "rvvm";
    }
    while (*str) {
        // Skip delimiters (spaces, newlines)
        while (rvvm_config_is_delim(*str)) {
            if (rvvm_config_is_newline(*str)) {
                // Found newline
                arg = true;
            }
            str++;
        }
        if (rvvm_config_is_comment(*str)) {
            // Skip comment
            while (*str && !rvvm_config_is_newline(*str)) {
                str++;
            }
        } else {
            // Parse token
            bool        quoted = rvvm_config_is_quot(*str);
            const char* token  = str + quoted;
            if (quoted) {
                // Find end of quotation
                do {
                    str++;
                } while (*str && !rvvm_config_is_quot(*str));
                str += rvvm_config_is_quot(*str);
            } else {
                // Find delimiter or start of comment
                while (*str && !rvvm_config_is_delim(*str) && !rvvm_config_is_comment(*str)) {
                    str++;
                }
            }
            if (ret < argc && argv) {
                // Fill argv
                size_t size   = str - token - quoted;
                char*  buffer = safe_new_arr(char, size + arg + 1);
                if (arg) {
                    buffer[0] = '-';
                }
                memcpy(buffer + arg, token, size);
                argv[ret] = buffer;
            }
            arg = false;
            ret++;
        }
    }
    return ret;
}

bool rvvm_load_config(const char* path)
{
    rvfile_t* cfg = rvopen(path, 0);
    if (rvfilesize(cfg) && rvfilesize(cfg) < 0x100000) {
        char* buf = safe_new_arr(char, rvfilesize(cfg) + 1);
        if (rvread(cfg, buf, rvfilesize(cfg), 0) == rvfilesize(cfg)) {
            int argc = rvvm_split_config(buf, 0, NULL);
            if (argc > 1) {
                char** argv = safe_new_arr(char*, argc + 1);
                rvvm_split_config(buf, argc, argv);
                rvvm_set_args_internal(argc, argv);
            }
        }
        safe_free(buf);
    }
    rvclose(cfg);
    return !!cfg;
}

const char* rvvm_next_arg(const char** val, int* iter)
{
    int i = *iter;
    if (argc_internal < 2) {
        DO_ONCE(rvvm_load_config("rvvm.cfg"));
    }
    if (val) {
        *val = NULL;
    }
    if (argv_internal && i < argc_internal && argv_internal[i]) {
        const char* arg = argv_internal[i];
        if (arg[0] == '-') {
            // Skip -, -- argument prefix
            arg += (arg[1] == '-') ? 2 : 1;
            // Get trailing argument value, if any
            if (i + 1 < argc_internal) {
                const char* next_arg = argv_internal[i + 1];
                if (next_arg && next_arg[0] != '-') {
                    if (val) {
                        *val = next_arg;
                    }
                    i++;
                }
            }
        } else {
            // This is a free-standing value without -arg prefix
            if (val) {
                *val = arg;
            }
            arg = "";
        }
        *iter = i + 1;
        return arg;
    }
    return NULL;
}

bool rvvm_has_arg(const char* arg)
{
    const char* arg_name = NULL;
    int         arg_iter = 1;
    while ((arg_name = rvvm_next_arg(NULL, &arg_iter))) {
        if (rvvm_strcmp(arg_name, arg)) {
            return true;
        }
    }
    return false;
}

const char* rvvm_getarg(const char* arg)
{
    const char* arg_name = NULL;
    const char* arg_val  = NULL;
    int         arg_iter = 1;
    while ((arg_name = rvvm_next_arg(&arg_val, &arg_iter))) {
        if (rvvm_strcmp(arg_name, arg)) {
            return arg_val;
        }
    }
    return NULL;
}

bool rvvm_getarg_bool(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        return rvvm_strcmp(arg_val, "true");
    }
    return false;
}

int rvvm_getarg_int(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        return str_to_int_dec(arg_val);
    }
    return 0;
}

uint64_t rvvm_getarg_size(const char* arg)
{
    const char* arg_val = rvvm_getarg(arg);
    if (arg_val) {
        size_t   len = 0;
        uint64_t ret = str_to_int_base(arg_val, &len, 0);
        return ret << mem_suffix_shift(arg_val[len]);
    }
    return 0;
}

/*
 * Logger
 */

static int rvvm_loglevel = LOG_WARN;

void rvvm_set_loglevel(int loglevel)
{
    rvvm_loglevel = loglevel;
}

static bool log_has_colors(void)
{
    // TERM is set on any POSIX, WT_SESSION is set by Windows Terminal
    return !!getenv("TERM") || !!getenv("WT_SESSION");
}

/* Milliseconds on the host's monotonic clock - the same epoch the host's own
 * log lines use (GetTickCount64 on win32) and the one a guest's
 * clock_gettime(CLOCK_MONOTONIC) reads, since the emulator serves that from the
 * host. Several streams (the core's trace, the host's console, a session
 * server's own lines, a driver's transcript) end up in one file, and only a
 * shared clock says in what order things really happened - which is the whole
 * point when a symptom is a race. */
static uint64_t log_time_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts = {0};
    if (!clock_gettime(CLOCK_MONOTONIC, &ts)) {
        return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    }
#endif
    return rvtimer_clocksource(1000);
}

static void log_print(const char* prefix, const char* fmt, const void* argv)
{
    char   buffer[384] = {0};
    size_t pos         = rvvm_snprintf(buffer, sizeof(buffer), "[%9llu ms] ",
                                       (unsigned long long)log_time_ms());
    pos = EVAL_MIN(pos, sizeof(buffer) - 1);
    pos += rvvm_strlcpy(buffer + pos, prefix, sizeof(buffer) - pos);
    pos = EVAL_MIN(pos, sizeof(buffer) - 1);
    size_t vsp_size    = sizeof(buffer) - EVAL_MIN(pos + 6, sizeof(buffer));
    if (vsp_size > 1) {
        int tmp = rvvm_vsnprintf(buffer + pos, vsp_size, fmt, argv);
        if (tmp > 0) {
            pos += EVAL_MIN(vsp_size - 1, (size_t)tmp);
        }
    }
    rvvm_strlcpy(buffer + pos, log_has_colors() ? "\033[0m\n" : "\n", sizeof(buffer) - pos);
    fputs(buffer, stderr);
    // Flush right away: a crash or a kill must not swallow the last messages
    fflush(stderr);
#if defined(ANDROID)
    {
        char* log_body = buffer;
        while (*log_body == ' ') {
            log_body++;
        }
        __android_log_print(ANDROID_LOG_WARN, "RVVM", "%s", log_body);
    }
#endif
}

/* The identity a trace line can carry, see rvvm_trace_set_id_fn(). Declared
 * here rather than next to the trace machinery because every level wants it. */
static rvvm_trace_id_fn rvvm_trace_id;

/*
 * One prefix builder for every level, so "INFO" and "TRACE" are attributed the
 * same way. The identity is not decoration: with a run's guest processes
 * interleaved into one stderr stream, "INFO: sys_connect(6, ...)" cannot be
 * told apart from a different process's line, and that is exactly the question
 * a log like this gets opened to answer. A run with no userland attached, or
 * one calling from outside any guest thread, still reads "INFO: " / "[host]".
 */
#define LOG_PREFIX_SIZE 64
static const char* log_prefix(char* buf, size_t size, const char* level, const char* color)
{
    char ids[40] = {0};

    if (rvvm_trace_id) {
        rvvm_trace_id(ids, sizeof ids);
    }
    if (log_has_colors()) {
        rvvm_snprintf(buf, size, ids[0] ? "\033[%s1m%s\033[0;1m%s: " : "\033[%s1m%s\033[0;1m: ",
                     color, level, ids);
    } else {
        rvvm_snprintf(buf, size, ids[0] ? "%s%s: " : "%s: ", level, ids);
    }
    return buf;
}

#if defined(USE_DEBUG)

PRINT_FORMAT void rvvm_debug(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_INFO) {
        char   prefix[LOG_PREFIX_SIZE];
        va_list args;
        va_start(args, format_str);
        log_print(log_prefix(prefix, sizeof prefix, "DEBUG", "33"), format_str, &args);
        va_end(args);
    }
}

#endif

PRINT_FORMAT void rvvm_info(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_INFO) {
        char   prefix[LOG_PREFIX_SIZE];
        va_list args;
        va_start(args, format_str);
        log_print(log_prefix(prefix, sizeof prefix, "INFO", "33"), format_str, &args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_warn(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_WARN) {
        char   prefix[LOG_PREFIX_SIZE];
        va_list args;
        va_start(args, format_str);
        log_print(log_prefix(prefix, sizeof prefix, "WARN", "31"), format_str, &args);
        va_end(args);
    }
}

/* ---------------------------------------------------------------- *
 * Trace categories (see utils.h)
 * ---------------------------------------------------------------- */

static uint32_t rvvm_trace_mask  = 0;
static bool     rvvm_trace_ready = false;

static const struct {
    uint32_t    bit;
    const char* name;
} rvvm_trace_names[] = {
    { RVVM_TRC_PATH,   "path"   },
    { RVVM_TRC_FD,     "fd"     },
    { RVVM_TRC_PTY,    "pty"    },
    { RVVM_TRC_PTY_VERBOSE,    "[V] pty"    },
    { RVVM_TRC_JOB,    "job"    },
    { RVVM_TRC_TTY,    "tty"    },
    { RVVM_TRC_SIGNAL, "signal" },
    { RVVM_TRC_MMAP,   "mmap"   },
    { RVVM_TRC_SYS,    "sys"    },
    { RVVM_TRC_DEV,    "dev"    },
    { RVVM_TRC_WSOCK,    "wsock"    },
};

/* A name (or "all") to its bit, 0 when unknown. Length-bounded so the value in
 * the environment can be sliced in place. */
static uint32_t rvvm_trace_lookup(const char* name, size_t len)
{
    if (len == 3 && !strncmp(name, "all", 3)) {
        return RVVM_TRC_ALL;
    }
    for (size_t i = 0; i < sizeof(rvvm_trace_names) / sizeof(rvvm_trace_names[0]); i++) {
        if (strlen(rvvm_trace_names[i].name) == len &&
            !strncmp(name, rvvm_trace_names[i].name, len)) {
            return rvvm_trace_names[i].bit;
        }
    }
    return 0;
}

PUBLIC void rvvm_trace_init(void)
{
    const char* env;
    const char* at;

    if (rvvm_trace_ready) {
        return;
    }
    rvvm_trace_ready = true;

    /* The switch this system replaced: RVVM_TRACE_PATH meant "the path trace". */
    if (getenv("RVVM_TRACE_PATH")) {
        rvvm_trace_mask |= RVVM_TRC_PATH;
    }
    env = getenv("RVVM_TRACE");
    if (!env || !*env) {
        return;
    }

    at = env;
    while (*at) {
        const char* start;
        size_t      len;
        bool        off;
        uint32_t    bit;

        while (*at == ',' || *at == ' ' || *at == '\t') {
            at++;
        }
        if (!*at) {
            break;
        }
        off   = *at == '-';
        at   += off ? 1 : 0;
        start = at;
        while (*at && *at != ',' && *at != ' ' && *at != '\t') {
            at++;
        }
        len = (size_t)(at - start);
        if (!len) {
            continue;
        }
        bit = rvvm_trace_lookup(start, len);
        if (!bit) {
            rvvm_warn("RVVM_TRACE: unknown category '%.*s'", (int)len, start);
            continue;
        }
        if (off) {
            rvvm_trace_mask &= ~bit;
        } else {
            rvvm_trace_mask |= bit;
        }
    }
}

PUBLIC bool rvvm_trace_enabled(uint32_t cat)
{
    if (unlikely(!rvvm_trace_ready)) {
        rvvm_trace_init();
    }
    return (rvvm_trace_mask & cat) != 0;
}

PUBLIC void rvvm_trace_set_id_fn(rvvm_trace_id_fn fn)
{
    rvvm_trace_id = fn;
}

/* Independent of loglevel on purpose: the category IS the switch, and asking
 * for one should not also require the noise of the others (which is what a
 * higher loglevel would bring). */
PUBLIC PRINT_FORMAT_ARG2 void rvvm_trace(uint32_t cat, const char* format_str, ...)
{
    va_list args;
    if (!rvvm_trace_enabled(cat)) {
        return;
    }
    {
        char prefix_buf[LOG_PREFIX_SIZE];
        va_start(args, format_str);
        log_print(log_prefix(prefix_buf, sizeof prefix_buf, "TRACE", "36"), format_str, &args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_error(const char* format_str, ...)
{
    if (rvvm_loglevel >= LOG_ERROR) {
        char   prefix[LOG_PREFIX_SIZE];
        va_list args;
        va_start(args, format_str);
        log_print(log_prefix(prefix, sizeof prefix, "ERROR", "31"), format_str, &args);
        va_end(args);
    }
}

PRINT_FORMAT void rvvm_fatal(const char* format_str, ...)
{
    char   prefix[LOG_PREFIX_SIZE];
    va_list args;
    va_start(args, format_str);
    log_print(log_prefix(prefix, sizeof prefix, "FATAL", "31"), format_str, &args);
    va_end(args);
    stacktrace_print();
    abort();
    // cppcheck-suppress unreachableCode
    must_never_reach();
}

/*
 * Initialization/deinitialization
 */

slow_path void do_once_finalize(uint32_t* ticket)
{
    while (atomic_load_uint32_ex(ticket, ATOMIC_ACQUIRE) != 2) {
        rvvm_sched_yield();
    }
}

typedef void (*deinit_func_t)(void);

static vector_t(deinit_func_t) deinit_funcs = ZERO_INIT;
static rvvm_lock_t             deinit_lock  = ZERO_INIT;
static bool                    deinit_made  = false;

void call_at_deinit(void (*function)(void))
{
    rvvm_spin_lock(&deinit_lock);
    if (!deinit_made) {
        vector_push_back(deinit_funcs, function);
        function = NULL;
    }
    rvvm_spin_unlock(&deinit_lock);
    if (function) {
        function();
    }
}

static deinit_func_t dequeue_func(void)
{
    deinit_func_t ret = NULL;
    rvvm_spin_lock(&deinit_lock);
    deinit_made = true;
    if (vector_size(deinit_funcs)) {
        size_t end = vector_size(deinit_funcs) - 1;
        ret        = vector_at(deinit_funcs, end);
        vector_erase(deinit_funcs, end);
    }
    rvvm_spin_unlock(&deinit_lock);
    return ret;
}

GNU_DESTRUCTOR void full_deinit(void)
{
    deinit_func_t func = dequeue_func();
    if (func) {
        rvvm_info("Fully deinitializing librvvm");
        do {
            func();
            func = dequeue_func();
        } while (func);
    }
}

POP_OPTIMIZATION_SIZE
