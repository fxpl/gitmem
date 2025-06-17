#include "graph.hh"
#include <iostream>
#include <sstream>
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
    file << "```mermaid" << std::endl;
    file << "flowchart TB" << std::endl;
  }

  void MermaidPrinter::visitStart(const Start* n) {
    file << "subgraph Thread " << n->id << std::endl;
    file << "\tdirection TB" << std::endl;
    emitNode(n, "start", "circle");
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
  }

  void MermaidPrinter::visitEnd(const End* n) {
    assert(!n->next);
    emitNode(n, "end", "dbl-circ");
    file << "end" << std::endl;
  }

  void MermaidPrinter::visitWrite(const Write* n) {
    emitNode(n, "write " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
  }

  void MermaidPrinter::visitRead(const Read* n) {
    emitNode(n, "read " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    assert(n->sauce);
    if (n->next) {
      emitEdge(n, n->next.get());
      n->next->accept(this);
    }
    emitEdge(n, n->sauce.get(), "rf");
  }

  void MermaidPrinter::visitSpawn(const Spawn* n) {
    emitNode(n, "spawn " + std::to_string(n->tid));
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
    if (n->spawned) {
      emitEdge(n, n->spawned.get());
      n->spawned->accept(this);
    }
  }

  void MermaidPrinter::visitJoin(const Join* n) {
    emitNode(n, "join Thread " + std::to_string(n->tid));
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
    if (n->joinee) emitEdge(n->joinee.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void MermaidPrinter::visitLock(const Lock* n) {
    emitNode(n, "lock " + n->var);
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
    if (n->ordered_after) emitEdge(n->ordered_after.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void MermaidPrinter::visitUnlock(const Unlock* n) {
    emitNode(n, "unlock " + n->var);
    assert(n->next);
    emitEdge(n, n->next.get());
    n->next->accept(this);
  }

  void GraphvizPrinter::emitNode(const Node* n, const std::string& label, const std::string& style) {
    file << "\t" << (size_t)n << "[label=\"" << label << "\", shape=rectangle, style=\"rounded,filled\", ";
    if (!style.empty()) file << style;
    file << "]" << ";" << std::endl;
  }

  void GraphvizPrinter::emitEdge(const Node* from, const Node* to, const std::string& label, const std::string& style) {
    file << "\t" << (size_t)from << " -> " << (size_t)to;
    if (!style.empty() || !label.empty()) {
      file << "[";
      if (!style.empty()) file << style;
      if (!label.empty()) file << " label=\"" << label << "\"";
      file << "]";
    }
    file << ";" << std::endl;
  }

  void GraphvizPrinter::emitProgramOrderEdge(const Node* from, const Node* to) {
    emitEdge(from, to, "");
  }

  void GraphvizPrinter::emitReadFromEdge(const Node* from, const Node* to) {
    emitEdge(from, to, "rf", "style=dashed, constraint=false");
  }

  void GraphvizPrinter::emitConflictEdge(const Node* from, const Node* to) {
    emitEdge(from, to, "", "style=dashed, color=red, constraint=false");
  }

  void GraphvizPrinter::emitSyncEdge(const Node* from, const Node* to) {
    emitEdge(from, to, "", "constraint=false");
  }

  void GraphvizPrinter::emitConflict(const Node* n, const Conflict& conflict) {
    file << "\t" << (size_t)n << "[fillcolor = red];" << std::endl;
    auto [s1, s2] = conflict.sources;
    emitConflictEdge(n, s1.get());
    emitConflictEdge(n, s2.get());
  }

  GraphvizPrinter::GraphvizPrinter(std::string filename) noexcept {
    file.open(filename);
  }

  void GraphvizPrinter::visit(const Node* n) {
    file << "digraph G {" << std::endl;
    file << "bgcolor=darkgray" << std::endl;
    n->accept(this);
    file << "}" << std::endl;
  }

  void GraphvizPrinter::visitStart(const Start* n) {
    file << "subgraph cluster_Thread_" << n->id << "{" << std::endl;
    file << "\tlabel = \"Thread #" << n->id << "\";" << std::endl;
    file << "\tcolor=black;" << std::endl;
    emitNode(n, "", "shape=circle width=.3 style=filled color=black");
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
  }

  void GraphvizPrinter::visitEnd(const End* n) {
    assert(!n->next);
    emitNode(n, "", "shape=doublecircle width=.2");
    file << "}" << std::endl;
  }

  void GraphvizPrinter::visitWrite(const Write* n) {
    emitNode(n, "write " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
  }

  void GraphvizPrinter::visitRead(const Read* n) {
    emitNode(n, "read " + n->var + " = " + to_string(n->value) + " : #" + std::to_string(n->id));
    assert(n->sauce);
    if (n->next) {
      emitProgramOrderEdge(n, n->next.get());
      n->next->accept(this);
    }
    emitReadFromEdge(n, n->sauce.get());
  }

  void GraphvizPrinter::visitSpawn(const Spawn* n) {
    emitNode(n, "spawn Thread " + std::to_string(n->tid));
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
    if (n->spawned) {
      emitSyncEdge(n, n->spawned.get());
      n->spawned->accept(this);
    }
  }

  void GraphvizPrinter::visitJoin(const Join* n) {
    emitNode(n, "join Thread " + std::to_string(n->tid));
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
    if (n->joinee) emitSyncEdge(n->joinee.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void GraphvizPrinter::visitLock(const Lock* n) {
    emitNode(n, "lock " + n->var);
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
    if (n->ordered_after) emitSyncEdge(n->ordered_after.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void GraphvizPrinter::visitUnlock(const Unlock* n) {
    emitNode(n, "unlock " + n->var);
    assert(n->next);
    emitProgramOrderEdge(n, n->next.get());
    n->next->accept(this);
  }

} // namespace graph
} // namespace gitmem