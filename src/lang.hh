#pragma once
#include <trieste/trieste.h>

namespace gitmem
{
  using namespace trieste;

  Reader reader();

  // Variables
  inline const auto Reg = TokenDef("reg", flag::print);
  inline const auto Var = TokenDef("var", flag::print);

  // Constants
  inline const auto Const = TokenDef("const", flag::print);

  // Arithmetic
  inline const auto Add = TokenDef("+");

  // Comparison
  inline const auto Eq = TokenDef("==");
  inline const auto Neq = TokenDef("!=");

  // Statements
  inline const auto Semi = TokenDef(";");
  inline const auto Assign = TokenDef("=", flag::lookup);
  inline const auto Spawn = TokenDef("spawn");
  inline const auto Join = TokenDef("join");
  inline const auto Lock = TokenDef("lock");
  inline const auto Unlock = TokenDef("unlock");
  inline const auto Nop = TokenDef("nop");
  inline const auto Assert = TokenDef("assert");
  inline const auto If = TokenDef("if");
  inline const auto Else = TokenDef("else");

  // Branching
  inline const auto Jump = TokenDef("jump");
  inline const auto Cond = TokenDef("cond");

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
  inline const auto Then = TokenDef("then");

  // Well-formedness
  // clang-format off
  inline const wf::Wellformed wf =
    (Top <<= File)
  | (File <<= Block)
  | (Block <<= Stmt++[1])
  | (Expr <<= (Reg | Var | Const | Spawn | Eq | Neq | Add))
  | (Spawn <<= Block)
  | (Eq <<= (Lhs >>= Expr) * (Rhs >>= Expr))
  | (Neq <<= (Lhs >>= Expr) * (Rhs >>= Expr))
  | (Add <<= Expr++[2])
  | (Stmt <<= (Nop | Assign | Join | Lock | Unlock | Assert | If))
  | (Assign <<= ((LVal >>= (Reg | Var)) * Expr))[LVal]
  | (Join <<= Expr)
  | (Lock <<= Var)
  | (Unlock <<= Var)
  | (Assert <<= Expr)
  | (Jump <<= Const)
  | (Cond <<= Expr * Const)
  ;
  // clang-format on

}
