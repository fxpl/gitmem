#include "graphviz.hh"
#include <cassert>

namespace gitmem {
namespace graph {

  using std::to_string;

  void GraphvizPrinter::emitNode(const Node* n, const std::string& label, const std::string& style) {
    file << "\t" << (size_t)n << "[label=\"" << label << "\", shape=rectangle, style=\"rounded,filled\", ";
    if (!style.empty()) file << style;
    file << "]" << ";" << std::endl;
  }

  void GraphvizPrinter::emitEdge(const Node* from, const Node* to, const std::string& label, const std::string& style) {
    if (!from || !to) return;

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
    emitEdge(from, to, "race", "style=dashed, color=red, constraint=false");
  }

  void GraphvizPrinter::emitSyncEdge(const Node* from, const Node* to) {
    emitEdge(from, to, "sync", "style=bold, constraint=false");
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
    n->accept(this);
    file << "}" << std::endl;
  }

  void GraphvizPrinter::visitProgramOrder(const Node* n) {
    if(n)
    {
      n->accept(this);
    }
    else
    {
      file << "}" << std::endl;
    }
  }

  void GraphvizPrinter::visitStart(const Start* n) {
    file << "subgraph cluster_Thread_" << n->id << "{" << std::endl;
    file << "\tlabel = \"Thread #" << n->id << "\";" << std::endl;
    file << "\tcolor=black;" << std::endl;
    emitNode(n, "", "shape=circle width=.3 style=filled color=black");
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

  void GraphvizPrinter::visitEnd(const End* n) {
    assert(!n->next);
    emitNode(n, "", "shape=doublecircle width=.2 style=empty");
    file << "}" << std::endl;
  }

  void GraphvizPrinter::visitWrite(const Write* n) {
    emitNode(n, "W" + n->var + " = " + to_string(n->value));
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

  void GraphvizPrinter::visitRead(const Read* n) {
    emitNode(n, "R" + n->var + " = " + to_string(n->value));
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());

    assert(n->sauce);
    emitReadFromEdge(n, n->sauce.get());
  }

  void GraphvizPrinter::visitSpawn(const Spawn* n) {
    emitNode(n, "Spawn " + std::to_string(n->tid));
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->spawned) {
      emitSyncEdge(n, n->spawned.get());
      visitProgramOrder(n->spawned.get());
    }
  }

  void GraphvizPrinter::visitJoin(const Join* n) {
    emitNode(n, "Join " + std::to_string(n->tid));
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->joinee) emitSyncEdge(n->joinee.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void GraphvizPrinter::visitLock(const Lock* n) {
    emitNode(n, "lock " + n->var);
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
    if (n->ordered_after) emitSyncEdge(n->ordered_after.get(), n);
    if (n->conflict) emitConflict(n, n->conflict.value());
  }

  void GraphvizPrinter::visitUnlock(const Unlock* n) {
    emitNode(n, "unlock " + n->var);
    emitProgramOrderEdge(n, n->next.get());
    visitProgramOrder(n->next.get());
  }

  void GraphvizPrinter::visitPending(const Pending* n) {
    assert(!n->next);
    emitNode(n, "" + n->statement + "", "style=dashed");
    file << "}" << std::endl;
  }

} // namespace graph
} // namespace gitmem
