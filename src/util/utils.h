/*
utils.h - Util functions
Copyright (C) 2021  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_UTILS_H
#define RVVM_UTILS_H

#include "atomics.h" // IWYU pragma: keep
#include "rvvmlib.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * String & numeric helpers
 */

// Evaluate max/min value
#define EVAL_MAX(a, b)         ((a) > (b) ? (a) : (b))
#define EVAL_MIN(a, b)         ((a) < (b) ? (a) : (b))

// Compute length of a static array
#define STATIC_ARRAY_SIZE(arr) (sizeof(arr) / sizeof(*(arr)))

// Align size up (To power of two!)
static inline size_t align_size_up(size_t x, size_t align)
{
    return (x + (align - 1)) & ~(align - 1);
}

// Align size down (To power of two!)
static inline size_t align_size_down(size_t x, size_t align)
{
    return x & ~(align - 1);
}

// Portable strtol/ltostr replacement
size_t uint_to_str_base(char* str, size_t size, uint64_t val, uint8_t base);
size_t int_to_str_base(char* str, size_t size, int64_t val, uint8_t base);
size_t int_to_str_dec(char* str, size_t size, int64_t val);


PUBLIC uint64_t str_to_uint_base(const char* str, size_t* len, uint8_t base);
int64_t         str_to_int_base(const char* str, size_t* len, uint8_t base);
int64_t         str_to_int_dec(const char* str);

// Portable & safer string.h replacement
size_t rvvm_strlen(const char* string);
size_t rvvm_strnlen(const char* string, size_t size);

PUBLIC bool rvvm_strcmp(const char* s1, const char* s2);
const char* rvvm_strfind(const char* string, const char* pattern);

size_t rvvm_strlcpy(char* dst, const char* src, size_t size);

// Portable vsnprintf() replacement
size_t rvvm_vsnprintf(char* buffer, size_t size, const char* fmt, const void* argv);
size_t rvvm_snprintf(char* buffer, size_t size, const char* fmt, ...);

static inline size_t mem_suffix_shift(char suffix)
{
    switch (suffix) {
        case 'k':
        case 'K':
            return 10;
        case 'm':
        case 'M':
            return 20;
        case 'g':
        case 'G':
            return 30;
        case 'T':
            return 40;
        default:
            return 0;
    }
}

// Generate random bytes
void rvvm_randombytes(void* buffer, size_t size);

// Generate random serial number (0-9, A-Z)
void rvvm_randomserial(char* serial, size_t size);

/*
 * Safe memory allocation
 */

#if GNU_ATTRIBUTE(__returns_nonnull__) && GNU_ATTRIBUTE(__warn_unused_result__) /**/                                   \
    && GNU_ATTRIBUTE(__malloc__) && GNU_ATTRIBUTE(__alloc_size__)
#define SAFE_MALLOC  __attribute__((__returns_nonnull__, __warn_unused_result__, __malloc__, __alloc_size__(1)))
#define SAFE_CALLOC  __attribute__((__returns_nonnull__, __warn_unused_result__, __malloc__, __alloc_size__(1, 2)))
#define SAFE_REALLOC __attribute__((__returns_nonnull__, __warn_unused_result__, __alloc_size__(2)))
#else
#define SAFE_MALLOC  GNU_DUMMY_ATTRIBUTE
#define SAFE_CALLOC  GNU_DUMMY_ATTRIBUTE
#define SAFE_REALLOC GNU_DUMMY_ATTRIBUTE
#endif

// These never return NULL
PUBLIC SAFE_MALLOC void*  safe_malloc(size_t size);
PUBLIC SAFE_CALLOC void*  safe_calloc(size_t size, size_t n);
PUBLIC SAFE_REALLOC void* safe_realloc(void* ptr, size_t size);

// Safe object allocation with type checking & zeroing
#define safe_new_arr(type, size) ((type*)safe_calloc(size, sizeof(type)))
#define safe_new_obj(type)       safe_new_arr(type, 1)

// Free and poison the pointer
#define safe_free(ptr)                                                                                                 \
    do {                                                                                                               \
        free(ptr);                                                                                                     \
        (ptr) = NULL;                                                                                                  \
    } while (0)

