#pragma once
#include "graph.hh"

namespace gitmem {
  namespace graph {
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

      void emitNode(const Node* n, const std::string& label, const std::string& shape = "");
      void emitEdge(const Node* from, const Node* to, const std::string& style = "");
      void emitConflict(const Node* n, const Conflict& conflict);
      void visitProgramOrder(const Node* n);
    };
  }
}