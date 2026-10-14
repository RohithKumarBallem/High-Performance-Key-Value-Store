#include "io.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace kv {

Status IOErrorFromErrno(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

Status SyncFd(int fd, bool full) {
#ifdef __APPLE__
  if (full && ::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
  // Some file systems don't support F_FULLFSYNC; fall back to fsync.
#else
  (void)full;
#endif
  if (::fsync(fd) == 0) return Status::OK();
  return IOErrorFromErrno("fsync");
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return IOErrorFromErrno("open dir " + dir);
  Status s;
  if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
    s = IOErrorFromErrno("fsync dir " + dir);
  }
  ::close(fd);
  return s;
}

Status WriteFully(int fd, const char* data, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return IOErrorFromErrno("write");
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status PReadFully(int fd, char* buf, size_t n, uint64_t offset) {
  while (n > 0) {
    ssize_t r = ::pread(fd, buf, n, static_cast<off_t>(offset));
    if (r < 0) {
      if (errno == EINTR) continue;
      return IOErrorFromErrno("pread");
    }
    if (r == 0) return Status::Corruption("unexpected end of file");
    buf += r;
    n -= static_cast<size_t>(r);
    offset += static_cast<uint64_t>(r);
  }
  return Status::OK();
}

Status ReadFileToString(const std::string& path, std::string* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  out->clear();
  char buf[64 * 1024];
  while (true) {
    ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      Status s = IOErrorFromErrno("read " + path);
      ::close(fd);
      return s;
    }
    if (r == 0) break;
    out->append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return Status::OK();
}

Status WriteFileAtomic(const std::string& path, const std::string& data, bool full_sync) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return IOErrorFromErrno("open " + tmp);
  Status s = WriteFully(fd, data.data(), data.size());
  if (s.ok()) s = SyncFd(fd, full_sync);
  ::close(fd);
  if (s.ok() && ::rename(tmp.c_str(), path.c_str()) != 0) {
    s = IOErrorFromErrno("rename " + tmp);
  }
  if (!s.ok()) {
    ::unlink(tmp.c_str());
    return s;
  }
  return SyncDir(std::filesystem::path(path).parent_path().string());
}

void RemoveFileQuietly(const std::string& path) { ::unlink(path.c_str()); }

}  // namespace kv
