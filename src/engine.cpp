#include "engine.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace rowdb {

namespace {
constexpr uint32_t kHeapMagic = 0x50414548;  // "HEAP"
constexpr uint32_t kMetaMagic = 0x42445752;  // "RWDB"
constexpr size_t kSlotCountOff = 4;
constexpr size_t kFreeEndOff = 6;
constexpr size_t kMagicOff = 8;
constexpr size_t kSlotsOff = 12;
constexpr size_t kSlotSize = 4;

uint16_t slot_count(const uint8_t* p) { return load_u16(p + kSlotCountOff); }
uint16_t free_end(const uint8_t* p) { return load_u16(p + kFreeEndOff); }
size_t free_space(const uint8_t* p) {
  size_t used_front = kSlotsOff + kSlotSize * slot_count(p);
  size_t fe = free_end(p);
  return fe > used_front ? fe - used_front : 0;
}
void slot_get(const uint8_t* p, uint16_t i, uint16_t& off, uint16_t& len) {
  off = load_u16(p + kSlotsOff + kSlotSize * i);
  len = load_u16(p + kSlotsOff + kSlotSize * i + 2);
}
void check_heap(const uint8_t* p, PageId pid) {
  if (load_u32(p + kMagicOff) != kHeapMagic) throw DbError("page " + std::to_string(pid) + " is not a heap page");
}
// Number of slots on a heap page, after checking that the slot directory fits in the page.
uint16_t checked_slot_count(const uint8_t* p, PageId pid) {
  uint16_t n = slot_count(p);
  if (kSlotsOff + kSlotSize * size_t{n} > kPageSize)
    throw DbError("page " + std::to_string(pid) + " has a slot directory larger than the page");
  return n;
}
// Reads slot i (< checked_slot_count) and checks that its value lies between the slot directory and
// the end of the page. Returns false for a free slot.
bool read_slot(const uint8_t* p, PageId pid, uint16_t i, uint16_t n, uint16_t& off, uint16_t& len) {
  slot_get(p, i, off, len);
  if (off == 0) return false;
  if (off < kSlotsOff + kSlotSize * size_t{n} || size_t{off} + len > kPageSize)
    throw DbError("page " + std::to_string(pid) + " slot " + std::to_string(i) + " is out of bounds");
  return true;
}
}  // namespace

Engine::Engine(const std::string& dir, Options opt) : dir_(dir), opt_(opt) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
    throw DbError("mkdir " + dir + ": " + std::strerror(errno));
  disk_ = std::make_unique<DiskManager>(dir + "/data.db");
  wal_ = std::make_unique<Wal>(dir + "/wal.log", opt_.bug != Bug::NoFsync);
  pool_ = std::make_unique<BufferPool>(*disk_, opt_.pool_frames);
  recover();
  if (page_count_ == 0) {
    uint8_t meta[kPageSize] = {};
    store_u32(meta + kMagicOff, kMetaMagic);
    store_u32(meta + 12, 1);  // format version
    seal_page(meta);
    disk_->write_page(0, meta);  // not WAL-logged; see DESIGN.md (power-loss scope)
    sync_data();
    page_count_ = 1;
  }
  const uint8_t* meta = pool_->fetch(0);
  if (load_u32(meta + kMagicOff) != kMetaMagic) throw DbError("page 0 is not a rowdb meta page");
}

Engine::~Engine() {
  try {
    if (in_txn_) abort();
    checkpoint();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "rowdb: shutdown checkpoint failed: %s\n", e.what());  // WAL still holds the data
  }
}

void Engine::recover() {
  rstats_ = {};
  auto records = wal_->read_all(&rstats_.wal_tail_bytes);
  rstats_.wal_records = records.size();
  std::vector<TxnId> committed;
  for (const auto& r : records)
    if (r.type == Wal::kCommit) committed.push_back(r.txn);
  std::sort(committed.begin(), committed.end());
  rstats_.committed_txns = committed.size();
  for (const auto& r : records) {
    if (r.type != Wal::kPage) continue;
    if (!std::binary_search(committed.begin(), committed.end(), r.txn)) {
      ++rstats_.discarded_records;
      continue;
    }
    if (opt_.bug == Bug::SkipRedo) continue;  // negative control: forget to replay
    if (!page_crc_ok(r.payload.data()))
      throw DbError("WAL image of page " + std::to_string(r.page_id) + " failed its checksum");
    disk_->write_page(r.page_id, r.payload.data());
    ++rstats_.pages_redone;
  }
  if (rstats_.pages_redone) sync_data();
  wal_->truncate();
  pool_->drop_all();
  page_count_ = disk_->page_count();
}

void Engine::sync_data() {
  if (opt_.bug != Bug::NoFsync) disk_->sync();
}

void Engine::steal_write(PageId pid, uint8_t* page) {
  if (opt_.bug != Bug::Steal) return;
  seal_page(page);
  disk_->write_page(pid, page);  // negative control: uncommitted image reaches the data file
}

void Engine::require_txn() const {
  if (!in_txn_) throw DbError("operation requires an active transaction");
}

void Engine::begin() {
  if (in_txn_) throw DbError("a transaction is already active");
  in_txn_ = true;
  page_count_at_begin_ = page_count_;
}

