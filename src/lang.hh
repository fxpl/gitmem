#pragma once
#include <trieste/trieste.h>

namespace gitmem
{
  using namespace trieste;

  Reader reader();
  int interpret(const Node ast);

  // Variables
  inline const auto Reg = TokenDef("reg", flag::print);
  inline const auto Var = TokenDef("var", flag::print);

  // Constants
  inline const auto Const = TokenDef("const", flag::print);

  // Comparison
  inline const auto Eq = TokenDef("==");

  // Statements
  inline const auto Semi = TokenDef(";");
  inline const auto Assign = TokenDef("=", flag::lookup);
  inline const auto Spawn = TokenDef("spawn");
  inline const auto Join = TokenDef("join");
  inline const auto Lock = TokenDef("lock");
  inline const auto Unlock = TokenDef("unlock");
  inline const auto Nop = TokenDef("nop");
  inline const auto Assert = TokenDef("assert");

  // Grouping tokens
  inline const auto Brace = TokenDef("brace");
  inline const auto Paren = TokenDef("paren");

  inline const auto Stmt = TokenDef("stmt");
  inline const auto Expr = TokenDef("expr");
  inline const auto Block = TokenDef("block", flag::symtab | flag::defbeforeuse);

  // Convenience
  inline const auto LVal = TokenDef("lval");
  inline const auto Lhs = TokenDef("lhs");
  inline const auto Rhs = TokenDef("rhs");
  inline const auto Op = TokenDef("op");

  // Well-formedness
  // clang-format off
  inline const wf::Wellformed wf =
    (Top <<= File)
  | (File <<= Block)
  | (Block <<= Stmt++[1])
  | (Expr <<= (Reg | Var | Const | Spawn | Eq))
  | (Spawn <<= Block)
  | (Eq <<= (Lhs >>= Expr) * (Rhs >>= Expr))
  | (Stmt <<= (Nop | Assign | Join | Lock | Unlock | Assert))
  | (Assign <<= ((LVal >>= (Reg | Var)) * Expr))[LVal]
  | (Join <<= Expr)
  | (Lock <<= Var)
  | (Unlock <<= Var)
  | (Assert <<= Expr)
  ;
  // clang-format on

}
