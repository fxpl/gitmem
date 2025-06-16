#pragma once

#include <unordered_map>
#include <vector>
#include <unordered_map>
#include <fstream>

namespace gitmem {

  namespace graph {

    struct Visitor;

    struct Node
    {
      std::shared_ptr<const Node> next = nullptr;

      virtual void accept(Visitor*) const = 0;
    };

    struct Start;
    struct End;
    struct Write;
    struct Read;
    struct Spawn;
    struct Join;
    struct Lock;
    struct Unlock;

    struct Conflict
    {
        std::string var;
        std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>> sources;
    };

    struct Visitor
    {
      virtual void visitStart(const Start*) = 0;
      virtual void visitEnd(const End*) = 0;
      virtual void visitWrite(const Write*) = 0;
      virtual void visitRead(const Read*) = 0;
      virtual void visitSpawn(const Spawn*) = 0;
      virtual void visitJoin(const Join*) = 0;
      virtual void visitLock(const Lock*) = 0;
      virtual void visitUnlock(const Unlock*) = 0;
      void visit(const Node* n) { n->accept(this); }
    };

    struct Start : Node
    {
      size_t id;

      Start(size_t id): id(id) {}

      void accept(Visitor* v) const override
      {
        v->visitStart(this);
      }
    };

    struct End : Node
    {
      End() {}

      void accept(Visitor* v) const override
      {
        v->visitEnd(this);
      }
    };

    struct Write : Node
    {
      const std::string var;
      const size_t value;
      const size_t id;

      Write(const std::string var, const size_t value, const size_t id): var(var), value(value), id(id) {}

      void accept(Visitor* v) const override
      {
        v->visitWrite(this);
      }
    };

    struct Read : Node
    {
      const std::string var;
      const size_t value;
      const size_t id;
      const std::shared_ptr<const Node> sauce;


      Read(const std::string var, const size_t value, const size_t id, const std::shared_ptr<const Node> sauce): var(var), value(value), id(id), sauce(sauce) {}

      void accept(Visitor* v) const override
      {
        v->visitRead(this);
      }
    };

    struct Spawn : Node
    {
      const size_t tid;
      const std::shared_ptr<const Node> spawned;

      Spawn(const size_t tid, const std::shared_ptr<const Node> spawned): tid(tid), spawned(spawned) {}

      void accept(Visitor* v) const override
      {
        v->visitSpawn(this);
      }
    };

    struct Join : Node
    {
      const size_t tid;
      const std::shared_ptr<const Node> joinee;
      const std::optional<Conflict> conflict;

      Join(const size_t tid,  const std::shared_ptr<const Node> joinee, std::optional<Conflict> conflict = std::nullopt): tid(tid), joinee(joinee), conflict(conflict) {}

      void accept(Visitor* v) const override
      {
        v->visitJoin(this);
      }
    };

    struct Lock : Node
    {
      const std::string var;
      const std::shared_ptr<const Node> ordered_after;
      const std::optional<Conflict> conflict;

      Lock(const std::string var,  const std::shared_ptr<const Node> ordered_after, std::optional<Conflict> conflict = std::nullopt): var(var), ordered_after(ordered_after), conflict(conflict) {}

      void accept(Visitor* v) const override
      {
        v->visitLock(this);
      }
    };

    struct Unlock : Node
    {
      const std::string var;

      Unlock(const std::string var): var(var) {}
      void accept(Visitor* v) const override
      {
        v->visitUnlock(this);
      }
    };

    struct MermaidPrinter : Visitor {
      void visitStart(const Start*) override;
      void visitEnd(const End*) override;
      void visitWrite(const Write*) override;
      void visitRead(const Read*) override;
      void visitSpawn(const Spawn*) override;
      void visitJoin(const Join*) override;
      void visitLock(const Lock*) override;
      void visitUnlock(const Unlock*) override;

      MermaidPrinter(std::string filename) noexcept;
    private:
      std::ofstream file;
    };

  }
}