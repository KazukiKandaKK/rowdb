#!/bin/bash
# Full crash-test suite. Usage: ./run_suite.sh <label>
# Exit status is nonzero if any run returned nonzero: expectation failed (1: mismatch in a
# correct-engine / no-fsync run, or a broken control that found nothing), impossible ledger (2),
# insufficient coverage in a must-match run (3).
set -u
cd "$(dirname "$0")"
L="$1"
SMALL=(--frames 8 --checkpoint-wal-bytes 65536)
# Sanitizer reports must stop the process (UBSan only reports by default), so they cannot hide in a pass.
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=halt_on_error=1
STATUS=0
echo "start $(date '+%H:%M:%S') $L"
run() {
  local name="$1"; shift
  python3 crash_test.py "$@" --out "runs/$L-$name" > /dev/null
  local rc=$?
  echo "$name exit=$rc"
  [ "$rc" -ne 0 ] && STATUS=1
}
run none            --cycles 300 --seed 41
run none-asan       --cycles 100 --seed 42 --binary build-asan/rowdb
run none-small      --cycles 200 --seed 43 "${SMALL[@]}"
run none-small-asan --cycles 60  --seed 44 "${SMALL[@]}" --binary build-asan/rowdb
run nowal           --cycles 60  --seed 45 --bug no-wal
run skipredo        --cycles 60  --seed 46 --bug skip-redo
run steal           --cycles 60  --seed 47 --bug steal
run steal-small     --cycles 60  --seed 48 --bug steal "${SMALL[@]}"
run nofsync         --cycles 100 --seed 49 --bug no-fsync
for d in runs/$L-*; do
  gzip -q "$d"/ledger_*.txt 2>/dev/null
  python3 -c "
import json
s=json.load(open('$d/summary.json'))
keys=['bug','frames','cycles','killed_by_sigkill','exited_before_kill','killed_before_load','txn_phase_kills','checks','pass','fail','harness_errors','stalled_cycles','indoubt_seen','indoubt_present','indoubt_absent','acked_txns','wal_tail_seen','elapsed_s','verdict']
print('$d', {k:s[k] for k in keys})"
done
du -sh runs
echo "end $(date '+%H:%M:%S') suite_status=$STATUS"
exit $STATUS
