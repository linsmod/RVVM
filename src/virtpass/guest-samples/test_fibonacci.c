/*
 * test_fibonacci.c - Fibonacci self-test for the RISC-V guest
 *
 * The pure-computation counterpart to the ABI samples: it stays inside one
 * process, needs no virtpass API beyond stdio, and checks the guest's integer
 * arithmetic rather than the bridge. Five independent algorithms have to agree
 * with each other, and the same sequence is then pushed through the 8/16/32-bit
 * and 64-bit boundaries, where every check pins an exact published value:
 *
 *   1. base cases, the F(n) = F(n-1) + F(n-2) recurrence and a table of
 *      milestone values (F(0)..F(90)) as external truth
 *   2. five implementations agree: rolling iteration, fast doubling, 2x2 matrix
 *      power, recursion with and without a memo - all four to n = 90, the plain
 *      recursion to n = 30 (it is exponential, and 30 is already 2.7M calls)
 *   3. sub-word truncation and the int32/uint32 boundaries - F(46) fits, F(47)
 *      is past INT32_MAX, F(47) fits uint32, F(48) is past UINT32_MAX. Narrow
 *      integer arithmetic is where an emulated CPU most easily goes wrong
 *   4. the uint64 boundary: F(93) is the largest value that fits, F(94) is the
 *      first overflow, and unsigned arithmetic has to wrap, not trap or
 *      saturate
 *   5. arbitrary precision past 64 bits: a base-1e9 limb adder checked against
 *      the published decimal string of F(100), and against the uint64 loop at
 *      F(93), the last value both representations can hold
 *
 * Every stage reports PASS/FAIL and the run exits non-zero if any check fails,
 * so this works as a regression gate rather than a manual eyeball check.
 *
 * The timing stage at the end is optional: RVVM_FIB_BENCH=<iterations>
 * (default 100000, 0 disables). The elapsed time is printed for information
 * only - the asserted part is the checksum, which proves the repeated
 * computation stayed correct. A guest whose clock_gettime is unavailable, or
 * whose readings are not a usable struct timespec, reports "skipped" for the
 * duration and still runs the checksum.
 *
 * Build: picked up automatically from guest-samples/ (zig cc, riscv64-musl)
 * Run:   WinHost -> Run -> test_fibonacci
 */

#include <stdio.h>
#include <stdlib.h>  /* getenv, strtol */
#include <string.h>  /* strcmp, strlen, memcpy, memset */
#include <stdint.h>  /* uint64_t, INT32_MAX, UINT32_MAX, UINT64_MAX */
#include <time.h>    /* clock_gettime (timing stage only) */

static int g_failures;
static int g_stage;

static void stage(const char* name)
{
    g_stage++;
    printf("\n[%d] %s\n", g_stage, name);
}

static void check(int ok, const char* what)
{
    printf("    %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("FIRST FAILURE: %s\n", what);
        g_failures++;
    }
}

/* Published values, used as the one piece of external truth in this file:
 * everything else is cross-checked between algorithms, so a wrong sequence
 * would have to be wrong in a typo here. */
static const uint64_t k_milestones[][2] = {
    { 0, 0ull },              { 1, 1ull },              { 2, 1ull },
    { 3, 2ull },              { 4, 3ull },              { 5, 5ull },
    { 6, 8ull },              { 7, 13ull },             { 8, 21ull },
    { 9, 34ull },             { 10, 55ull },            { 15, 610ull },
    { 20, 6765ull },          { 25, 75025ull },         { 30, 832040ull },
    { 35, 9227465ull },       { 40, 102334155ull },     { 45, 1134903170ull },
    { 50, 12586269025ull },   { 60, 1548008755920ull }, { 70, 190392490709135ull },
    { 80, 23416728348467685ull }, { 90, 2880067194370816120ull },
};

/* ---------------- algorithm 1: rolling iteration ---------------- */

