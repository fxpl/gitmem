#pragma once

#include "conflict.hh"
#include "sync_state.hh"
#include "execution_state.hh"
#include "read_result.hh"
#include <memory>
#include <optional>

namespace gitmem {

using MemoryModelFactory = std::function<std::unique_ptr<MemoryModel>()>;

struct Event;  // Forward declaration

// Forward declaration for builder
class MemoryModelBuilder;

// Types of synchronization operations that may be scheduling points
enum class SyncOperation {
  Spawn,
  Join,
  Start,
  End,
  Lock,
  Unlock,
  VolatileRead,
  VolatileWrite
};

class MemoryModel {
public:
  virtual ~MemoryModel() = default;

  virtual std::unique_ptr<ThreadSyncState> make_thread_state(ThreadID tid) const = 0;
  virtual std::unique_ptr<LockSyncState> make_lock_state() const = 0;
  virtual std::unique_ptr<VolatileSyncState> make_volatile_state() const = 0;

  // Read a shared variable into the thread context
  virtual ReadResult read(ThreadContext &ctx, const std::string &var) = 0;

  // Write a shared variable (staged, not committed)
  virtual void write(ThreadContext &ctx, const std::string &var,
                     ValueWithSource value) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_spawn(ThreadContext &parent, ThreadContext &child) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_join(ThreadContext &joiner, ThreadContext &joinee) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_start(ThreadContext &thread) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_end(ThreadContext &thread) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_lock(ThreadContext &thread, Lock &lock) = 0;

  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_unlock(ThreadContext &thread, Lock &lock) = 0;

  // A volatile read is an acquire: it synchronises-with the previous release
  // (write) of `v`. Like on_lock it reports only a conflict (from racing on the
  // piggybacked non-volatile state); the value observed is obtained separately
  // via `read`, whose source_event identifies the writer synchronized with.
  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_volatile_read(ThreadContext &thread, Volatile &v) = 0;

  // A volatile write is a release: it publishes `value` for `v`. It may itself
  // conflict (release ordering) but never on the volatile's own value --
  // volatiles are race-free by construction. The ValueWithSource carries the
  // write event as provenance so a later volatile read can follow it as the
  // acquire->release edge (not for conflict blame).
  virtual std::optional<std::shared_ptr<ConflictBase>>
  on_volatile_write(ThreadContext &thread, Volatile &v,
                    ValueWithSource value) = 0;

  // Returns true if the given sync operation is a scheduling point for this protocol
  // (i.e., the scheduler should consider switching threads here)
  virtual bool is_scheduling_point(SyncOperation op) const = 0;

  // Returns true if all unlock events share a single global g-lane (linear
  // memory model), so that a lock on any variable follows the most recent
  // unlock on ANY variable rather than just the same one.
  virtual bool uses_global_lock_ordering() const { return false; }

  virtual std::string build_revision_graph_dot(const std::vector<const ThreadSyncState*>& thread_states) const = 0;

  virtual std::ostream &print(std::ostream &os) const = 0;
  friend std::ostream &operator<<(std::ostream &os,
                                  const MemoryModel &model) {
    return model.print(os);
  }
};

} // namespace gitmem