/*
 * Unicode handling
 */

static inline size_t utf8_decode_code_point(const char* str, size_t size, uint32_t* code_point)
{
    const uint8_t* str_u8 = (const uint8_t*)str;

    if (size >= 1 && str_u8[0] < 0x80UL) {
        // ASCII character
        *code_point = str_u8[0];
        return 1;
    } else if (size >= 1 && str_u8[0] < 0xC2UL) {
        // Illegal UTF8 sequence (Character starts with continuation/reserved byte)
        *code_point = 0;
        return 0;
    } else if (size >= 2 && str_u8[0] < 0xE0UL) {
        // 2-byte code point
        *code_point = ((str_u8[0] & 0x1FUL) << 6) | (str_u8[1] & 0x3FUL);
        return 2;
    } else if (size >= 3 && str_u8[0] < 0xF0UL) {
        // 3-byte code point
        *code_point = ((str_u8[0] & 0x0FUL) << 12) | ((str_u8[1] & 0x1FUL) << 6) | (str_u8[2] & 0x3FUL);
        return 3;
    } else if (size >= 4 && str_u8[0] < 0xF6U) {
        // 4-byte code point
        *code_point  = ((str_u8[0] & 0x07UL) << 18) | ((str_u8[1] & 0x0FUL) << 12);
        *code_point |= ((str_u8[2] & 0x1FUL) << 6) | (str_u8[3] & 0x3FUL);
        return 4;
    }

    *code_point = 0;
    return 0;
}

static inline size_t utf8_encode_code_point(char* str, size_t size, uint32_t code_point)
{
    uint8_t* str_u8 = (uint8_t*)str;

    if (size >= 1 && code_point < 0x80UL) {
        // ASCII character
        str_u8[0] = code_point;
        return 1;
    } else if (size >= 2 && code_point < 0x800UL) {
        // 2-byte code point
        str_u8[0] = (0xC0UL | (code_point >> 6));
        str_u8[1] = (0x80UL | (code_point & 0x3FUL));
        return 2;
    } else if (size >= 3 && code_point < 0x10000UL) {
        // 3-byte code point
        str_u8[0] = (0xE0UL | (code_point >> 12));
        str_u8[1] = (0x80UL | ((code_point >> 6) & 0x3FUL));
        str_u8[2] = (0x80UL | (code_point & 0x3F));
        return 3;
    } else if (size >= 4 && code_point < 0x200000UL) {
        // 4-byte code point
        str_u8[0] = (0xF0UL | (code_point >> 18));
        str_u8[1] = (0x80UL | ((code_point >> 12) & 0x3FUL));
        str_u8[2] = (0x80UL | ((code_point >> 6) & 0x3FUL));
        str_u8[3] = (0x80UL | (code_point & 0x3FUL));
        return 4;
    }

    return 0;
}

static inline size_t utf16_decode_code_point(const uint16_t* str, size_t size, uint32_t* code_point)
{
    const uint16_t* str_u16 = str;

    if (size >= 1 && (str_u16[0] >= 0xE000UL || str_u16[0] < 0xD800UL)) {
        // Single code unit
        *code_point = str_u16[0];
        return 1;
    } else if (size >= 2 && str_u16[0] >= 0xD800UL && str_u16[0] < 0xE000UL) {
        // Surrogate pair encoding
        *code_point = (str_u16[0] & 0x3FFUL) << 10 | (str_u16[1] & 0x3FFUL);
        return 2;
    }

    *code_point = 0;
    return 0;
}

static inline size_t utf16_encode_code_point(uint16_t* str, size_t size, uint32_t code_point)
{
    uint16_t* str_u16 = str;

    if (size >= 1 && (code_point < 0xD800UL || (code_point >= 0xE000UL && code_point < 0x10000UL))) {
        // Single code unit
        str_u16[0] = code_point;
        return 1;
    } else if (size >= 2 && code_point <= 0x110000UL) {
        // Surrogate pair encoding
        code_point -= 0x10000UL;
        str_u16[0]  = (0xD800UL | (code_point >> 10));
        str_u16[1]  = (0xDC00UL | (code_point & 0x3FFUL));
        return 2;
    }

    return 0;
}

