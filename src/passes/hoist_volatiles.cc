#include "../internal.hh"

namespace gitmem {

namespace lang {

using namespace trieste;

// Give every statement at most one volatile (synchronisation) access.
//
// A statement executes atomically and is a single scheduling point, so a
// statement that performs more than one volatile access -- e.g. `@v = @v + 1`,
// which reads @v and writes @v -- collapses two sync actions into one. The
// scheduler can then never interleave between the read and the write, so the
// read/write race (both threads read the old value, both write) is never
// explored.
//
// This pass hoists volatile reads that share a statement with another sync
// action into their own preceding `$tmp = @v;` statement (lifted to the
// enclosing block), until the only volatile read left in any statement is a
// bare `$reg = @v` -- which is already its own scheduling point. A volatile
// read feeding an ordinary register/variable (`$r = @v`, `x = @v`) is the sole
// sync action of its statement and is left alone.
//
// Generated statements keep their original AST locations; the state/graph
// listings pretty-print statements from the tree (see render.hh), so the split
// is shown faithfully without perturbing the file positions used in diagnostics.
PassDef hoist_volatiles() {
  return {
      "hoist_volatiles",
      statements_wf,
      dir::bottomup,
      {
          // A volatile read used as an operand of arithmetic or a comparison
          // is not isolated: hoist it into a fresh temporary.
          In(Add, Eq, Neq) * (T(Expr) << (T(Volatile)[Volatile] * End)) >>
              [](Match &_) -> Node {
            auto tmp = _.fresh({"vtmp"});
            return Seq << (Lift << Block
                                << (Stmt << (Assign << (Reg ^ tmp)
                                                    << (Expr << _(Volatile)))))
                       << (Expr << (Reg ^ tmp));
          },

          // `@v = @w;` reads @w and writes @v in one statement: hoist the read
          // so the write stands alone. (A read into a register or ordinary
          // variable is the statement's only sync action, so is not matched.)
          T(Assign)
                  << (T(Volatile)[LVal] *
                      (T(Expr) << (T(Volatile)[Volatile] * End))) >>
              [](Match &_) -> Node {
            auto tmp = _.fresh({"vtmp"});
            return Seq << (Lift << Block
                                << (Stmt << (Assign << (Reg ^ tmp)
                                                    << (Expr << _(Volatile)))))
                       << (Assign << _(LVal) << (Expr << (Reg ^ tmp)));
          },
      }};
}

} // namespace lang

} // namespace gitmem
