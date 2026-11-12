/**
 * @file test_latin_square.c
 * @brief Checks every property the latin_square module promises.
 *
 * - each GF(2^e) reduction polynomial passes Rabin's irreducibility test,
 *   so k*r + c over GF(2^e) is a field construction for every order 2^e up
 *   to 2^31;
 * - the Gusuryak order-9 table is an orthogonal pair;
 * - orders 2..100: every square is Latin, and the squares are mutually
 *   orthogonal (all pairs up to order 64, the first four squares above);
 * - orders 2 mod 4 give one square and refuse a request for two;
 * - large and HPC-scale orders (up to 2^31) build with the expected square
 *   count, and sampled rows and columns hold no repeated symbol;
 * - the derived orthogonal arrays cover every level pair exactly once.
 *
 * The module source is included directly so the static tables are visible.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/core/design/latin_square.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* --- GF(2)[x] helpers for Rabin's test ---------------------------------- */

static int pdeg(uint64_t a) { return a ? 63 - __builtin_clzll(a) : -1; }

static uint64_t pmod(uint64_t a, uint64_t m) {
    int dm = pdeg(m);
    while (a && pdeg(a) >= dm) a ^= m << (pdeg(a) - dm);
    return a;
}

static uint64_t pmulmod(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t r = 0;
    a = pmod(a, m);
    int dm = pdeg(m);
    while (b) {
        if (b & 1) r ^= a;
        b >>= 1;
        a <<= 1;
        if ((a >> dm) & 1) a ^= m;
    }
    return r;
}

static uint64_t pgcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = pmod(a, b);
        a = b;
        b = t;
    }
    return a;
}

/* x^(2^k) mod f */
static uint64_t x_pow_2k(unsigned k, uint64_t f) {
    uint64_t x = pmod(2, f);
    for (unsigned i = 0; i < k; i++) x = pmulmod(x, x, f);
    return x;
}

/* Rabin (1980): f of degree n is irreducible iff x^(2^n) = x mod f and
 * gcd(x^(2^(n/q)) - x, f) = 1 for every prime q dividing n. */
static bool rabin_irreducible(uint64_t f) {
    unsigned n = (unsigned)pdeg(f);
    if (x_pow_2k(n, f) != pmod(2, f)) return false;
    for (unsigned q = 2; q <= n; q++) {
        if (n % q) continue;
        bool prime = true;
        for (unsigned d = 2; d * d <= q; d++) prime &= (q % d) != 0;
        if (!prime) continue;
        if (pgcd(f, x_pow_2k(n / q, f) ^ 2) != 1) return false;
    }
    return true;
}

static void test_gf2_polynomials(void) {
    for (unsigned e = 1; e <= 31; e++) {
        uint32_t f = k_gf2_poly[e];
        assert(pdeg(f) == (int)e);
        assert(rabin_irreducible(f));
    }
    printf("  GF(2^e) polynomials e=1..31 irreducible\n");
}

/* --- Latin / orthogonality checks --------------------------------------- */

static uint32_t expected_count(uint32_t n) {
    uint32_t best = UINT32_MAX;
    for (uint32_t p = 2; n > 1; p = (p == 2) ? 3 : p + 2) {
        if ((uint64_t)p * p > n) p = n;
        if (n % p) continue;
        uint32_t q = 1, e = 0;
        while (n % p == 0) {
            n /= p;
            q *= p;
            e++;
        }
        uint32_t c = (p == 2) ? q - 1 : p - 1;
        if (c < best) best = c;
    }
    return best;
}

static void assert_latin(const cwist_latin_family_t *f, uint32_t k) {
    uint32_t n = cwist_latin_family_order(f);
    uint8_t *seen = calloc(n, 1);
    assert(seen);
    for (uint32_t r = 0; r < n; r++) {
        memset(seen, 0, n);
        for (uint32_t c = 0; c < n; c++) {
            uint32_t s = cwist_latin_family_symbol(f, k, r, c);
            assert(s < n && !seen[s]);
            seen[s] = 1;
        }
    }
    for (uint32_t c = 0; c < n; c++) {
        memset(seen, 0, n);
        for (uint32_t r = 0; r < n; r++) {
            uint32_t s = cwist_latin_family_symbol(f, k, r, c);
            assert(s < n && !seen[s]);
            seen[s] = 1;
        }
    }
    free(seen);
}

