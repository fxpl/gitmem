#pragma once

#include "lang.hh"
#include <string>

namespace gitmem {

namespace lang {

using namespace trieste;

// Pretty-print a statement or expression from the AST, so listings reflect the
// actual (possibly pass-transformed) tree rather than the original source span.
// Node locations are left untouched -- they still carry the true file position
// used for diagnostics -- this is display only.
inline std::string render(const Node &n);

inline std::string render_block(const Node &block) {
  std::string s = "{\n";
  for (const auto &stmt : *block)
    s += "  " + render(stmt) + ";\n";
  s += "}";
  return s;
}

inline std::string render(const Node &n) {
  const auto &t = n->type();

  if (t == Reg || t == Var || t == Volatile || t == Const)
    return std::string(n->location().view());

  if (t == Expr || t == Stmt)
    return render(n->front());

  if (t == Add) {
    std::string s;
    bool first = true;
    for (const auto &c : *n) {
      if (!first)
        s += " + ";
      first = false;
      s += render(c);
    }
    return s;
  }

  if (t == Eq)
    return render(n / Lhs) + " == " + render(n / Rhs);
  if (t == Neq)
    return render(n / Lhs) + " != " + render(n / Rhs);

  if (t == Spawn)
    return "spawn " + render_block(n / Block);
  if (t == Assign)
    return render(n / LVal) + " = " + render(n / Expr);
  if (t == Assert)
    return "assert(" + render(n / Expr) + ")";
  if (t == Join)
    return "join " + render(n / Expr);
  if (t == Lock)
    return "lock " + render(n / Var);
  if (t == Unlock)
    return "unlock " + render(n / Var);
  if (t == Nop)
    return "nop";
  if (t == Jump)
    return "jump " + render(n / Const);
  if (t == Cond)
    return "if (" + render(n / Expr) + ") jump " + render(n / Const);

  // Fallback: original source span.
  return std::string(n->location().view());
}

} // namespace lang

} // namespace gitmem
