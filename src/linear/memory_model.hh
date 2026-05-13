#pragma once

#include "../memory_model.hh"
#include "conflict.hh"
#include "execution_state.hh"
#include "linear/version_store.hh"

namespace gitmem {

using LinearConflict = Conflict<linear::Timestamp>;

namespace linear {

class LinearMemoryModel final : public MemoryModel {
  GlobalVersionStore _global_store;

  std::optional<LinearConflict> push(LocalVersionStore &local);
  std::optional<LinearConflict> pull(LocalVersionStore &local);

public:
  ~LinearMemoryModel() override;

  ReadResult read(ThreadContext &ctx, const std::string &var) override;

  void write(ThreadContext &ctx, const std::string &var, ValueWithSource value) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_spawn(ThreadContext &parent, ThreadContext &child) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_join(ThreadContext &joiner, ThreadContext &joinee) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_start(ThreadContext &thread) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_end(ThreadContext &thread) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_lock(ThreadContext &thread, Lock &lock) override;

  std::optional<std::shared_ptr<ConflictBase>>
  on_unlock(ThreadContext &thread, Lock &lock) override;

  std::ostream &print(std::ostream &os) const override;

  std::string build_revision_graph_dot(const std::vector<const ThreadSyncState*>& thread_states) const override;

  bool is_scheduling_point(SyncOperation op) const override;

  std::unique_ptr<ThreadSyncState> make_thread_state(ThreadID tid) const override {
    return std::make_unique<LocalVersionStore>(tid);
  }

  std::unique_ptr<LockSyncState> make_lock_state() const override {
    return nullptr;
  }
};

} // namespace linear

} // namespace gitmem