static inline uint16_t* utf8_to_utf16(const char* str_u8)
{
    size_t    size_u16 = 16;
    size_t    pos_u16  = 0;
    size_t    size_u8  = 0;
    size_t    pos_u8   = 0;
    uint16_t* str_u16  = safe_new_arr(uint16_t, size_u16 + 1);
    while (str_u8 && str_u8[size_u8]) {
        size_u8++;
    }
    while (pos_u8 < size_u8) {
        uint32_t code = 0;
        size_t   b_u8 = utf8_decode_code_point(str_u8 + pos_u8, size_u8 - pos_u8, &code);
        if (b_u8) {
            size_t b_u16 = utf16_encode_code_point(str_u16 + pos_u16, size_u16 - pos_u16, code);
            if (b_u16) {
                pos_u16 += b_u16;
                pos_u8  += b_u8;
            } else if (size_u16 - pos_u16 < 2) {
                size_u16 += size_u16 >> 2;
                str_u16   = (uint16_t*)safe_realloc(str_u16, (size_u16 + 1) * sizeof(uint16_t));
            } else {
                // Failed to encode UTF-16
                safe_free(str_u16);
                return NULL;
            }
        } else {
            // Invalid UTF-8 input
            safe_free(str_u16);
            return NULL;
        }
    }
    str_u16[pos_u16] = 0;
    return str_u16;
}

static inline char* utf16_to_utf8(const uint16_t* str_u16)
{
    size_t size_u8  = 16;
    size_t pos_u8   = 0;
    size_t size_u16 = 0;
    size_t pos_u16  = 0;
    char*  str_u8   = safe_new_arr(char, size_u8 + 1);
    while (str_u16 && str_u16[size_u16]) {
        size_u16++;
    }
    while (pos_u16 < size_u16) {
        uint32_t code  = 0;
        size_t   b_u16 = utf16_decode_code_point(str_u16 + pos_u16, size_u16 - pos_u16, &code);
        if (b_u16) {
            size_t b_u8 = utf8_encode_code_point(str_u8 + pos_u8, size_u8 - pos_u8, code);
            if (b_u8) {
                pos_u16 += b_u16;
                pos_u8  += b_u8;
            } else if (size_u8 - pos_u8 < 4) {
                size_u8 += size_u8 >> 2;
                str_u8   = (char*)safe_realloc(str_u8, size_u8 + 1);
            } else {
                // Failed to encode UTF-8
                safe_free(str_u8);
                return NULL;
            }
        } else {
            // Invalid UTF-16 input
            safe_free(str_u8);
            return NULL;
        }
    }
    str_u8[pos_u8] = 0;
    return str_u8;
}

/*
 * Command line & config parsing
 */

// Set command line arguments
PUBLIC void rvvm_set_args(int argc, char** argv);

// Load config file
PUBLIC bool rvvm_load_config(const char* path);

// Iterate over arguments in form of <-arg> [val], or <val> ("" is returned in such case)
PUBLIC const char* rvvm_next_arg(const char** val, int* iter);

// Check if argument is present on the command line or in the config
PUBLIC bool rvvm_has_arg(const char* arg);

// Get argument value
PUBLIC const char* rvvm_getarg(const char* arg);
PUBLIC bool        rvvm_getarg_bool(const char* arg);
PUBLIC int         rvvm_getarg_int(const char* arg);
PUBLIC uint64_t    rvvm_getarg_size(const char* arg);

/*
 * Logger
 */

#define LOG_NONE  0
#define LOG_ERROR 1
#define LOG_WARN  2
#define LOG_INFO  3

/* A debug build says more, not at another severity: LOG_DEBUG answers to the
 * same switch as LOG_INFO, and only the word in the prefix differs - which is
 * the whole point of having it, since a log you cannot tell a debug line from
 * an info line is one you cannot filter. */
#define LOG_DEBUG 4

