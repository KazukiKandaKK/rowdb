#include "disk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rowdb {

static std::string errno_msg(const std::string& what) { return what + ": " + std::strerror(errno); }

uint32_t crc32(const void* data, size_t n, uint32_t seed) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = seed ^ 0xFFFFFFFFu;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

DiskManager::DiskManager(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) throw DbError(errno_msg("open " + path));
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) ::close(fd_);
}

void DiskManager::read_page(PageId pid, uint8_t* out) const {
  off_t off = static_cast<off_t>(pid) * kPageSize;
  size_t done = 0;
  while (done < kPageSize) {
    ssize_t r = ::pread(fd_, out + done, kPageSize - done, off + static_cast<off_t>(done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw DbError(errno_msg("pread " + path_));
    }
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
  if (done < kPageSize) std::memset(out + done, 0, kPageSize - done);
}

void DiskManager::write_page(PageId pid, const uint8_t* data) {
  off_t off = static_cast<off_t>(pid) * kPageSize;
  size_t done = 0;
  while (done < kPageSize) {
    ssize_t w = ::pwrite(fd_, data + done, kPageSize - done, off + static_cast<off_t>(done));
    if (w < 0) {
      if (errno == EINTR) continue;
      throw DbError(errno_msg("pwrite " + path_));
    }
    done += static_cast<size_t>(w);
  }
}

void DiskManager::sync() {
  if (::fsync(fd_) != 0) throw DbError(errno_msg("fsync " + path_));
}

PageId DiskManager::page_count() const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw DbError(errno_msg("fstat " + path_));
  return static_cast<PageId>((st.st_size + kPageSize - 1) / kPageSize);
}

void DiskManager::shrink_to(PageId pages) {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw DbError(errno_msg("fstat " + path_));
  const off_t want = static_cast<off_t>(pages) * static_cast<off_t>(kPageSize);
  if (st.st_size > want && ::ftruncate(fd_, want) != 0) throw DbError(errno_msg("ftruncate " + path_));
}

}  // namespace rowdb