Rid Engine::insert(std::string_view value) {
  require_txn();
  if (value.empty() || value.size() > kMaxRecord) throw DbError("record size must be 1.." + std::to_string(kMaxRecord));
  const size_t need = value.size() + kSlotSize;
  PageId pid = kInvalidPage;
  uint8_t* p = nullptr;
  if (page_count_ > 1) {
    PageId last = page_count_ - 1;
    const uint8_t* rp = pool_->fetch(last);
    check_heap(rp, last);
    if (free_space(rp) >= need) {
      pid = last;
      p = pool_->fetch_for_write(last);
    }
  }
  if (p == nullptr) {
    pid = page_count_;
    p = pool_->new_page(pid);  // may throw TxnTooLarge; only count the page once it exists
    ++page_count_;
    store_u16(p + kSlotCountOff, 0);
    store_u16(p + kFreeEndOff, static_cast<uint16_t>(kPageSize));  // 4096 fits in u16
    store_u32(p + kMagicOff, kHeapMagic);
  }
  uint16_t n = slot_count(p);
  uint16_t fe = static_cast<uint16_t>(free_end(p) - value.size());
  std::memcpy(p + fe, value.data(), value.size());
  store_u16(p + kSlotsOff + kSlotSize * n, fe);
  store_u16(p + kSlotsOff + kSlotSize * n + 2, static_cast<uint16_t>(value.size()));
  store_u16(p + kSlotCountOff, static_cast<uint16_t>(n + 1));
  store_u16(p + kFreeEndOff, fe);
  steal_write(pid, p);
  return Rid{pid, n};
}

bool Engine::remove(Rid rid) {
  require_txn();
  if (rid.page == 0 || rid.page >= page_count_) return false;
  const uint8_t* rp = pool_->fetch(rid.page);
  check_heap(rp, rid.page);
  uint16_t n = checked_slot_count(rp, rid.page);
  if (rid.slot >= n) return false;
  uint16_t off, len;
  if (!read_slot(rp, rid.page, rid.slot, n, off, len)) return false;
  uint8_t* p = pool_->fetch_for_write(rid.page);
  store_u16(p + kSlotsOff + kSlotSize * rid.slot, 0);
  store_u16(p + kSlotsOff + kSlotSize * rid.slot + 2, 0);
  steal_write(rid.page, p);
  return true;
}

std::optional<std::string> Engine::get(Rid rid) {
  if (rid.page == 0 || rid.page >= page_count_) return std::nullopt;
  const uint8_t* p = pool_->fetch(rid.page);
  check_heap(p, rid.page);
  uint16_t n = checked_slot_count(p, rid.page);
  if (rid.slot >= n) return std::nullopt;
  uint16_t off, len;
  if (!read_slot(p, rid.page, rid.slot, n, off, len)) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(p + off), len);
}

void Engine::scan(const std::function<void(Rid, std::string_view)>& fn) {
  uint8_t copy[kPageSize];
  for (PageId pid = 1; pid < page_count_; ++pid) {
    std::memcpy(copy, pool_->fetch(pid), kPageSize);  // fn may call back into the engine
    check_heap(copy, pid);
    uint16_t n = checked_slot_count(copy, pid);
    for (uint16_t i = 0; i < n; ++i) {
      uint16_t off, len;
      if (!read_slot(copy, pid, i, n, off, len)) continue;
      fn(Rid{pid, i}, std::string_view(reinterpret_cast<const char*>(copy + off), len));
    }
  }
}

void Engine::commit() {
  require_txn();
  auto pages = pool_->txn_pages();
  std::sort(pages.begin(), pages.end());
  if (!pages.empty() && opt_.bug != Bug::NoWal) {
    std::vector<std::pair<PageId, const uint8_t*>> images;
    for (PageId pid : pages) {
      uint8_t* d = pool_->frame_data(pid);
      seal_page(d);
      images.emplace_back(pid, d);
    }
    wal_->append_commit(next_txn_++, images);
  }
  pool_->on_commit();
  in_txn_ = false;
  wal_bytes_since_ckpt_ += pages.size() * (Wal::kHeaderSize + kPageSize) + Wal::kHeaderSize;
  if (wal_bytes_since_ckpt_ > opt_.checkpoint_wal_bytes) checkpoint();
}

void Engine::abort() {
  require_txn();
  std::vector<PageId> touched;
  if (opt_.bug == Bug::Steal) touched = pool_->txn_pages();
  pool_->on_abort();
  // Steal control: undo the stolen writes of pre-existing pages on disk too, so the variant stays
  // consistent while the process lives and only a crash (no undo log) exposes uncommitted data.
  for (PageId pid : touched)
    if (pid < page_count_at_begin_) steal_write(pid, pool_->frame_data(pid));
  // Pages this txn allocated were also stolen to disk; drop them, otherwise recovery would count
  // them from the file size and an aborted row would reappear even without a crash.
  if (opt_.bug == Bug::Steal) disk_->shrink_to(page_count_at_begin_);
  page_count_ = page_count_at_begin_;
  in_txn_ = false;
}

void Engine::checkpoint() {
  if (in_txn_) throw DbError("checkpoint during an active transaction");
  pool_->flush_all();
  sync_data();
  wal_->truncate();
  wal_bytes_since_ckpt_ = 0;
}

}  // namespace rowdb