/* Not thresholds on rvvm_loglevel - these two say *how* a line reached the
 * logger rather than how loudly it is. LOG_FATAL is the last line before the
 * process dies, so it is never gated; LOG_TRACE has its own switch, the trace
 * category. Both sort above the levels so an existing ">= LOG_INFO"
 * comparison cannot mistake them for one the host asked for. */
#define LOG_FATAL 5
#define LOG_TRACE 6

PUBLIC void rvvm_set_loglevel(int loglevel);

/*
 * Reading a command line: one scanner, for everyone who has one.
 *
 * A machine's command line is asked about from three layers - the core applying
 * root=, the logger reading loglevel=, and each host reading init= - and the
 * answers have to agree. They agree because there is one scanner here rather
 * than three, and it takes a plain string so no layer has to own the machine to
 * ask a question about it.
 *
 * Tokens are separated by spaces and tabs. `key=value` splits at the *first* '='
 * in the token, so a value may contain one; `key` alone is a flag. Unknown
 * arguments are simply not found: a command line is not a schema, and refusing
 * one because it names something this build has never heard of would make the
 * string unusable for the guest program it was written for.
 */
PUBLIC const char* rvvm_cmdline_get(const char* cmdline, const char* key);
PUBLIC bool        rvvm_cmdline_has(const char* cmdline, const char* key);

/* Apply the logging arguments of @cmdline - `debug` (bare) and `loglevel=`,
 * which takes either a name (none/error/warn/info/debug) or the kernel's 0..4.
 *
 * Both are read from the command line rather than from a host flag because the
 * answer they give is global (rvvm_set_loglevel is process-wide) and a host that
 * wanted it per machine could not honour that. So the argument is spelled once
 * and applies to the process, which is the same bargain the -v flag makes.
 *
 * Applied, not validated: an unreadable loglevel= leaves the level the host
 * already chose, because a typo in a verbosity argument should not change how
 * much of the run is reported - and should not stop the run either.
 */
PUBLIC void rvvm_apply_cmdline_logging(const char* cmdline);

#if GNU_ATTRIBUTE(__format__)
#define PRINT_FORMAT     __attribute__((__format__(printf, 1, 2)))
#define PRINT_FORMAT_ARG2 __attribute__((__format__(printf, 2, 3)))
#define PRINT_FORMAT_ARG3 __attribute__((__format__(printf, 3, 4)))
#else
#define PRINT_FORMAT     GNU_DUMMY_ATTRIBUTE
#define PRINT_FORMAT_ARG2 GNU_DUMMY_ATTRIBUTE
#define PRINT_FORMAT_ARG3 GNU_DUMMY_ATTRIBUTE
#endif

#if defined(USE_DEBUG)

// Debug logger enabled at build time
PRINT_FORMAT void rvvm_debug(const char* format_str, ...);

#else

// Debug logs optimized out, but still performs compile-time format / unused arguments checking
static PRINT_FORMAT forceinline void rvvm_debug(const char* format_str, ...)
{
    UNUSED(format_str);
}

#endif

// Logging functions (controlled by loglevel)
PUBLIC PRINT_FORMAT void rvvm_info(const char* format_str, ...);
PUBLIC PRINT_FORMAT void rvvm_warn(const char* format_str, ...);
PUBLIC PRINT_FORMAT void rvvm_error(const char* format_str, ...);
PUBLIC PRINT_FORMAT void rvvm_fatal(const char* format_str, ...); // Aborts the process

/*
 * The tagged entry point, and the one place a host decides where a line goes.
 *
 * Every log line in the tree - the core's own levels, the Android bridge's
 * LOGI/LOGW/LOGE, the cmdpost bridge's CMDLOG, the sensor's SENSLOG, the
 * win32 hosts' fprintf(stderr) - ends up in one function, log_emit(), and
 * leaves through exactly one sink. What used to be a destination chosen by an
 * #ifdef at each of those sites is now one decision made once, at startup.
 *
 * The sink sees the line already formatted, plus the three things a host
 * genuinely needs to route it:
 *
 *   level  LOG_ERROR .. LOG_TRACE, so a host can map to its own severity
 *          instead of pinning everything to one value (which is what made
 *          logcat's priority filter useless here - see log_default_sink).
 *   tag    the subsystem name, or NULL for a core line. Android wants it as
 *          logcat's tag so `adb logcat RVVM-JNI:D` keeps working.
 *   ids    who called: "[1234:1235]" for a guest thread, "[host:7]" for one
 *          of the host's own. A sink is free to print it, or to ignore it
 *          because its destination already attributes lines itself.
 *   cat    the trace category, or 0 for a non-trace line.
 *
 * A sink must not call back into the logger; the reentrancy guard in
 * log_emit() drops such a line rather than recursing. It runs with no lock
 * held and must not block - it is called from paths where blocking is not
 * an option (a fault handler, a render thread).
 */
