#include "../internal.hh"

namespace gitmem
{
    using namespace trieste;

    PassDef statements()
    {
        auto RVal = T(Expr) << (T(Reg, Var, Add, Const, Spawn));
        auto Condition = T(Expr) << (T(Eq, Neq));
        return {
            "statements",
            statements_wf,
            dir::bottomup,
            {
                // Make Semi into Block
                In(File) * T(Semi)[Semi] >>
                    [](Match &_) -> Node
                    {
                        return Block << *_(Semi);
                    },

                T(Brace) << T(Semi)[Semi] >>
                    [](Match &_) -> Node
                    {
                        return Block << *_(Semi);
                    },

                // Statements
                --In(Stmt) * T(Nop)[Nop] >>
                    [](Match &_) -> Node
                    {
                        return Stmt << _(Nop);
                    },

                --In(Stmt) * T(Join)[Join] << (RVal * End) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << _(Join);
                    },

                --In(Stmt) * T(Lock) << ((T(Expr) << T(Var)[Var]) * End) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (Lock << _(Var));
                    },

                --In(Stmt) * T(Unlock) << ((T(Expr) << T(Var)[Var]) * End) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (Unlock << _(Var));
                    },

                --In(Stmt) * T(Assign) << ((T(Expr) << (T(Reg, Var)[LVal] * End)) * RVal[Expr] * End) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (Assign << _(LVal)
                                               << _(Expr));
                    },

                --In(Stmt) * T(Assert) << (Condition[Expr] * End) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (Assert << _(Expr));
                    },

                --In(Stmt) * (T(Group) << (T(If) << (T(Group) << (Condition[Expr] * T(Block)[Then])) * End))
                           * (T(Group) << ((T(Else) << T(Block)[Else]) * End)) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (If << _(Expr) << _(Then) << _(Else));
                    },

                --In(Stmt) * (T(Group) << (T(If) << (T(Group) << (T(Expr)[Expr] * T(Block)[Then])) * End)) >>
                    [](Match &_) -> Node
                    {
                        return Stmt << (If << _(Expr)
                                           << _(Then)
                                           << (Block << ((Stmt ^ "nop") << Nop)));
                    },

                T(Group) << (T(Stmt)[Stmt] * End) >>
                    [](Match &_) -> Node
                    {
                        return _(Stmt);
                    },

                // Error rules
                In(Group) * T(Stmt) * Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Unexpected term");
                    },

                T(Brace, File)[Brace] << End >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Brace))
                                     << (ErrorMsg ^ "Expected statement");
                    },

                T(Paren)[Paren] << End >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Paren))
                                     << (ErrorMsg ^ "Expected expression");
                    },

                --In(Spawn) * T(Brace)[Brace] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Brace))
                                     << (ErrorMsg ^ "Unexpected block");
                    },

                --In(Stmt) * T(Join) << End >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Join))
                                     << (ErrorMsg ^ "Expected thread identifier");
                    },

                --In(Stmt) * T(Join) << Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid thread identifier");
                    },

                --In(Stmt) * T(Lock, Unlock) << End >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Lock))
                                     << (ErrorMsg ^ "Expected lock identifier");
                    },

                --In(Stmt) * T(Lock, Unlock) << Any[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid lock identifier");
                    },

                --In(Stmt) * T(Assign) << (Any * End) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Assign))
                                     << (ErrorMsg ^ "Expected right-hand side to assignment");
                    },

                --In(Stmt) * T(Assign) << ((T(Expr) << T(Reg, Var)) * Any[Expr]) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid right-hand side to assignment");
                    },

                --In(Stmt) * T(Assign) << Any[LVal] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(LVal))
                                     << (ErrorMsg ^ "Invalid left-hand side to assignment");
                    },

                --In(Stmt) * T(Assert)[Assert] << (T(Group) << End) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Assert))
                                     << (ErrorMsg ^ "Expected condition");
                    },

                --In(Stmt) * T(Assert) << (Any[Expr] * End) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid assertion");
                    },

                In(If) * (Start * T(Block)[Expr]) / (T(Group) << (!Condition)[Expr]) >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Invalid condition");
                    },

                In(File, Brace) * T(Stmt)[Stmt] >>
                [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Stmt))
                                     << (ErrorMsg ^ "Expected semicolon");
                    },

                In(Brace, File, Semi) * (!T(Stmt, Semi, Block))[Expr] >>
                    [](Match &_) -> Node
                    {
                        return Error << (ErrorAst << _(Expr))
                                     << (ErrorMsg ^ "Expected statement");
                    },
            }};
    }

}
