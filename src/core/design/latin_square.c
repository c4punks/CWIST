/**
 * @file latin_square.c
 * @brief Internal: mutually orthogonal Latin squares of any order up to 2^31,
 *        built as a MacNeish product of finite-field and prime-digit squares.
 *
 * Sources for every construction used here are listed in
 * docs/references/latin_square_references.md.
 */
#include "latin_square.h"

#include <cwist/core/mem/alloc.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* At most 9 distinct prime factors fit in 32 bits (2*3*5*...*23). */
#define LATIN_MAX_COMPONENTS 10

typedef enum {
    LATIN_GF2,        /* q = 2^e: L_k(r,c) = k*r + c in GF(2^e) */
    LATIN_PRIME,      /* q = p:   L_k(r,c) = (k*r + c) mod p */
    LATIN_PRIME_DIGITS, /* q = p^e, p odd, e >= 2: the prime rule per base-p digit */
    LATIN_CHOI9       /* q = 9:   the Gusuryak order-9 pair */
} latin_kind_t;

typedef struct {
    latin_kind_t kind;
    uint32_t q;     /* component order */
    uint32_t p;     /* prime */
    uint32_t e;     /* exponent */
    uint32_t poly;  /* GF(2^e) reduction polynomial without the x^e term */
    uint32_t count; /* squares this component provides */
} latin_component_t;

struct cwist_latin_family {
    uint32_t order;
    uint32_t count;
    uint32_t ncomp;
    latin_component_t comp[LATIN_MAX_COMPONENTS];
    char construction[96];
};

/*
 * Irreducible polynomials over GF(2) of degree e = 1..31, one per degree,
 * written with the x^e term: 0x13 is x^4 + x + 1. Each is the first
 * trinomial (else pentanomial) that passes Rabin's irreducibility test;
 * degree 8 is x^8 + x^4 + x^3 + x + 1, the AES polynomial.
 * tests/test_latin_square.c re-runs Rabin's test on every entry.
 */
static const uint32_t k_gf2_poly[32] = {
    0,          0x3,        0x7,        0xb,        0x13,       0x25,       0x43,
    0x83,       0x11b,      0x203,      0x409,      0x805,      0x1009,     0x201b,
    0x4021,     0x8003,     0x1002b,    0x20009,    0x40009,    0x80027,    0x100009,
    0x200005,   0x400003,   0x800021,   0x100001b,  0x2000009,  0x400001b,  0x8000027,
    0x10000003, 0x20000005, 0x40000003, 0x80000009,
};

/*
 * Order-9 orthogonal Latin square pair, symbols 1..9 as printed.
 *
 * Origin: the figure "Gugu mosu byeongung yangdo" in Choi Seok-jeong's
 * Gusuryak (c. 1700), the earliest known orthogonal pair of order 9, some
 * eighty years before Euler's 1782 study (see
 * docs/references/latin_square_references.md, section 3). Cell (r, c) of the
 * printed table is the pair (k_choi9_a[9r + c], k_choi9_b[9r + c]).
 * tests/test_latin_square.c checks that both are Latin and orthogonal.
 */
static const uint8_t k_choi9_a[81] = {
    5, 6, 4, 8, 9, 7, 2, 3, 1, /* row 1 */
    4, 5, 6, 7, 8, 9, 1, 2, 3, /* row 2 */
    6, 4, 5, 9, 7, 8, 3, 1, 2, /* row 3 */
    2, 3, 1, 5, 6, 4, 8, 9, 7, /* row 4 */
    1, 2, 3, 4, 5, 6, 7, 8, 9, /* row 5 */
    3, 1, 2, 6, 4, 5, 9, 7, 8, /* row 6 */
    8, 9, 7, 2, 3, 1, 5, 6, 4, /* row 7 */
    7, 8, 9, 1, 2, 3, 4, 5, 6, /* row 8 */
    9, 7, 8, 3, 1, 2, 6, 4, 5, /* row 9 */
};
static const uint8_t k_choi9_b[81] = {
    1, 3, 2, 7, 9, 8, 4, 6, 5, /* row 1 */
    3, 2, 1, 9, 8, 7, 6, 5, 4, /* row 2 */
    2, 1, 3, 8, 7, 9, 5, 4, 6, /* row 3 */
    7, 9, 8, 4, 6, 5, 1, 3, 2, /* row 4 */
    9, 8, 7, 6, 5, 4, 3, 2, 1, /* row 5 */
    8, 7, 9, 5, 4, 6, 2, 1, 3, /* row 6 */
    4, 6, 5, 1, 3, 2, 7, 9, 8, /* row 7 */
    6, 5, 4, 3, 2, 1, 9, 8, 7, /* row 8 */
    5, 4, 6, 2, 1, 3, 8, 7, 9, /* row 9 */
};

/** @brief Product a*b in GF(2^e) modulo @p poly (which includes x^e). */
static uint32_t gf2_mul(uint32_t a, uint32_t b, uint32_t e, uint32_t poly) {
    uint64_t r = 0;
    uint64_t x = a;
    const uint64_t top = UINT64_C(1) << e;
    while (b) {
        if (b & 1u) r ^= x;
        b >>= 1;
        x <<= 1;
        if (x & top) x ^= poly;
    }
    return (uint32_t)r;
}

