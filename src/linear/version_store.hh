#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include "sync_state.hh"
#include "thread_id.hh"
#include "read_result.hh"

namespace gitmem {

struct Event;  // Forward declaration

namespace linear {

// -----------------------------
// Timestamp
// -----------------------------

struct Timestamp {
  size_t thread{0};
  uint64_t counter{0};

  auto operator<=>(const Timestamp &) const = default;

  friend std::ostream &operator<<(std::ostream &os, const Timestamp &ts) {
    os << "t" << ts.thread << ":" << ts.counter;
    return os;
  }
};

using Value = size_t;

// -----------------------------
// Version
// -----------------------------

class Version {
  Timestamp _timestamp;
  ValueWithSource _value;

public:
  Version(Timestamp ts, ValueWithSource value)
    : _timestamp(ts), _value(value) {}

  Timestamp timestamp() const { return _timestamp; }
  ValueWithSource value() const { return _value; }
};

using VersionHistory = std::vector<Version>;

using Conflict = gitmem::Conflict<Timestamp>;

// -----------------------------
// LocalVersionStore
// -----------------------------

class LocalVersionStore : public ThreadSyncState {
  ThreadID tid;
  uint64_t _timestamp;
  std::unordered_map<std::string, ValueWithSource> _staging;

public:
  ~LocalVersionStore() = default;

  LocalVersionStore(ThreadID tid) : tid(tid), _timestamp(0) {}

  ThreadID thread() const { return tid; }

  uint64_t timestamp() const { return _timestamp; }
  const auto &staged_changes() const { return _staging; }

  void stage(std::string obj, ValueWithSource value);
  void clear_staging();
  void advance_base(uint64_t ts);
  std::optional<ValueWithSource> get_staged(std::string obj);

  bool operator==(const LocalVersionStore& other) const;

  bool operator==(const ThreadSyncState& other) const override {
    auto* o = dynamic_cast<const LocalVersionStore*>(&other);
    if (!o)
      return false;
    return *this == *o;
  }

  friend std::ostream& operator<<(std::ostream&, const LocalVersionStore&);

  std::ostream &print(std::ostream &os) const override {
    os << *dynamic_cast<const LocalVersionStore*>(this);
    return os;
  }
};

// -----------------------------
// GlobalVersionStore
// -----------------------------

class GlobalVersionStore {
  uint64_t _counter{0};
  std::unordered_map<std::string, VersionHistory> _history;

public:
  uint64_t current_counter() const { return _counter; }

  std::optional<ValueWithSource> get_version_for_timestamp(std::string, uint64_t) const;

  std::optional<Conflict>
  check_conflicts(Timestamp current_timestamp,
                  const std::unordered_map<std::string, ValueWithSource> &changes) const;

  uint64_t
  apply_changes(ThreadID tid, uint64_t current_counter,
                const std::unordered_map<std::string, ValueWithSource> &changes);

  friend std::ostream& operator<<(std::ostream&, const GlobalVersionStore&);

  std::unordered_map<std::string, VersionHistory> get_history() const {
    return _history;
  }
};

} // namespace linear

} // namespace gitmem