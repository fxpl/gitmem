#pragma once
#include "graph.hh"

namespace gitmem {
  namespace graph {
    struct GraphvizPrinter : Visitor {
      void visitStart(const Start*) override;
      void visitEnd(const End*) override;
      void visitWrite(const Write*) override;
      void visitRead(const Read*) override;
      void visitSpawn(const Spawn*) override;
      void visitJoin(const Join*) override;
      void visitLock(const Lock*) override;
      void visitUnlock(const Unlock*) override;
      void visitAssertionFailure(const AssertionFailure*) override;
      void visitPending(const Pending*) override;
      void visit(const Node* n) override;

      GraphvizPrinter(std::string filename) noexcept;
    private:
      std::ofstream file;
      void emitNode(const Node* n, const std::string& label, const std::string& style = "");
      void emitEdge(const Node* from, const Node* to, const std::string& label, const std::string& style = "");
      void emitProgramOrderEdge(const Node* from, const Node* to);
      void emitReadFromEdge(const Node* from, const Node* to);
      void emitFillColor(const Node* n, const std::string& color);
      void emitConflictEdge(const Node* from, const Node* to);
      void emitSyncEdge(const Node* from, const Node* to);
      void emitConflict(const Node* n, const Conflict& conflict);
      void visitProgramOrder(const Node* n);
    };
  }
}
