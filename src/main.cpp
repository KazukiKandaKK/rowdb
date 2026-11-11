// rowdb CLI
//
//   rowdb workload --dir D --ledger L --seed S --txn-base B [--bug none|no-wal|skip-redo|steal|no-fsync]
//                  [--frames N] [--checkpoint-wal-bytes N]
//       Opens (and recovers) the database, then runs random transactions until killed.
//       Every committing transaction appends its operations and "S <txn> <us>" to the ledger
//       (one write + fsync) before calling commit(), and "C <txn> <us>" after commit() returns.
//       <us> = microseconds since process start. The first line is "R <recovery stats...> <us>",
//       the second "L <live rows> <us> <crc>" once the startup scan has finished, where <crc> is the
//       CRC32 (hex) of the scanned rows written as "<page> <slot> <value>\n" in scan order.
//   rowdb dump --dir D [--bug ...] [--frames N]
//       Opens (and recovers) the database and prints every live row as "<page> <slot> <value>".
//       Every printed row is then read back with get() and must match (exit 3 otherwise; any other
//       error also exits 3, with "error: ..." on stderr). Values never contain a newline (the workload
//       writes "t<txn>o<i>-" plus hex digits); both this listing and the ledger digest rely on that.
//       Recovery statistics go to stderr.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "engine.h"

using namespace rowdb;

namespace {

std::map<std::string, std::string> parse_args(int argc, char** argv, int start) {
  std::map<std::string, std::string> m;
  for (int i = start; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k.rfind("--", 0) != 0) throw DbError("bad argument: " + k);
    m[k.substr(2)] = argv[i + 1];
  }
  return m;
}

Options options_from(const std::map<std::string, std::string>& a) {
  Options o;
  auto it = a.find("bug");
  if (it != a.end()) {
    if (it->second == "none") o.bug = Bug::None;
    else if (it->second == "no-wal") o.bug = Bug::NoWal;
    else if (it->second == "skip-redo") o.bug = Bug::SkipRedo;
    else if (it->second == "steal") o.bug = Bug::Steal;
    else if (it->second == "no-fsync") o.bug = Bug::NoFsync;
    else throw DbError("unknown --bug " + it->second);
  }
  if (a.count("frames")) o.pool_frames = std::stoul(a.at("frames"));
  if (a.count("checkpoint-wal-bytes")) o.checkpoint_wal_bytes = std::stoull(a.at("checkpoint-wal-bytes"));
  return o;
}

void ledger_write(int fd, const std::string& s) {
  size_t done = 0;
  while (done < s.size()) {
    ssize_t w = ::write(fd, s.data() + done, s.size() - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw DbError(std::string("ledger write: ") + std::strerror(errno));
    }
    done += static_cast<size_t>(w);
  }
  if (::fsync(fd) != 0) throw DbError(std::string("ledger fsync: ") + std::strerror(errno));
}