static uint64_t fib_iter(int n)
{
    uint64_t a = 0, b = 1;                 /* F(0), F(1) */
    for (int i = 0; i < n; i++) {
        uint64_t t = a + b;
        a = b;
        b = t;
    }
    return a;
}

/* The same loop, but it reports the first n whose value no longer fits in 64
 * bits instead of silently wrapping. Note the shape: it adds exactly once per
 * produced term, so the last addition is the one that yields F(n) - checking a
 * term the caller never asked for would reject F(93) over the F(94) overflow. */
static int fib_iter_checked(int n, uint64_t* out)
{
    uint64_t a = 0, b = 1;                 /* F(0), F(1) */
    if (n == 0) { *out = a; return 1; }

    for (int i = 1; i < n; i++) {
        uint64_t t = a + b;                /* F(i+1) */
        if (t < b) return 0;               /* wrapped: F(i+1) does not fit */
        a = b;
        b = t;
    }
    *out = b;
    return 1;
}

/* ---------------- algorithm 2: fast doubling ----------------
 *
 *   F(2k)   = F(k) * (2*F(k+1) - F(k))
 *   F(2k+1) = F(k)^2 + F(k+1)^2
 *
 * Exact in 64 bits up to n = 93, the same ceiling as the plain loop: the
 * largest intermediate is F(47)^2, still below 2^64. */
static void fib_doubling(int n, uint64_t* fn, uint64_t* fn1)
{
    if (n == 0) { *fn = 0; *fn1 = 1; return; }

    uint64_t a, b;
    fib_doubling(n >> 1, &a, &b);          /* F(k), F(k+1) with k = n / 2 */

    uint64_t c = a * ((b << 1) - a);       /* F(2k)   */
    uint64_t d = a * a + b * b;            /* F(2k+1) */

    if (n & 1) { *fn = d; *fn1 = c + d; }
    else       { *fn = c; *fn1 = d; }
}

static uint64_t fib_fast(int n)
{
    uint64_t fn, fn1;
    fib_doubling(n, &fn, &fn1);
    return fn;
}

/* ---------------- algorithm 3: 2x2 matrix power ----------------
 *
 *   [[1,1],[1,0]]^n = [[F(n+1),F(n)],[F(n),F(n-1)]]
 *
 * Row major. The temporary in mat_mul lets the output alias either input, so
 * the squaring step can write back into the matrix it is squaring. */
static void mat_mul(const uint64_t a[4], const uint64_t b[4], uint64_t out[4])
{
    uint64_t r[4];
    r[0] = a[0] * b[0] + a[1] * b[2];
    r[1] = a[0] * b[1] + a[1] * b[3];
    r[2] = a[2] * b[0] + a[3] * b[2];
    r[3] = a[2] * b[1] + a[3] * b[3];
    memcpy(out, r, sizeof(r));
}

static uint64_t fib_matrix(int n)
{
    uint64_t r[4] = { 1, 0, 0, 1 };        /* identity */
    uint64_t m[4] = { 1, 1, 1, 0 };

    while (n > 0) {
        if (n & 1) mat_mul(r, m, r);
        mat_mul(m, m, m);
        n >>= 1;
    }
    return r[1];                           /* F(n) */
}

/* ---------------- algorithm 4: recursion, with and without a memo ---------------- */

static uint64_t fib_naive(int n)
{
    if (n < 2) return (uint64_t)n;
    return fib_naive(n - 1) + fib_naive(n - 2);
}

#define MEMO_MAX 91                        /* indices 0..90 */

static uint64_t g_memo[MEMO_MAX];

static uint64_t fib_memo(int n)
{
    if (n < 2) return (uint64_t)n;
    /* F(n) is never 0 for n >= 1, so 0 doubles as "not computed yet". */
    if (g_memo[n] == 0) g_memo[n] = fib_memo(n - 1) + fib_memo(n - 2);
    return g_memo[n];
}

