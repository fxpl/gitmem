#pragma once

#include "thread_id.hh"
#include "conflict.hh"
#include "overloaded.hh"
#include "read_result.hh"

#include <ostream>
#include <sstream>
#include <variant>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>

namespace gitmem {

struct Event;

struct StartEvent {};
struct SpawnEvent { const ThreadID child_tid; };
struct ReadValue { const size_t value; const std::shared_ptr<Event> source_event; };
struct ReadEvent { const std::string var; std::variant<const ReadValue, std::shared_ptr<ConflictBase>> value_or_conflict; };
struct WriteEvent { const std::string var; const size_t value; const FileLocation location; };
struct VolatileReadEvent { const std::string var; std::variant<const ReadValue, std::shared_ptr<ConflictBase>> value_or_conflict; };
struct VolatileWriteEvent { const std::string var; const size_t value; const FileLocation location; std::shared_ptr<ConflictBase> maybe_conflict = nullptr; std::shared_ptr<Event> sync_predecessor = nullptr; };
struct LockEvent { std::string lock_name; std::shared_ptr<ConflictBase> maybe_conflict; std::shared_ptr<Event> last_unlock_event; };
struct UnlockEvent { const std::string lock_name; std::shared_ptr<ConflictBase> maybe_conflict; std::shared_ptr<Event> g_predecessor = nullptr; };
struct JoinEvent { const ThreadID joinee_tid; std::shared_ptr<ConflictBase> maybe_conflict; };
struct AssertEvent { const std::string condition; bool pass; };

struct EndEvent {};

using EventID = size_t;

struct Event {
  ThreadID tid;
  EventID eid;
  std::variant<
    StartEvent,
    SpawnEvent,
    ReadEvent,
    WriteEvent,
    VolatileReadEvent,
    VolatileWriteEvent,
    LockEvent,
    UnlockEvent,
    JoinEvent,
    AssertEvent,
    EndEvent
  > data;
};

inline std::string event_header(const Event& e) {
  std::ostringstream oss;
  oss << "[tid=" << e.tid << ", eid=" << e.eid << "]";
  return oss.str();
}

// --- operator<< overloads for individual event types ---
inline std::ostream& operator<<(std::ostream& os, const StartEvent&) {
  return os << "StartEvent";
}

inline std::ostream& operator<<(std::ostream& os, const SpawnEvent& e) {
  return os << "SpawnEvent(child_tid=" << e.child_tid << ")";
}

inline std::ostream& operator<<(std::ostream& os, const ReadEvent& e) {
  os << "ReadEvent(var=\"" << e.var << "\", ";
  std::visit(overloaded{
    [&os](const ReadValue& val) { os << "value=" << val.value << " (from " << val.source_event->eid << ")"; },
    [&os](const std::shared_ptr<ConflictBase>&) { os << "conflict"; }
  }, e.value_or_conflict);
  os << ")";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const WriteEvent& e) {
  os << "WriteEvent(var=\"" << e.var << "\", value=" << e.value;
  os << ", at " << e.location.linecol();
  return os << ")";
}

inline std::ostream& operator<<(std::ostream& os, const VolatileReadEvent& e) {
  os << "VolatileReadEvent(var=\"" << e.var << "\", ";
  std::visit(overloaded{
    [&os](const ReadValue& val) { os << "value=" << val.value << " (from " << val.source_event->eid << ")"; },
    [&os](const std::shared_ptr<ConflictBase>&) { os << "conflict"; }
  }, e.value_or_conflict);
  os << ")";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const VolatileWriteEvent& e) {
  os << "VolatileWriteEvent(var=\"" << e.var << "\", value=" << e.value;
  os << ", at " << e.location.linecol();
  if (e.maybe_conflict)
    os << ", conflict";
  return os << ")";
}

inline std::ostream& operator<<(std::ostream& os, const LockEvent& e) {
  os << "LockEvent(lock_name=\"" << e.lock_name << "\"";
  if (e.last_unlock_event)
    os << ", last unlock " << event_header(*e.last_unlock_event);
  if (e.maybe_conflict)
    os << ", conflict)";
  else
    os << ")";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const UnlockEvent& e) {
  os << "UnlockEvent(lock_name=\"" << e.lock_name << "\"";
  if (e.maybe_conflict)
    os << ", conflict)";
  else
    os << ")";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const JoinEvent& e) {
  os << "JoinEvent(joinee_tid=" << e.joinee_tid;
  if (e.maybe_conflict)
    os << ", conflict)";
  else
    os << ")";
  return os;
}

inline std::ostream& operator<<(std::ostream& os, const AssertEvent& e) {
  return os << "AssertEvent(condition=\"" << e.condition << "\", " << (e.pass ? "pass" : "fail") << ")";
}

inline std::ostream& operator<<(std::ostream& os, const EndEvent&) {
  return os << "EndEvent";
}

// --- operator<< for the wrapper Event ---
inline std::ostream& operator<<(std::ostream& os, const Event& e) {
  os << event_header(e) << " ";
  std::visit([&os](auto&& arg) { os << arg; }, e.data);
  return os;
}

static EventID next_eid = 0;

struct ThreadTrace {
  std::vector<std::shared_ptr<Event>> trace;
  ThreadID tid;

