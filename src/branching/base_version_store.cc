#include "base_version_store.hh"
#include <iostream>
#include <unordered_set>
#include <sstream>
#include <stack>
#include "debug.hh"
#include <algorithm>

namespace gitmem {

namespace branching {

// Helper: recursive print with 2-space indentation and cycle protection
void print_commit_recursive(std::ostream& os,
                            const std::shared_ptr<const Commit>& commit,
                            std::unordered_set<const Commit*>& visited,
                            int depth = 0)
{
  if (!commit) return;
  if (!visited.insert(commit.get()).second) {
    os << std::string(depth * 2, ' ') << "(already printed commit " << commit->id << ")\n";
    return;
  }

  os << std::string(depth * 2, ' ') << "Commit " << commit->id << " {\n";

  // Print changes
  for (const auto& [obj, val] : commit->changes) {
    os << std::string((depth + 1) * 2, ' ') << obj << " -> " << val.value << "\n";
  }

  // Print parents
  if (!commit->parents.empty()) {
    os << std::string((depth + 1) * 2, ' ') << "Parents: ";
    for (size_t i = 0; i < commit->parents.size(); ++i) {
      os << commit->parents[i]->id;
      if (i + 1 < commit->parents.size()) os << ", ";
    }
    os << "\n";
  }

  os << std::string(depth * 2, ' ') << "}\n";

  // Recursively print parents
  for (auto& parent : commit->parents) {
    print_commit_recursive(os, parent, visited, depth + 1);
  }
}

// operator<< for Commit
std::ostream& operator<<(std::ostream& os, const Commit& commit) {
  std::unordered_set<const Commit*> visited;
  // Wrap the commit in a shared_ptr to reuse the recursive helper
  print_commit_recursive(os, std::make_shared<const Commit>(commit), visited);
  return os;
}

void LocalVersionStore::stage(std::string obj, ValueWithSource value) {
  staging[obj] = value;
}

void LocalVersionStore::commit_staging() {
  // No-op commit does nothing unless verbose mode
  if (staging.empty() && !verbose) {
    return;
  }

  // Create the new commit with the staged changes
  auto new_commit = std::make_shared<Commit>(base_timestamp++, std::move(staging));

  // Update last_writer for each staged variable
  for (const auto& [obj, _] : new_commit->changes) {
    last_writer[obj] = new_commit;
  }

  // Clear staging
  staging.clear();

  // Set parent to previous head if it exists
  if (head)
    new_commit->parents.push_back(head);

  // Update head
  head = new_commit;
}

BranchingReadResult LocalVersionStore::read(std::string var) const {
  auto it = staging.find(var);
  if (it != staging.end())
    return it->second;

  return get_committed(var);
}

void LocalVersionStore::adopt_history(const LocalVersionStore& other) {
  // Inherit the DAG head
  head = other.head;

  // Inherit the last_writer cache so the child sees all latest commits
  last_writer = other.last_writer;
}

std::ostream& operator<<(std::ostream& os, const LocalVersionStore& store) {
  os << "LocalVersionStore{"
     << "base=" << store.base_timestamp
     << ", head=";

  if (store.head)
    os << store.head->id;
  else
    os << "null";

  os << ", staged={";

  bool first = true;
  for (const auto& [obj, val] : store.staging) {
    if (!first) os << ", ";
    first = false;
    os << obj << "->" << val.value;
  }

  os << "}}";
  return os;
}

bool LocalVersionStore::operator==(const LocalVersionStore& other) const {
  return base_timestamp == other.base_timestamp &&
         head == other.head &&
         staging == other.staging;
}

std::ostream& operator<<(std::ostream& os, const GlobalVersionStore& store) {
  os << "GlobalVersionStore()";
  return os;
}

bool can_reach(const std::shared_ptr<const Commit>& commit, const std::shared_ptr<const Commit>& other, std::unordered_map<std::shared_ptr<const Commit>, bool>& memo) {
  if (!commit)
    return false;

  if (commit == other)
    return true;

  auto it = memo.find(commit);
  if (it != memo.end())
    return it->second;

  for (const auto& parent : commit->parents) {
    if (can_reach(parent, other, memo)) {
      memo[commit] = true;
      return true;
    }
  }

  memo[commit] = false;
  return false;
}

std::string build_commit_graph_dot(const std::vector<std::shared_ptr<const Commit>>& leaves) {
  std::ostringstream dot;
  dot << "digraph CommitGraph {\n";
  dot << "  rankdir=BT;\n";
  dot << "  node [shape=box];\n";

  std::unordered_set<const Commit*> visited;
  std::unordered_map<ThreadID, std::vector<std::shared_ptr<const Commit>>> commits_by_thread;
  std::stack<std::shared_ptr<const Commit>> stack;

  // First pass: collect all commits and organize by thread
  for (const auto& leaf : leaves)
    if (leaf) stack.push(leaf);

  while (!stack.empty()) {
    auto commit = stack.top();
    stack.pop();

    if (!commit || !visited.insert(commit.get()).second)
      continue;

    commits_by_thread[commit->id.thread].push_back(commit);

    for (const auto& parent : commit->parents) {
      if (parent) stack.push(parent);
    }
  }

  // Create subgraph clusters for each thread
  for (const auto& [thread_id, commits] : commits_by_thread) {
    dot << "  subgraph cluster_" << thread_id << " {\n";
    dot << "    label=\"Thread " << thread_id << "\";\n";
    dot << "    style=dashed;\n";

    for (const auto& commit : commits) {
      const std::string cid = to_string(commit->id);

      std::ostringstream label;
      label << cid;

      // Mark merge commits
      if (commit->parents.size() >= 2) {
        label << " (merge";
        if (commit->conflicted) {
          label << " - CONFLICT";
        }
        label << ")";
      }

      if (!commit->changes.empty()) {
        label << "\\n";
        bool first = true;
        for (const auto& [obj, val] : commit->changes) {
          if (!first) label << "\\n";
          first = false;
          label << obj << "→" << val.value;
        }
      }

      // Style merge commits differently
      if (commit->parents.size() >= 2) {
        std::string fillcolor = commit->conflicted ? "pink" : "lightgray";
        dot << "    \"" << cid << "\" [label=\"" << label.str() << "\", style=filled, fillcolor=" << fillcolor << "];\n";
      } else {
        dot << "    \"" << cid << "\" [label=\"" << label.str() << "\"];\n";
      }
    }

    dot << "  }\n";
  }

  // Draw edges (outside clusters so they can cross boundaries)
  visited.clear();
  for (const auto& leaf : leaves)
    if (leaf) stack.push(leaf);

  while (!stack.empty()) {
    auto commit = stack.top();
    stack.pop();

    if (!commit || !visited.insert(commit.get()).second)
      continue;

    const std::string cid = to_string(commit->id);

    for (const auto& parent : commit->parents) {
      if (!parent) continue;

      const std::string pid = to_string(parent->id);
      dot << "  \"" << cid << "\" -> \"" << pid << "\";\n";
      stack.push(parent);
    }
  }

  dot << "}\n";
  return dot.str();
}

} // branching

} // gitmem