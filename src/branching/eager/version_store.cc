#include "branching/eager/version_store.hh"
#include "debug.hh"
#include "thread_trace.hh"

#include <queue>
#include <unordered_set>

namespace gitmem {

namespace branching {

bool traverse_until_lca(
  const std::shared_ptr<const Commit>& commit,
  const std::shared_ptr<const Commit>& lca,
  std::unordered_map<std::string, std::shared_ptr<const Commit>>& out_map,
  std::unordered_set<std::shared_ptr<const Commit>>& visited,
  std::unordered_map<std::shared_ptr<const Commit>, bool>& reach_memo)
{
  if (!commit || !visited.insert(commit).second)
    return true;

  if (lca && commit == lca)
    return true;

  if (lca && !can_reach(commit, lca, reach_memo))
    return true;

  for (const auto& [obj, _] : commit->changes) {
    // first write seen dominates
    if (out_map.find(obj) == out_map.end())
      out_map[obj] = commit;
  }

  for (auto& parent : commit->parents) {
    if (!traverse_until_lca(parent, lca, out_map, visited, reach_memo))
      return false;
  }

  return true;
}

std::shared_ptr<const Commit>
find_lowest_common_ancestor(std::shared_ptr<const Commit> a,
                            std::shared_ptr<const Commit> b)
{
  if (!a || !b) return nullptr;
  if (a == b) return a;

  // Check if 'a' is an ancestor of 'b'
  {
    std::unordered_set<std::shared_ptr<const Commit>> visited;
    std::queue<std::shared_ptr<const Commit>> q;
    q.push(b);
    while (!q.empty()) {
      auto c = q.front(); q.pop();
      if (!c) continue;
      if (c == a) return a;
      if (!visited.insert(c).second) continue;
      for (auto& p : c->parents)
        q.push(p);
    }
  }

  // Check if 'b' is an ancestor of 'a'
  {
    std::unordered_set<std::shared_ptr<const Commit>> visited;
    std::queue<std::shared_ptr<const Commit>> q;
    q.push(a);
    while (!q.empty()) {
      auto c = q.front(); q.pop();
      if (!c) continue;
      if (c == b) return b;
      if (!visited.insert(c).second) continue;
      for (auto& p : c->parents)
        q.push(p);
    }
  }

  // Step 1: collect all ancestors of 'a'
  std::unordered_set<std::shared_ptr<const Commit>> ancestors_a;
  std::queue<std::shared_ptr<const Commit>> q;
  q.push(a);

  while (!q.empty()) {
    auto c = q.front(); q.pop();
    if (!c) continue;
    if (!ancestors_a.insert(c).second) continue; // already visited

    for (auto& p : c->parents)
      q.push(p);
  }

  // Step 2: BFS from 'b' to find first common ancestor
  std::unordered_set<std::shared_ptr<const Commit>> visited_b;
  q.push(b);

  while (!q.empty()) {
    auto c = q.front(); q.pop();
    if (!c) continue;
    if (!visited_b.insert(c).second) continue;

    if (ancestors_a.count(c))
      return c;  // first common ancestor seen

    for (auto& p : c->parents)
      q.push(p);
  }

  return nullptr;  // disjoint histories (shouldn’t happen)
}

std::optional<Conflict> EagerLocalVersionStore::merge_with_commit(const std::shared_ptr<const Commit>& commit) {
  assert(staging.empty());

  // No incoming history to merge.
  if (!commit)
    return std::nullopt;

  // trivial case: same history
  if (head == commit)
    return std::nullopt;

  // Fast-forward: if the incoming commit is already in our history there is
  // nothing to merge. Building a merge commit here would add a redundant parent
  // edge straight to an old ancestor, which can mislead ancestor queries.
  std::unordered_map<std::shared_ptr<const Commit>, bool> ff_memo;
  if (can_reach(head, commit, ff_memo))
    return std::nullopt;

  // Find lowest common ancestor of the two heads
  std::shared_ptr<const Commit> lca = find_lowest_common_ancestor(head, commit);
  verbose::out << "found lca of "
               << (head ? to_string(head->id) : std::string("<null>"))
               << " and "
               << to_string(commit->id)
               << " to be "
               << (lca ? to_string(lca->id) : std::string("<null>"))
               << std::endl;

  // Collect all writes after LCA for each branch
  std::unordered_map<std::string, std::shared_ptr<const Commit>> branch_a, branch_b;
  std::unordered_set<std::shared_ptr<const Commit>> visited;

  std::unordered_map<std::shared_ptr<const Commit>, bool> reach_memo;
  traverse_until_lca(head, lca, branch_a, visited, reach_memo);
  visited.clear();
  traverse_until_lca(commit, lca, branch_b, visited, reach_memo);

  // 1. Eager conflict detection
  auto get_loc = [](const std::shared_ptr<const Commit>& c, const std::string& obj)
      -> FileLocation {
    auto it = c->changes.find(obj);
    if (it == c->changes.end() || !it->second.source_event)
      throw std::logic_error("missing source event for conflicting write");
    if (auto* we = std::get_if<WriteEvent>(&it->second.source_event->data))
      return we->location;
    // Volatile writes can race (write-write with no happens-before).
    if (auto* vwe = std::get_if<VolatileWriteEvent>(&it->second.source_event->data))
      return vwe->location;
    throw std::logic_error("conflicting source event is not a write");
  };
  std::optional<Conflict> conflict;
  for (const auto& [obj, commit_a] : branch_a) {
    auto it = branch_b.find(obj);
    if (it != branch_b.end() && it->second != commit_a) {
      conflict = Conflict(
        obj,
        {commit_a->id, get_loc(commit_a, obj)},
        {it->second->id, get_loc(it->second, obj)},
        commit_a->changes.at(obj).source_event,
        it->second->changes.at(obj).source_event);
      break;
    }
  }

  // Create merge commit (even if conflicted, for visualization)
  auto merge_commit = std::make_shared<Commit>(
    Commit{
      .id = base_timestamp++,
      .changes = {},  // merge commit does not write anything
      .parents = {head, commit},
      .conflicted = conflict.has_value()
    }
  );

  // If there was a conflict, update head but return the conflict
  if (conflict) {
    head = merge_commit;
    return conflict;
  }

  // 2. Update thread-local last_writer incrementally
  // Only overwrite variables that were touched along either branch after LCA
  for (const auto& [obj, commit] : branch_a)
    last_writer[obj] = commit;

  for (const auto& [obj, commit] : branch_b)
    last_writer[obj] = commit;

  // 3. Variables not touched in either branch remain unchanged (from before LCA)

  // 4. Update head
  head = merge_commit;

  return std::nullopt;
}

BranchingReadResult EagerLocalVersionStore::get_committed(std::string var) const {
  if (auto it = last_writer.find(var); it != last_writer.end())
    return it->second->changes.at(var);

  return std::monostate{};
}

} // end branching

} // end gitmem