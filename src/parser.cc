#include "lang.hh"
#include "internal.hh"

namespace gitmem
{
  using namespace trieste;
  using namespace trieste::detail;

  Parse parser()
  {
    Parse p(depth::file, parser_wf);
/*
    auto infix = [](Make& m, Token t) {
      // This precedence table maps infix operators to the operators that have
      // *higher* precedence, and which should therefore be terminated when that
      // operator is encountered. Note that operators with the same precedence
      // terminate each other. (for reasons, it has to be defined inside the lambda)
      // TODO: Should be able to have prefix operators as well (e.g. Not)
      // TODO: Can this be computed from something more declarative?
      // {
      //  {Dot},
      //  {Mul, Div},
      //  {Add, Sub},
      //  {Colon},
      //  {LT, Equals},
      //  {And},
      //  {Or},
      //  {Assign},
      // }
      const auto precedence_table = std::map<Token, std::initializer_list<Token>> {
        {Dot,    {}},
        {Mul,    {Dot, Div}},
        {Div,    {Dot, Mul}},
        {Add,    {Dot, Sub, Mul, Div}},
        {Sub,    {Dot, Add, Mul, Div}},
        {Colon,  {Dot, Add, Sub, Mul, Div}},
        {LT,     {Dot, Add, Sub, Mul, Div, Colon, Equals}},
        {Equals, {Dot, Add, Sub, Mul, Div, Colon, LT}},
        {And,    {Dot, Add, Sub, Mul, Div, Colon, LT, Equals}},
        {Or,     {Dot, Add, Sub, Mul, Div, Colon, LT, Equals, And}},
        {Assign, {Dot, Add, Sub, Mul, Colon, LT, Equals, Or, And}},
      };

      auto skip = precedence_table.at(t);
      m.seq(t, skip);
      // Push group to be able to check whether an operand follows
      m.push(Group);
    };

    auto pair_with = [pop_until](Make &m, Token preceding, Token following) {
      pop_until(m, preceding, {Paren, Brace, File});
      m.term();

      if (!m.in(preceding)) {
        const std::string msg = (std::string) "Unexpected '" + following.str() + "'";
        m.error(msg);
        return;
      }

      m.pop(preceding);
      m.push(following);
    };
*/

    auto pop_until = [](Make &m, Token t, std::initializer_list<Token> stop = {File}) {
      while (!m.in(t) && !m.group_in(t)
             && !m.in(stop) && !m.group_in(stop)) {
        m.term();
        m.pop();
      }

      return (m.in(t) || m.group_in(t));
    };

    p("start",
    {
        // Whitespace
        "[[:space:]]+" >> [](auto&) { }, // no-op

        // Line comment
        "//[^\n]*" >> [](auto&) { }, // no-op

        // Constant
        "[[:digit:]]+" >> [](auto& m) { m.add(Const); },

        // Comparison
        "==" >> [](auto& m) { m.seq(Eq); },

        // Statements
        ";" >> [](auto& m) { m.seq(Semi, {Assign, Spawn, Join, Lock, Unlock, Assert, Eq, Group}); },
        "=" >> [](auto& m) { m.seq(Assign); },
        "spawn" >> [](auto& m) { m.push(Spawn); },
        "join" >> [](auto& m) { m.push(Join); },
        "lock" >> [](auto& m) { m.push(Lock); },
        "unlock" >> [](auto& m) { m.push(Unlock); },
        "assert" >> [](auto& m) { m.push(Assert); },
        "nop" >> [](auto& m) { m.add(Nop); },

        // Variables
        R"(\$[_[:alpha:]][_[:alnum:]]*)" >> [](auto& m) { m.add(Reg); },
        R"([_[:alpha:]][_[:alnum:]]*)" >> [](auto& m) { m.add(Var); },

        // Grouping
        "\\{" >> [](auto& m) { m.push(Brace); },
        "\\}" >> [pop_until](auto& m) { pop_until(m, Brace, {Paren}); m.term(); m.pop(Brace); m.extend(Brace); },

        "\\(" >> [](auto& m) { m.push(Paren); },
        "\\)" >> [pop_until](auto& m) { pop_until(m, Paren, {Brace}); m.term(); m.pop(Paren); m.extend(Paren); },
    }
    );

    p.done([pop_until](auto& m) {
      pop_until(m, File, {Brace, Paren});
    });

    return p;
  }
}