/* ---------------- algorithm 5: arbitrary precision ----------------
 *
 * Base 1e9 limbs, little endian, each limb < 1e9. 96 limbs hold 864 decimal
 * digits - far past F(1000) (209) - so nothing here can silently overflow; the
 * normalised-limb check below is what catches a dropped carry. */
#define BIG_LIMBS 96
#define BIG_BASE  1000000000u
#define BIG_DEC_MAX (BIG_LIMBS * 9 + 1)

typedef struct {
    uint32_t limb[BIG_LIMBS];
    int      used;                         /* 0 for the value zero */
} Big;

static void big_set(Big* d, uint32_t v)
{
    memset(d, 0, sizeof(*d));
    d->limb[0] = v;
    d->used = (v != 0);
}

static void big_add(const Big* a, const Big* b, Big* dst)
{
    Big r;
    uint32_t carry = 0;
    int n = a->used > b->used ? a->used : b->used;

    memset(&r, 0, sizeof(r));
    for (int i = 0; i < n; i++) {
        uint32_t s = a->limb[i] + b->limb[i] + carry;
        if (s >= BIG_BASE) {
            s -= BIG_BASE;
            carry = 1;
        } else {
            carry = 0;
        }
        r.limb[i] = s;
    }
    r.used = n;
    if (carry && n < BIG_LIMBS) {
        r.limb[n] = carry;
        r.used = n + 1;
    }
    *dst = r;
}

static int big_cmp(const Big* a, const Big* b)
{
    if (a->used != b->used) return a->used < b->used ? -1 : 1;
    for (int i = a->used - 1; i >= 0; i--) {
        if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
    }
    return 0;
}

/* Every limb below the base: a carry that was added instead of propagated
 * would leave a limb >= 1e9 behind. */
static int big_normalised(const Big* a)
{
    for (int i = 0; i < a->used; i++) {
        if (a->limb[i] >= BIG_BASE) return 0;
    }
    return a->used <= BIG_LIMBS;
}

/* Most significant limb first, no separators: the format of the published
 * values. Writes at most BIG_DEC_MAX bytes. */
static void big_to_dec(const Big* a, char* out)
{
    int p = 0;

    if (a->used == 0) {
        out[p++] = '0';
    } else {
        p += snprintf(out + p, 16, "%u", a->limb[a->used - 1]);
        for (int i = a->used - 2; i >= 0; i--) {
            p += snprintf(out + p, 16, "%09u", a->limb[i]);
        }
    }
    out[p] = 0;
}

static void fib_big(int n, Big* out)
{
    Big a, b, t;

    big_set(&a, 0);                        /* F(0) */
    big_set(&b, 1);                        /* F(1) */
    for (int i = 0; i < n; i++) {
        big_add(&a, &b, &t);
        a = b;
        b = t;
    }
    *out = a;
}

/* ---------------- stages ---------------- */

static void stage_base_cases(void)
{
    stage("Base cases, recurrence and the published table");

    size_t rows = sizeof(k_milestones) / sizeof(k_milestones[0]);
    for (size_t i = 0; i < rows; i++) {
        int n = (int)k_milestones[i][0];
        uint64_t want = k_milestones[i][1];
        char what[64];
        snprintf(what, sizeof(what), "F(%d) == %llu", n,
                 (unsigned long long)want);
        check(fib_iter(n) == want, what);
    }

    int rec = 1;
    for (int n = 2; n <= 90; n++) {
        if (fib_iter(n) != fib_iter(n - 1) + fib_iter(n - 2)) rec = 0;
    }
    check(rec, "F(n) == F(n-1) + F(n-2) for n = 2..90");
    check(fib_iter(0) == 0 && fib_iter(1) == 1, "F(0) = 0 and F(1) = 1");
}

