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
LinearMemoryModel::pullpush(LocalVersionStore &local) {
  if (auto conflict = _global_store.check_conflicts({local.thread(), local.timestamp()},
                                                    local.staged_changes())) {
    return *conflict;
  }

  push(local);
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

void LinearMemoryModel::push(LocalVersionStore &local) {
  uint64_t new_base = _global_store.apply_changes(
      local.thread(), local.timestamp(), local.staged_changes());

  local.clear_staging();
  local.advance_base(new_base);
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
  // pullpush parent to global history
  auto& store = get_store(parent);
  if (auto conflict = pullpush(store))
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
  // we assume the joinee has already terminated and pullpushed

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
  // pullpush changes to global history
  auto& store = get_store(thread);
  if (auto conflict = pullpush(store))
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
  // pullpush changes to global history
  auto& store = get_store(thread);
  if (auto conflict = pullpush(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_volatile_read(ThreadContext &thread, Volatile &v) {
  auto& store = get_store(thread);
  if (auto conflict = pull(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  return std::nullopt;
}

std::optional<std::shared_ptr<ConflictBase>>
LinearMemoryModel::on_volatile_write(ThreadContext &thread, Volatile &v,
                                     ValueWithSource value) {
  auto& store = get_store(thread);

  // Version @v in g, in addition to the atomic value on the object. Then
  // publish, checking for conflicts at the current base: a concurrent write to
  // @v -- one not happens-before ours -- is caught here as a write-write race
  // (write->write is not a synchronizes-with edge, only write->read is). We
  // must NOT pull first: that would absorb the concurrent write and hide it.
  store.stage(v.name, value);
  if (auto conflict = pullpush(store))
    return std::make_shared<LinearConflict>(std::move(*conflict));

  // The single current value (with provenance) lives on the object; the g
  // version exists only to detect the race.
  v.value = value;

  return std::nullopt;
}

bool LinearMemoryModel::is_scheduling_point(SyncOperation op) const {
  switch (op) {
    case SyncOperation::Lock:
    case SyncOperation::Unlock:
    case SyncOperation::Join:
    case SyncOperation::Spawn:
    case SyncOperation::Start:
    case SyncOperation::End:
    case SyncOperation::VolatileRead:
    case SyncOperation::VolatileWrite: return true;  }
  assert(false && "Unknown SyncOperation");
}

} // namespace linear

} // namespace gitmem
