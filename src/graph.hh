#pragma once

#include <fstream>
#include <unordered_map>
#include <vector>
#include <variant>

namespace gitmem {

namespace graph {

struct Visitor;

struct Node {
  std::shared_ptr<const Node> next = nullptr;

  virtual void accept(Visitor *) const = 0;
};

struct Start;
struct End;
struct Write;
struct Read;
struct VolatileWrite;
struct VolatileRead;
struct Spawn;
struct Join;
struct Lock;
struct Unlock;
struct Assertion;
struct Pending;

struct Conflict {
  std::string var;
  // Optional: if we can determine the conflicting nodes, store them here
  // Otherwise these can be nullptr and we just mark the node red
  std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>> sources;

  // Constructor that allows creating conflicts without sources
  Conflict(std::string v) : var(std::move(v)), sources{nullptr, nullptr} {}
  Conflict(std::string v, std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>> s)
      : var(std::move(v)), sources(std::move(s)) {}
};

struct Visitor {
  virtual void visitStart(const Start *) = 0;
  virtual void visitEnd(const End *) = 0;
  virtual void visitWrite(const Write *) = 0;
  virtual void visitRead(const Read *) = 0;
  virtual void visitVolatileWrite(const VolatileWrite *) = 0;
  virtual void visitVolatileRead(const VolatileRead *) = 0;
  virtual void visitSpawn(const Spawn *) = 0;
  virtual void visitJoin(const Join *) = 0;
  virtual void visitLock(const Lock *) = 0;
  virtual void visitUnlock(const Unlock *) = 0;
  virtual void visitAssertion(const Assertion *) = 0;
  virtual void visitPending(const Pending *) = 0;
  virtual void visit(const Node *n) { n->accept(this); }
};

struct Start : Node {
  size_t id;

  Start(size_t id) : id(id) {}

  void accept(Visitor *v) const override { v->visitStart(this); }
};

struct End : Node {
  End() {}

  void accept(Visitor *v) const override { v->visitEnd(this); }
};

struct Write : Node {
  const std::string var;
  const size_t value;
  const size_t id;

  Write(const std::string var, const size_t value, const size_t id)
      : var(var), value(value), id(id) {}

  void accept(Visitor *v) const override { v->visitWrite(this); }
};

struct Read : Node {
  const std::string var;
  const size_t id;

  struct SuccessfulRead {
    size_t value;
    std::shared_ptr<const Node> source;
  };

  std::variant<SuccessfulRead, Conflict> read_result;

  // Constructor for successful read
  Read(const std::string var, const size_t value, const size_t id,
       const std::shared_ptr<const Node> source)
      : var(var), id(id), read_result(SuccessfulRead{value, source}) {}

  // Constructor for conflicting read
  Read(const std::string var, const size_t id, Conflict conflict)
      : var(var), id(id), read_result(std::move(conflict)) {}

  void set_source(const std::shared_ptr<const Node> source) {
    if (std::holds_alternative<SuccessfulRead>(read_result)) {
      std::get<SuccessfulRead>(read_result).source = source;
    }
  }

  void accept(Visitor *v) const override { v->visitRead(this); }
};

// A volatile write is an ordinary write plus the write->write synchronisation
// order: `sync_predecessor` is the prior volatile write (release) this one
// chains onto (null for the first write to the volatile).
struct VolatileWrite : Write {
  std::shared_ptr<const Node> sync_predecessor = nullptr;
  // A volatile write can race (write-write with no happens-before); when it
  // does, this holds the conflict so the node renders as an error.
  std::optional<Conflict> conflict = std::nullopt;

  VolatileWrite(const std::string var, const size_t value, const size_t id)
      : Write(var, value, id) {}

  void accept(Visitor *v) const override { v->visitVolatileWrite(this); }
};

// A volatile read is an ordinary read whose edge to the source write is a
// synchronisation (acquire) edge rather than a plain reads-from edge.
struct VolatileRead : Read {
  VolatileRead(const std::string var, const size_t value, const size_t id,
               const std::shared_ptr<const Node> source)
      : Read(var, value, id, source) {}

  VolatileRead(const std::string var, const size_t id, Conflict conflict)
      : Read(var, id, std::move(conflict)) {}

  void accept(Visitor *v) const override { v->visitVolatileRead(this); }
};

struct Spawn : Node {
  const size_t tid;
  const std::shared_ptr<const Node> spawned;

  Spawn(const size_t tid, const std::shared_ptr<const Node> spawned)
      : tid(tid), spawned(spawned) {}

  void accept(Visitor *v) const override { v->visitSpawn(this); }
};

struct Join : Node {
  const size_t tid;
  const std::shared_ptr<const Node> joinee;
  const std::optional<Conflict> conflict;

  Join(const size_t tid, const std::shared_ptr<const Node> joinee,
       std::optional<Conflict> conflict = std::nullopt)
      : tid(tid), joinee(joinee), conflict(conflict) {}

  void accept(Visitor *v) const override { v->visitJoin(this); }
};

struct Lock : Node {
  const std::string var;
  const std::shared_ptr<const Node> ordered_after;
  const std::optional<Conflict> conflict;

  Lock(const std::string var, const std::shared_ptr<const Node> ordered_after,
       std::optional<Conflict> conflict = std::nullopt)
      : var(var), ordered_after(ordered_after), conflict(conflict) {}

  void accept(Visitor *v) const override { v->visitLock(this); }
};

struct Unlock : Node {
  const std::string var;
  const std::optional<Conflict> conflict;
  std::shared_ptr<const Node> g_predecessor;

  Unlock(const std::string var, std::optional<Conflict> conflict = std::nullopt,
         std::shared_ptr<const Node> g_predecessor = nullptr)
      : var(var), conflict(conflict), g_predecessor(g_predecessor) {}

  void accept(Visitor *v) const override { v->visitUnlock(this); }
};

struct Assertion : Node {
  const std::string cond;
  const bool passed;

  Assertion(const std::string &cond, const bool passed) : cond(cond), passed(passed) {}

  void accept(Visitor *v) const override { v->visitAssertion(this); }
};

struct Pending : Node {
  const std::string statement;

  Pending(const std::string statement) : statement(statement) {}
  void accept(Visitor *v) const override { v->visitPending(this); }
};

struct ExecutionGraph {
  std::shared_ptr<const Node> entry;
  std::vector<std::shared_ptr<graph::Start>> threads;

  ExecutionGraph(std::shared_ptr<const Node> entry) : entry(entry) {}

  ExecutionGraph(const ExecutionGraph&) = delete;
  ExecutionGraph& operator=(const ExecutionGraph&) = delete;

  ExecutionGraph(ExecutionGraph&&) = default;
  ExecutionGraph& operator=(ExecutionGraph&&) = default;
};

} // namespace graph
} // namespace gitmem
