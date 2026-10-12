# Release Codename Sequencing Specification

The codename of a CWIST LTS release is a state identifier. It carries no
semantic meaning and no tier (major, minor, and so on); it follows only the
alphabet's **tail-in sequential propagation** rule.

---

## 1. Principles

- A codename consists of uppercase ASCII letters.
- A new letter always enters at the **rightmost (tail)** position.
- Each following release pushes the new letter one position to the left.
  Once every position holds that letter, the sequence moves on to the next
  letter.
- No position carries a functional meaning or a version weight.

---

## 2. Two-letter sequence (default)

$$\text{AA} \rightarrow \text{AB} \rightarrow \text{BA} \rightarrow \text{BB} \rightarrow \text{BC} \rightarrow \text{CB} \rightarrow \text{CC} \rightarrow \text{CD} \rightarrow \dots$$

1. `AA`
2. `AB` (`B` enters at the tail)
3. `BA` (`B` propagates left)
4. `BB` (`B` complete)
5. `BC` (`C` enters at the tail)
6. `CB` (`C` propagates left)
7. `CC` (`C` complete)

---

## 3. Three-letter extension

A three-letter codename expands the same way as the two-letter one: the
new letter always enters at the tail (rightmost position) and propagates
one position to the left per release until every position holds it; then
the next letter enters at the tail.

$$\text{AAA} \rightarrow \text{AAB} \rightarrow \text{ABB} \rightarrow \text{BBB} \rightarrow \text{BBC} \rightarrow \text{BCC} \rightarrow \text{CCC} \rightarrow \dots$$

1. `AAA`
2. `AAB` (`B` enters at the tail)
3. `ABB` (`B` propagates left)
4. `BBB` (`B` complete)
5. `BBC` (`C` enters at the tail)
6. `BCC` (`C` propagates left)
7. `CCC` (`C` complete)

---

## 4. Notation

The codename is shown next to the release identifier in build binaries and
CLI output:

```sh
$ cwist --version
cwist 4.0.0-LTS (AA)
```

---

## 5. API stability gate (from the v4 LTS)

Independently of the codename, from v4 on the existing public API does not
change (ROADMAP.md "API stability from v4.0"). CI checks this with
`scripts/ci/check_api_baseline.py`.

- `docs/api/v4.0-api-baseline.txt` records the public headers: names,
  function signatures (parameter names dropped) and struct/union/enum
  layouts.
- CI fails when the headers no longer match that file. For an intended
  change, regenerate it with `make api-baseline` and commit it in the same
  PR.
- Once the LTS tag `v4` exists, CI also fails when a guaranteed entry from
  that tag's baseline is removed or changed, even with the baseline
  regenerated. New API passes; the experimental (WebTransport) sections are
  not checked.
