#include "linear/memory_model.hh"
#include "debug.hh"
#include <iostream>
#include <sstream>
#include <set>

namespace gitmem {

namespace linear {

LocalVersionStore& get_store(ThreadContext& ctx) {
  return static_cast<LocalVersionStore&>(*ctx.sync);
}

// --------------------
// LinearMemoryModel
// --------------------

std::ostream &LinearMemoryModel::print(std::ostream &os) const {
  os << _global_store << std::endl;
  return os;
}

std::string LinearMemoryModel::build_revision_graph_dot(
    const std::vector<const ThreadSyncState*>& thread_states) const {

  std::ostringstream dot;
  dot << "digraph LinearHistory {\n";
  dot << "  rankdir=BT;\n";
  dot << "  node [shape=box];\n";

  const auto& history = _global_store.get_history();

  if (history.empty()) {
    dot << "}\n";
    return dot.str();
  }

  // Create a subgraph for each variable showing its version history
  for (const auto& [obj_name, versions] : history) {
    dot << "  subgraph cluster_" << obj_name << " {\n";
    dot << "    label=\"" << obj_name << "\";\n";
    dot << "    style=dashed;\n";

    // Create nodes for each version
    for (size_t i = 0; i < versions.size(); ++i) {
      const auto& version = versions[i];
      std::ostringstream node_id;
      node_id << obj_name << "_v" << i;

      std::ostringstream label;
      label << version.timestamp() << "\\n" << obj_name << "=" << version.value().value;

      dot << "    \"" << node_id.str() << "\" [label=\"" << label.str() << "\"];\n";
    }

    // Create edges between consecutive versions
    for (size_t i = 1; i < versions.size(); ++i) {
      std::ostringstream prev_id, curr_id;
      prev_id << obj_name << "_v" << (i - 1);
      curr_id << obj_name << "_v" << i;
      dot << "    \"" << curr_id.str() << "\" -> \"" << prev_id.str() << "\";\n";
    }

    dot << "  }\n";
  }

  dot << "}\n";
  return dot.str();
}

std::optional<LinearConflict>
LinearMemoryModel::push(LocalVersionStore &local) {
  if (auto conflict = _global_store.check_conflicts({local.thread(), local.timestamp()},
                                                    local.staged_changes())) {
    return *conflict;
  }

  uint64_t new_base = _global_store.apply_changes(
      local.thread(), local.timestamp(), local.staged_changes());

  local.clear_staging();
  local.advance_base(new_base);
  return std::nullopt;
}

std::optional<LinearConflict>
LinearMemoryModel::pull(LocalVersionStore &local) {
  if (auto conflict = _global_store.check_conflicts({local.thread(), local.timestamp()},
                                                    local.staged_changes())) {
    return *conflict;
  }

  local.advance_base( _global_store.current_counter());
  return std::nullopt;
}

LinearMemoryModel::~LinearMemoryModel() = default;

ReadResult LinearMemoryModel::read(ThreadContext &ctx,
                                               const std::string &var) {
  auto& store = get_store(ctx);

  if (auto result = store.get_staged(var)) {
    return *result;
  }

  std::optional<ValueWithSource> value = _global_store.get_version_for_timestamp(
      var, store.timestamp());
  if (value) {
    return value.value();
  }

  return std::monostate{};
}

void LinearMemoryModel::write(ThreadContext &ctx, const std::string &var,
                               ValueWithSource value) {
  // write into the staging area of the thread
  auto& store = get_store(ctx);
  store.stage(var, value);
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_spawn(ThreadContext &parent, ThreadContext &child) {
  // push parent to global history
  auto& store = get_store(parent);
  if (auto conflict = push(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  // pull into the child
  store = get_store(child);
  if (auto conflict = pull(store)) {
    throw std::logic_error("This code path should never be reached");
  }

  return std::nullopt;
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_join(ThreadContext &joiner, ThreadContext &joinee) {
  // we assume the joinee has already terminated and pushed

  // pull changes into parent
  auto& store = get_store(joiner);
  if (auto conflict = pull(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_start(ThreadContext &thread) {
  // pull state from global history
  auto& store = get_store(thread);
  auto conflict = pull(store);
  assert(!conflict && "cannot conflict from starting state");

  return std::nullopt;
};

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_end(ThreadContext &thread) {
  // push changes to global history
  auto& store = get_store(thread);
  if (auto conflict = push(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
};

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_lock(ThreadContext &thread, Lock &lock) {

  auto& store = get_store(thread);
  if (auto conflict = pull(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_unlock(ThreadContext &thread, Lock &) {
  // push changes to global history
  auto& store = get_store(thread);
  if (auto conflict = push(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
}

bool LinearMemoryModel::is_scheduling_point(SyncOperation op) const {
  switch (op) {
    case SyncOperation::Lock:
    case SyncOperation::Unlock:
    case SyncOperation::Join:
    case SyncOperation::Spawn:
    case SyncOperation::Start:
    case SyncOperation::End:      return true;  }
  assert(false && "Unknown SyncOperation");
}

} // namespace linear

} // namespace gitmem
