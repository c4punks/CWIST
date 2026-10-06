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

자릿수를 3자리로 확장할 경우에도 동일한 후입 전파 규칙을 적용합니다:

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