  auto begin() { return trace.begin(); }
  auto end()   { return trace.end(); }

  auto begin() const { return trace.begin(); }
  auto end()   const { return trace.end(); }

  explicit ThreadTrace(ThreadID tid) : tid(tid) {}

private:
  template<class T, class... Args>
  std::shared_ptr<Event> append(Args&&... args) {
    auto event = std::make_shared<Event>(tid, next_eid++, T(std::forward<Args>(args)...));
    trace.push_back(event);
    return event;
  }

  public:
  std::shared_ptr<Event> on_start() {
    return append<StartEvent>();
  }

  std::shared_ptr<Event> on_spawn(ThreadID child_tid) {
    return append<SpawnEvent>(child_tid);
  }

  std::shared_ptr<Event> on_read(const std::string text, ValueWithSource value) {
    return append<ReadEvent>(std::move(text), ReadValue{value.value, value.source_event});
  }

  std::shared_ptr<Event> on_read(const std::string text, std::shared_ptr<ConflictBase> conflict) {
    return append<ReadEvent>(std::move(text), conflict);
  }

  std::shared_ptr<Event> on_write(const std::string text, const size_t value,
                                  FileLocation location) {
    return append<WriteEvent>(std::move(text), value, std::move(location));
  }

  std::shared_ptr<Event> on_volatile_read(const std::string text, ValueWithSource value) {
    return append<VolatileReadEvent>(std::move(text), ReadValue{value.value, value.source_event});
  }

  std::shared_ptr<Event> on_volatile_read(const std::string text, std::shared_ptr<ConflictBase> conflict) {
    return append<VolatileReadEvent>(std::move(text), conflict);
  }


  std::shared_ptr<Event> on_volatile_write(const std::string text, const size_t value,
                                           FileLocation location,
                                           std::shared_ptr<Event> sync_predecessor = nullptr) {
    return append<VolatileWriteEvent>(std::move(text), value, std::move(location),
                                      nullptr, std::move(sync_predecessor));
  }

  // The volatile-write event is the value's provenance, so it is created before
  // the model checks the release; attach a conflict here if the release raced.
  void on_volatile_write_conflict(const std::shared_ptr<Event>& event,
                                  std::shared_ptr<ConflictBase> conflict) {
    std::get<VolatileWriteEvent>(event->data).maybe_conflict = std::move(conflict);
  }

  std::shared_ptr<Event> on_lock(const std::string lock_name,
                                 std::shared_ptr<Event> last_unlock_event,
                                 std::shared_ptr<ConflictBase> conflict = nullptr) {
    return append<LockEvent>(std::move(lock_name), std::move(conflict), last_unlock_event);
  }

  std::shared_ptr<Event> on_unlock(const std::string lock_name,
                                   std::shared_ptr<ConflictBase> conflict = nullptr,
                                   std::shared_ptr<Event> g_predecessor = nullptr) {
    return append<UnlockEvent>(std::move(lock_name), conflict, g_predecessor);
  }

  std::shared_ptr<Event> on_join(ThreadID tid, std::shared_ptr<ConflictBase> conflict = nullptr) {
    return append<JoinEvent>(tid, conflict);
  }

  std::shared_ptr<Event> on_assert(std::string expr, bool pass) {
    return append<AssertEvent>(std::move(expr), pass);
  }

  std::shared_ptr<Event> on_assert_pass(std::string expr) {
    return append<AssertEvent>(std::move(expr), true);
  }

  std::shared_ptr<Event> on_assert_fail(std::string expr) {
    return append<AssertEvent>(std::move(expr), false);
  }

  std::shared_ptr<Event> on_end() {
    return append<EndEvent>();
  }
};

// --- operator<< for ThreadTrace ---
inline std::ostream& operator<<(std::ostream& os, const ThreadTrace& tt) {
  os << "ThreadTrace[" << tt.trace.size() << " events]:\n";
  for (size_t i = 0; i < tt.trace.size(); ++i) {
    os << "  " << i << ": " << *(tt.trace[i]) << "\n";
  }
  return os;
}

} // namespace gitmem