#pragma once

#include <string>

#include "common.h"

namespace rowdb {

// Thin wrapper around the data file. Page i lives at byte offset i * kPageSize.
class DiskManager {
 public:
  explicit DiskManager(const std::string& path);
  ~DiskManager();
  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  void read_page(PageId pid, uint8_t* out) const;  // short reads are zero-filled
  void write_page(PageId pid, const uint8_t* data);
  void sync();
  PageId page_count() const;  // file size / kPageSize (rounded up)
  void shrink_to(PageId pages);  // truncate the file to `pages` pages if it is longer (never extends)

 private:
  int fd_ = -1;
  std::string path_;
};

}  // namespace rowdb
