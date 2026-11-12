# 라틴방진 참고 문헌

이 문서는 `src/core/design/latin_square.c`가 무엇을 계산하는지, 어떤 표준
결과에 기대는지, 그리고 코드에 들어간 역사적 표 하나가 어디서 왔는지를
기록합니다. 소스 주석은 계산을 먼저 설명하고, 역사적 이름은 이 문서를
가리키는 출처 메모로만 둡니다([discussion #705](https://github.com/c4punks/CWIST/discussions/705) 참고).

---

## 1. 정의

- 차수 *n*의 **라틴방진**은 *n*개 기호로 채운 *n* × *n* 배열로, 모든 행과
  모든 열에 각 기호가 정확히 한 번씩 나옵니다.
- 차수 *n*의 두 라틴방진 *A*, *B*를 겹쳤을 때 *n*²개의 순서쌍
  (*A*[r][c], *B*[r][c])이 모두 다르면 두 방진은 **직교**합니다. 어느 두 개를
  골라도 직교하는 방진들의 모임을 **서로 직교하는 라틴방진(MOLS)**이라 합니다.
  차수 *n*의 MOLS는 최대 *n* − 1개입니다.
- **직교배열** OA(*N*, *k*, *s*, *t*)는 *s*개 수준으로 채운 *N* × *k* 배열로,
  어떤 *N* × *t* 부분배열에도 수준의 *t*-튜플이 모두 같은 횟수만큼 나옵니다.
  차수 *n*의 MOLS *k*개로 OA(*n*², *k* + 2, *n*, 2)를 만들 수 있습니다. 칸
  (r, c)에 해당하는 실행은 (r, c, *L*₁[r][c], …, *L*ₖ[r][c])입니다. 강도 2
  배열에서는 어느 두 인자를 골라도 수준 쌍이 모두 정확히 한 번씩 나옵니다.

**참고:** C. J. Colbourn, J. H. Dinitz (편), *Handbook of Combinatorial
Designs*, 2판, Chapman & Hall/CRC, 2007 (라틴방진, MOLS, 직교배열 장).
A. S. Hedayat, N. J. A. Sloane, J. Stufken, *Orthogonal Arrays: Theory and
Applications*, Springer, 1999.

---

## 2. 코드에 쓰인 구성법

### 2.1 유한체 (소수 거듭제곱)

체 GF(*q*)에서 *L*ₐ(r, c) = *a*·r + *c* (*a* ≠ 0)는 차수 *q*의 MOLS *q* − 1개입니다.
*c*를 더하는 것이 전단사이므로 행이 순열이 되고, 0이 아닌 *a*를 곱하는 것이
전단사이므로 열이 순열이 됩니다. *a* ≠ *b*인 두 방진은 (*a* − *b*)·r이
전단사이므로 직교합니다.

- *q* = *p* (소수): mod *p* 연산.
- *q* = 2^*e*, *e* = 1..31: 차수 *e*의 고정된 기약다항식을 법으로 하는 GF(2)
  위 다항식 연산 (차수 8은 AES 다항식 x⁸ + x⁴ + x³ + x + 1). 표의 모든
  다항식은 `tests/test_latin_square.c`에서 Rabin 기약성 판정으로 검사합니다.

**참고:** R. C. Bose, "On the application of the properties of Galois fields
to the problem of construction of hyper-Graeco-Latin squares", *Sankhyā* 3
(1938), 323–338. M. O. Rabin, "Probabilistic algorithms in finite fields",
*SIAM Journal on Computing* 9(2) (1980), 273–280.

### 2.2 홀수 소수 거듭제곱 *p*^*e*, *e* ≥ 2

코드는 소수 차수 규칙을 *p*진 자릿수마다 적용합니다. 즉 차수 *p* 모임 *e*개의
직접곱입니다. 이렇게 하면 MOLS가 *p* − 1개 나오는데, 온전한 GF(*p*^*e*)가 주는
*p*^*e* − 1개보다 적습니다. 모듈은 더 많다고 주장하지 않고 작은 개수를
보고합니다. 차수 9는 3절의 역사적 표를 쓰며, 이 표도 방진 2개를 담고 있습니다.

### 2.3 합성수 차수: MacNeish 곱

*n* = *n*₁·*n*₂이고 각 차수의 MOLS가 *k*개씩 있으면, 그 직접곱이 차수 *n*의
MOLS *k*개를 줍니다. 그래서 *n*을 소수 거듭제곱의 곱으로 나타냈을 때 코드가
주는 방진 수는 min(*q*ᵢ − 1)입니다 (2.2절의 인수는 *p* − 1).

**참고:** H. F. MacNeish, "Euler squares", *Annals of Mathematics* 23(3)
(1922), 221–227.

### 2.4 코드가 만들지 않는 것

*n* ≡ 2 (mod 4)인 차수는 인수 2¹을 가지므로 곱 구성으로는 방진이 하나만
나옵니다. 직교쌍은 2와 6을 뺀 모든 차수에 존재하지만, *n* ≡ 2 (mod 4)에 대한
구성은 곱 구성이 아니며 이 모듈은 구현하지 않습니다. 이 차수에서
`cwist_latin_family_create(n, 2)`는 성질이 없는 모임을 주는 대신 `NULL`을
돌려줍니다.

**참고:** L. Euler, "Recherches sur une nouvelle espèce de quarrés magiques",
1782 (*n* ≡ 2 mod 4에서는 쌍이 없다고 추측). G. Tarry, 1900 (차수 6의 쌍은
없음). R. C. Bose, S. S. Shrikhande, E. T. Parker, "Further results on the
construction of mutually orthogonal Latin squares and the falsity of Euler's
conjecture", *Canadian Journal of Mathematics* 12 (1960), 189–203 (2와 6을
뺀 모든 *n*에 쌍이 존재).

---

## 3. 역사적 표: 구수략 9차 쌍

**코드에서 쓰는 것:** `k_choi9_a` / `k_choi9_b`. 차수 9의 직교 라틴방진 쌍을
인쇄된 그대로(기호 1–9) 저장한 것으로, 2.3절 곱에서 차수 9 인수로 쓰입니다.

**출처:** **최석정(崔錫鼎, 1646–1715)**, 《구수략(九數略)》(1700년경)의
구구모수변궁양도(九九母數變宮陽圖). 각 칸은 쌍 (*a*, *b*)이고, *a*와 *b*가 각각
라틴방진을 이루며 81개 쌍이 모두 다릅니다. 알려진 가장 이른 9차 직교쌍으로,
Euler의 1782년 연구보다 약 80년 앞섭니다.

짝을 이루는 구구모수변궁음도(九九母數變宮陰圖)는 쓰지 **않습니다**. 81개 쌍이
모두 다르고 3 × 3 궁마다 각 성분에 1–9가 한 번씩 들어 있지만, 행과 열이
순열이 아니어서 라틴방진 쌍이 아닙니다.

`tests/test_latin_square.c`가 저장된 표가 라틴이고 직교인지 검사하므로, 옮겨
적다가 생긴 오류는 보장을 조용히 약하게 만드는 대신 테스트 실패로 드러납니다.

---

## 4. CWIST에서의 사용: 테스트에서 런타임 knob 쌍 덮기

**코드가 하는 일:** `tests/test_bdr_privacy.c`는 GF(4) 위 차수 4의 MOLS 3개로
만든 OA(16, 5, 4, 2)의 실행마다 서버를 한 번씩 띄웁니다. 다섯 인자는 서로 다른
서빙 경로를 고르는 런타임 knob입니다 (`CWIST_C1M_MODE`, 최대 64까지의
`CWIST_WORKER_THREADS`, `CWIST_REACTOR_BACKEND`, `CWIST_HTTP_BATCH`,
`CWIST_RX_URING`). 값이 두 개인 knob은 네 수준에 값을 반복해 넣으며, 그래도 모든
값 쌍이 덮입니다. 따라서 모든 knob 설정 쌍이 적어도 한 실행에서 만나고, 전체
조합 4⁵ = 1024번 대신 16번이면 됩니다.

**이유:** 한 서빙 경로에는 있고 다른 경로에는 없는 보안 검사는 알맞은 설정
쌍이 만날 때만 드러납니다. 이슈 #706이 그런 경우였습니다. 호출자별 응답이
classic 경로에서만 캐시를 통해 샜습니다. 수정 전 코드에서는 classic 실행 8개가
실패하고 C1M 실행 8개는 통과합니다.

**참고:** R. Mandl, "Orthogonal Latin squares: an application of experiment
design to compiler testing", *Communications of the ACM* 28(10) (1985),
1054–1058.
