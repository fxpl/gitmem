#pragma once

#include "branching/base_memory_model.hh"
#include "branching/lazy/version_store.hh"

namespace gitmem {

namespace branching {

class BranchingLazyMemoryModel final : public BranchingMemoryModelBase {
private:
  bool raise_early_conflicts; // currently not used

public:
  explicit BranchingLazyMemoryModel(bool verbose = false, bool raise_early_conflicts = false)
      : BranchingMemoryModelBase(verbose), raise_early_conflicts(raise_early_conflicts) {}

  ~BranchingLazyMemoryModel() = default;

  std::unique_ptr<ThreadSyncState> make_thread_state(ThreadID tid) const override {
    return std::make_unique<LazyLocalVersionStore>(tid, verbose_commits, raise_early_conflicts);
  }
};

}

}