#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sstream>
#include "thread_id.hh"
#include "sync_state.hh"
#include "read_result.hh"

namespace gitmem {

namespace branching {

struct Timestamp {
  ThreadID thread;
  size_t counter;

  auto operator<=>(const Timestamp &) const = default;

  // pre-increment
  Timestamp& operator++() {
    ++counter;
    return *this;
  }

  // post-increment
  Timestamp operator++(int) {
    Timestamp old = *this;
    ++(*this);
    return old;
  }

  friend std::ostream &operator<<(std::ostream &os,
                                  const Timestamp &ts) {
    os << ts.thread << ":" << ts.counter;
    return os;
  }
};

inline std::string to_string(const Timestamp& ts) {
  std::ostringstream ss;
  ss << ts;
  return ss.str();
}

struct Commit {
  Timestamp id;
  std::unordered_map<std::string, ValueWithSource> changes;
  std::vector<std::shared_ptr<const Commit>> parents;
  bool conflicted = false;
};

std::string build_commit_graph_dot(const std::vector<std::shared_ptr<const Commit>>& leaves);

bool can_reach(const std::shared_ptr<const Commit>& commit, const std::shared_ptr<const Commit>& lca, std::unordered_map<std::shared_ptr<const Commit>, bool>& memo);

std::ostream& operator<<(std::ostream& os, const Commit& commit);

using Conflict = gitmem::Conflict<Timestamp>;

using BranchingReadResult = std::variant<std::monostate, ValueWithSource, Conflict>;

class LocalVersionStore : public ThreadSyncState {
protected:
  Timestamp base_timestamp;
  std::shared_ptr<const Commit> head;
  std::unordered_map<std::string, ValueWithSource> staging;

  std::unordered_map<std::string, std::shared_ptr<const Commit>> last_writer; // cached

  bool verbose;

public:
  ~LocalVersionStore() = default;

  LocalVersionStore(ThreadID tid, bool verbose = false): base_timestamp(tid, 0), verbose(verbose) {}

  void stage(std::string obj, ValueWithSource value);
  void commit_staging();

  bool has_commited() { return staging.empty(); }

  std::shared_ptr<const Commit> get_head() const { return head; }

private:
  virtual BranchingReadResult get_committed(std::string var) const = 0;

public:
  BranchingReadResult read(std::string var) const;

  void adopt_history(const LocalVersionStore& other);
  virtual std::optional<Conflict> merge_with_commit(const std::shared_ptr<const Commit>& other_head) = 0;

  friend std::ostream& operator<<(std::ostream& os, const LocalVersionStore& store);
  std::ostream &print(std::ostream &os) const override {
    os << *dynamic_cast<const LocalVersionStore*>(this);
    return os;
  }

  bool operator==(const LocalVersionStore& other) const;
  bool operator==(const ThreadSyncState& other) const override {
    auto* o = dynamic_cast<const LocalVersionStore*>(&other);
    if (!o)
      return false;
    return *this == *o;
  }
};

class GlobalVersionStore {
public:
  friend std::ostream& operator<<(std::ostream&, const GlobalVersionStore&);
};

class LockState : public LockSyncState {
public:
  ~LockState() = default;

  std::shared_ptr<const branching::Commit> commit;

  inline std::ostream &print(std::ostream &os) const override {
    os << "LockState{commit=";
    if (commit)
      os << commit->id;
    else
      os << "empty";
    os << "}";
    return os;
  }
};

} // namespace branching

} // namespace gitmem