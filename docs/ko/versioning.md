# Release Codename Sequencing Specification

CWIST LTS 릴리즈의 코드네임은 의미적 해석이나 계층(Major, Minor 등)에 대한 구분 없이, 알파벳의 **순차적 후입 전파(Tail-In Sequential Propagation)** 규칙만을 따르는 상태 식별자입니다.

---

## 1. 기본 원칙

- 코드네임은 영문 대문자 ASCII로 구성됩니다.
- 신규 알파벳은 항상 **가장 오른쪽(후미)**으로 진입합니다.
- 다음 릴리즈마다 신규 알파벳이 왼쪽으로 한 자리씩 밀고 들어가며, 모든 자리가 해당 알파벳으로 채워지면 다음 알파벳으로 순환합니다.
- 특정 알파벳 자리에 대한 기능적 의미나 버전 가중치는 두지 않습니다.

---

## 2. 2자리 시퀀스 (기본)

$$\text{AA} \rightarrow \text{AB} \rightarrow \text{BA} \rightarrow \text{BB} \rightarrow \text{BC} \rightarrow \text{CB} \rightarrow \text{CC} \rightarrow \text{CD} \rightarrow \dots$$

1. `AA`
2. `AB` (`B` 후입)
3. `BA` (`B` 좌측 전파)
4. `BB` (`B` 완료)
5. `BC` (`C` 후입)
6. `CB` (`C` 좌측 전파)
7. `CC` (`C` 완료)

---

## 3. 3자리 확장 시퀀스

3자리 확장은 2자리 시퀀스가 고갈되었을 때만 합니다. 3자리로 확장할 때도 2자리와 같은 방식입니다. 신규 알파벳은 항상 꼬리(가장 오른쪽)로 들어오고, 릴리즈마다 한 자리씩 왼쪽으로 전파되어 모든 자리를 채웁니다. 다 채워지면 다음 알파벳이 다시 꼬리로 들어옵니다:

$$\text{AAA} \rightarrow \text{AAB} \rightarrow \text{ABB} \rightarrow \text{BBB} \rightarrow \text{BBC} \rightarrow \text{BCC} \rightarrow \text{CCC} \rightarrow \dots$$

1. `AAA`
2. `AAB` (`B` 후입)
3. `ABB` (`B` 좌측 전파)
4. `BBB` (`B` 완료)
5. `BBC` (`C` 후입)
6. `BCC` (`C` 좌측 전파)
7. `CCC` (`C` 완료)

---

## 4. 표기

빌드 바이너리 및 CLI 출력에 릴리즈 식별자로 병기됩니다:

```sh
$ cwist --version
cwist 4.0.0-LTS (AA)
```

---

## 5. API 안정성 게이트 (v4 LTS부터)

코드네임과 별개로, v4부터 기존 공개 API는 바뀌지 않습니다(ROADMAP.md "API stability from v4.0").
CI는 `scripts/ci/check_api_baseline.py`로 이를 확인합니다.

- `docs/api/v4.0-api-baseline.txt`에는 공개 헤더의 이름, 함수 시그니처(파라미터 이름 제외), struct/union/enum 레이아웃이 기록됩니다.
- 헤더가 이 파일과 다르면 CI가 실패합니다. 의도한 변경이면 `make api-baseline`으로 다시 생성해 같은 PR에 커밋합니다.
- LTS 태그 `v4`가 생긴 뒤에는, 그 태그의 baseline에 있는 보장 항목이 사라지거나 바뀌면 baseline을 다시 생성해도 실패합니다. 새 API 추가는 통과하고, experimental(WebTransport) 섹션은 검사하지 않습니다.

---

## 6. 메이저 버전의 첫 상승

메이저 버전 라인이 처음 열릴 때(메이저 숫자가 올라갈 때), 그 라인의 릴리즈는 `v1`부터 순서대로 이름 붙입니다: `v1 → v2 → v3 → v4 → …`. 카운터는 항상 1에서 시작하며 건 넘기지 않습니다.

이 규칙은 새 메이저 라인마다 독립적으로 적용됩니다: 다음 메이저의 첫 상승은 이전 라인이 어디까지 셌든 상관없이 다시 `v1`부터 시작합니다. 이전 라인의 카운터를 이어받지 않고, 편집기나 자동완성이 제안한다고 해서 (예: `.0` 스타일 이름으로) 건 넘어가지 않습니다.
