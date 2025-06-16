#include "graph.hh"
#include <iostream>
#include <sstream>
#include <cassert>

namespace gitmem
{

  namespace graph
  {
    MermaidPrinter::MermaidPrinter(std::string filename) noexcept {
        file.open(filename);
        file << "```mermaid" << std::endl;
        file << "flowchart TB" << std::endl;
    }

    void MermaidPrinter::visitStart(const Start* n)
    {
      file << "subgraph Thread " << n->id << std::endl;
      file << "\tdirection TB" << std::endl;
      file << "\t" << (size_t)n << "(start)" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }
    }

    void MermaidPrinter::visitEnd(const End* n)
    {
      assert(!n->next);
      file << "\t" << (size_t)n << "(end)" << std::endl;
      file << "end" << std::endl;
    }

    void MermaidPrinter::visitWrite(const Write* n)
    {
      file << "\t" << (size_t)n << "(write " << n->var << " = " << n->value << " : #" << n->id << ")" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }
    }

    void MermaidPrinter::visitRead(const Read* n)
    {
      file << "\t" << (size_t)n << "(read " << n->var << " = " << n->value << " : #" << n->id << ")" << std::endl;
      assert(n->sauce);
      file << "\t" << (size_t)n << " -.rf.-> " << (size_t)n->sauce.get() << std::endl;

      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }
    }

    void MermaidPrinter::visitSpawn(const Spawn* n)
    {
      file << "\t" << (size_t)n << "(spawn " << n->tid << ")" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }

      if (const Node* spawned = n->spawned.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)spawned << std::endl;
        spawned->accept(this);
      }
    }

    void MermaidPrinter::visitJoin(const Join* n)
    {
      file << "\t" << (size_t)n << "(join Thread " << n->tid << ")" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }

      if (const Node* joinee = n->joinee.get())
      {
        file << "\t" << (size_t)joinee << " --> " << (size_t)n  << std::endl;
      }

      if (n->conflict)
      {
        file << "\tstyle " << (size_t)n << " fill:red" << std::endl;
        auto [s1, s2] = n->conflict->sources;
        file << "\t" << (size_t)n << " -.-> " << (size_t)s1.get() << std::endl;
        file << "\t" << (size_t)n << " -.-> " << (size_t)s2.get() << std::endl;
      }
    }

    void MermaidPrinter::visitLock(const Lock* n)
    {
      file << "\t" << (size_t)n << "(lock " << n->var << ")" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }

      if (const Node* ordered_after = n->ordered_after.get())
      {
        file << "\t" << (size_t)ordered_after << " -->" << (size_t)n << std::endl;
      }

      if (n->conflict)
      {
        file << "\tstyle " << (size_t)n << " fill:red" << std::endl;
        auto [s1, s2] = n->conflict->sources;
        file << "\t" << (size_t)n << " -.-> " << (size_t)s1.get() << std::endl;
        file << "\t" << (size_t)n << " -.-> " << (size_t)s2.get() << std::endl;
      }
    }

    void MermaidPrinter::visitUnlock(const Unlock* n)
    {
      file << "\t" << (size_t)n << "(unlock " << n->var << ")" << std::endl;

      assert(n->next);
      if (const Node* next = n->next.get())
      {
        file << "\t" << (size_t)n << " --> " << (size_t)next << std::endl;
        next->accept(this);
      }
    }

  }

}