// Microseconds since this process started (steady clock); written into the ledger so that
// startup time and commit latency can be read back per cycle.
uint64_t now_us() {
  static const auto t0 = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

int run_workload(const std::map<std::string, std::string>& a) {
  now_us();
  Engine db(a.at("dir"), options_from(a));
  int lfd = ::open(a.at("ledger").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) throw DbError("open ledger");
  const auto& rs = db.recovery_stats();
  ledger_write(lfd, "R " + std::to_string(rs.wal_records) + " " + std::to_string(rs.committed_txns) + " " +
                        std::to_string(rs.pages_redone) + " " + std::to_string(rs.discarded_records) + " " +
                        std::to_string(rs.wal_tail_bytes) + " " + std::to_string(now_us()) + "\n");

  std::vector<Rid> live;
  uint32_t digest = 0;  // lets the harness tell exactly which state recovery produced
  db.scan([&](Rid r, std::string_view v) {
    live.push_back(r);
    std::string row = std::to_string(r.page) + " " + std::to_string(r.slot) + " ";
    row.append(v.data(), v.size());
    row += "\n";
    digest = crc32(row.data(), row.size(), digest);
  });
  char hexd[9];
  std::snprintf(hexd, sizeof hexd, "%08x", digest);
  ledger_write(lfd, "L " + std::to_string(live.size()) + " " + std::to_string(now_us()) + " " + hexd + "\n");  // startup scan done

  std::mt19937_64 rng(std::stoull(a.at("seed")));
  auto uni = [&](uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(rng); };
  static const char* hex = "0123456789abcdef";
  TxnId txn = std::stoull(a.at("txn-base"));

  for (;; ++txn) {
    std::vector<Rid> snapshot = live;
    std::string ops;
    db.begin();
    int nops = static_cast<int>(uni(1, 12));
    bool too_large = false;
    for (int i = 0; i < nops && !too_large; ++i) try {
      bool do_insert = live.empty() || uni(0, 99) < (live.size() < 1500 ? 70u : 40u);
      if (do_insert) {
        std::string v = "t" + std::to_string(txn) + "o" + std::to_string(i) + "-";
        size_t pad = uni(8, 200);
        for (size_t k = 0; k < pad; ++k) v.push_back(hex[uni(0, 15)]);
        Rid r = db.insert(v);
        live.push_back(r);
        ops += "I " + std::to_string(txn) + " " + std::to_string(r.page) + " " + std::to_string(r.slot) + " " + v + "\n";
      } else {
        size_t idx = uni(0, live.size() - 1);
        Rid r = live[idx];
        if (!db.remove(r)) throw DbError("remove of a live row returned false");
        live[idx] = live.back();
        live.pop_back();
        ops += "D " + std::to_string(txn) + " " + std::to_string(r.page) + " " + std::to_string(r.slot) + "\n";
      }
    } catch (const TxnTooLarge&) {
      too_large = true;  // small buffer pool: give up this transaction, like a real caller would
    }
    if (too_large || uni(0, 99) < 5) {
      db.abort();
      live = std::move(snapshot);
      continue;
    }
    ledger_write(lfd, ops + "S " + std::to_string(txn) + " " + std::to_string(now_us()) + "\n");
    db.commit();
    ledger_write(lfd, "C " + std::to_string(txn) + " " + std::to_string(now_us()) + "\n");
  }
}

int run_dump(const std::map<std::string, std::string>& a) {
  Engine db(a.at("dir"), options_from(a));
  const auto& rs = db.recovery_stats();
  std::cerr << "recovery wal_records=" << rs.wal_records << " committed_txns=" << rs.committed_txns
            << " pages_redone=" << rs.pages_redone << " discarded_records=" << rs.discarded_records
            << " wal_tail_bytes=" << rs.wal_tail_bytes << "\n";
  std::string out;
  std::vector<std::pair<Rid, std::string>> rows;
  db.scan([&](Rid r, std::string_view v) {
    rows.emplace_back(r, std::string(v));
    out += std::to_string(r.page) + " " + std::to_string(r.slot) + " ";
    out.append(v.data(), v.size());
    out += "\n";
  });
  std::fwrite(out.data(), 1, out.size(), stdout);  // before the cross-check: kept even if it fails
  std::fflush(stdout);
  // Cross-check the point lookup against the scan: every scanned row must read back the same through
  // get(), and a slot past the end of the slot directory must read as absent.
  for (const auto& [r, v] : rows) {
    auto g = db.get(r);
    if (!g || *g != v) {
      std::cerr << "dump: get(" << r.page << "," << r.slot << ") disagrees with scan\n";
      return 3;
    }
    if (db.get(Rid{r.page, 0xFFFF})) {
      std::cerr << "dump: get() returned a row for a slot that cannot exist\n";
      return 3;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: rowdb workload|dump --dir D ...\n";
    return 2;
  }
  try {
    std::string cmd = argv[1];
    auto a = parse_args(argc, argv, 2);
    if (cmd == "workload") return run_workload(a);
    if (cmd == "dump") return run_dump(a);
    std::cerr << "unknown command " << cmd << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 3;
  }
}