typedef void (*rvvm_log_sink_fn)(int level, uint32_t cat, const char* tag, const char* ids, const char* line,
                                 size_t len);

PUBLIC void rvvm_log_set_sink(rvvm_log_sink_fn sink);

/* The default sink: stderr everywhere, plus logcat on Android. Exposed so a
 * host that installs its own can delegate to it instead of re-deriving the
 * platform behaviour. */
PUBLIC void rvvm_log_default_sink(int level, uint32_t cat, const char* tag, const char* ids, const char* line,
                                  size_t len);

PUBLIC PRINT_FORMAT_ARG3 void rvvm_log(int level, const char* tag, const char* format_str, ...);

#define RVVM_LOGE(tag, ...) rvvm_log(LOG_ERROR, tag, __VA_ARGS__)
#define RVVM_LOGW(tag, ...) rvvm_log(LOG_WARN, tag, __VA_ARGS__)
#define RVVM_LOGI(tag, ...) rvvm_log(LOG_INFO, tag, __VA_ARGS__)

/*
 * The ring.
 *
 * Everything that reaches the sink also lands here, in a fixed static buffer
 * with no allocation - which is the whole point: the lines worth having after
 * a crash are the ones written when the heap and the process's streams were
 * already going away. Old lines are overwritten, never the new ones, so the
 * buffer always ends up holding the tail of the run.
 *
 * The ring is what makes a noisy trace affordable. RVVM_TRACE=sys is a few
 * thousand lines a second and would bury the guest's console on win32, where
 * the host's stderr is shared with it, but it costs nothing to keep the tail
 * and dump it after the fact. It is also the answer to "which host thread
 * said that" - [host:N] is stable per thread for the life of the process.
 *
 * Dumped on rvvm_fatal() before the abort, and available on demand:
 *   rvvm_logring_dump(FILE*)        stream the contents out oldest-first
 *   rvvm_logring_dump_path(path)    the same into a file, created if needed
 *   rvvm_logring_clear()            drop the contents (a run's worth of history
 *                                   between two guests, say)
 */
PUBLIC void rvvm_logring_dump(FILE* stream);
PUBLIC bool rvvm_logring_dump_path(const char* path);
PUBLIC void rvvm_logring_clear(void);
PUBLIC size_t rvvm_logring_lines(void);

/*
 * Trace logging, by category.
 *
 * A trace is not a warning: it is a fact worth seeing only when the subsystem
 * it belongs to is being looked at, and several may be looked at at once. Each
 * category is one bit, selected from the RVVM_TRACE environment variable - a
 * comma/space separated list of names, "all" for every one, and "-name" to turn
 * one back off (RVVM_TRACE=all,-fd). RVVM_TRACE_PATH is kept as an alias for
 * "path", the switch this replaced.
 *
 * rvvm_warn() stays what it is: a message that belongs in every run.
 */
