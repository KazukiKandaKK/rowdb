#include "wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rowdb {

static std::string errno_msg(const std::string& what) { return what + ": " + std::strerror(errno); }

Wal::Wal(const std::string& path, bool do_fsync) : path_(path), do_fsync_(do_fsync) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) throw DbError(errno_msg("open " + path));
}

Wal::~Wal() {
  if (fd_ >= 0) ::close(fd_);
}

static void put_record(std::vector<uint8_t>& buf, Wal::Type type, TxnId txn, PageId pid, const uint8_t* payload,
                       uint32_t len) {
  size_t start = buf.size();
  buf.resize(start + Wal::kHeaderSize + len, 0);
  uint8_t* h = buf.data() + start;
  h[4] = type;
  store_u64(h + 8, txn);
  store_u32(h + 16, pid);
  store_u32(h + 20, len);
  if (len) std::memcpy(h + Wal::kHeaderSize, payload, len);
  store_u32(h, crc32(h + 4, Wal::kHeaderSize - 4 + len));
}

void Wal::append_commit(TxnId txn, const std::vector<std::pair<PageId, const uint8_t*>>& pages) {
  std::vector<uint8_t> buf;
  buf.reserve(pages.size() * (kHeaderSize + kPageSize) + kHeaderSize);
  for (const auto& [pid, data] : pages) put_record(buf, kPage, txn, pid, data, kPageSize);
  put_record(buf, kCommit, txn, kInvalidPage, nullptr, 0);
  size_t done = 0;
  while (done < buf.size()) {
    ssize_t w = ::write(fd_, buf.data() + done, buf.size() - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw DbError(errno_msg("write " + path_));
    }
    done += static_cast<size_t>(w);
  }
  if (do_fsync_ && ::fsync(fd_) != 0) throw DbError(errno_msg("fsync " + path_));
}

std::vector<Wal::Record> Wal::read_all(uint64_t* tail_bytes) const {
  std::vector<uint8_t> data;
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw DbError(errno_msg("fstat " + path_));
  data.resize(static_cast<size_t>(st.st_size));
  size_t done = 0;
  while (done < data.size()) {
    ssize_t r = ::pread(fd_, data.data() + done, data.size() - done, static_cast<off_t>(done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw DbError(errno_msg("pread " + path_));
    }
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
  data.resize(done);

  std::vector<Record> out;
  size_t pos = 0;
  while (pos + kHeaderSize <= data.size()) {
    const uint8_t* h = data.data() + pos;
    uint32_t len = load_u32(h + 20);
    uint8_t type = h[4];
    if ((type != kPage && type != kCommit) || (type == kPage && len != kPageSize) || (type == kCommit && len != 0))
      break;
    if (pos + kHeaderSize + len > data.size()) break;  // truncated tail
    if (load_u32(h) != crc32(h + 4, kHeaderSize - 4 + len)) break;  // torn or garbage tail
    Record rec;
    rec.type = static_cast<Type>(type);
    rec.txn = load_u64(h + 8);
    rec.page_id = load_u32(h + 16);
    rec.payload.assign(h + kHeaderSize, h + kHeaderSize + len);
    out.push_back(std::move(rec));
    pos += kHeaderSize + len;
  }
  if (tail_bytes) *tail_bytes = data.size() - pos;
  return out;
}

void Wal::truncate() {
  if (::ftruncate(fd_, 0) != 0) throw DbError(errno_msg("ftruncate " + path_));
  if (do_fsync_ && ::fsync(fd_) != 0) throw DbError(errno_msg("fsync " + path_));
}

uint64_t Wal::size_bytes() const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw DbError(errno_msg("fstat " + path_));
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace rowdb
