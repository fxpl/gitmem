#include "../internal.hh"

namespace gitmem
{
    using namespace trieste;

    PassDef expressions()
    {
        auto Operand = T(Expr) << (T(Reg, Var, Const, Add));
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

                --In(Expr) * T(Spawn)[Spawn] << (T(Brace) * End) >>
                    [](Match &_) -> Node
                    {
                        return Expr << _(Spawn);
                    },

                // Additions must have *at least* two operands
                --In(Expr) * T(Add)[Add] << (Operand * Operand) >>
                    [](Match &_) -> Node
                    {
                        return Expr << _(Add);
                    },

                --In(Expr) * T(Eq, Neq)[Eq] << (Operand * Operand * End) >>
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

                T(Paren) << (T(Expr)[Expr] * End) >>
                    [](Match &_) -> Node
                    {
                        return _(Expr);
                    },

                // Error rules
                In(Group) * T(Expr) * (!T(Brace))[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Unexpected term (did you forget a brace or a semicolon?)");
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

                --In(Expr) * T(Add)[Add] << ((T(Group) << End) / (Any * (T(Group) << End))) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Add))
                                     << (ErrorMsg ^ "Expected operand");
                    },

                --In(Expr) * T(Add)[Add] << (Any) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Add))
                                     << (ErrorMsg ^ "Invalid operands for addition");
                    },


                --In(Expr) * T(Eq, Neq)[Eq] << (Any * (T(Group) << End)) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Eq))
                                     << (ErrorMsg ^ "Expected right-hand side of equality");
                    },

                --In(Expr) * T(Eq, Neq)[Eq] << Any >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Eq))
                                     << (ErrorMsg ^ "Bad equality");
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
                                     << (ErrorMsg ^ "Unexpected term (did you forget a brace or semicolon?)");
                    },

            }};
    }

}
