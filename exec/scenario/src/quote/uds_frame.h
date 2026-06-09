#ifndef KAIROS_EXEC_UDS_FRAME_H_
#define KAIROS_EXEC_UDS_FRAME_H_

// Length-prefixed framing matching core/src/uds/frame.rs: u32 little-endian
// length + payload (max 1 MiB).

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

namespace kairos::exec {

constexpr std::uint32_t kMaxFrameLen = 1u << 20;

// MSG_NOSIGNAL: a peer that closed mid-write returns EPIPE instead of killing us
// with SIGPIPE (a scenario disconnecting must never take down the hub).
inline bool WriteAll(int fd, const std::uint8_t* data, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    ssize_t w = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
    if (w <= 0) return false;
    off += static_cast<std::size_t>(w);
  }
  return true;
}

// Reads exactly n bytes. Returns 1 on success, 0 on clean EOF, -1 on error.
inline int ReadAll(int fd, std::uint8_t* data, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, data + off, n - off);
    if (r == 0) return 0;
    if (r < 0) return -1;
    off += static_cast<std::size_t>(r);
  }
  return 1;
}

inline bool WriteFrame(int fd, const std::vector<std::uint8_t>& payload) {
  std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  std::uint8_t hdr[4] = {
      static_cast<std::uint8_t>(len & 0xff), static_cast<std::uint8_t>((len >> 8) & 0xff),
      static_cast<std::uint8_t>((len >> 16) & 0xff), static_cast<std::uint8_t>((len >> 24) & 0xff)};
  return WriteAll(fd, hdr, 4) && (payload.empty() || WriteAll(fd, payload.data(), payload.size()));
}

// Reads one frame into *out. Returns 1 on success, 0 on clean EOF, -1 on error.
inline int ReadFrame(int fd, std::vector<std::uint8_t>* out) {
  std::uint8_t hdr[4];
  int r = ReadAll(fd, hdr, 4);
  if (r <= 0) return r;
  std::uint32_t len =
      static_cast<std::uint32_t>(hdr[0]) | (static_cast<std::uint32_t>(hdr[1]) << 8) |
      (static_cast<std::uint32_t>(hdr[2]) << 16) | (static_cast<std::uint32_t>(hdr[3]) << 24);
  if (len > kMaxFrameLen) return -1;
  out->resize(len);
  if (len == 0) return 1;
  return ReadAll(fd, out->data(), len);
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_UDS_FRAME_H_