enum rvvm_trace_cat {
    RVVM_TRC_PATH   = 1u << 0,   // path resolution (wrap_guest_path, shadow)
    RVVM_TRC_FD     = 1u << 1,   // the fd table: install/close/dump, fstat
    RVVM_TRC_PTY    = 1u << 2,   // pty reads/writes, line discipline
    RVVM_TRC_JOB    = 1u << 3,   // processes: fork/wait/kill/stop, proc table
    RVVM_TRC_TTY    = 1u << 4,   // the console line discipline
    RVVM_TRC_SIGNAL = 1u << 5,   // signal delivery and return
    RVVM_TRC_MMAP   = 1u << 6,   // mmap/brk and their host backing
    RVVM_TRC_SYS    = 1u << 7,   // generic syscall enter/exit traces
    RVVM_TRC_DEV    = 1u << 8,   // synthetic devices
    RVVM_TRC_PTY_VERBOSE = 1u << 9,  // pty poll-loop noise (parking), off unless asked
    RVVM_TRC_WSOCK = 1u << 10,  
    RVVM_TRC_ALL    = 0xffffffffu,
};

/*
 * Optional identity appended to every log line, whatever its level: the
 * userland layer registers a formatter that writes the calling guest thread's
 * "[pid:tid]", so interleaved lines from two in-process fork()ed processes - a
 * socketpair close on one side, a recv on the other - can be told apart even
 * when they share a millisecond timestamp.
 *
 * This covers INFO/WARN/ERROR/FATAL as well as traces. It has to: a run's guest
 * processes all share one stderr stream, so an unattributed "INFO:
 * sys_connect(6, ...)" is a line nobody can act on - the question a log like
 * this gets opened to answer is precisely *whose* call that was.
 *
 * A formatter that leaves the buffer empty means "not a guest thread" - the
 * host's own threads, of which there are several (the JNI binder thread, the
 * GL render thread, an audio callback). Those are then labelled "[host:N]",
 * with N stable for the life of the process. Without that, the Android
 * bridge's GL lines and its binder lines were both just "[host]" and could
 * not be told apart at all once they were out of logcat's own tid column.
 *
 * With no formatter registered at all (a build with no userland attached)
 * lines keep the plain "INFO: " form, so rvvm_cli and the library are
 * unchanged.
 */
typedef void (*rvvm_trace_id_fn)(char* buf, size_t size);
PUBLIC void rvvm_trace_set_id_fn(rvvm_trace_id_fn fn);

/* Parse RVVM_TRACE once. Idempotent, and called lazily by the helpers below. */
PUBLIC void rvvm_trace_init(void);
PUBLIC bool rvvm_trace_enabled(uint32_t cat);
PUBLIC PRINT_FORMAT_ARG2 void rvvm_trace(uint32_t cat, const char* format_str, ...);

#define RVVM_TRC(cat, ...) \
    do { if (unlikely(rvvm_trace_enabled(cat))) rvvm_trace((cat), __VA_ARGS__); } while (0)

/*
 * Initialization/deinitialization
 */

slow_path void do_once_finalize(uint32_t* ticket);

/*
 * Run scoped statement once per library lifetime (In a thread-safe way)
 *
 * DO_ONCE_SCOPED {
 *     if (!feature_avail()) {
 *         break;
 *     }
 *
 *     global_feature_init();
 * }
 */
#define DO_ONCE_SCOPED                                                                                                 \
    static uint32_t MACRO_IDENT(do_once_ticket) = 0;                                  /**/                             \
    POST_STMT_NAMED (unlikely(atomic_load_uint32(&MACRO_IDENT(do_once_ticket)) != 2), /**/                             \
                     do_once_finalize(&MACRO_IDENT(do_once_ticket)), ticket_stmt)     /**/                             \
        POST_STMT_NAMED (atomic_cas_uint32(&MACRO_IDENT(do_once_ticket), 0, 1),       /**/                             \
                         atomic_store_uint32(&MACRO_IDENT(do_once_ticket), 2), claim_stmt)

/*
 * Run an expression once per library lifetime (In a thread-safe way)
 *
 * DO_ONCE(rvvm_warn("This will only be printed once upon reaching!"));
 */
#define DO_ONCE(expr_once)                                                                                             \
    do {                                                                                                               \
        DO_ONCE_SCOPED {                                                                                               \
            expr_once;                                                                                                 \
        }                                                                                                              \
    } while (0)

// Register a callback to be ran at deinitialization (exit or library unload)
void call_at_deinit(void (*function)(void));

// Perform manual deinitialization
GNU_DESTRUCTOR void full_deinit(void);

#endif