static void assert_orthogonal(const cwist_latin_family_t *f, uint32_t a, uint32_t b) {
    uint32_t n = cwist_latin_family_order(f);
    uint8_t *seen = calloc((size_t)n * n, 1);
    assert(seen);
    for (uint32_t r = 0; r < n; r++) {
        for (uint32_t c = 0; c < n; c++) {
            size_t idx = (size_t)cwist_latin_family_symbol(f, a, r, c) * n +
                         cwist_latin_family_symbol(f, b, r, c);
            assert(!seen[idx]);
            seen[idx] = 1;
        }
    }
    free(seen);
}

static void test_choi9(void) {
    cwist_latin_family_t *f = cwist_latin_family_create(9, 2);
    assert(f);
    assert(cwist_latin_family_count(f) == 2);
    assert(strcmp(cwist_latin_family_construction(f), "Choi-9") == 0);
    for (uint32_t r = 0; r < 9; r++) {
        for (uint32_t c = 0; c < 9; c++) {
            assert(cwist_latin_family_symbol(f, 0, r, c) == k_choi9_a[r * 9 + c] - 1u);
            assert(cwist_latin_family_symbol(f, 1, r, c) == k_choi9_b[r * 9 + c] - 1u);
        }
    }
    assert_latin(f, 0);
    assert_latin(f, 1);
    assert_orthogonal(f, 0, 1);
    assert(cwist_latin_family_create(9, 3) == NULL);
    cwist_latin_family_destroy(f);
    printf("  Gusuryak order-9 pair is Latin and orthogonal\n");
}

static void test_small_orders(void) {
    for (uint32_t n = 2; n <= 100; n++) {
        cwist_latin_family_t *f = cwist_latin_family_create(n, 1);
        assert(f);
        uint32_t k = cwist_latin_family_count(f);
        assert(k == expected_count(n));
        if (n % 4 == 2) {
            assert(k == 1);
            assert(cwist_latin_family_create(n, 2) == NULL);
        }
        for (uint32_t a = 0; a < k; a++) assert_latin(f, a);
        uint32_t limit = (n <= 64) ? k : (k < 4 ? k : 4);
        for (uint32_t a = 0; a < limit; a++)
            for (uint32_t b = a + 1; b < limit; b++) assert_orthogonal(f, a, b);
        cwist_latin_family_destroy(f);
    }
    printf("  orders 2..100 Latin and mutually orthogonal\n");
}

static void test_mid_orders(void) {
    static const uint32_t orders[] = {128, 243, 256, 625, 1024};
    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        cwist_latin_family_t *f = cwist_latin_family_create(orders[i], 2);
        assert(f);
        assert_latin(f, 0);
        assert_latin(f, 1);
        assert_orthogonal(f, 0, 1);
        printf("  order %u (%s): %u squares, first pair checked\n", orders[i],
               cwist_latin_family_construction(f), cwist_latin_family_count(f));
        cwist_latin_family_destroy(f);
    }
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Orders too large to enumerate: sample cells sharing a row or a column and
 * check they never repeat a symbol. Correctness for these orders rests on
 * the construction (Rabin-verified fields, primes, MacNeish products); the
 * sampling guards the indexing arithmetic at full width. */
