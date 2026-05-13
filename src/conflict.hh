#pragma once

#include <iostream>
#include <optional>
#include <string>

namespace gitmem {

struct FileLocation {
  std::string file;
  size_t line = 0;
  size_t column = 0;

  std::string linecol() const {
    return std::to_string(line) + ":" + std::to_string(column);
  }

  std::string str() const {
    if (file.empty()) {
      return linecol();
    } else {
      return file + ":" + linecol();
    }
  }
};

inline std::ostream& operator<<(std::ostream& os, const FileLocation& loc) {
  os << loc.str();
  return os;
}

struct ConflictBase {
  virtual ~ConflictBase() = default;
  virtual std::ostream &print(std::ostream &os) const = 0;
  virtual std::string object_name() const = 0;
  virtual std::pair<FileLocation, FileLocation> source_locations() const = 0;
  friend std::ostream &operator<<(std::ostream &os,
                                  const ConflictBase &conflict) {
    return conflict.print(os);
  }
  virtual bool operator==(const ConflictBase &other) const = 0;
};

template <typename VersionID>
struct Conflict : ConflictBase {
  std::string var;
  std::pair<VersionID, FileLocation> version_a;
  std::pair<VersionID, FileLocation> version_b;

  Conflict(std::string var,
           std::pair<VersionID, FileLocation> version_a,
           std::pair<VersionID, FileLocation> version_b)
      : var(std::move(var)), version_a(std::move(version_a)), version_b(std::move(version_b)) {}

  std::ostream &print(std::ostream &os) const override;

  std::string object_name() const override {
    return var;
  }

  std::pair<FileLocation, FileLocation> source_locations() const override {
    return std::make_pair(version_a.second, version_b.second);
  }

  bool operator==(const Conflict &other) const {
    // Ignore the FileLocation information for equality, as it is only for reporting purposes
    return var == other.var && version_a.first == other.version_a.first && version_b.first == other.version_b.first;
  }

  bool operator==(const ConflictBase& other) const override {
    auto* o = dynamic_cast<const Conflict*>(&other);
    if (!o)
      return false;
    return *this == *o;
  }
};

template <typename T>
std::ostream &Conflict<T>::print(std::ostream &os) const {
  os << "conflict on " << var << " { " << version_a.first << "@" << version_a.second << ", "
     << version_b.first << "@" << version_b.second << " }";
  return os;
}


}