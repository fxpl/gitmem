#pragma once

namespace gitmem {

enum class ProgressStatus { progress, no_progress };
inline bool operator!(ProgressStatus p) {
  return p == ProgressStatus::no_progress;
}
inline ProgressStatus operator||(const ProgressStatus &p1,
                                 const ProgressStatus &p2) {
  return (p1 == ProgressStatus::progress || p2 == ProgressStatus::progress)
             ? ProgressStatus::progress
             : ProgressStatus::no_progress;
}
inline void operator|=(ProgressStatus &p1, const ProgressStatus &p2) {
  p1 = (p1 || p2);
}

}