static void test_hpc_orders(void) {
    static const struct {
        uint32_t order;
        uint32_t count;
    } cases[] = {
        {UINT32_C(1) << 20, (UINT32_C(1) << 20) - 1},
        {CWIST_LATIN_MAX_ORDER, CWIST_LATIN_MAX_ORDER - 1},
        {UINT32_C(2147483647), UINT32_C(2147483646)}, /* prime 2^31 - 1 */
        {UINT32_C(387420489), 2},                     /* 3^18: (Z_3)^18 */
        {UINT32_C(111546435), 2},                     /* 3*5*7*11*13*17*19*23 */
        {UINT32_C(4096), 4095},
        {UINT32_C(1536), 2},                          /* 2^9 * 3 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t n = cases[i].order;
        cwist_latin_family_t *f = cwist_latin_family_create(n, 2);
        assert(f);
        assert(cwist_latin_family_count(f) == cases[i].count);
        uint32_t k = cwist_latin_family_count(f);
        for (int t = 0; t < 200000; t++) {
            uint32_t sq = (uint32_t)(rng() % (k < 8 ? k : 8));
            uint32_t r = (uint32_t)(rng() % n), c1 = (uint32_t)(rng() % n),
                     c2 = (uint32_t)(rng() % n);
            if (c1 == c2) continue;
            assert(cwist_latin_family_symbol(f, sq, r, c1) != cwist_latin_family_symbol(f, sq, r, c2));
            assert(cwist_latin_family_symbol(f, sq, c1, r) != cwist_latin_family_symbol(f, sq, c2, r));
            uint32_t s = cwist_latin_family_symbol(f, sq, r, c1);
            assert(s < n);
        }
        printf("  order %u (%s): %u squares, sampled\n", n, cwist_latin_family_construction(f), k);
        cwist_latin_family_destroy(f);
    }
}

static void test_orthogonal_arrays(void) {
    static const uint32_t levels[] = {2, 3, 4, 5, 7, 8, 9, 12};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        uint32_t n = levels[i];
        cwist_latin_family_t *f = cwist_latin_family_create(n, 1);
        assert(f);
        uint32_t factors = cwist_latin_oa_factors(f);
        uint64_t runs = cwist_latin_oa_runs(f);
        assert(runs == (uint64_t)n * n);
        uint32_t *count = calloc((size_t)n * n, sizeof(uint32_t));
        assert(count);
        for (uint32_t a = 0; a < factors; a++) {
            for (uint32_t b = a + 1; b < factors; b++) {
                memset(count, 0, (size_t)n * n * sizeof(uint32_t));
                for (uint64_t run = 0; run < runs; run++) {
                    uint32_t la = cwist_latin_oa_level(f, run, a);
                    uint32_t lb = cwist_latin_oa_level(f, run, b);
                    assert(la < n && lb < n);
                    count[(size_t)la * n + lb]++;
                }
                for (size_t j = 0; j < (size_t)n * n; j++) assert(count[j] == 1);
            }
        }
        free(count);
        printf("  OA(%llu, %u, %u, 2) covers every level pair once\n", (unsigned long long)runs,
               factors, n);
        cwist_latin_family_destroy(f);
    }
}

static void test_bounds(void) {
    assert(cwist_latin_family_create(0, 1) == NULL);
    assert(cwist_latin_family_create(1, 1) == NULL);
    assert(cwist_latin_family_create(CWIST_LATIN_MAX_ORDER + 1u, 1) == NULL);
    assert(cwist_latin_family_create(6, 2) == NULL);
    assert(cwist_latin_family_create(7, 7) == NULL);
    cwist_latin_family_t *f = cwist_latin_family_create(7, 6);
    assert(f);
    assert(cwist_latin_family_symbol(f, 6, 0, 0) == UINT32_MAX);
    assert(cwist_latin_family_symbol(f, 0, 7, 0) == UINT32_MAX);
    assert(cwist_latin_family_symbol(f, 0, 0, 7) == UINT32_MAX);
    assert(cwist_latin_oa_level(f, 49, 0) == UINT32_MAX);
    assert(cwist_latin_oa_level(f, 0, 8) == UINT32_MAX);
    assert(cwist_latin_family_symbol(NULL, 0, 0, 0) == UINT32_MAX);
    cwist_latin_family_destroy(f);
    cwist_latin_family_destroy(NULL);
    printf("  bounds and refusals\n");
}

int main(void) {
    printf("Testing latin_square...\n");
    test_gf2_polynomials();
    test_choi9();
    test_small_orders();
    test_mid_orders();
    test_hpc_orders();
    test_orthogonal_arrays();
    test_bounds();
    printf("All latin_square tests passed!\n");
    return 0;
}
