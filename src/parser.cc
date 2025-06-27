#include "lang.hh"
#include "internal.hh"

namespace gitmem
{
  using namespace trieste;
  using namespace trieste::detail;

  Parse parser()
  {
    Parse p(depth::file, parser_wf);
    auto infix = [](Make& m, Token t) {
      // This precedence table maps infix operators to the operators that have
      // *higher* precedence, and which should therefore be terminated when that
      // operator is encountered. Note that operators with the same precedence
      // terminate each other. (for reasons, it has to be defined inside the lambda)
      const auto precedence_table = std::map<Token, std::initializer_list<Token>> {
        {Add,    {}},
        {Eq,     {Add}},
        {Neq,    {Add}},
        {Assign, {Add, Eq, Neq}},
      };

      auto skip = precedence_table.at(t);
      m.seq(t, skip);
      // Push group to be able to check whether an operand follows
      m.push(Group);
    };

/*
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

        // Addition
        R"(\+)" >> [infix](auto& m) { infix(m, Add); },

        // Comparison
        "==" >> [infix](auto& m) { infix(m, Eq); },
        "!=" >> [infix](auto& m) { infix(m, Neq); },

        // Statements
        ";" >> [](auto& m) { m.seq(Semi, {Assign, Spawn, Join, Lock, Unlock, Assert, If, Else, Eq, Neq, Add, Group}); },
        "=" >> [infix](auto& m) { infix(m, Assign); },
        "spawn" >> [](auto& m) { m.push(Spawn); },
        "join" >> [](auto& m) { m.push(Join); },
        "lock" >> [](auto& m) { m.push(Lock); },
        "unlock" >> [](auto& m) { m.push(Unlock); },
        "assert" >> [](auto& m) { m.push(Assert); },
        "nop" >> [](auto& m) { m.add(Nop); },

        "if" >> [](auto& m) { m.push(If); },
        "else" >> [pop_until](auto &m)
        {
          pop_until(m, Semi, {Brace, Paren, File});
          m.push(Else);
        },

        // Variables
        R"(\$[_[:alpha:]][_[:alnum:]]*)" >> [](auto& m) { m.add(Reg); },
        R"([_[:alpha:]][_[:alnum:]]*)" >> [](auto& m) { m.add(Var); },

        // Grouping
        "\\{" >> [](auto& m) { m.push(Brace); },
        "\\}" >> [pop_until](auto& m)
        {
          pop_until(m, Brace, {Paren});
          m.term();
          m.pop(Brace);
          m.extend(Brace);
          if (m.group_in(If))
          {
            m.term();
            m.pop(If);
          }
          else if (m.group_in(Else))
          {
            m.term();
            m.pop(Else);
          }
          if (m.group_in({Semi, Brace, File}))
          {
            m.seq(Semi);
          }
        },

        "\\(" >> [](auto& m) { m.push(Paren); },
        "\\)" >> [pop_until](auto& m)
        {
          pop_until(m, Paren, {Brace});
          m.term();
          m.pop(Paren);
          m.extend(Paren);
        },
    }
    );

    p.done([pop_until](auto& m) {
      if (!m.in(Semi))
        m.error("Expected ';' at end of file");
      pop_until(m, File, {Brace, Paren});
    });

    return p;
  }
}
