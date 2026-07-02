#include "tikz.hh"
#include "overloaded.hh"
#include <fstream>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <optional>
#include <cstdio>

namespace gitmem {
namespace graph {

static std::string latex_escape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '_': r += "\\_";  break;
      case '$': r += "\\$";  break;
      case '&': r += "\\&";  break;
      case '%': r += "\\%";  break;
      case '#': r += "\\#";  break;
      case '{': r += "\\{";  break;
      case '}': r += "\\}";  break;
      default:  r += c;      break;
    }
  }
  return r;
}

static std::string node_id(size_t tid, size_t idx) {
  return "t" + std::to_string(tid) + "e" + std::to_string(idx);
}

static std::string fmt(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

// Formatter macros from the paper (verbatim). Stored as a raw string so that
// LaTeX special characters (backslashes, #, etc.) are preserved.
static const char* FORMATTER = R"(
\newlength{\codeboxleftshift}
\newlength{\codeboxrightshift}
\setlength{\codeboxrightshift}{3pt}
\tikzset{
  common timeline styles/.style={
    >={Latex[scale=0.8]},
    every node/.style={font=\scriptsize\ttfamily},
    codebox/.style={rounded corners=1.5pt, inner sep=1pt},
    thread lane/.style={ultra thick, draw=threadtime, dashed},
    shared lane/.style={thread lane, draw=sharedmem},
    history link/.style={line width=0.8pt},
    link left/.style={history link, {Latex[round, scale=1.1]}-, draw=communicationblue},
    link right/.style={history link, -{Latex[round, scale=1.1]}, draw=communicationblue},
    link oneway/.style={history link, -{Latex[round, scale=1.1]}, draw=communicationblue},
    link both/.style={history link,
      {Latex[round, scale=1.1]}-{Latex[round, scale=1.1]},
      draw=communicationblue},
    conflict/.style={line width=3pt, -{Latex[round,scale=0.8]}, draw=conflictred},
    stateupdate/.style={draw=threadtime, fill=threadtime!15, rounded corners=4pt,
      inner sep=1.5pt, font=\scriptsize\ttfamily},
    sharedupdate/.style={draw=sharedmem!80!black, fill=sharedmem!30, rounded corners=4pt,
      inner sep=1.5pt, font=\scriptsize\ttfamily},
    t1 code/.style={anchor=west, xshift=\the\codeboxleftshift, codebox},
    t2 code/.style={anchor=west, xshift=\the\codeboxrightshift, codebox},
    t1 state/.style={right, xshift=3pt,  stateupdate},
    t2 state/.style={left,  xshift=-3pt, stateupdate},
    g state right/.style={right, xshift=3pt, sharedupdate},
    g state left/.style={left,  xshift=-3pt, sharedupdate},
  }
}
\newcommand{\TimelineColorSetup}{
  \definecolor{threadtime}{rgb}{0.5,0.5,0.5}
  \definecolor{sharedmem}{rgb}{0.3,0.3,0.3}
  \definecolor{communicationblue}{rgb}{0.2,0.4,1}
  \definecolor{conflictred}{rgb}{1,0.2,0.2}
}
\newcommand{\DefineThreadEventMacros}{
  \newcommand{\ResolveThreadX}[1]{%
    \def\tx{\onex}\ifnum\pdfstrcmp{##1}{t2}=0\def\tx{\twox}\fi}
  \newcommand{\ThreadEvent}[4]{%
    \ResolveThreadX{##1}%
    \fill[fill=threadtime] (\tx,##2) circle (2pt) coordinate (##3) node[##1 code] {##4};}
  \newcommand{\ThreadEventFrom}[5]{%
    \fill[fill=threadtime] ($(##2)+(0,##3)$) circle (2pt) coordinate (##4) node[##1 code] {##5};}
  \newcommand{\ThreadSyncEvent}[4]{%
    \ResolveThreadX{##1}%
    \fill (\tx,##2) circle (0pt) coordinate (##3) node[##1 code] {##4};}
  \newcommand{\ThreadSyncEventFrom}[5]{%
    \fill ($(##2)+(0,##3)$) circle (0pt) coordinate (##4) node[##1 code] {##5};}
  \newcommand{\PullPush}[2]{
    \path let \p1 = ##1, \p2 = ##2 in \pgfextra{%
      \ifdim\x1<\x2
        \draw[link oneway] ##2 to[looseness=.5, out=140, in=30] ##1;
        \draw[link oneway] ##1 to[looseness=.5, out=320, in=210] ##2;
      \else
        \draw[link oneway] ##2 to[looseness=.5, out=40, in=150] ##1;
        \draw[link oneway] ##1 to[looseness=.5, out=220, in=330] ##2;
      \fi
    };
  }
  \newcommand{\ConflictEvent}[2]{
    \node[regular polygon, regular polygon sides=8,
        draw=black, fill=black, line width=2pt,
        minimum size=20pt, inner sep=0pt] (##2) at (##1) {};
    \node[regular polygon, regular polygon sides=8,
        draw=white, fill=conflictred, line width=0.9pt,
        minimum size=14pt, inner sep=0pt,
        font=\scriptsize\bfseries\sffamily, text=white] at (##1) {\textsc{fail}};
  }
}
\newcommand{\CodeWidthOf}[1]{\widthof{{\scriptsize\ttfamily\selectfont #1}}}
\newcommand{\SetLeftCodeShift}[1]{%
  \settowidth{\codeboxleftshift}{#1}%
  \setlength{\codeboxleftshift}{-\codeboxleftshift}%
  \addtolength{\codeboxleftshift}{-3mm}%
}
\newcommand{\SetRightCodeShift}[1]{\setlength{\codeboxrightshift}{#1}}
)";

// ─────────────────────────────────────────────────────────────────────────────

struct EventInfo {
  const Node* node;
  std::string name;
  std::string label;
  bool is_sync     = false;
  bool is_conflict = false;
  bool is_pending  = false;
  double y = 0.0;
  size_t tid = 0;
};

struct ConflictEdge {
  const Node* src1;
  const Node* src2;
  const Node* ordered_after; // Unlock that links src1 to the conflict via shared lane
  const Node* conflict_node;
  std::string lock_var;      // non-empty when routed through a lock lane
};

void TikzPrinter::print(const ExecutionGraph& g, const std::filesystem::path& path,
                        bool linear_mode) {
  const double Y_STEP         = -0.8;  // gap between regular events
  const double Y_SYNC_STEP    = -1.2;  // gap when either neighbour is a sync event
  const double Y_SPAWN_OFFSET =  0.0;  // child thread starts at same y as its spawn
  const double Y_JOIN_GAP     =  0.3;  // min gap between joinee's end and join event
  const double Y_LOCK_GAP     =  0.4;  // min gap between ordered_after unlock and lock
  const double SPACING        =  1.8;
  const size_t n_threads = g.threads.size();

  // ── Phase 1: collect events ───────────────────────────────────────────────
  std::vector<std::vector<EventInfo>>      per_thread(n_threads);
  std::unordered_map<const Node*, std::string> node_name;
  std::unordered_map<const Node*, double>      node_y;
  std::unordered_map<const Node*, size_t>      node_tid;
  std::vector<std::string>                     lock_vars;
  std::vector<ConflictEdge>                    conflicts;

  auto is_sync_node = [](const Node* nd) -> bool {
    return dynamic_cast<const Start*>(nd)  || dynamic_cast<const End*>(nd)
        || dynamic_cast<const Spawn*>(nd)  || dynamic_cast<const Join*>(nd)
        || dynamic_cast<const Lock*>(nd)   || dynamic_cast<const Unlock*>(nd);
  };

  auto add_lock_var = [&](const std::string& v) {
    if (std::find(lock_vars.begin(), lock_vars.end(), v) == lock_vars.end())
      lock_vars.push_back(v);
  };

  // Spawned threads start just below their parent spawn event rather than at
  // y=0, so concurrent events in different threads land at different y values.
  std::vector<double> thread_start_y(n_threads, 0.0);

  for (size_t tid = 0; tid < n_threads; ++tid) {
    double y = thread_start_y[tid];
    size_t idx = 0;
    const Node* n = g.threads[tid].get();
    while (n) {
      EventInfo ev;
      ev.node = n;
      ev.name = node_id(tid, idx++);
      ev.y    = y;
      ev.tid  = tid;

      if (auto* nd = dynamic_cast<const Start*>(n)) {
        (void)nd;
        ev.label   = "<start>";
        ev.is_sync = true;
      } else if (auto* nd = dynamic_cast<const End*>(n)) {
        (void)nd;
        ev.label   = "<end>";
        ev.is_sync = true;
      } else if (auto* nd = dynamic_cast<const Write*>(n)) {
        ev.label = "W(" + latex_escape(nd->var) + "$_{" + std::to_string(tid) + "}$) = " + std::to_string(nd->value);
      } else if (auto* nd = dynamic_cast<const Read*>(n)) {
        std::visit(overloaded{
          [&](const Read::SuccessfulRead& sr) {
            ev.label = "R(" + latex_escape(nd->var) + "$_{" + std::to_string(tid) + "}$) = " + std::to_string(sr.value);
          },
          [&](const Conflict&) {
            ev.label       = "R(" + latex_escape(nd->var) + "$_{" + std::to_string(tid) + "}$) = ?";
            ev.is_conflict = true;
          }
        }, nd->read_result);
      } else if (auto* nd = dynamic_cast<const Spawn*>(n)) {
        ev.label   = "spawn";
        ev.is_sync = true;
        if (nd->tid < n_threads && thread_start_y[nd->tid] == 0.0)
          thread_start_y[nd->tid] = y + Y_SPAWN_OFFSET;
      } else if (auto* nd = dynamic_cast<const Join*>(n)) {
        ev.label   = "join";
        ev.is_sync = true;
        if (nd->conflict) {
          ev.is_conflict = true;
          conflicts.push_back({
            nd->conflict->sources.first.get(),
            nd->conflict->sources.second.get(),
            nd->joinee.get(),
            n, ""
          });
        }
      } else if (auto* nd = dynamic_cast<const Lock*>(n)) {
        ev.label   = "lock(" + latex_escape(nd->var) + ")";
        ev.is_sync = true;
        add_lock_var(nd->var);
        if (nd->conflict) {
          ev.is_conflict = true;
          conflicts.push_back({
            nd->conflict->sources.first.get(),
            nd->conflict->sources.second.get(),
            nd->ordered_after.get(),
            n, nd->var
          });
        }
      } else if (auto* nd = dynamic_cast<const Unlock*>(n)) {
        ev.label   = "unlock(" + latex_escape(nd->var) + ")";
        ev.is_sync = true;
        add_lock_var(nd->var);
        if (nd->conflict) {
          ev.is_conflict = true;
          conflicts.push_back({
            nd->conflict->sources.first.get(),
            nd->conflict->sources.second.get(),
            nd->g_predecessor.get(),
            n, ""
          });
        }
      } else if (auto* nd = dynamic_cast<const Assertion*>(n)) {
        ev.label = "assert(" + latex_escape(nd->cond) + ")";
        if (!nd->passed) ev.is_conflict = true;
      } else if (auto* nd = dynamic_cast<const Pending*>(n)) {
        ev.label      = latex_escape(nd->statement);
        ev.is_pending = true;
        ev.is_sync    = true;
      }

      node_name[n] = ev.name;
      node_y[n]    = y;
      node_tid[n]  = tid;
      const Node* next_n = n->next.get();
      // Use extra spacing when the current or next event connects to g so that
      // PullPush arcs and annotation boxes don't crowd adjacent events.
      const bool near_sync = linear_mode
          && (ev.is_sync || (next_n && is_sync_node(next_n)));
      per_thread[tid].push_back(std::move(ev));
      y += near_sync ? Y_SYNC_STEP : Y_STEP;
      n  = next_n;
    }
  }

  // Post-pass 1: enforce causal y-ordering for lock events.
  // A lock that follows an unlock in another thread (via ordered_after) must
  // appear below that unlock — otherwise the diagram looks like a race even
  // when the execution is conflict-free.  Process threads in order so that
  // multi-hop chains (T0.unlock → T1.lock … T1.unlock → T2.lock) propagate
  // correctly in a single pass.
  // This must run BEFORE the join pass so that a joined thread's final
  // position is already correct when the join pass computes its gap.
  for (size_t tid = 0; tid < n_threads; ++tid) {
    for (size_t i = 0; i < per_thread[tid].size(); ++i) {
      auto& ev = per_thread[tid][i];
      if (auto* lk = dynamic_cast<const Lock*>(ev.node)) {
        if (lk->ordered_after) {
          auto it = node_y.find(lk->ordered_after.get());
          if (it != node_y.end()) {
            double required_y = it->second - Y_LOCK_GAP;
            if (ev.y > required_y) {
              double shift = required_y - ev.y;
              for (size_t j = i; j < per_thread[tid].size(); ++j) {
                per_thread[tid][j].y += shift;
                node_y[per_thread[tid][j].node] = per_thread[tid][j].y;
              }
            }
          }
        }
      }
    }
  }

  // Post-pass 1b: enforce causal y-ordering for conflicting unlock events.
  // A conflicting unlock must appear below the predecessor unlock that already
  // pushed to g — otherwise the FAIL node appears before the push it conflicts
  // with, making the diagram look wrong.
  for (size_t tid = 0; tid < n_threads; ++tid) {
    for (size_t i = 0; i < per_thread[tid].size(); ++i) {
      auto& ev = per_thread[tid][i];
      if (auto* ul = dynamic_cast<const Unlock*>(ev.node)) {
        if (ul->g_predecessor) {
          auto it = node_y.find(ul->g_predecessor.get());
          if (it != node_y.end()) {
            double required_y = it->second - Y_LOCK_GAP;
            if (ev.y > required_y) {
              double shift = required_y - ev.y;
              for (size_t j = i; j < per_thread[tid].size(); ++j) {
                per_thread[tid][j].y += shift;
                node_y[per_thread[tid][j].node] = per_thread[tid][j].y;
              }
            }
          }
        }
      }
    }
  }

  // Post-pass 2: if a join event sits above the last event of the joined thread,
  // shift the join and all later events in this thread downward so the join
  // arrow always points up-and-across, not sideways or backward.
  for (size_t tid = 0; tid < n_threads; ++tid) {
    for (size_t i = 0; i < per_thread[tid].size(); ++i) {
      auto& ev = per_thread[tid][i];
      if (auto* jn = dynamic_cast<const Join*>(ev.node)) {
        if (jn->tid < n_threads && !per_thread[jn->tid].empty()) {
          double joinee_end_y = per_thread[jn->tid].back().y;
          double required_y   = joinee_end_y - Y_JOIN_GAP;
          if (ev.y > required_y) {
            double shift = required_y - ev.y;
            for (size_t j = i; j < per_thread[tid].size(); ++j) {
              per_thread[tid][j].y += shift;
              node_y[per_thread[tid][j].node] = per_thread[tid][j].y;
            }
          }
        }
      }
    }
  }

  // Post-pass 3: pull joinee's <end> down to the join's y so that the
  // join arrow is perfectly horizontal, matching the spawn↔start alignment.
  for (size_t tid = 0; tid < n_threads; ++tid) {
    for (auto& ev : per_thread[tid]) {
      if (auto* jn = dynamic_cast<const Join*>(ev.node)) {
        if (jn->tid < n_threads && !per_thread[jn->tid].empty()) {
          auto& end_ev = per_thread[jn->tid].back();
          if (dynamic_cast<const End*>(end_ev.node)) {
            end_ev.y = ev.y;
            node_y[end_ev.node] = ev.y;
          }
        }
      }
    }
  }

  // ── Phase 2: layout ───────────────────────────────────────────────────────
  const bool has_g_lane = linear_mode;
  const size_t n_locks = has_g_lane ? 0 : lock_vars.size();

  std::unordered_map<std::string, size_t> lock_idx;
  for (size_t i = 0; i < n_locks; ++i)
    lock_idx[lock_vars[i]] = i;

  // In linear mode g sits in the middle of the threads.
  const size_t g_mid = has_g_lane ? (n_threads + 1) / 2 : 0;

  // In branching mode interleave threads with their associated locks: T0, L0, T1, L1, …
  // A lock is placed immediately after the first thread that uses it.
  std::unordered_map<size_t, double> thread_col_x;
  std::unordered_map<size_t, double> lock_col_x;
  if (!has_g_lane) {
    std::vector<int> lock_owner(n_locks, -1);
    for (size_t tid = 0; tid < n_threads; ++tid)
      for (auto& ev : per_thread[tid])
        if (auto* nd = dynamic_cast<const Lock*>(ev.node)) {
          size_t li = lock_idx.at(nd->var);
          if (lock_owner[li] < 0) lock_owner[li] = (int)tid;
        } else if (auto* nd = dynamic_cast<const Unlock*>(ev.node)) {
          size_t li = lock_idx.at(nd->var);
          if (lock_owner[li] < 0) lock_owner[li] = (int)tid;
        }

    double col = 0.0;
    std::vector<bool> placed(n_locks, false);
    for (size_t tid = 0; tid < n_threads; ++tid) {
      thread_col_x[tid] = col; col += SPACING;
      for (size_t li = 0; li < n_locks; ++li)
        if (!placed[li] && lock_owner[li] == (int)tid) {
          lock_col_x[li] = col; col += SPACING;
          placed[li] = true;
        }
    }
    for (size_t li = 0; li < n_locks; ++li)
      if (!placed[li]) { lock_col_x[li] = col; col += SPACING; }
  }

  auto thread_x = [&](size_t tid) -> double {
    if (has_g_lane)
      return tid < g_mid ? SPACING * tid : SPACING * (tid + 1);
    return thread_col_x.at(tid);
  };
  auto lock_x = [&](size_t li) -> double {
    return lock_col_x.count(li) ? lock_col_x.at(li) : SPACING * (li + 1);
  };
  const double g_x = has_g_lane ? SPACING * g_mid : 0.0;

  double lane_bottom = -0.5;
  for (size_t tid = 0; tid < n_threads; ++tid)
    if (!per_thread[tid].empty())
      lane_bottom = std::min(lane_bottom, per_thread[tid].back().y - 0.5);

  // ── Phase 1b: sync state annotations (push/pull labels on g-lane arcs) ────
  // push_annotation: dark grey sharedupdate box — what the thread staged → g
  // pull_annotation: light grey stateupdate box — what the thread receives ← g
  std::unordered_map<const Node*, std::string> push_annotation;
  std::unordered_map<const Node*, std::string> pull_annotation;

  if (has_g_lane) {
    auto format_state = [&](size_t tid,
                            const std::map<std::string, size_t>& state) -> std::string {
      if (state.empty()) return "";
      std::string lbl = "$";
      bool first = true;
      for (auto& [var, val] : state) {
        if (!first) lbl += ",\\,";
        lbl += latex_escape(var) + "_{" + std::to_string(tid) + "}="
             + std::to_string(val);
        first = false;
      }
      return lbl + "$";
    };

    // Pass A: push annotations — writes accumulated between sync points.
    // Lock resets the accumulator; Unlock, Spawn, and End emit the push.
    for (size_t tid = 0; tid < n_threads; ++tid) {
      std::map<std::string, size_t> pending;
      for (auto& ev : per_thread[tid]) {
        if (auto* wr = dynamic_cast<const Write*>(ev.node)) {
          pending[wr->var] = wr->value;
        } else if (dynamic_cast<const Lock*>(ev.node)) {
          pending.clear();
        } else if (dynamic_cast<const Unlock*>(ev.node)
                   || dynamic_cast<const Spawn*>(ev.node)
                   || dynamic_cast<const End*>(ev.node)) {
          std::string lbl = format_state(tid, pending);
          if (!lbl.empty())
            push_annotation[ev.node] = lbl;
          pending.clear();
        }
      }
    }

    // Pass B: pull annotations — derived from the corresponding push.
    //   Start of spawned thread ← inherits Spawn's push state
    //   Join                    ← inherits joinee's End push state
    //   Lock                    ← inherits the ordered_after Unlock's push state
    for (size_t tid = 0; tid < n_threads; ++tid) {
      for (auto& ev : per_thread[tid]) {
        if (auto* sp = dynamic_cast<const Spawn*>(ev.node)) {
          auto it = push_annotation.find(ev.node);
          if (it != push_annotation.end() && sp->spawned)
            pull_annotation[sp->spawned.get()] = it->second;
        } else if (auto* jn = dynamic_cast<const Join*>(ev.node)) {
          // In linear mode, pushes happen at Unlock, so End may be empty.
          // Scan the joinee's thread for the last push annotation.
          if (jn->tid < per_thread.size()) {
            const std::string* last_push = nullptr;
            for (auto& jev : per_thread[jn->tid]) {
              auto it = push_annotation.find(jev.node);
              if (it != push_annotation.end())
                last_push = &it->second;
            }
            if (last_push)
              pull_annotation[ev.node] = *last_push;
          }
        } else if (auto* lk = dynamic_cast<const Lock*>(ev.node)) {
          if (lk->ordered_after) {
            auto it = push_annotation.find(lk->ordered_after.get());
            if (it != push_annotation.end())
              pull_annotation[ev.node] = it->second;
          }
        }
      }
    }
  } else {
    // Branching mode: push at Unlock/Spawn/End, pull at Lock/Start(spawned)/Join.
    auto format_state = [&](size_t tid,
                            const std::map<std::string, size_t>& state) -> std::string {
      if (state.empty()) return "";
      std::string lbl = "$";
      bool first = true;
      for (auto& [var, val] : state) {
        if (!first) lbl += ",\\,";
        lbl += latex_escape(var) + "_{" + std::to_string(tid) + "}="
             + std::to_string(val);
        first = false;
      }
      return lbl + "$";
    };
    // Pass A: push annotations.
    // Lock resets the accumulator — writes between acquire and release are the
    // section that gets staged to the lock at Unlock.
    for (size_t tid = 0; tid < n_threads; ++tid) {
      std::map<std::string, size_t> pending;
      for (auto& ev : per_thread[tid]) {
        if (auto* wr = dynamic_cast<const Write*>(ev.node))
          pending[wr->var] = wr->value;
        else if (dynamic_cast<const Lock*>(ev.node))
          pending.clear();
        else if (dynamic_cast<const Unlock*>(ev.node)
              || dynamic_cast<const Spawn*>(ev.node)
              || dynamic_cast<const End*>(ev.node)) {
          std::string lbl = format_state(tid, pending);
          if (!lbl.empty()) push_annotation[ev.node] = lbl;
          pending.clear();
        }
      }
    }
    // Pass B: pull annotations.
    //   Start of spawned thread ← inherits Spawn's push
    //   Join                    ← inherits joinee's End push
    //   Lock                    ← inherits the ordered_after Unlock's push
    for (size_t tid = 0; tid < n_threads; ++tid) {
      for (auto& ev : per_thread[tid]) {
        if (auto* sp = dynamic_cast<const Spawn*>(ev.node)) {
          auto it = push_annotation.find(ev.node);
          if (it != push_annotation.end() && sp->spawned)
            pull_annotation[sp->spawned.get()] = it->second;
        } else if (auto* jn = dynamic_cast<const Join*>(ev.node)) {
          if (jn->joinee) {
            auto it = push_annotation.find(jn->joinee.get());
            if (it != push_annotation.end())
              pull_annotation[ev.node] = it->second;
          }
        } else if (auto* lk = dynamic_cast<const Lock*>(ev.node)) {
          if (lk->ordered_after) {
            auto it = push_annotation.find(lk->ordered_after.get());
            if (it != push_annotation.end())
              pull_annotation[ev.node] = it->second;
          }
        }
      }
    }
  }

  // ── Phase 3: emit ─────────────────────────────────────────────────────────
  std::ofstream f(path);

  // Document preamble
  f << "\\documentclass{standalone}\n"
    << "\\usepackage{tikz}\n"
    << "\\usepackage{calc}\n"
    << "\\usetikzlibrary{automata,shapes,decorations,arrows,calc,"
       "arrows.meta,fit,positioning,quotes,tikzmark,shadows}\n"
    << "\n"
    << FORMATTER
    << "\n\\begin{document}\n"
    << "\\begin{tikzpicture}[common timeline styles]\n"
    << "\\TimelineColorSetup\n\n";


  // Named coordinates for each lock lane (used with |- notation)
  for (size_t li = 0; li < n_locks; ++li) {
    std::string suffix;
    for (char c : lock_vars[li]) if (std::isalnum(c)) suffix += c;
    if (suffix.empty()) suffix = "L" + std::to_string(li);
    f << "\\coordinate (lane" << suffix << ") at (" << fmt(lock_x(li)) << ", 0);\n";
  }
  f << "\n";

  // Compute longest label for thread 0 and set left code shift
  {
    std::string longest;
    for (auto& ev : per_thread[0])
      if (ev.label.size() > longest.size()) longest = ev.label;
    if (!longest.empty())
      f << "\\SetLeftCodeShift{\\CodeWidthOf{" << longest << "}}\n";
  }
  f << "\\DefineThreadEventMacros\n\n";

  // Column headers
  for (size_t tid = 0; tid < n_threads; ++tid)
    f << "\\node at (" << fmt(thread_x(tid)) << ", 0.6) {T$_{" << tid << "}$};\n";
  for (size_t li = 0; li < n_locks; ++li)
    f << "\\node at (" << fmt(lock_x(li)) << ", 0.6) {"
      << latex_escape(lock_vars[li]) << "};\n";
  if (has_g_lane) {
    f << "\\coordinate (laneG) at (" << fmt(g_x) << ", 0);\n";
    f << "\\node at (" << fmt(g_x) << ", 0.6) {g};\n";
  }
  f << "\n";

  // Helper: emit a single event node
  auto emit_event = [&](const EventInfo& ev) {
    double x = thread_x(ev.tid);
    // Label anchor: threads left of g go LEFT (east anchor), threads right go RIGHT (west anchor)
    const bool is_left = has_g_lane ? (ev.tid < g_mid) : (ev.tid == 0);
    const char* anchor = is_left ? "anchor=east, xshift=-3pt" : "anchor=west, xshift=3pt";

    if (ev.is_conflict) {
      // Conflict event: emit a black outer octagon + red inner with "fail" text.
      // Using a named \node (not a coordinate) so the name can be used with |-.
      f << "\\node[regular polygon, regular polygon sides=8,\n"
        << "  draw=black, fill=black, line width=2pt,\n"
        << "  minimum size=20pt, inner sep=0pt] (" << ev.name << ")\n"
        << "  at (" << fmt(x) << ", " << fmt(ev.y) << ") {};\n"
        << "\\node[regular polygon, regular polygon sides=8,\n"
        << "  draw=white, fill=conflictred, line width=0.9pt,\n"
        << "  minimum size=14pt, inner sep=0pt,\n"
        << "  font=\\scriptsize\\bfseries\\sffamily, text=white]\n"
        << "  at (" << fmt(x) << ", " << fmt(ev.y) << ") {\\textsc{fail}};\n";
    } else if (ev.is_pending) {
      f << "\\fill[fill=threadtime!50] (" << fmt(x) << ", " << fmt(ev.y) << ")\n"
        << "  circle (1.5pt) coordinate (" << ev.name << ")\n"
        << "  node[" << anchor << ", codebox, opacity=0.6] {"
        << ev.label << "};\n";
    } else if (ev.is_sync) {
      // Sync event: invisible dot, label only
      f << "\\fill (" << fmt(x) << ", " << fmt(ev.y) << ")\n"
        << "  circle (0pt) coordinate (" << ev.name << ")\n"
        << "  node[" << anchor << ", codebox] {" << ev.label << "};\n";
    } else {
      f << "\\fill[fill=threadtime] (" << fmt(x) << ", " << fmt(ev.y) << ")\n"
        << "  circle (2pt) coordinate (" << ev.name << ")\n"
        << "  node[" << anchor << ", codebox] {" << ev.label << "};\n";
    }
  };

  // Emit all events
  for (size_t tid = 0; tid < n_threads; ++tid) {
    f << "% Thread " << tid << "\n";
    for (auto& ev : per_thread[tid])
      emit_event(ev);
    f << "\n";
  }

  // ── Lane lines ────────────────────────────────────────────────────────────
  f << "% Lane lines\n";
  for (size_t tid = 0; tid < n_threads; ++tid) {
    double x = thread_x(tid);
    auto& evs = per_thread[tid];
    if (evs.empty()) continue;
    auto& last = evs.back();
    bool ends = dynamic_cast<const End*>(last.node) != nullptr;
    bool conflict_last = last.is_conflict;

    const std::string& start_name = evs.front().name;
    if (ends || conflict_last) {
      f << "\\draw[thread lane, ->] (" << start_name << ") -- (" << last.name << ");\n";
    } else {
      f << "\\draw[thread lane, ->] (" << start_name << ") -- ("
        << fmt(x) << ", " << fmt(lane_bottom) << ");\n";
    }
  }
  for (size_t li = 0; li < n_locks; ++li) {
    double lx = lock_x(li);
    f << "\\draw[shared lane, ->] (" << fmt(lx) << ", 0.3) -- ("
      << fmt(lx) << ", " << fmt(lane_bottom) << ");\n";
  }
  if (has_g_lane)
    f << "\\draw[shared lane] (" << fmt(g_x) << ", 0.3) -- ("
      << fmt(g_x) << ", " << fmt(lane_bottom) << ");\n";
  f << "\n";

  // ── Sync connections ──────────────────────────────────────────────────────
  f << "% Sync connections\n";
  for (size_t tid = 0; tid < n_threads; ++tid) {
    for (auto& ev : per_thread[tid]) {
      if (auto* nd = dynamic_cast<const Spawn*>(ev.node)) {
        if (has_g_lane)
          // linear: spawn pulls + pushes g
          f << "\\PullPush{(" << ev.name << ")}{(laneG |- " << ev.name << ")}\n";
        else if (nd->tid < n_threads && !per_thread[nd->tid].empty())
          // branching: push to spawned thread's start
          f << "\\draw[link oneway] (" << ev.name << ") -- ("
            << per_thread[nd->tid].front().name << ");\n";
      } else if (auto* nd = dynamic_cast<const Join*>(ev.node)) {
        if (has_g_lane)
          // linear: join pulls from g
          f << "\\draw[link oneway] (laneG |- " << ev.name << ") -- (" << ev.name << ");\n";
        else if (nd->tid < n_threads && !per_thread[nd->tid].empty())
          // branching: pull from joined thread's last node
          f << "\\draw[link oneway] (" << per_thread[nd->tid].back().name
            << ") -- (" << ev.name << ");\n";
      } else if (auto* nd = dynamic_cast<const Lock*>(ev.node)) {
        if (has_g_lane) {
          // linear: lock pulls from g
          f << "\\draw[link oneway] (laneG |- " << ev.name << ") -- (" << ev.name << ");\n";
        } else {
          // branching: pull from lock lane
          std::string suffix;
          for (char c : nd->var) if (std::isalnum(c)) suffix += c;
          if (suffix.empty()) suffix = "L" + std::to_string(lock_idx.at(nd->var));
          f << "\\draw[link oneway] (lane" << suffix << " |- " << ev.name
            << ") -- (" << ev.name << ");\n";
        }
      } else if (auto* nd = dynamic_cast<const Unlock*>(ev.node)) {
        if (has_g_lane) {
          if (ev.is_conflict)
            // conflicting unlock: push failed, g notifies thread of conflict
            f << "\\draw[link oneway] (laneG |- " << ev.name << ") -- (" << ev.name << ");\n";
          else
            // successful unlock: pull + push
            f << "\\PullPush{(" << ev.name << ")}{(laneG |- " << ev.name << ")}\n";
        } else {
          // branching: push from thread to lock lane only
          std::string suffix;
          for (char c : nd->var) if (std::isalnum(c)) suffix += c;
          if (suffix.empty()) suffix = "L" + std::to_string(lock_idx.at(nd->var));
          f << "\\draw[link oneway] (" << ev.name << ") -- (lane" << suffix
            << " |- " << ev.name << ");\n";
        }
      } else if (dynamic_cast<const Start*>(ev.node)) {
        // g: start pulls
        if (has_g_lane)
          f << "\\draw[link oneway] (laneG |- " << ev.name << ") -- (" << ev.name << ");\n";
      } else if (dynamic_cast<const End*>(ev.node)) {
        // g: end pulls + pushes
        if (has_g_lane)
          f << "\\PullPush{(" << ev.name << ")}{(laneG |- " << ev.name << ")}\n";
      }
      // State annotations on sync arcs
      if (has_g_lane) {
        if (push_annotation.count(ev.node))
          f << "\\node[sharedupdate, below=8pt] at ($(" << ev.name
            << ")!0.5!(laneG |- " << ev.name << ")$) {"
            << push_annotation.at(ev.node) << "};\n";
        if (pull_annotation.count(ev.node))
          f << "\\node[stateupdate, above=8pt] at ($(" << ev.name
            << ")!0.5!(laneG |- " << ev.name << ")$) {"
            << pull_annotation.at(ev.node) << "};\n";
      } else if (auto* an = dynamic_cast<const Spawn*>(ev.node)) {
        // push annotation midway along the spawn→child arrow
        if (push_annotation.count(ev.node) && an->tid < n_threads
            && !per_thread[an->tid].empty())
          f << "\\node[sharedupdate, below=8pt] at ($(" << ev.name
            << ")!0.5!(" << per_thread[an->tid].front().name << ")$) {"
            << push_annotation.at(ev.node) << "};\n";
      } else if (auto* an = dynamic_cast<const Join*>(ev.node)) {
        // pull annotation midway along the joinee-end→join arrow
        if (pull_annotation.count(ev.node) && an->tid < n_threads
            && !per_thread[an->tid].empty())
          f << "\\node[stateupdate, above=8pt] at ($(" << ev.name
            << ")!0.5!(" << per_thread[an->tid].back().name << ")$) {"
            << pull_annotation.at(ev.node) << "};\n";
      } else if (auto* an = dynamic_cast<const Lock*>(ev.node)) {
        if (pull_annotation.count(ev.node)) {
          std::string suffix;
          for (char c : an->var) if (std::isalnum(c)) suffix += c;
          if (suffix.empty()) suffix = "L" + std::to_string(lock_idx.at(an->var));
          f << "\\node[stateupdate, above=8pt] at ($(" << ev.name
            << ")!0.5!(lane" << suffix << " |- " << ev.name << ")$) {"
            << pull_annotation.at(ev.node) << "};\n";
        }
      } else if (auto* an = dynamic_cast<const Unlock*>(ev.node)) {
        if (push_annotation.count(ev.node)) {
          std::string suffix;
          for (char c : an->var) if (std::isalnum(c)) suffix += c;
          if (suffix.empty()) suffix = "L" + std::to_string(lock_idx.at(an->var));
          f << "\\node[sharedupdate, below=8pt] at ($(" << ev.name
            << ")!0.5!(lane" << suffix << " |- " << ev.name << ")$) {"
            << push_annotation.at(ev.node) << "};\n";
        }
      }
    }
  }
  f << "\n";

  // ── Conflict paths ────────────────────────────────────────────────────────
  if (!conflicts.empty()) {
    f << "% Conflict paths\n"
      << "\\begin{scope}[opacity=0.5]\n";
    for (auto& ce : conflicts) {
      auto get_name = [&](const Node* n) -> std::optional<std::string> {
        if (!n) return std::nullopt;
        auto it = node_name.find(n);
        if (it == node_name.end()) return std::nullopt;
        return it->second;
      };

      auto cn = get_name(ce.conflict_node);
      if (!cn) continue;

      if (ce.src1 && !ce.lock_var.empty() && ce.ordered_after) {
        // Route via shared lane: src1 → unlock → lane → conflict
        auto s1   = get_name(ce.src1);
        auto unl  = get_name(ce.ordered_after);
        std::string suffix;
        for (char c : ce.lock_var) if (std::isalnum(c)) suffix += c;
        if (suffix.empty()) suffix = "L" + std::to_string(lock_idx.at(ce.lock_var));
        if (s1 && unl)
          f << "\\draw[conflict] (" << *s1 << ") -- (" << *unl
            << ") -- (lane" << suffix << " |- " << *unl
            << ") -- (lane" << suffix << " |- " << *cn
            << ") -- (" << *cn << ");\n";
      } else if (has_g_lane && ce.lock_var.empty() && ce.ordered_after
                 && (ce.src1 || ce.src2)) {
        // G-lane routing for unlock conflicts: each source routes based on thread.
        // Cross-thread: src → g_predecessor → g_lane → conflict.
        // Same-thread:  src → conflict (direct).
        auto pred = get_name(ce.ordered_after);
        auto conflict_tid_it = node_tid.find(ce.conflict_node);
        size_t conflict_tid  = (conflict_tid_it != node_tid.end())
                                 ? conflict_tid_it->second : SIZE_MAX;
        auto emit_unlock_src = [&](const Node* src) {
          auto s = get_name(src);
          if (!s) return;
          auto src_tid_it = node_tid.find(src);
          size_t src_tid  = (src_tid_it != node_tid.end())
                              ? src_tid_it->second : SIZE_MAX;
          if (src_tid != conflict_tid && pred) {
            const char* arc = (thread_x(src_tid) < g_x)
                                ? "looseness=.5, out=320, in=210"
                                : "looseness=.5, out=220, in=330";
            f << "\\draw[conflict] (" << *s << ") -- (" << *pred
              << ") to[" << arc << "] (laneG |- " << *pred
              << ") -- (laneG |- " << *cn << ") -- (" << *cn << ");\n";
          } else {
            f << "\\draw[conflict] (" << *s << ") -- (" << *cn << ");\n";
          }
        };
        if (ce.src1) emit_unlock_src(ce.src1);
        if (ce.src2) emit_unlock_src(ce.src2);
      } else if (ce.src1 || ce.src2) {
        // Emit one conflict path per source.
        // Same-thread source → direct line along the thread lane.
        // Cross-thread source → route through the source thread's last sync
        //   node (its End) then down the g lane to the conflict.
        auto conflict_tid_it = node_tid.find(ce.conflict_node);
        size_t conflict_tid  = (conflict_tid_it != node_tid.end())
                                 ? conflict_tid_it->second : SIZE_MAX;

        auto emit_src = [&](const Node* src) {
          auto s = get_name(src);
          if (!s) return;
          auto src_tid_it = node_tid.find(src);
          size_t src_tid  = (src_tid_it != node_tid.end())
                              ? src_tid_it->second : SIZE_MAX;

          if (has_g_lane && src_tid != conflict_tid) {
            // Cross-thread (linear): src → End → arc → g lane → conflict
            const std::string& end_name = per_thread[src_tid].back().name;
            const char* push_arc = (thread_x(src_tid) < g_x)
                                     ? "looseness=.5, out=320, in=210"
                                     : "looseness=.5, out=220, in=330";
            f << "\\draw[conflict] (" << *s << ") -- (" << end_name
              << ") to[" << push_arc << "] (laneG |- " << end_name
              << ") -- (laneG |- " << *cn
              << ") -- (" << *cn << ");\n";
          } else if (!has_g_lane && src_tid != conflict_tid
                     && src_tid < per_thread.size()) {
            // Cross-thread (branching): src → End of src thread → conflict
            const std::string& end_name = per_thread[src_tid].back().name;
            f << "\\draw[conflict] (" << *s << ") -- (" << end_name
              << ") -- (" << *cn << ");\n";
          } else {
            // Same thread: direct line
            f << "\\draw[conflict] (" << *s << ") -- (" << *cn << ");\n";
          }
        };

        if (ce.src1) emit_src(ce.src1);
        if (ce.src2) emit_src(ce.src2);
      }
    }
    f << "\\end{scope}\n\n";
  }

  f << "\\end{tikzpicture}\n\\end{document}\n";
}

} // namespace graph
} // namespace gitmem
