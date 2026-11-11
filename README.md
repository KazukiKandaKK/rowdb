# rowdb

A small row-oriented storage engine in C++20, together with a crash test for one property: after `kill -9`, every transaction that was reported as committed is present, and a transaction killed while committing is either entirely present or entirely absent.

The code was written by an AI (Claude). Design and verification decisions were made by Kazuki Kanda.

## The engine

- 4 KB pages, each with a CRC32; a fixed-size buffer pool with LRU replacement
- Slotted heap pages with insert, get, remove and scan; one transaction at a time
- NO-STEAL: pages touched by an uncommitted transaction are never written to the data file, so recovery only has to redo
- The write-ahead log stores full page images. A commit appends the images and a COMMIT record in one write, calls fsync, and only then returns
- Recovery replays the transactions whose COMMIT record is intact and drops a torn tail
- Checkpointing happens inside a commit once the WAL is over 512 KB (by default)

[DESIGN.md](DESIGN.md) (in Japanese) explains the design decisions and how the tests work.

## Build and test

Developed and tested on macOS (Apple Silicon) with CMake 3.20 or later and Python 3.

```sh
cmake -S . -B build && cmake --build build
cmake -S . -B build-asan -DROWDB_ASAN=ON && cmake --build build-asan   # AddressSanitizer + UBSan
./build/unit_test
python3 tests/test_harness.py
```

## Crash test

`crash_test.py` runs the workload (`rowdb workload`) and sends SIGKILL after a random delay (0–250 ms by default). In most cycles it then reopens the database with `rowdb dump` and compares the result against the state expected from a ledger that the workload fsyncs as it goes. The database is kept between cycles, so crashes pile up, and recovery itself can be interrupted. Each workload also writes a checksum of the rows it sees at startup, and the harness checks that this state is one the ledger allows, so a row that one recovery made visible cannot quietly disappear before the next dump.

```sh
python3 crash_test.py --cycles 300 --seed 41 --out runs/none
./run_suite.sh <label>
```

`run_suite.sh` runs nine configurations. The correct engine (two buffer-pool sizes, each with and without sanitizers) must never mismatch. The deliberately broken modes `--bug no-wal`, `--bug skip-redo` and `--bug steal` must be caught; `steal` is run at both pool sizes, the other two at the default size. `--bug no-fsync` is expected to pass, because `kill -9` leaves the OS page cache intact.

Only process crashes are tested, not power loss. A plain `fsync` on macOS does not force the drive's own cache to stable storage (`F_FULLFSYNC` does).

## Report

[report/rowdb-m1-report.pdf](report/rowdb-m1-report.pdf) (in Japanese) reports the first milestone as of 2026-11-10: the method, the results of the final suite, and what the test cannot show. It describes the code at the first commit. Later commits added a startup-state check and more bounds checks. The raw runs are not in the repository; `report/data/aggregates.json` holds the numbers the report uses.
