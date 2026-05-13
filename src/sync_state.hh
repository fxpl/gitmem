#pragma once

#include <iostream>

namespace gitmem {

class ThreadSyncState {
public:
  virtual ~ThreadSyncState() = default;

  virtual bool operator==(const ThreadSyncState& other) const = 0;

  virtual std::ostream &print(std::ostream &os) const = 0;
  friend std::ostream &operator<<(std::ostream &os,
                                  const ThreadSyncState &state) {
    return state.print(os);
  }
};

class LockSyncState {
public:
  virtual ~LockSyncState() = default;

  virtual std::ostream &print(std::ostream &os) const = 0;
  friend std::ostream &operator<<(std::ostream &os,
                                  const LockSyncState &state) {
    return state.print(os);
  }
};

}