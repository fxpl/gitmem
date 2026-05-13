#pragma once

#include "thread_id.hh"
#include "conflict.hh"

#include <variant>

namespace gitmem {

namespace termination {

struct Completed {
  friend std::ostream& operator<<(std::ostream& os, const Completed&) {
    os << "Completed successfully";
    return os;
  }
};

struct DataRace {
  std::shared_ptr<ConflictBase> conflict;

  explicit DataRace(std::shared_ptr<ConflictBase> conflict): conflict(conflict) {}

  friend std::ostream& operator<<(std::ostream& os, const DataRace& r) {
    assert(r.conflict != nullptr);
    os << "Data race occurred: " << *r.conflict;
    return os;
  }
};

struct UnlockError {
  std::string lock;
  friend std::ostream& operator<<(std::ostream& os, const UnlockError& e) {
    os << "Attempted to unlock '" << e.lock << "' without ownership";
    return os;
  }
};

struct AssertionFailure {
  std::string expression;
  friend std::ostream& operator<<(std::ostream& os, const AssertionFailure& a) {
    os << "Assertion failed: " << a.expression;
    return os;
  }
};

struct UnassignedRead {
  std::string variable;
  friend std::ostream& operator<<(std::ostream& os, const UnassignedRead& u) {
    os << "Read of unassigned variable '" << u.variable << "'";
    return os;
  }
};

inline bool operator==(const Completed&, const Completed&) { return true; }

inline bool operator==(const DataRace& a, const DataRace& b) {
    return *a.conflict == *b.conflict;
}

inline bool operator==(const UnlockError& a, const UnlockError& b) {
    return a.lock == b.lock;
}

inline bool operator==(const AssertionFailure& a, const AssertionFailure& b) {
    return a.expression == b.expression;
}

inline bool operator==(const UnassignedRead& a, const UnassignedRead& b) {
    return a.variable == b.variable;
}

// Optional: != operators for convenience
inline bool operator!=(const Completed& a, const Completed& b) { return !(a == b); }
inline bool operator!=(const DataRace& a, const DataRace& b) { return !(a == b); }
inline bool operator!=(const UnlockError& a, const UnlockError& b) { return !(a == b); }
inline bool operator!=(const AssertionFailure& a, const AssertionFailure& b) { return !(a == b); }
inline bool operator!=(const UnassignedRead& a, const UnassignedRead& b) { return !(a == b); }

using TerminationStatus =
  std::variant<
    Completed,
    DataRace,
    UnlockError,
    AssertionFailure,
    UnassignedRead
  >;

} // end termination

} // end gitmem