/** @brief Symbol of square @p k (0-based) of one component at (r, c). */
static uint32_t component_symbol(const latin_component_t *cp, uint32_t k, uint32_t r, uint32_t c) {
    switch (cp->kind) {
    case LATIN_GF2:
        return gf2_mul(k + 1, r, cp->e, cp->poly) ^ c;
    case LATIN_PRIME:
        return (uint32_t)(((uint64_t)(k + 1) * r + c) % cp->p);
    case LATIN_PRIME_DIGITS: {
        uint32_t out = 0, stride = 1;
        for (uint32_t i = 0; i < cp->e; i++) {
            uint32_t rd = r % cp->p, cd = c % cp->p;
            out += (uint32_t)(((uint64_t)(k + 1) * rd + cd) % cp->p) * stride;
            r /= cp->p;
            c /= cp->p;
            stride *= cp->p;
        }
        return out;
    }
    case LATIN_CHOI9: {
        const uint8_t *t = (k == 0) ? k_choi9_a : k_choi9_b;
        return (uint32_t)t[r * 9 + c] - 1u;
    }
    }
    return 0;
}

static void component_describe(const latin_component_t *cp, char *buf, size_t cap) {
    switch (cp->kind) {
    case LATIN_GF2:
        snprintf(buf, cap, "GF(2^%u)", cp->e);
        break;
    case LATIN_PRIME:
        snprintf(buf, cap, "Z_%u", cp->p);
        break;
    case LATIN_PRIME_DIGITS:
        snprintf(buf, cap, "(Z_%u)^%u", cp->p, cp->e);
        break;
    case LATIN_CHOI9:
        snprintf(buf, cap, "Choi-9");
        break;
    }
}

cwist_latin_family_t *cwist_latin_family_create(uint32_t order, uint32_t min_squares) {
    if (order < 2 || order > CWIST_LATIN_MAX_ORDER) return NULL;
    if (min_squares == 0) min_squares = 1;

    latin_component_t comp[LATIN_MAX_COMPONENTS];
    uint32_t ncomp = 0;
    uint32_t count = UINT32_MAX;
    uint32_t n = order;

    /* Trial division: at most sqrt(2^31) ~ 46341 steps, once per create. */
    for (uint32_t p = 2; n > 1; p = (p == 2) ? 3 : p + 2) {
        if ((uint64_t)p * p > n) p = n; /* remaining n is prime */
        if (n % p != 0) continue;
        uint32_t e = 0, q = 1;
        while (n % p == 0) {
            n /= p;
            q *= p;
            e++;
        }
        latin_component_t *cp = &comp[ncomp++];
        memset(cp, 0, sizeof(*cp));
        cp->q = q;
        cp->p = p;
        cp->e = e;
        if (p == 2) {
            cp->kind = LATIN_GF2;
            cp->poly = k_gf2_poly[e];
            cp->count = q - 1;
        } else if (e == 1) {
            cp->kind = LATIN_PRIME;
            cp->count = p - 1;
        } else if (q == 9) {
            cp->kind = LATIN_CHOI9;
            cp->count = 2;
        } else {
            cp->kind = LATIN_PRIME_DIGITS;
            cp->count = p - 1;
        }
        if (cp->count < count) count = cp->count;
    }

    if (count < min_squares) return NULL;

    cwist_latin_family_t *f = cwist_alloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->order = order;
    f->count = count;
    f->ncomp = ncomp;
    memcpy(f->comp, comp, ncomp * sizeof(comp[0]));

    size_t used = 0;
    for (uint32_t i = 0; i < ncomp && used < sizeof(f->construction); i++) {
        char part[32];
        component_describe(&comp[i], part, sizeof(part));
        int w = snprintf(f->construction + used, sizeof(f->construction) - used, "%s%s",
                         i ? " x " : "", part);
        if (w < 0) break;
        used += (size_t)w;
    }
    return f;
}

void cwist_latin_family_destroy(cwist_latin_family_t *family) {
    if (family) cwist_free(family);
}

uint32_t cwist_latin_family_order(const cwist_latin_family_t *family) {
    return family ? family->order : 0;
}

uint32_t cwist_latin_family_count(const cwist_latin_family_t *family) {
    return family ? family->count : 0;
}

const char *cwist_latin_family_construction(const cwist_latin_family_t *family) {
    return family ? family->construction : "";
}

uint32_t cwist_latin_family_symbol(const cwist_latin_family_t *family, uint32_t square,
                                   uint32_t row, uint32_t col) {
    if (!family || square >= family->count || row >= family->order || col >= family->order) {
        return UINT32_MAX;
    }
    /* MacNeish product: split row and column into mixed-radix digits, one
     * per component, and recombine the per-component symbols. */
    uint32_t out = 0, stride = 1;
    for (uint32_t i = 0; i < family->ncomp; i++) {
        const latin_component_t *cp = &family->comp[i];
        uint32_t s = component_symbol(cp, square, row % cp->q, col % cp->q);
        out += s * stride;
        row /= cp->q;
        col /= cp->q;
        stride *= cp->q;
    }
    return out;
}

uint64_t cwist_latin_oa_runs(const cwist_latin_family_t *family) {
    return family ? (uint64_t)family->order * family->order : 0;
}

uint32_t cwist_latin_oa_factors(const cwist_latin_family_t *family) {
    return family ? family->count + 2 : 0;
}

uint32_t cwist_latin_oa_level(const cwist_latin_family_t *family, uint64_t run, uint32_t factor) {
    if (!family || run >= cwist_latin_oa_runs(family) || factor >= cwist_latin_oa_factors(family)) {
        return UINT32_MAX;
    }
    uint32_t r = (uint32_t)(run / family->order);
    uint32_t c = (uint32_t)(run % family->order);
    if (factor == 0) return r;
    if (factor == 1) return c;
    return cwist_latin_family_symbol(family, factor - 2, r, c);
}
