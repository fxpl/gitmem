#include "graphviz.hh"
#include "overloaded.hh"
#include <cassert>

namespace gitmem {
namespace graph {

using std::to_string;

void GraphvizPrinter::emitNode(const Node *n, const std::string &label,
                               const std::string &style) {
  file << "\t" << (size_t)n << "[label=\"" << label
       << "\", shape=rectangle, style=\"rounded,filled\", ";
  if (!style.empty())
    file << style;
  file << "]" << ";" << std::endl;
}

void GraphvizPrinter::emitEdge(const Node *from, const Node *to,
                               const std::string &label,
                               const std::string &style) {
  if (!from || !to)
    return;

  file << "\t" << (size_t)from << " -> " << (size_t)to;
  if (!style.empty() || !label.empty()) {
    file << "[";
    if (!style.empty())
      file << style;
    if (!label.empty())
      file << " label=\"" << label << "\"";
    file << "]";
  }
  file << ";" << std::endl;
}

void GraphvizPrinter::emitProgramOrderEdge(const Node *from, const Node *to) {
  emitEdge(from, to, "");
}

void GraphvizPrinter::emitReadFromEdge(const Node *from, const Node *to) {
  emitEdge(from, to, "rf", "style=dashed, constraint=false");
}

void GraphvizPrinter::emitConflictEdge(const Node *from, const Node *to) {
  emitEdge(from, to, "race", "style=dashed, color=red, constraint=false");
}

void GraphvizPrinter::emitSyncEdge(const Node *from, const Node *to) {
  emitEdge(from, to, "sync", "style=bold, constraint=false");
}

void GraphvizPrinter::emitFillColor(const Node *n, const std::string &color) {
  file << "\t" << (size_t)n << "[fillcolor = " << color << "];" << std::endl;
}

void GraphvizPrinter::emitShape(const Node *n, const std::string &shape) {
  file << "\t" << (size_t)n << "[shape = " << shape << "];" << std::endl;
}

void GraphvizPrinter::emitConflict(const Node *n, const Conflict &conflict) {
  emitFillColor(n, "red");
  // emitShape(n, "doubleoctagon");

  // Only draw conflict edges to actual *other* source nodes (a racing write is
  // often one of the two sources itself; don't draw a self-loop).
  auto [s1, s2] = conflict.sources;
  if (s1 && s1.get() != n) {
    emitConflictEdge(n, s1.get());
  }
  if (s2 && s2.get() != n) {
    emitConflictEdge(n, s2.get());
  }
}

GraphvizPrinter::GraphvizPrinter(std::string filename) noexcept {
  file.open(filename);
}

void GraphvizPrinter::visit(const Node *n) {
  file << "digraph G {" << std::endl;
  if (n) n->accept(this);
  file << "}" << std::endl;
}

void GraphvizPrinter::visitProgramOrder(const Node *n) {
  if (n) {
    n->accept(this);
  } else {
    file << "}" << std::endl;
  }
}

void GraphvizPrinter::visitStart(const Start *n) {
  file << "subgraph cluster_Thread_" << n->id << "{" << std::endl;
  file << "\tlabel = \"Thread #" << n->id << "\";" << std::endl;
  file << "\tcolor=black;" << std::endl;
  emitNode(n, "", "shape=circle width=.3 style=filled color=black");
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
}

void GraphvizPrinter::visitEnd(const End *n) {
  assert(!n->next);
  emitNode(n, "", "shape=doublecircle width=.2 style=empty");
  file << "}" << std::endl;
}

void GraphvizPrinter::visitWrite(const Write *n) {
  emitNode(n, "W" + n->var + " = " + to_string(n->value));
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
}

void GraphvizPrinter::visitRead(const Read *n) {
  std::string label = "R" + n->var + " = ";

  std::visit(overloaded{
    [&](const Read::SuccessfulRead& success) {
      label += to_string(success.value);
    },
    [&](const Conflict& conflict) {
      label += "conflict";
    }
  }, n->read_result);

  emitNode(n, label);
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());

  if (auto* conflict = std::get_if<Conflict>(&n->read_result)) {
    emitConflict(n, *conflict);
  } else {
    auto& success = std::get<Read::SuccessfulRead>(n->read_result);
    if (success.source) {
      emitReadFromEdge(n, success.source.get());
    }
  }
}

void GraphvizPrinter::visitVolatileWrite(const VolatileWrite *n) {
  emitNode(n, "W" + n->var + " = " + to_string(n->value));
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
  // write->write synchronisation order (release chain).
  if (n->sync_predecessor)
    emitSyncEdge(n->sync_predecessor.get(), n);
  // A concurrent write-write race renders the node as an error.
  if (n->conflict)
    emitConflict(n, n->conflict.value());
}

void GraphvizPrinter::visitVolatileRead(const VolatileRead *n) {
  std::string label = "R" + n->var + " = ";

  std::visit(overloaded{
    [&](const Read::SuccessfulRead& success) {
      label += to_string(success.value);
    },
    [&](const Conflict& conflict) {
      label += "conflict";
    }
  }, n->read_result);

  emitNode(n, label);
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());

  if (auto* conflict = std::get_if<Conflict>(&n->read_result)) {
    emitConflict(n, *conflict);
  } else {
    auto& success = std::get<Read::SuccessfulRead>(n->read_result);
    // write->read is an acquire: a synchronisation edge, not a reads-from edge.
    if (success.source) {
      emitSyncEdge(success.source.get(), n);
    }
  }
}

void GraphvizPrinter::visitSpawn(const Spawn *n) {
  emitNode(n, "Spawn " + std::to_string(n->tid));
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
  if (n->spawned) {
    emitSyncEdge(n, n->spawned.get());
    visitProgramOrder(n->spawned.get());
  }
}

void GraphvizPrinter::visitJoin(const Join *n) {
  emitNode(n, "Join " + std::to_string(n->tid));
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
  if (n->joinee)
    emitSyncEdge(n->joinee.get(), n);
  if (n->conflict)
    emitConflict(n, n->conflict.value());
}

void GraphvizPrinter::visitLock(const Lock *n) {
  emitNode(n, "Lock " + n->var);
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
  if (n->ordered_after)
    emitSyncEdge(n->ordered_after.get(), n);
  if (n->conflict)
    emitConflict(n, n->conflict.value());
}

void GraphvizPrinter::visitUnlock(const Unlock *n) {
  emitNode(n, "Unlock " + n->var);
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
}

void GraphvizPrinter::visitAssertion(const Assertion *n) {
  emitNode(n, "Assert " + n->cond);
  if (!n->passed) emitFillColor(n, "red");
  // emitShape(n, "doubleoctagon");
  emitProgramOrderEdge(n, n->next.get());
  visitProgramOrder(n->next.get());
}

void GraphvizPrinter::visitPending(const Pending *n) {
  assert(!n->next);
  emitNode(n, "" + n->statement + "", "style=dashed");
  file << "}" << std::endl;
}

} // namespace graph
} // namespace gitmem
