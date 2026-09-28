#!/usr/bin/env bash
# Workload only. benchmark_session.py owns server/descendant cleanup.
set -eu
PID=${BENCHMARK_SERVER_PID:?missing supervised server PID}
PORT=$1
TXT=$2
STAT_TXT=$3
WAIT=$4
THREADS=$5
CONNECTIONS=$6
for i in $(seq 1 $WAIT); do
  curl -sf http://127.0.0.1:$PORT/ > /dev/null 2>&1 && break
  sleep 1
done

# Warmup (results discarded)
wrk -t"$THREADS" -c"$CONNECTIONS" -d10s http://127.0.0.1:$PORT/ > /dev/null 2>&1 || true

# Context switches: `ps -o nvcsw/nivcsw` is not a procps keyword on Linux
# (it prints "-", which awk summed to 0 -- every row recorded Csw 0). Read
# the counters from /proc instead: sum voluntary + nonvoluntary switches
# over every task of every process in the server's process group, keyed by
# (pid, process start time, tid, task start time) so a thread created or
# exited mid-run has no delta and is skipped rather than corrupting the sum.
csw_snapshot() {
  python3 - "$1" > "$2" <<'EOF'
import os, sys
pgid = int(sys.argv[1])
for entry in sorted(os.listdir('/proc')):
    if not entry.isdigit():
        continue
    try:
        fields = open(f'/proc/{entry}/stat').read().rsplit(') ', 1)[1].split()
        if int(fields[2]) != pgid or fields[0] in ('Z', 'X'):
            continue
        for task in os.listdir(f'/proc/{entry}/task'):
            tstat = open(f'/proc/{entry}/task/{task}/stat').read().rsplit(') ', 1)[1].split()
            status = dict(line.split(':', 1) for line in open(f'/proc/{entry}/task/{task}/status') if ':' in line)
            csw = int(status['voluntary_ctxt_switches']) + int(status['nonvoluntary_ctxt_switches'])
            print(f'{entry}:{fields[19]}:{task}:{tstat[19]} {csw}')
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        continue
EOF
}

# Initial stat (after warmup)
BEFORE_CSW=$(mktemp)
csw_snapshot "$PID" "$BEFORE_CSW"

wrk -t"$THREADS" -c"$CONNECTIONS" -d10s -s "$GITHUB_WORKSPACE/scripts/ci/tail_latency.lua" --latency http://127.0.0.1:$PORT/ > $TXT 2>&1 || true

AFTER_CSW=$(mktemp)
csw_snapshot "$PID" "$AFTER_CSW"
RSS=$(ps -o rss= -p $PID 2>/dev/null | awk '{print $1}')
[ -z "$RSS" ] && RSS=0

CSW=$(python3 - "$BEFORE_CSW" "$AFTER_CSW" <<'EOF'
import sys
def load(path):
    rows = {}
    for line in open(path):
        key, value = line.split()
        rows[key] = int(value)
    return rows
before, after = load(sys.argv[1]), load(sys.argv[2])
common = [k for k in before.keys() & after.keys() if after[k] >= before[k]]
print(sum(after[k] - before[k] for k in common) if common else 0)
EOF
)
rm -f "$BEFORE_CSW" "$AFTER_CSW"

echo "rss_kib=$RSS csw=$CSW" > $STAT_TXT
cat $TXT
