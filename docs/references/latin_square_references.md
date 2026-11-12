# Latin Square References

This page records what `src/core/design/latin_square.c` computes, the
standard results it relies on, and where its one historical table comes from.
Source comments describe the computation first; historical names appear only
as a marked origin note that points here (see
[discussion #705](https://github.com/c4punks/CWIST/discussions/705)).

---

## 1. Definitions

- A **Latin square** of order *n* is an *n* × *n* array over *n* symbols in
  which every row and every column holds each symbol exactly once.
- Two Latin squares *A* and *B* of order *n* are **orthogonal** when the
  *n*² ordered pairs (*A*[r][c], *B*[r][c]) are all different. A set of
  squares is **mutually orthogonal (MOLS)** when every two of them are.
  At most *n* − 1 MOLS of order *n* exist.
- An **orthogonal array** OA(*N*, *k*, *s*, *t*) is an *N* × *k* array over
  *s* levels in which every *N* × *t* subarray contains each *t*-tuple of
  levels equally often. *k* MOLS of order *n* give an OA(*n*², *k* + 2, *n*, 2):
  the run for cell (r, c) is (r, c, *L*₁[r][c], …, *L*ₖ[r][c]). In a
  strength-2 array every pair of factors shows every pair of levels exactly
  once.

**Reference:** C. J. Colbourn and J. H. Dinitz (eds.), *Handbook of
Combinatorial Designs*, 2nd ed., Chapman & Hall/CRC, 2007 (the chapters on
Latin squares, MOLS and orthogonal arrays). A. S. Hedayat, N. J. A. Sloane and
J. Stufken, *Orthogonal Arrays: Theory and Applications*, Springer, 1999.

---

## 2. Constructions used in the code

### 2.1 Finite fields (prime powers)

For a field GF(*q*), the squares *L*ₐ(r, c) = *a*·r + *c*, *a* ≠ 0, are *q* − 1
MOLS of order *q*. Rows are permutations because adding *c* is a bijection;
columns because multiplying by a non-zero *a* is; two squares *a* ≠ *b* are
orthogonal because (*a* − *b*)·r is a bijection.

- *q* = *p* prime: arithmetic mod *p*.
- *q* = 2^*e*, *e* = 1..31: polynomial arithmetic over GF(2) modulo a fixed
  irreducible polynomial of degree *e* (degree 8 is the AES polynomial
  x⁸ + x⁴ + x³ + x + 1). Every polynomial in the table is checked with
  Rabin's irreducibility test in `tests/test_latin_square.c`.

**References:** R. C. Bose, "On the application of the properties of Galois
fields to the problem of construction of hyper-Graeco-Latin squares",
*Sankhyā* 3 (1938), 323–338. M. O. Rabin, "Probabilistic algorithms in
finite fields", *SIAM Journal on Computing* 9(2) (1980), 273–280.

### 2.2 Odd prime powers *p*^*e*, *e* ≥ 2

The code applies the prime-order rule to each base-*p* digit, i.e. it takes
the direct product of *e* copies of the order-*p* family. This yields *p* − 1
MOLS, fewer than the *p*^*e* − 1 a full GF(*p*^*e*) would give; the module
reports the smaller count rather than claiming more. Order 9 uses the
historical table in section 3 instead, which also holds two squares.

### 2.3 Composite orders: MacNeish's product

If *n* = *n*₁·*n*₂ and there are *k* MOLS of each order, their direct product
gives *k* MOLS of order *n*. Writing *n* as a product of prime powers, the
code therefore provides min(*q*ᵢ − 1) squares (with *p* − 1 for the factors of
section 2.2).

**Reference:** H. F. MacNeish, "Euler squares", *Annals of Mathematics*
23(3) (1922), 221–227.

### 2.4 What the code does not construct

Orders *n* ≡ 2 (mod 4) have a factor 2¹, so the product gives a single
square. Orthogonal pairs exist for every order except 2 and 6, but their
constructions for *n* ≡ 2 (mod 4) are not product constructions, and the
module does not implement them: `cwist_latin_family_create(n, 2)` returns
`NULL` for these orders instead of a family without the property.

**References:** L. Euler, "Recherches sur une nouvelle espèce de quarrés
magiques", 1782 (conjectured that no pair exists for *n* ≡ 2 mod 4).
G. Tarry, 1900 (no pair of order 6). R. C. Bose, S. S. Shrikhande and
E. T. Parker, "Further results on the construction of mutually orthogonal
Latin squares and the falsity of Euler's conjecture", *Canadian Journal of
Mathematics* 12 (1960), 189–203 (pairs exist for every *n* other than 2
and 6).

---

## 3. Historical table: the Gusuryak order-9 pair

**What the code uses:** `k_choi9_a` / `k_choi9_b`, an orthogonal pair of
Latin squares of order 9 stored as printed (symbols 1–9), used as the order-9
factor of the product in section 2.3.

**Origin:** the figure *Gugu mosu byeongung yangdo* (九九母數變宮陽圖) in
**Choi Seok-jeong (崔錫鼎, 1646–1715)**, *Gusuryak* (九數略), c. 1700. Each
cell is a pair (*a*, *b*); the *a* and *b* entries each form a Latin square
and the 81 pairs are all different. It is the earliest known orthogonal pair
of order 9, some eighty years before Euler's 1782 study.

The companion figure *Gugu mosu byeongung eumdo* (九九母數變宮陰圖) is **not**
used: its 81 pairs are all different and every 3 × 3 block holds 1–9 in each
coordinate, but its rows and columns are not permutations, so it is not a
pair of Latin squares.

`tests/test_latin_square.c` checks that the stored table is Latin and
orthogonal, so a transcription error fails the build's tests rather than
silently weakening the guarantee.

---

## 4. Use in CWIST: covering runtime knob pairs in tests

**What the code does:** `tests/test_bdr_privacy.c` runs its server once per
run of OA(16, 5, 4, 2), built from the three MOLS of order 4 over GF(4). The
five factors are runtime knobs that choose different serving paths
(`CWIST_C1M_MODE`, `CWIST_WORKER_THREADS` up to 64, `CWIST_REACTOR_BACKEND`,
`CWIST_HTTP_BATCH`, `CWIST_RX_URING`). Two-valued knobs repeat their values
across the four levels, which keeps every pair of values covered. Every pair
of knob settings therefore meets in at least one run, in 16 runs instead of
the 4⁵ = 1024 of the full product.

**Why:** a security check that exists on one serving path and not another is
exposed only when the right pair of settings meets. Issue #706 was such a
case: caller-specific replies leaked through the cache on the classic path
only. On the unfixed tree the eight classic runs fail and the eight C1M runs
pass.

**Reference:** R. Mandl, "Orthogonal Latin squares: an application of
experiment design to compiler testing", *Communications of the ACM* 28(10)
(1985), 1054–1058.
