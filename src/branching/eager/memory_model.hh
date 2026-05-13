#pragma once

#include "branching/base_memory_model.hh"
#include "branching/eager/version_store.hh"

namespace gitmem {

namespace branching {

class BranchingEagerMemoryModel final : public BranchingMemoryModelBase {
public:
  explicit BranchingEagerMemoryModel(bool verbose = false)
      : BranchingMemoryModelBase(verbose) {}

  ~BranchingEagerMemoryModel() = default;

  std::unique_ptr<ThreadSyncState> make_thread_state(ThreadID tid) const override {
    return std::make_unique<EagerLocalVersionStore>(tid, verbose_commits);
  }
};

}

}