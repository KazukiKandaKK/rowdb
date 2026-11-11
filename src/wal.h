#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace rowdb {

// Write-ahead log of full page images.
//
// Record layout (little endian):
//   u32 crc     CRC32 of every byte after this field (rest of header + payload)
//   u8  type    1 = PAGE image, 2 = COMMIT
//   u8  pad[3]
//   u64 txn
//   u32 page_id (PAGE only)
//   u32 payload_len (kPageSize for PAGE, 0 for COMMIT)
//   payload
//
// A transaction's PAGE records and its COMMIT record are built in one buffer and
// appended with write() (retried on short writes, so a crash can still leave a torn
// tail), then fsync'ed; the commit is acknowledged only after that fsync.
// Reading stops at the first record that is truncated or fails its CRC, so a torn
// tail drops the whole unfinished transaction (its COMMIT record is last).
class Wal {
 public:
  enum Type : uint8_t { kPage = 1, kCommit = 2 };
  static constexpr size_t kHeaderSize = 24;

  struct Record {
    Type type;
    TxnId txn;
    PageId page_id;
    std::vector<uint8_t> payload;
  };

  explicit Wal(const std::string& path, bool do_fsync = true);
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  // Appends page images followed by a COMMIT record, then fsyncs.
  void append_commit(TxnId txn, const std::vector<std::pair<PageId, const uint8_t*>>& pages);
  // Reads records up to the first truncated/corrupt one. *tail_bytes = bytes after the valid prefix.
  std::vector<Record> read_all(uint64_t* tail_bytes = nullptr) const;
  void truncate();  // empties the log and fsyncs
  uint64_t size_bytes() const;

 private:
  int fd_ = -1;
  std::string path_;
  bool do_fsync_ = true;
};

}  // namespace rowdb
