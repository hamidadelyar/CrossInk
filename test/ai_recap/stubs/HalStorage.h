#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>

struct MemoryFs {
  std::map<std::string, std::string> files;
  std::map<std::string, unsigned> readers;
  std::string failWrite;
  std::string failRead;
  std::string failRename;
  std::string failSync;
};

class FsFile {
 public:
  FsFile() = default;
  FsFile(MemoryFs& fs, std::string path, const bool writable) : fs(&fs), path(std::move(path)), writable(writable) {
    if (!writable) ++fs.readers[this->path];
  }
  ~FsFile() { close(); }
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;
  FsFile(FsFile&& other) noexcept { *this = std::move(other); }
  FsFile& operator=(FsFile&& other) noexcept {
    close();
    fs = std::exchange(other.fs, nullptr);
    path = std::move(other.path);
    writable = other.writable;
    cursor = other.cursor;
    return *this;
  }

  int read(void* output, const size_t length) {
    if (!fs || writable || fs->failRead == path) return -1;
    const std::string& contents = fs->files[path];
    const size_t count = std::min(length, contents.size() - std::min(contents.size(), cursor));
    std::memcpy(output, contents.data() + cursor, count);
    cursor += count;
    return static_cast<int>(count);
  }
  size_t write(const void* input, const size_t length) {
    if (!fs || !writable || fs->failWrite == path) return 0;
    std::string& contents = fs->files[path];
    if (cursor + length > contents.size()) contents.resize(cursor + length);
    std::memcpy(contents.data() + cursor, input, length);
    cursor += length;
    return length;
  }
  bool sync() { return fs && fs->failSync != path; }
  bool close() {
    if (fs && !writable) --fs->readers[path];
    fs = nullptr;
    return true;
  }
  size_t size() const { return fs ? fs->files[path].size() : 0; }
  size_t position() const { return cursor; }
  bool seek(const size_t position) {
    if (!fs || position > size()) return false;
    cursor = position;
    return true;
  }

 private:
  MemoryFs* fs = nullptr;
  std::string path;
  bool writable = false;
  size_t cursor = 0;
};

class HalStorage {
 public:
  MemoryFs fs;
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) const { return fs.files.contains(path); }
  bool remove(const char* path) { return fs.files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (fs.failRename == from || !exists(from) || exists(to)) return false;
    fs.files[to] = std::move(fs.files[from]);
    fs.files.erase(from);
    return true;
  }
  bool openFileForRead(const char*, const char* path, FsFile& file) {
    if (!exists(path) || fs.readers[path] != 0) return false;
    file = FsFile(fs, path, false);
    return true;
  }
  bool openFileForWrite(const char*, const char* path, FsFile& file) {
    if (fs.readers[path] != 0) return false;
    fs.files[path].clear();
    file = FsFile(fs, path, true);
    return true;
  }
};

inline HalStorage Storage;
using HalFile = FsFile;
