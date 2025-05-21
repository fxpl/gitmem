#include "../internal.hh"

namespace gitmem
{
    using namespace trieste;

    PassDef expressions()
    {
        auto Operand = T(Expr) << (T(Reg, Var, Const));
        return {
            "expressions",
            expressions_wf,
            dir::bottomup,
            {
                --In(Expr) * T(Const, Reg, Var)[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Expr << _(Expr);
                    },

                --In(Expr) * T(Spawn)[Spawn] << T(Brace) >>
                    [](Match &_) -> Node
                    {
                        return Expr << _(Spawn);
                    },

                --In(Expr) * T(Eq)[Eq] << (Operand * Operand * End) >>
                    [](Match &_) -> Node
                    {
                        return Expr << _(Eq);
                    },

                T(Group) << (T(Brace)[Brace] * End) >>
                    [](Match &_) -> Node
                    {
                        return _(Brace);
                    },

                T(Group) << (T(Paren)[Paren] * End) >>
                    [](Match &_) -> Node
                    {
                        return _(Paren);
                    },

                T(Group) << (T(Expr)[Expr] * End) >>
                    [](Match &_) -> Node
                    {
                        return _(Expr);
                    },

                // Error rules
                In(Group) * T(Expr) * Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Unexpected term (did you forget a semicolon?)");
                    },

                In(Group) * Any * T(Expr)[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Unexpected expression");
                    },

                T(Spawn)[Spawn] << End >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Spawn))
                                     << (ErrorMsg ^ "Expected body of spawn");
                    },

                --In(Expr) * T(Spawn) << Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid body of spawn");
                    },

                --In(Expr) * T(Eq)[Eq] << (Any * End) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Eq))
                                     << (ErrorMsg ^ "Expected right-hand side of equality");
                    },

                --In(Expr) * T(Eq)[Eq] << Any >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Eq))
                                     << (ErrorMsg ^ "Bad equality");
                    },

                Any * T(Brace)[Brace] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Brace))
                                     << (ErrorMsg ^ "Unexpected block");
                    },

                T(Brace)[Brace] * Any >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Brace))
                                     << (ErrorMsg ^ "Expected semicolon after block");
                    },

                Any * T(Paren)[Paren] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Paren))
                                     << (ErrorMsg ^ "Unexpected parenthesis");
                    },

                T(Paren) * Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Unexpected term (did you forget a semicolon?)");
                    },

            }};
    }

}
