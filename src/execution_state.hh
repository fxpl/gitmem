#pragma once

#include <optional>
#include <trieste/trieste.h>
#include <unordered_map>
#include <vector>

#include "lang.hh"
#include "sync_state.hh"
#include "graphviz.hh"
#include "termination_status.hh"
#include "thread_trace.hh"
#include "thread_id.hh"

namespace gitmem {

class MemoryModel;

struct ThreadContext {
  std::unordered_map<std::string, size_t> locals;

  std::unique_ptr<ThreadSyncState> sync;

  ThreadContext(const ThreadContext&) = delete;
  ThreadContext& operator=(const ThreadContext&) = delete;

  ThreadContext(ThreadContext&&) = default;
  ThreadContext& operator=(ThreadContext&&) = default;

  ThreadContext(ThreadID tid, std::unique_ptr<MemoryModel>&);

  bool operator==(const ThreadContext &other) const;

  friend std::ostream& operator<<(std::ostream&, const ThreadContext&);
};

using termination::TerminationStatus;

struct Thread {
  ThreadID tid;
  ThreadContext ctx;
  ThreadTrace trace;
  trieste::Node block;
  size_t pc = 0;
  std::optional<TerminationStatus> terminated = std::nullopt;

  Thread(ThreadID tid, ThreadContext ctx, trieste::Node block):
    tid(tid), ctx(std::move(ctx)), trace(tid), block(block) {};

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  Thread(Thread&&) = default;
  Thread& operator=(Thread&&) = default;

  bool operator==(const Thread &other) const;

  friend std::ostream& operator<<(std::ostream&, const Thread&);
};

struct Lock {
  std::optional<ThreadID> owner = std::nullopt;
  std::shared_ptr<Event> last_unlock_event = nullptr;
  std::unique_ptr<LockSyncState> sync;
};

// A volatile variable is a synchronising object like a lock: an acquire (read)
// synchronises-with the previous release (write). Its value lives here on the
// object (not in the versioned store) -- volatiles are race-free, so their
// value is not versioned heap, the same way lock state is not versioned. The
// synchronisation of *ordinary* memory still flows through the model; per-model
// bookkeeping (e.g. the branching release commit) lives in `sync`.
struct Volatile {
  std::string name;
  std::optional<ValueWithSource> value = std::nullopt;
  std::unique_ptr<VolatileSyncState> sync;
};

struct GlobalContext {
  // Execution state
  std::deque<Thread> threads;
private:
  std::unordered_map<std::string, Lock> locks;
  std::unordered_map<std::string, Volatile> volatiles;
public:
  Lock& get_lock(std::string);
  Volatile& get_volatile(std::string);

  // Most recent unlock event across ALL lock variables (used by the linear
  // memory model so that lock(l2) gets an ordered_after edge to the last
  // unlock(l1) that pushed to g, even though l2 was never unlocked before).
  std::shared_ptr<Event> last_g_push_event = nullptr;

  // AST evaluation cache
  lang::NodeMap<size_t> cache;

  // Graph root
  // std::shared_ptr<graph::Node> entry_node;

  // Synchronisation semantics (policy)
  std::unique_ptr<MemoryModel> model;

  GlobalContext(trieste::Node starting_block,
                std::unique_ptr<MemoryModel> model);
  ~GlobalContext();

  GlobalContext clone() const;

  GlobalContext(GlobalContext&&) = default;
  GlobalContext& operator=(GlobalContext&&) = default;

  GlobalContext(const GlobalContext&) = delete;
  GlobalContext& operator=(const GlobalContext&) = delete;

  bool operator==(const GlobalContext &other) const;

  void print(std::ostream& os, bool show_all = false) const;
  friend std::ostream& operator<<(std::ostream&, const GlobalContext&);
};

} // namespace gitmem