static void stage_algorithms(void)
{
    stage("Cross-check: iteration / doubling / matrix / memo / recursion");

    int vs_fast = 1, vs_matrix = 1, vs_memo = 1, vs_naive = 1;

    for (int n = 0; n <= 90; n++) {
        uint64_t want = fib_iter(n);
        if (fib_fast(n) != want)   vs_fast = 0;
        if (fib_matrix(n) != want) vs_matrix = 0;
        if (fib_memo(n) != want)   vs_memo = 0;
        if (n <= 30 && fib_naive(n) != want) vs_naive = 0;
    }

    check(vs_fast,   "fast doubling == rolling iteration");
    check(vs_matrix, "2x2 matrix power == rolling iteration");
    check(vs_memo,   "memoised recursion == rolling iteration");
    check(vs_naive,  "plain recursion == rolling iteration for n <= 30");

    /* The memo table itself, not just what came back through it: a cache seeded
     * from a wrong base case would still agree above. */
    check(g_memo[20] == 6765ull && g_memo[90] == 2880067194370816120ull,
          "memo table holds the published values");
}

static void stage_narrow_integers(void)
{
    const uint64_t f46 = 1836311903ull;    /* fits int32      */
    const uint64_t f47 = 2971215073ull;    /* past int32      */
    const uint64_t f48 = 4807526976ull;    /* past uint32     */

    stage("Sub-word and 32-bit boundaries");

    check(fib_iter(46) == f46, "F(46) == 1836311903");
    check(fib_iter(47) == f47, "F(47) == 2971215073");
    check(fib_iter(48) == f48, "F(48) == 4807526976");

    check(f46 <= (uint64_t)INT32_MAX && f47 > (uint64_t)INT32_MAX,
          "F(47) is the first value past INT32_MAX");
    check(f47 <= (uint64_t)UINT32_MAX && f48 > (uint64_t)UINT32_MAX,
          "F(48) is the first value past UINT32_MAX");

    /* Truncation, not saturation: the low bits of the sum have to survive. */
    check((uint32_t)(f46 + f47) == 512559680u, "uint32 wrap of F(48)");
    check((uint16_t)fib_iter(25) == 9489u, "uint16 truncation of F(25) = 75025");
    check((uint8_t)fib_iter(14) == 121u, "uint8 truncation of F(14) = 377");
    check((int32_t)f46 == 1836311903, "F(46) survives a signed 32-bit round trip");
}

static void stage_wide_integers(void)
{
    const uint64_t f92 = 7540113804746346429ull;
    const uint64_t f93 = 12200160415121876738ull;  /* largest F in uint64 */

    stage("uint64 boundary: F(93) fits, F(94) is the first overflow");

    check(fib_iter(93) == f93, "F(93) == 12200160415121876738");
    check(f93 > UINT64_MAX - f92, "F(93) + F(92) leaves the uint64 range");

    uint64_t v = 0;
    check(fib_iter_checked(93, &v) && v == f93, "the checked loop accepts F(93)");
    check(!fib_iter_checked(94, &v), "the checked loop flags F(94) as the first overflow");
    check(fib_iter_checked(0, &v) && v == 0, "the checked loop handles F(0)");

    int agree = 1;
    for (int n = 0; n <= 93; n++) {
        uint64_t a = fib_iter(n), b = 0;
        if (!fib_iter_checked(n, &b) || a != b) agree = 0;
    }
    check(agree, "checked and plain loops agree over n = 0..93");

    /* Unsigned arithmetic must wrap; a JIT that trapped or saturated instead
     * would be caught here and nowhere else. */
    check(fib_iter(94) == f93 + f92, "the unchecked loop wraps at F(94)");
}

