#pragma once
#include "lang.hh"

namespace gitmem
{
  using namespace trieste;

  Parse parser();
  PassDef expressions();
  PassDef statements();
  PassDef check_refs();

  inline const auto parse_token =
     Reg | Var | Const | Nop | Brace | Paren |
     Spawn | Join | Lock | Unlock | Assert;

  inline const auto parse_op = Group | Assign | Eq | Neq | Add | Semi;

  // clang-format off
	inline const wf::Wellformed parser_wf =
		(Top <<= File)
		| (File	<<= ~parse_op)
    | (Semi <<= (parse_op - Semi)++[1])
    | (Assign <<= (parse_op - Assign)++[2])
    | (Eq <<= parse_op++[2])
    | (Neq <<= parse_op++[2])
    | (Add <<= parse_op++[2])
    | (Spawn <<= ~parse_op)
    | (Join <<= ~parse_op)
    | (Lock <<= ~parse_op)
    | (Unlock <<= ~parse_op)
    | (Assert <<= ~parse_op)
    | (Brace <<= ~parse_op)
    | (Paren <<= ~parse_op)
    | (Group <<= parse_token++)
		;

		inline const auto expressions_token = parse_token - Reg - Var - Const - Spawn - Brace - Paren;
    inline const auto expressions_op = parse_op | Brace | Paren | Expr;

  inline const wf::Wellformed expressions_wf =
    parser_wf
    | (File <<= ~expressions_op)
    | (Expr <<= (Reg | Var | Const | Spawn | Eq | Neq | Add))
    | (Brace <<= ~expressions_op)
    | (Paren <<= ~expressions_op)
    | (Semi <<= (expressions_op - Semi)++[1])
    | (Assign <<= (expressions_op - Assign)++[1])
    | (Spawn <<= Brace)
    | (Eq <<= (Lhs >>= Expr) * (Rhs >>= Expr))
    | (Neq <<= (Lhs >>= Expr) * (Rhs >>= Expr))
    | (Add <<= Expr++[2])
    | (Join <<= ~expressions_op)
    | (Lock <<= ~expressions_op)
    | (Unlock <<= ~expressions_op)
    | (Assert <<= ~expressions_op)
    | (Group <<= expressions_token++)
    ;

  inline const wf::Wellformed statements_wf =
    expressions_wf - Group - Semi - Brace - Paren
    | (File <<= Block)
    | (Spawn <<= Block)
    | (Block <<= Stmt++[1])
    | (Stmt <<= (Nop | Assign | Join | Lock | Unlock | Assert))
    | (Assign <<= ((LVal >>= (Reg | Var)) * Expr))[LVal]
    | (Join <<= Expr)
    | (Lock <<= Var)
    | (Unlock <<= Var)
    | (Assert <<= Expr)
    ;
  // clang-format on
}
