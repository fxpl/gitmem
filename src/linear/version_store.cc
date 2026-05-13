#include <iostream>
#include <stdexcept>

#include "memory_model.hh"
#include "version_store.hh"
#include "thread_trace.hh"
#include "read_result.hh"

namespace gitmem {

namespace linear {

// -----------------------------
// LocalVersionStore
// -----------------------------

void LocalVersionStore::stage(std::string obj, ValueWithSource value) {
  _staging[obj] = value;
}

void LocalVersionStore::clear_staging() {
  _staging.clear();
}

void LocalVersionStore::advance_base(uint64_t ts) { _timestamp = ts; }

std::optional<ValueWithSource> LocalVersionStore::get_staged(std::string obj) {
  auto it = _staging.find(obj);
  return it != _staging.end() ? std::make_optional(it->second) : std::nullopt;
}

bool LocalVersionStore::operator==(const LocalVersionStore& other) const {
  return _timestamp == other._timestamp &&
         _staging == other._staging;
}

std::ostream& operator<<(std::ostream& os, const LocalVersionStore& store) {
  os << "LocalVersionStore{"
    << "base=" << store._timestamp
    << ", staged={";

  bool first = true;
  for (const auto& [obj, val] : store._staging) {
    if (!first) os << ", ";
    first = false;
    os << obj << "->" << val.value << " (" << val.source_event << ")";
  }

  os << "}}";
  return os;
}

// -----------------------------
// GlobalVersionStore
// -----------------------------

std::optional<ValueWithSource>
GlobalVersionStore::get_version_for_timestamp(std::string obj,
                                              uint64_t ts) const {
  const auto it = _history.find(obj);

  if (it == _history.end())
    return std::nullopt;

  const VersionHistory &history = it->second;
  for (VersionHistory::const_reverse_iterator riter = history.rbegin();
       riter != history.rend(); ++riter) {
    if (riter->timestamp().counter <= ts)
      return riter->value();
  }

  return std::nullopt;
}

std::optional<Conflict> GlobalVersionStore::check_conflicts(
  Timestamp current_timestamp,
    const std::unordered_map<std::string, ValueWithSource> &changes) const {
  auto event_location = [](const ValueWithSource& value) -> FileLocation {
    if (!value.source_event)
      throw std::logic_error("missing source event for conflicting write");

    auto* write = std::get_if<WriteEvent>(&value.source_event->data);
    if (!write)
      throw std::logic_error("conflicting source event is not a WriteEvent");

    return write->location;
  };

  for (const auto &[obj, local_value] : changes) {
    auto it = _history.find(obj);
    if (it == _history.end()) {
      continue;
    }

    const Version &latest = it->second.back();
    if (latest.timestamp().counter > current_timestamp.counter) {
      return Conflict(
          obj,
          {current_timestamp, event_location(local_value)},
          {latest.timestamp(), event_location(latest.value())});
    }
  }
  return std::nullopt;
}

uint64_t GlobalVersionStore::apply_changes(
    ThreadID tid, uint64_t current_counter,
    const std::unordered_map<std::string, ValueWithSource> &changes) {
  if (auto conflict = check_conflicts({tid, current_counter}, changes)) {
    throw std::logic_error("apply_changes called with conflicts");
  }

  // Increment the global counter and create the next global timestamp for this thread.
  Timestamp new_ts{tid, ++_counter};

  for (const auto &[obj, value] : changes) {
    _history[obj].emplace_back(new_ts, value);
  }

  return _counter;
}

std::ostream& operator<<(std::ostream& os, const GlobalVersionStore& store) {
  os << "GlobalVersionStore(counter=" << store._counter << ")\n";

  for (const auto& [obj_name, history] : store._history) {
    os << "  Object " << obj_name << ":\n";

    for (const auto& version : history) {
      os << "    [" << version.timestamp() << "] = " << version.value().value << "\n";
    }
  }

  return os;
}

} // namespace linear

} // namespace gitmem