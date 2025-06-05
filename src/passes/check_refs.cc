#include "../internal.hh"

namespace gitmem
{
    using namespace trieste;

    PassDef check_refs()
    {
        return {
            "check_refs",
            wf,
            dir::bottomup | dir::once,
            {
                In(Expr) * T(Reg)[Reg] >>
                    [](Match &_) -> Node
                    {
                        auto reg = _(Reg);
                        auto enclosing_block = reg->scope();
                        auto bindings = reg->lookup(enclosing_block);
                        if (bindings.empty())
                        {
                            return Error << (ErrorAst << _(Reg))
                                         << (ErrorMsg ^ "Register has not been assigned");
                        }
                        return NoChange;
                    },
            }};
    }

}
