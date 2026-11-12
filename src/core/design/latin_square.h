/**
 * @file latin_square.h
 * @brief Internal: families of mutually orthogonal Latin squares (MOLS) and
 *        the strength-2 orthogonal arrays built from them.
 *
 * Not part of the public API (nothing under include/ exposes it).
 *
 * A Latin square of order n is an n x n array over n symbols in which every
 * row and every column is a permutation of the symbols. Two Latin squares
 * are orthogonal when superimposing them yields each of the n^2 ordered
 * symbol pairs exactly once; a family is mutually orthogonal when every two
 * of its members are. k MOLS of order n are equivalent to an orthogonal
 * array OA(n^2, k + 2, n, 2): n^2 runs over k + 2 factors with n levels, in
 * which every two factors show every pair of levels exactly once.
 *
 * Access goes through an opaque handle. A handle is only returned when the
 * requested number of squares can be constructed for the requested order,
 * so holding one is the guarantee: callers never infer a property from an
 * order or index that the family does not have. See
 * docs/references/latin_square_references.md for the constructions and
 * their sources.
 */
#ifndef CWIST_CORE_DESIGN_LATIN_SQUARE_H
#define CWIST_CORE_DESIGN_LATIN_SQUARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest supported order (2^31): symbols and indices fit in uint32_t. */
#define CWIST_LATIN_MAX_ORDER (UINT32_C(1) << 31)

/** Opaque family of mutually orthogonal Latin squares of one order. */
typedef struct cwist_latin_family cwist_latin_family_t;

/**
 * @brief Build a family of at least @p min_squares MOLS of order @p order.
 *
 * The family is the MacNeish direct product of one family per prime-power
 * factor q of @p order:
 * - q = 2^e: L_k(r, c) = k*r + c over GF(2^e), q - 1 squares;
 * - q = p prime: L_k(r, c) = (k*r + c) mod p, p - 1 squares;
 * - q = 9: the order-9 orthogonal pair from Choi Seok-jeong's Gusuryak;
 * - other q = p^e: the same linear rule digit-wise over (Z_p)^e, p - 1
 *   squares.
 * The family therefore holds min(q_i - 1) squares over the factors (p - 1
 * for odd prime powers with e > 1). Orders that are 2 mod 4 get a single
 * Latin square: orthogonal pairs exist for every order except 2 and 6, but
 * this module does not construct them for orders 2 mod 4.
 *
 * @param order Order n, 2 <= n <= CWIST_LATIN_MAX_ORDER.
 * @param min_squares Minimum number of squares required (0 is treated as 1).
 * @return New family, or NULL when the order is out of range, fewer than
 *         @p min_squares squares can be built, or allocation fails.
 */
cwist_latin_family_t *cwist_latin_family_create(uint32_t order, uint32_t min_squares);

/** @brief Release a family; NULL is ignored. */
void cwist_latin_family_destroy(cwist_latin_family_t *family);

/** @brief Order n of the family. */
uint32_t cwist_latin_family_order(const cwist_latin_family_t *family);

/** @brief Number of squares in the family (at least 1). */
uint32_t cwist_latin_family_count(const cwist_latin_family_t *family);

/**
 * @brief Symbol of square @p square at (@p row, @p col), in [0, order).
 * @return The symbol, or UINT32_MAX when an argument is out of range.
 */
uint32_t cwist_latin_family_symbol(const cwist_latin_family_t *family, uint32_t square,
                                   uint32_t row, uint32_t col);

/**
 * @brief Short description of the construction, for logs and test output
 *        (e.g. "GF(2^3)", "Choi-9 x Z_5").
 */
const char *cwist_latin_family_construction(const cwist_latin_family_t *family);

/** @brief Number of runs of the derived orthogonal array: order^2. */
uint64_t cwist_latin_oa_runs(const cwist_latin_family_t *family);

/** @brief Number of factors of the derived orthogonal array: count + 2. */
uint32_t cwist_latin_oa_factors(const cwist_latin_family_t *family);

/**
 * @brief Level of factor @p factor in run @p run of OA(n^2, count + 2, n, 2).
 *
 * Run i is the cell (r, c) = (i / n, i % n). Factor 0 is r, factor 1 is c,
 * and factor j >= 2 is the symbol of square j - 2 at (r, c).
 *
 * @return The level in [0, order), or UINT32_MAX when out of range.
 */
uint32_t cwist_latin_oa_level(const cwist_latin_family_t *family, uint64_t run, uint32_t factor);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_CORE_DESIGN_LATIN_SQUARE_H */
