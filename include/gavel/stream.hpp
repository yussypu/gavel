#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gavel/input.hpp"

namespace gavel {

// Input stream file: 8 byte magic then raw little endian InputMsg records.
inline constexpr char kStreamMagic[8] = {'G', 'V', 'L', 'S', 'T', 'R', 'M', '1'};

class StreamWriter {
 public:
  explicit StreamWriter(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
    if (f_) std::fwrite(kStreamMagic, 1, 8, f_);
  }
  ~StreamWriter() { close(); }
  StreamWriter(const StreamWriter&) = delete;
  StreamWriter& operator=(const StreamWriter&) = delete;

  bool ok() const { return f_ != nullptr; }
  void write(const InputMsg& m) {
    std::fwrite(&m, sizeof(m), 1, f_);
    ++count_;
  }
  std::uint64_t count() const { return count_; }
  void close() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
  }

 private:
  std::FILE* f_{nullptr};
  std::uint64_t count_{0};
};

class StreamReader {
 public:
  explicit StreamReader(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
    char magic[8];
    if (f_ && (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, kStreamMagic, 8) != 0)) {
      std::fclose(f_);
      f_ = nullptr;
    }
  }
  ~StreamReader() {
    if (f_) std::fclose(f_);
  }
  StreamReader(const StreamReader&) = delete;
  StreamReader& operator=(const StreamReader&) = delete;

  bool ok() const { return f_ != nullptr; }
  bool read(InputMsg& m) { return std::fread(&m, sizeof(m), 1, f_) == 1; }

 private:
  std::FILE* f_{nullptr};
};

}  // namespace gavel
