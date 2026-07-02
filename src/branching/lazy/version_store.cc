#include "branching/lazy/version_store.hh"
#include "debug.hh"
#include "thread_trace.hh"
#include <unordered_set>
#include <functional>

namespace gitmem {

namespace branching {

std::optional<Conflict> LazyLocalVersionStore::merge_with_commit(const std::shared_ptr<const Commit>& commit) {
  assert(staging.empty());

  // No incoming history to merge.
  if (!commit)
    return std::nullopt;

  // trivial case: same history
  if (head == commit)
    return std::nullopt;

  // Create merge commit (no changes itself)
  auto merge_commit = std::make_shared<const Commit>(
    Commit{
      .id = base_timestamp++,
      .parents = {head, commit},
      .changes = {}  // merge commit does not write anything
    }
  );

  // Don't check for conflicts, we do that when later read a variable
  head = merge_commit;

  return std::nullopt;
}

// Thought, if we merge two paths that conflict on a variable, but we never read it
// and just write to it, is that okay?
// if (auto it = last_writer.find(number); it != last_writer.end()) {
//   return it->second->changes.at(number);
// }

BranchingReadResult LazyLocalVersionStore::get_committed(std::string var) const {
  // Invalidate cached reads when history head changes.
  if (cached_head != head) {
    cached_head = head;
    read_cache.clear();
  }

  if (auto it = read_cache.find(var); it != read_cache.end()) {
    return it->second;
  }

  using CommitPtr = std::shared_ptr<const Commit>;
  using ReachKey = std::pair<const Commit*, const Commit*>;
  struct ReachKeyHash {
    size_t operator()(const ReachKey& k) const {
      return std::hash<const Commit*>{}(k.first) ^
             (std::hash<const Commit*>{}(k.second) << 1);
    }
  };

  std::unordered_map<ReachKey, bool, ReachKeyHash> reach_memo;
  std::function<bool(const CommitPtr&, const CommitPtr&)> can_reach_cached;
  can_reach_cached = [&](const CommitPtr& from, const CommitPtr& to) -> bool {
    if (!from || !to) return false;
    if (from == to) return true;

    const ReachKey key{from.get(), to.get()};
    if (auto it = reach_memo.find(key); it != reach_memo.end()) {
      return it->second;
    }

    for (const auto& parent : from->parents) {
      if (can_reach_cached(parent, to)) {
        reach_memo[key] = true;
        return true;
      }
    }

    reach_memo[key] = false;
    return false;
  };

  std::vector<std::shared_ptr<const Commit>> writers;

  std::function<void(std::shared_ptr<const Commit>)> dfs;
  dfs = [&](std::shared_ptr<const Commit> c) {
    if (!c) return;

    // Check if c is an ancestor of any existing writer
    for (const auto& writer : writers) {
      if (can_reach_cached(writer, c)) {
        // c is ancestor of existing writer, ignore this path
        return;
      }
    }

    // Remove any existing writers that are ancestors of c
    writers.erase(
      std::remove_if(writers.begin(), writers.end(),
        [&](const auto& writer) { return can_reach_cached(c, writer); }),
      writers.end()
    );

    if (c->changes.contains(var)) {
      writers.push_back(c);
      return;
    }

    for (auto& p : c->parents)
      dfs(p);
  };

  dfs(head);

  if (writers.empty()) {
    auto result = BranchingReadResult(std::monostate{});
    read_cache[var] = result;
    return result;
  }

  if (writers.size() == 1) {
    auto result = BranchingReadResult(writers[0]->changes.at(var));
    read_cache[var] = result;
    return result;
  }

  // conflict
  auto get_loc = [](const ValueWithSource& vws) -> FileLocation {
    if (!vws.source_event)
      throw std::logic_error("missing source event for conflicting write");
    auto* we = std::get_if<WriteEvent>(&vws.source_event->data);
    if (!we)
      throw std::logic_error("conflicting source event is not a WriteEvent");
    return we->location;
  };
  auto a = writers[0]->id;
  auto b = writers[1]->id;
  auto result = BranchingReadResult(Conflict(
    var,
    {a, get_loc(writers[0]->changes.at(var))},
    {b, get_loc(writers[1]->changes.at(var))},
    writers[0]->changes.at(var).source_event,
    writers[1]->changes.at(var).source_event));
  read_cache[var] = result;
  return result;
}


}

}