static void stage_big_integers(void)
{
    Big f;
    char dec[BIG_DEC_MAX];
    char want[32];

    stage("Arbitrary precision past 64 bits (base-1e9 limb adder)");

    /* External anchor for the limb path: the published decimal string of
     * F(100), 21 digits. */
    fib_big(100, &f);
    big_to_dec(&f, dec);
    check(strcmp(dec, "354224848179261915075") == 0,
          "F(100) == 354224848179261915075");
    printf("    F(100) = %s\n", dec);
    check(big_normalised(&f), "limbs stayed below 1e9");

    /* Both representations have to agree where they overlap: the limb adder
     * against the uint64 loop at the last shared value. */
    fib_big(93, &f);
    big_to_dec(&f, dec);
    snprintf(want, sizeof(want), "%llu", (unsigned long long)fib_iter(93));
    check(strcmp(dec, want) == 0, "the limb adder agrees with the loop at F(93)");

    /* F(300) is 63 digits: floor(n*log10(phi) - log10(sqrt(5))) + 1, the same
     * formula that puts F(100) at 21. The exact digits are printed for eyeballing
     * - they are computed here, not typed in. */
    fib_big(300, &f);
    big_to_dec(&f, dec);
    check(strlen(dec) == 63, "F(300) has 63 decimal digits");
    printf("    F(300) = %s\n", dec);
    check(big_normalised(&f), "limbs stayed below 1e9 at F(300)");

    /* Monotonic: a dropped carry would show up as a smaller successor. */
    Big prev, cur;
    fib_big(200, &prev);
    fib_big(201, &cur);
    check(big_cmp(&cur, &prev) > 0, "F(201) > F(200)");
    fib_big(299, &prev);
    fib_big(300, &cur);
    check(big_cmp(&cur, &prev) > 0, "F(300) > F(299)");
}

/* A conforming timespec only ever reports 0 <= tv_nsec < 1e9. RVVM's user mode
 * serves clock_gettime() by handing the host's own struct timespec to the
 * guest, and on a 64-bit Windows host that struct is 8 + 4 bytes (LLP64 long):
 * the upper half of the guest's 64-bit tv_nsec is never written, so it holds
 * whatever the stack did. Zeroing the structs first makes those bytes zero and
 * this check still catches a host that leaves more behind, so the duration is
 * printed only when the readings are actually usable. */
static int ts_usable(const struct timespec* ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

static void stage_timing(void)
{
    stage("Bulk iteration (informational; RVVM_FIB_BENCH=<iterations>)");

    const char* env = getenv("RVVM_FIB_BENCH");
    long iters = (env && *env) ? strtol(env, NULL, 10) : 100000;

    if (iters <= 0) {
        printf("    skipped: RVVM_FIB_BENCH=%ld\n", iters);
        return;
    }

    struct timespec t0 = { 0, 0 }, t1 = { 0, 0 };
    int timed = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;

    volatile uint64_t sink = 0;            /* keeps the loop from being elided */
    for (long i = 0; i < iters; i++) {
        sink += fib_iter(90);
    }

    timed = timed && clock_gettime(CLOCK_MONOTONIC, &t1) == 0;

    if (timed && ts_usable(&t0) && ts_usable(&t1)) {
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                    (double)(t1.tv_nsec - t0.tv_nsec);
        printf("    fib_iter(90) x %ld in %.3f ms (%.1f ns per call)\n",
               iters, ns / 1e6, ns / (double)iters);
    } else {
        printf("    skipped: no usable clock_gettime(CLOCK_MONOTONIC)\n");
    }

    /* The elapsed time is informational, the checksum is not: it proves the
     * repeated computation stayed correct under load. It runs either way. */
    check(sink == (uint64_t)iters * fib_iter(90), "checksum of the repeated runs");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== Fibonacci Self-Test ===\n");
    printf("iterative / fast doubling / matrix / recursion / limb adder\n");

    stage_base_cases();
    stage_algorithms();
    stage_narrow_integers();
    stage_wide_integers();
    stage_big_integers();
    stage_timing();

    printf("\n=== %d stage(s), %d failure(s) ===\n", g_stage, g_failures);
    printf("Fibonacci self-test %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
