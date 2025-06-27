#include "../internal.hh"

namespace gitmem
{
    using namespace trieste;

    PassDef branching()
    {
        return {
            "branching",
            branching_wf,
            dir::bottomup | dir::once,
            {
                T(Stmt) << (T(If)[If] << (T(Expr)[Expr] * T(Block)[Then] * T(Block)[Else])) >>
                    [](Match &_) -> Node
                    {
                        auto then_length = std::to_string(_(Then)->size() + 1 + 1); // +1 for the jump
                        auto else_length = std::to_string(_(Else)->size() + 1);
                        auto cond_loc = Location("if (" + std::string(_(Expr)->location().view()) + ") jump " + then_length);
                        auto jump_loc = Location("jump " + else_length);
                        auto cond = (Stmt ^ cond_loc) << (Cond << _(Expr) << (Const ^ then_length));
                        auto jump = (Stmt ^ jump_loc) << (Jump << (Const ^ else_length));
                        return Seq << cond
                                   << *_(Then)
                                   << jump
                                   << *_(Else);
                    },
            }};
    }

}
