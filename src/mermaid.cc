#include "mermaid.hh"
#include <cassert>

namespace gitmem {
namespace graph {

  using std::to_string;

  void MermaidPrinter::emitNode(const Node* n, const std::string& label, const std::string& shape) {
    file << "\t" << (size_t)n;
    if (!shape.empty()) {
      file << "@{ shape: " << shape << ", label: \"" << label << "\" }";
    } else {
      file << "(" << label << ")";
    }
    file << std::endl;
  }

  void MermaidPrinter::emitEdge(const Node* from, const Node* to, const std::string& style) {
    if(!from || !to) return;
    file << "\t" << (size_t)from;
    if (!style.empty()) file << " -." << style << ".-> ";
    else file << " --> ";
    file << (size_t)to << std::endl;
  }

  void MermaidPrinter::emitConflict(const Node* n, const Conflict& conflict) {
    file << "\tstyle " << (size_t)n << " fill:red" << std::endl;
    auto [s1, s2] = conflict.sources;
    emitEdge(n, s1.get(), "");
    emitEdge(n, s2.get(), "");
  }

  MermaidPrinter::MermaidPrinter(std::string filename) noexcept {
    file.open(filename);
    file << "flowchart TB" << std::endl;
  }

  void MermaidPrinter::visitProgramOrder(const Node* n)
  {
    if(n)
    {
      n->accept(this);
    }
    else
    {
      file << "end" << std::endl;
    }
  }

  void MermaidPrinter::visitStart(const Start* n) {
    file << "subgraph Thread " << n->id << std::endl;
    file << "\tdirection TB" << std::endl;
    emitNode(n, "start", "circle");
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

  void MermaidPrinter::visitEnd(const End* n) {
    assert(!n->next);
    emitNode(n, "end", "dbl-circ");
    file << "end" << std::endl;
  }

  void MermaidPrinter::visitWrite(const Write* n) {
    emitNode(n, "write " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

  void MermaidPrinter::visitRead(const Read* n) {
    emitNode(n, "read " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    assert(n->sauce);
    if (n->next) {
      emitEdge(n, n->next.get());
      visitProgramOrder(n->next.get());
    }
    emitEdge(n, n->sauce.get(), "rf");
  }

  void MermaidPrinter::visitSpawn(const Spawn* n) {
    emitNode(n, "spawn " + std::to_string(n->tid));
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->spawned) {
      emitEdge(n, n->spawned.get());
      n->spawned->accept(this);
    }
  }

  void MermaidPrinter::visitJoin(const Join* n) {
    emitNode(n, "join Thread " + std::to_string(n->tid));
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->joinee) emitEdge(n->joinee.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void MermaidPrinter::visitLock(const Lock* n) {
    emitNode(n, "lock " + n->var);
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->ordered_after) emitEdge(n->ordered_after.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void MermaidPrinter::visitUnlock(const Unlock* n) {
    emitNode(n, "unlock " + n->var);
    emitEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

} // namespace graph
} // namespace gitmem