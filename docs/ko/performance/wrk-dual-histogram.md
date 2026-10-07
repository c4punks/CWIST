# 선택적 wrk 원시/보정 지연 히스토그램

이 진단 도구는 이슈 #25의 측정 모호성을 다룹니다. CWIST, 일반 CI 작업 플래그, 런타임 기본값은
바꾸지 않습니다. CI의 기존 Python 테스트 탐색이 오프라인 계약 테스트를 실행하며, 벤치마크는
운영자가 생성된 wrk 소스를 명시적으로 빌드할 때만 이 도구를 씁니다.

wrk 4.2.0은 일반 보고와 Lua `done` 콜백 전에 `stats_correct`를 호출합니다. 그래서 Lua
`latency:percentile(99.999)`는 수정되지 않은 관측 히스토그램이 아닙니다. 이 내보내기 도구는 모든
워커가 join되고 측정 시간이 고정된 뒤, 기존 보정 호출을 앞뒤로 감싸서 같은 실행의 두 분포를 모두
내보냅니다. 요청 루프나 타이밍은 바꾸지 않습니다.

## 버려도 되는 wrk 체크아웃에서 빌드하기

wg/wrk 태그 `4.2.0`을 사용하세요. 생성기는 정확한 업스트림 `wrk.c`, `stats.c`, `stats.h`, 또는
#25 실험에서 쓴, 지문으로 명시된 CLOCK_MONOTONIC 타이밍 변형만 받습니다. 그 밖의 내용은
거부합니다. 그 대체 변형은 `time_us`만 CLOCK_MONOTONIC으로 바꾸고 시간 헤더를 추가하며,
내보내기 도구 자체는 시계를 바꾸지 않습니다.

```sh
python3 scripts/ci/instrument_wrk_histogram.py \
  --source-dir /path/to/disposable-wrk/src \
  --output-dir /path/to/new-output
# Inspect new-output/manifest.json and retain it with the run evidence.
cp /path/to/new-output/wrk.c /path/to/disposable-wrk/src/
cp /path/to/new-output/wrk_dual_histogram.h /path/to/disposable-wrk/src/
make -C /path/to/disposable-wrk
```

생성기는 입력 소스 디렉터리를 읽기만 합니다. 출력 디렉터리는 존재하지 않아야 하고 그 부모
디렉터리는 존재해야 하며, 생성은 재개할 수도 트랜잭션도 아닙니다. I/O 실패가 일부만 생성된 출력
디렉터리를 남길 수 있습니다. 실패 후에는 그 디렉터리를 확인하고, 일부만 생성된 파일은 쓰지
마세요. 운영자 소유의 비공개 디렉터리를 사용하세요. 생성된 헤더는 wrk 빌드 전용이며 CWIST 공개
헤더가 아닙니다. 이미 패치된 wrk에 대한 재생성은 의도적으로 거부합니다.

## 출력과 해석

stdout/stderr, 종료 코드, 작업 에러, 토폴로지, 소스/빌드 매니페스트와 바이너리 해시를 보관하세요.
바뀌지 않은 표준 출력에 더해, `CWIST_HISTOGRAM ` 접두사가 붙은 JSON 줄이 두 개 있습니다.

```text
CWIST_HISTOGRAM {"kind":"raw","count":2,"unit":"us","bins":[[40,2]]}
CWIST_HISTOGRAM {"kind":"corrected","count":6,"unit":"us","bins":[[20,2],[30,2],[40,2]]}
```

이 예시 bin은 희소한 `[latency_us, sample_count]` 쌍입니다. 항상 각 종류가 정확히 한 번, 버킷
위치는 엄격히 증가하는 정수, 개수는 양수, sum(bins)==count를 요구하세요. 보정이 그 필드를
갱신하지 않고 더 낮은 버킷을 채울 수 있으므로, 내보내기 도구는 예전 `min`보다 아래도 훑습니다.
빈 분포는 count가 0이고 bins가 []이며, 백분위수는 0이 아니라 사용할 수 없는 값입니다.

원시 분포는 wrk가 **기록한** 지연 히스토그램이지, 모든 요청/도착이 관측되었다는 증명이
아닙니다. 요청 총수와 에러/타임아웃 수를 따로 보존하고, 개수 차이, 파이프라인 묶음, 범위 밖
샘플, 진행 중인 요청을 설명하세요. 어느 히스토그램도 제공된 작업 부하의 coordinated omission을
고치지 않으며, 개방 루프 SLO를 확립하지도 않습니다. bin을 줄일 때는 백분위수 순위 규칙을
문서화하고, nearest-rank 통계를 wrk 자체의 반올림 알고리즘과 조용히 같은 것으로 취급하지 마세요.
예전의 보정된 요약 스칼라에서 원시 분위수를 추론하지 마세요.

출력은 타이밍과 워커 join 이후에 일어나지만 여전히 CPU, I/O, 출력량, 벽시계 시간을 씁니다.
프로세스 정리를 통해 실행을 격리하고, 최종 출력을 위해 감독자 타임아웃을 넉넉히 두세요. 내보내기와
정리가 진행 중일 때 다른 부하를 끼워 넣지 마세요. 원시 결과와 보정 결과는 같은 실행끼리
비교하세요. 이전의 계측하지 않은 실행은 짝지은 성능 대조군이 아닙니다.

## 검증

```sh
python3 scripts/ci/test_wrk_dual_histogram.py
python3 -O scripts/ci/test_wrk_dual_histogram.py
```

테스트는 명시적인 희소/빈/원래 최솟값보다 아래인 분포로 내보내기 도구를 컴파일하고, 보정 블록의
배치와 보존을 확인하며, 반복되거나 빠진 훅과 이미 계측된 입력을 거부하고, 알 수 없는 소스는
쓰기 없이 거부합니다. C 컴파일러와 Python3가 필요합니다. 실제 wrk 컴파일과 공개 리스너 통합은
별도의 관문이며, 이 오프라인 테스트가 그것을 뜻하지는 않습니다.
