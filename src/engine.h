#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "buffer_pool.h"
#include "common.h"
#include "disk.h"
#include "wal.h"

namespace rowdb {

struct Rid {
  PageId page = kInvalidPage;
  uint16_t slot = 0;
  bool operator==(const Rid& o) const { return page == o.page && slot == o.slot; }
};

struct Options {
  Bug bug = Bug::None;
  size_t pool_frames = 32;
  uint64_t checkpoint_wal_bytes = 512 * 1024;
};

struct RecoveryStats {
  size_t wal_records = 0;
  size_t committed_txns = 0;
  size_t pages_redone = 0;
  size_t discarded_records = 0;  // records of transactions without COMMIT
  uint64_t wal_tail_bytes = 0;   // bytes after the last valid record (torn/garbage tail)
};

// Heap table on slotted pages, one transaction at a time.
//
// Data file: page 0 is a meta page, heap pages start at 1.
// Heap page layout (after the 4-byte CRC):
//   [4,6)  u16 slot_count
//   [6,8)  u16 free_end   (record bytes live in [free_end, kPageSize))
//   [8,12) u32 magic 'HEAP'
//   [12, 12 + 4*slot_count) slots: u16 offset, u16 length; offset 0 = deleted
class Engine {
 public:
  static constexpr size_t kMaxRecord = 1024;

  Engine(const std::string& dir, Options opt);
  ~Engine();

  void begin();
  Rid insert(std::string_view value);
  bool remove(Rid rid);
  std::optional<std::string> get(Rid rid);
  void scan(const std::function<void(Rid, std::string_view)>& fn);
  void commit();
  void abort();

  void checkpoint();
  const RecoveryStats& recovery_stats() const { return rstats_; }
  bool in_txn() const { return in_txn_; }

 private:
  void recover();
  void require_txn() const;
  void sync_data();
  void steal_write(PageId pid, uint8_t* page);

  std::string dir_;
  Options opt_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<BufferPool> pool_;
  RecoveryStats rstats_;
  PageId page_count_ = 0;          // pages that exist (on disk or allocated in memory)
  PageId page_count_at_begin_ = 0;
  TxnId next_txn_ = 1;
  uint64_t wal_bytes_since_ckpt_ = 0;  // counted for every variant, so NoWal checkpoints on the same schedule
  bool in_txn_ = false;
};

}  // namespace rowdb
