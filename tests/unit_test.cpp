// Minimal unit tests for rowdb (no external framework).
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "engine.h"

using namespace rowdb;

static int failures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
      ++failures;                                                                \
    }                                                                            \
  } while (0)

static std::string fresh_dir(const std::string& name) {
  std::system(("mkdir -p '" + std::string(ROWDB_TEST_TMP) + "'").c_str());
  // Keep test databases inside the build tree (never the system temp dir).
  std::string d = std::string(ROWDB_TEST_TMP) + "/rowdb_test_" + name + "_" + std::to_string(::getpid());
  std::system(("rm -rf '" + d + "'").c_str());
  return d;
}

static std::map<std::string, std::string> contents(Engine& db) {
  std::map<std::string, std::string> m;
  db.scan([&](Rid r, std::string_view v) { m[std::to_string(r.page) + ":" + std::to_string(r.slot)] = std::string(v); });
  return m;
}

// Simulates a process crash: the Engine object is leaked, so no destructor-time checkpoint runs.
// File descriptors stay open, which matches kill -9 (the kernel keeps written data in the page cache).
static void crash(Engine* db) { (void)db; }

static void test_basic_and_reopen() {
  auto d = fresh_dir("basic");
  std::vector<Rid> rids;
  {
    Engine db(d, Options{});
    db.begin();
    for (int i = 0; i < 100; ++i) rids.push_back(db.insert("value-" + std::to_string(i)));
    CHECK(db.remove(rids[10]));
    CHECK(!db.remove(rids[10]));
    db.commit();
    CHECK(db.get(rids[0]).value_or("") == "value-0");
    CHECK(!db.get(rids[10]).has_value());
  }
  Engine db(d, Options{});
  auto m = contents(db);
  CHECK(m.size() == 99);
  CHECK(db.get(rids[99]).value_or("") == "value-99");
}

static void test_abort_restores() {
  auto d = fresh_dir("abort");
  Engine db(d, Options{});
  db.begin();
  Rid a = db.insert("keep");
  db.commit();
  auto before = contents(db);
  db.begin();
  db.remove(a);
  for (int i = 0; i < 300; ++i) db.insert(std::string(100, 'x'));  // allocates new pages
  db.abort();
  CHECK(contents(db) == before);
  db.begin();
  Rid b = db.insert("after-abort");  // must reuse the page space released by abort
  db.commit();
  CHECK(b.page == a.page);
  CHECK(db.get(a).value_or("") == "keep");
}

static void test_crash_redo() {
  auto d = fresh_dir("redo");
  std::map<std::string, std::string> expect;
  {
    auto* db = new Engine(d, Options{});
    for (int t = 0; t < 20; ++t) {
      db->begin();
      for (int i = 0; i < 10; ++i) db->insert("t" + std::to_string(t) + "-" + std::to_string(i));
      db->commit();
    }
    expect = contents(*db);
    db->begin();
    db->insert("uncommitted");
    crash(db);
  }
  Engine db(d, Options{});
  CHECK(db.recovery_stats().committed_txns == 20);
  CHECK(db.recovery_stats().pages_redone > 0);
  CHECK(contents(db) == expect);
}

static void test_skip_redo_control_loses_data() {
  auto d = fresh_dir("skipredo");
  size_t committed_rows = 0;
  {
    auto* db = new Engine(d, Options{Bug::SkipRedo, 32, 512 * 1024});
    db->begin();
    for (int i = 0; i < 10; ++i) db->insert("row" + std::to_string(i));
    db->commit();
    committed_rows = contents(*db).size();
    crash(db);
  }
  Engine db(d, Options{Bug::SkipRedo, 32, 512 * 1024});
  CHECK(committed_rows == 10);
  bool lost = true;
  try {
    lost = contents(db).size() < committed_rows;
  } catch (const DbError&) {
    lost = true;  // a never-written page is also a detected loss
  }
  CHECK(lost);
}

static void test_eviction_and_checkpoint() {
  auto d = fresh_dir("evict");
  std::map<std::string, std::string> expect;
  {
    auto* db = new Engine(d, Options{Bug::None, 8, 64 * 1024});  // tiny pool, frequent checkpoints
    for (int t = 0; t < 200; ++t) {
      db->begin();
      for (int i = 0; i < 5; ++i) db->insert(std::string(150, static_cast<char>('a' + (t % 26))) + std::to_string(t));
      db->commit();
    }
    expect = contents(*db);
    crash(db);
  }
  Engine db(d, Options{Bug::None, 8, 64 * 1024});
  CHECK(contents(db) == expect);
  CHECK(expect.size() == 1000);
}

static void test_txn_too_large() {
  auto d = fresh_dir("toolarge");
  Engine db(d, Options{Bug::None, 4, 512 * 1024});
  db.begin();
  bool threw = false;
  try {
    for (int i = 0; i < 1000; ++i) db.insert(std::string(500, 'z'));
  } catch (const TxnTooLarge&) {
    threw = true;
  }
  CHECK(threw);
  db.abort();
  CHECK(contents(db).empty());
}

