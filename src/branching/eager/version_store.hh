#pragma once

#include "branching/base_version_store.hh"

namespace gitmem {

namespace branching {

class EagerLocalVersionStore : public LocalVersionStore {
public:
  ~EagerLocalVersionStore() = default;

  EagerLocalVersionStore(ThreadID tid, bool verbose) : LocalVersionStore(tid, verbose) {}

  std::optional<Conflict> merge_with_commit(const std::shared_ptr<const Commit>&) override;
  BranchingReadResult get_committed(std::string var) const override;

};

}

}