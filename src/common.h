#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace rowdb {

constexpr uint32_t kPageSize = 4096;
using PageId = uint32_t;
using TxnId = uint64_t;
constexpr PageId kInvalidPage = UINT32_MAX;

// Page header shared by every page: bytes [0,4) hold a CRC32 of bytes [4, kPageSize).
constexpr size_t kPageCrcOffset = 0;
constexpr size_t kPageBodyOffset = 4;

class DbError : public std::runtime_error {
 public:
  explicit DbError(const std::string& msg) : std::runtime_error(msg) {}
};

// Thrown when a single transaction touches more pages than the buffer pool can hold.
class TxnTooLarge : public DbError {
 public:
  TxnTooLarge() : DbError("transaction touches more pages than the buffer pool holds") {}
};

uint32_t crc32(const void* data, size_t n, uint32_t seed = 0);

inline uint32_t load_u32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
inline uint16_t load_u16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
inline uint64_t load_u64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
inline void store_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }
inline void store_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof v); }
inline void store_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof v); }

// Writes the CRC of the page body into the page header.
inline void seal_page(uint8_t* page) {
  store_u32(page + kPageCrcOffset, crc32(page + kPageBodyOffset, kPageSize - kPageBodyOffset));
}
inline bool page_crc_ok(const uint8_t* page) {
  return load_u32(page + kPageCrcOffset) == crc32(page + kPageBodyOffset, kPageSize - kPageBodyOffset);
}
inline bool page_all_zero(const uint8_t* page) {
  for (uint32_t i = 0; i < kPageSize; ++i)
    if (page[i] != 0) return false;
  return true;
}

// Deliberately broken variants used as negative controls for the crash test.
// NoWal:    commit does not write the WAL.
// SkipRedo: recovery discards the WAL without replaying it.
// Steal:    every modified page is written to the data file immediately, before its txn commits
//           (violates NO-STEAL; recovery has no undo, so uncommitted data can survive a crash).
// NoFsync:  every fsync is skipped. kill -9 keeps the OS page cache, so this variant is expected
//           to PASS the crash test; it documents what the test cannot see.
enum class Bug { None, NoWal, SkipRedo, Steal, NoFsync };

}  // namespace rowdb