// A caller that catches TxnTooLarge and then commits the work done so far must not be left
// with a phantom page (page_count_ advanced for a page that was never created).
static void test_txn_too_large_keeps_page_count() {
  auto d = fresh_dir("toolarge2");
  Engine db(d, Options{Bug::None, 4, 512 * 1024});
  db.begin();
  bool threw = false;
  size_t inserted = 0;
  try {
    for (int i = 0; i < 1000; ++i) {
      db.insert(std::string(500, 'z'));
      ++inserted;
    }
  } catch (const TxnTooLarge&) {
    threw = true;
  }
  CHECK(threw);
  db.commit();
  bool ok = true;
  try {
    CHECK(contents(db).size() == inserted);
    db.begin();
    db.insert("after");
    db.commit();
    CHECK(contents(db).size() == inserted + 1);
  } catch (const DbError& e) {
    std::cerr << "  after TxnTooLarge+commit: " << e.what() << "\n";
    ok = false;
  }
  CHECK(ok);
}

// Steal control: an aborted txn must not reappear after a clean close/reopen (only a crash may expose it).
static void test_steal_abort_then_reopen() {
  auto d = fresh_dir("stealabort");
  Options steal;
  steal.bug = Bug::Steal;
  {
    Engine db(d, steal);
    db.begin();
    db.insert("committed");
    db.commit();
    db.begin();
    db.insert("aborted-same-page");
    for (int i = 0; i < 200; ++i) db.insert(std::string(100, 'n'));  // forces new pages
    db.abort();
  }
  std::vector<std::string> rows;
  {
    Engine db(d, Options{});
    db.scan([&](Rid, std::string_view v) { rows.emplace_back(v); });
  }
  CHECK(rows.size() == 1);
  CHECK(!rows.empty() && rows[0] == "committed");
}

// Overwrites slot `slot` of page `page` in data.db (offset/length, little-endian) and re-seals the page,
// so the page passes its CRC check and only the slot bounds can catch the damage.
static void corrupt_slot(const std::string& dir, PageId page, uint16_t slot, uint16_t off, uint16_t len) {
  FILE* f = std::fopen((dir + "/data.db").c_str(), "r+b");
  CHECK(f != nullptr);
  if (!f) return;
  uint8_t buf[kPageSize];
  std::fseek(f, static_cast<long>(page) * kPageSize, SEEK_SET);
  CHECK(std::fread(buf, 1, kPageSize, f) == kPageSize);
  const size_t at = 12 + 4 * size_t{slot};  // slot directory layout from engine.cpp (kSlotsOff, kSlotSize)
  store_u16(buf + at, off);
  store_u16(buf + at + 2, len);
  seal_page(buf);
  std::fseek(f, static_cast<long>(page) * kPageSize, SEEK_SET);
  CHECK(std::fwrite(buf, 1, kPageSize, f) == kPageSize);
  std::fclose(f);
}

static bool throws_db_error(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const DbError&) {
    return true;
  }
  return false;
}

static void test_get_bounds() {
  auto d = fresh_dir("getbounds");
  Rid a, b;
  {
    Engine db(d, Options{});
    db.begin();
    a = db.insert("first");
    b = db.insert("second");
    db.commit();
  }  // clean close: checkpoint writes page 1 to data.db and empties the WAL
  {
    Engine db(d, Options{});
    CHECK(db.get(a).value_or("") == "first");
    CHECK(!db.get(Rid{a.page, 0xFFFF}).has_value());  // past the last slot
    CHECK(!db.get(Rid{0, 0}).has_value());              // meta page
    CHECK(!db.get(Rid{a.page + 100, 0}).has_value());   // past the last page
  }
  corrupt_slot(d, b.page, b.slot, 4090, 100);  // value would run past the end of the page
  {
    Engine db(d, Options{});
    CHECK(db.get(a).value_or("") == "first");  // the intact slot still reads
    CHECK(throws_db_error([&] { db.get(b); }));
    CHECK(throws_db_error([&] { db.scan([](Rid, std::string_view) {}); }));
  }
  corrupt_slot(d, b.page, b.slot, 4, 4);  // value would overlap the page header / slot directory
  {
    Engine db(d, Options{});
    CHECK(throws_db_error([&] { db.get(b); }));
  }
}

int main() {
  test_basic_and_reopen();
  test_abort_restores();
  test_crash_redo();
  test_skip_redo_control_loses_data();
  test_eviction_and_checkpoint();
  test_txn_too_large();
  test_txn_too_large_keeps_page_count();
  test_steal_abort_then_reopen();
  test_get_bounds();
  if (failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all unit tests passed\n";
  return 0;
}
