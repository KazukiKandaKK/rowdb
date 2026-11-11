#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "disk.h"

namespace rowdb {

// Fixed-size page cache with LRU replacement.
//
// NO-STEAL: a page modified by the active transaction ("txn-dirty") is never
// written to the data file and never evicted until the transaction commits or
// aborts. Committed-but-unflushed pages ("dirty") may be evicted; their images
// are already durable in the WAL.
//
// Pointers returned by fetch*/new_page stay valid only until the next call that
// may evict (any fetch*/new_page). Callers use one page at a time.
class BufferPool {
 public:
  BufferPool(DiskManager& disk, size_t frames);

  const uint8_t* fetch(PageId pid);
  uint8_t* fetch_for_write(PageId pid);  // saves a before-image the first time in a txn
  uint8_t* new_page(PageId pid);         // zero-filled page created by the active txn

  std::vector<PageId> txn_pages() const;  // pages touched by the active txn
  uint8_t* frame_data(PageId pid);        // resident page (must be resident)
  void on_commit();  // txn-dirty -> dirty, drop before-images
  void on_abort();   // restore before-images, drop pages created by the txn
  void flush_all();  // write every dirty page (no active txn allowed)
  void drop_all();   // forget everything without writing (used after recovery rewrites the file)

 private:
  struct Frame {
    PageId pid = kInvalidPage;
    std::unique_ptr<uint8_t[]> data;
    bool dirty = false;
    bool txn_dirty = false;
    bool created_in_txn = false;
    std::unique_ptr<uint8_t[]> before;  // before-image for abort
    uint64_t last_use = 0;
  };

  Frame& get_frame(PageId pid, bool load);
  size_t pick_victim();
  void write_frame(Frame& f);

  DiskManager& disk_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, size_t> table_;
  uint64_t clock_ = 0;
};

}  // namespace rowdb
