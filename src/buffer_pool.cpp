#include "buffer_pool.h"

#include <cstring>
#include <string>

namespace rowdb {

BufferPool::BufferPool(DiskManager& disk, size_t frames) : disk_(disk), frames_(frames) {
  for (auto& f : frames_) f.data = std::make_unique<uint8_t[]>(kPageSize);
}

size_t BufferPool::pick_victim() {
  size_t best = frames_.size();
  for (size_t i = 0; i < frames_.size(); ++i) {
    const Frame& f = frames_[i];
    if (f.pid == kInvalidPage) return i;
    if (f.txn_dirty) continue;
    if (best == frames_.size() || f.last_use < frames_[best].last_use) best = i;
  }
  if (best == frames_.size()) throw TxnTooLarge();
  return best;
}

void BufferPool::write_frame(Frame& f) {
  seal_page(f.data.get());
  disk_.write_page(f.pid, f.data.get());
  f.dirty = false;
}

BufferPool::Frame& BufferPool::get_frame(PageId pid, bool load) {
  auto it = table_.find(pid);
  if (it != table_.end()) {
    Frame& f = frames_[it->second];
    f.last_use = ++clock_;
    return f;
  }
  size_t idx = pick_victim();
  Frame& f = frames_[idx];
  if (f.pid != kInvalidPage) {
    if (f.dirty) write_frame(f);
    table_.erase(f.pid);
  }
  f.pid = pid;
  f.dirty = f.txn_dirty = f.created_in_txn = false;
  f.before.reset();
  if (load) {
    disk_.read_page(pid, f.data.get());
    if (!page_crc_ok(f.data.get())) {
      bool zero = page_all_zero(f.data.get());
      f.pid = kInvalidPage;
      throw DbError("page " + std::to_string(pid) + (zero ? " was never written (all zero)" : " failed its checksum"));
    }
  } else {
    std::memset(f.data.get(), 0, kPageSize);
  }
  table_[pid] = idx;
  f.last_use = ++clock_;
  return f;
}

const uint8_t* BufferPool::fetch(PageId pid) { return get_frame(pid, true).data.get(); }

uint8_t* BufferPool::fetch_for_write(PageId pid) {
  Frame& f = get_frame(pid, true);
  if (!f.txn_dirty) {
    f.before = std::make_unique<uint8_t[]>(kPageSize);
    std::memcpy(f.before.get(), f.data.get(), kPageSize);
    f.txn_dirty = true;
  }
  return f.data.get();
}

uint8_t* BufferPool::new_page(PageId pid) {
  if (table_.count(pid)) throw DbError("new_page: page " + std::to_string(pid) + " already resident");
  Frame& f = get_frame(pid, false);
  f.txn_dirty = true;
  f.created_in_txn = true;
  return f.data.get();
}

std::vector<PageId> BufferPool::txn_pages() const {
  std::vector<PageId> out;
  for (const auto& f : frames_)
    if (f.pid != kInvalidPage && f.txn_dirty) out.push_back(f.pid);
  return out;
}

uint8_t* BufferPool::frame_data(PageId pid) {
  auto it = table_.find(pid);
  if (it == table_.end()) throw DbError("frame_data: page " + std::to_string(pid) + " not resident");
  return frames_[it->second].data.get();
}

void BufferPool::on_commit() {
  for (auto& f : frames_) {
    if (f.pid == kInvalidPage || !f.txn_dirty) continue;
    f.txn_dirty = false;
    f.created_in_txn = false;
    f.dirty = true;
    f.before.reset();
  }
}

void BufferPool::on_abort() {
  for (auto& f : frames_) {
    if (f.pid == kInvalidPage || !f.txn_dirty) continue;
    if (f.created_in_txn) {
      table_.erase(f.pid);
      f.pid = kInvalidPage;
      f.dirty = false;
    } else {
      std::memcpy(f.data.get(), f.before.get(), kPageSize);
    }
    f.txn_dirty = false;
    f.created_in_txn = false;
    f.before.reset();
  }
}

void BufferPool::flush_all() {
  for (auto& f : frames_) {
    if (f.pid == kInvalidPage) continue;
    if (f.txn_dirty) throw DbError("flush_all during an active transaction");
    if (f.dirty) write_frame(f);
  }
}

void BufferPool::drop_all() {
  for (auto& f : frames_) {
    f.pid = kInvalidPage;
    f.dirty = f.txn_dirty = f.created_in_txn = false;
    f.before.reset();
  }
  table_.clear();
}

}  // namespace rowdb
