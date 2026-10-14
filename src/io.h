#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "kv/status.h"

namespace kv {

// Builds an IOError from `context` and the current errno.
Status IOErrorFromErrno(const std::string& context);

// Flushes file data to stable storage. With `full` set, also forces the drive's
// write cache on macOS (F_FULLFSYNC), where plain fsync() does not; that is
// several milliseconds per call, so callers opt in. On other platforms fsync()
// already gives the full guarantee and `full` has no effect.
Status SyncFd(int fd, bool full);

// fsyncs a directory so a create/rename inside it is durable.
Status SyncDir(const std::string& dir);

Status WriteFully(int fd, const char* data, size_t n);
Status PReadFully(int fd, char* buf, size_t n, uint64_t offset);

Status ReadFileToString(const std::string& path, std::string* out);

// Writes `data` to `path` via temp file + fsync + rename, so readers observe
// either the old or the new contents, never a partial file.
Status WriteFileAtomic(const std::string& path, const std::string& data, bool full_sync = false);

void RemoveFileQuietly(const std::string& path);

}  // namespace kv
