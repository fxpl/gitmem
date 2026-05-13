#include <regex>

#include "execution_state.hh"
#include "memory_model.hh"

namespace gitmem {

ThreadContext::ThreadContext(ThreadID tid, std::unique_ptr<MemoryModel>& model) {
  sync = model->make_thread_state(tid);
}

bool ThreadContext::operator==(const ThreadContext &other) const {
  if (locals != other.locals)
    return false;

  // ignore the graph node, we're not interested in that

  return *sync == *other.sync;
}

bool Thread::operator==(const Thread &other) const {
  return ctx == other.ctx &&
          block == other.block &&
          pc == other.pc &&
          terminated == other.terminated;
}

GlobalContext::GlobalContext(trieste::Node starting_block,
                             std::unique_ptr<MemoryModel> model)
    : model(std::move(model)) {
  ThreadID main_tid = 0;

  ThreadContext starting_ctx(main_tid, this->model);

  this->threads.emplace_back(main_tid, std::move(starting_ctx), starting_block);
}

GlobalContext::~GlobalContext() = default;

Lock& GlobalContext::get_lock(std::string lock) {
  auto it = locks.find(lock);
  if (it != locks.end())
    return it->second;

  auto [new_it, inserted] = locks.emplace(
    lock,
    Lock{
        .owner = std::nullopt,
        .last_unlock_event = nullptr,
        .sync = model->make_lock_state()
    }
  );

  return new_it->second;
}

bool GlobalContext::operator==(const GlobalContext &other) const {
  if (threads.size() != other.threads.size() ||
      locks.size() != other.locks.size())
    return false;

  // Threads may have been spawned in a different order, so we
  // find the thread with the same block in the other context
  for (auto &thread : threads) {
    auto it = std::find_if(other.threads.begin(), other.threads.end(),
                            [&thread](auto &t)
                            { return t.block == thread.block; });
    if (it == other.threads.end() || !(thread == *it))
      return false;
  }

  for (auto &[name, lock] : locks) {
    if (!other.locks.contains(name))
      return false;
    auto &other_lock = other.locks.at(name);
    if (lock.owner != other_lock.owner)
      return false;
  }
  return true;
}

/** Print the state of a thread, including its local and global variables,
 * and the current position in the program. */
std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  os << thread.ctx << std::endl;

  size_t idx = 0;
  for (const auto &stmt : *(thread.block)) {
    if (idx == thread.pc) {
      os << "-> ";
    } else {
      os << "   ";
    }

    // This should be somewhere else
    // Fix indentation of nested blocks
    auto s = std::string(stmt->location().view());
    s = std::regex_replace(s, std::regex("\n"), "\n   ");
    os << s << ";" << std::endl;

    idx++;
  }
  if (thread.pc == thread.block->size()) {
    os << "-> " << std::endl;
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, const ThreadContext& ctx) {
  os << "ThreadContext{locals={";

  bool first = true;
  for (const auto& [k, v] : ctx.locals) {
    if (!first) os << ", ";
    first = false;
    os << k << "=" << v;
  }

  os << "}, "; //, tail=" << ctx.tail;

  os << *(ctx.sync);

  os << "}";
  return os;
}

void show_lock(const std::string &lock_name, const struct Lock &lock) {
  std::cout << lock_name << ": ";
  if (lock.owner) {
    std::cout << "held by thread " << *lock.owner;
  } else {
    std::cout << "<free>";
  }
  if (lock.sync) {
    std::cout << ", " << *(lock.sync);
  }
  std::cout << std::endl;
}

void GlobalContext::print(std::ostream& os, bool show_all) const {
  os << *model << std::endl;

  bool showed_any = false;
  for (size_t i = 0; i < threads.size(); i++) {
    auto& thread = threads[i];
    if (show_all || !thread.terminated ||
        !std::holds_alternative<termination::Completed>(*thread.terminated)) {
      os << "---- Thread " << i << std::endl;
      os << threads[i] << std::endl;
      os << std::endl;
      showed_any = true;
    }
  }

  if (showed_any && locks.size() > 0) {
    os << "---- Locks" << std::endl;

    for (const auto &[lock_name, lock] : locks) {
      show_lock(lock_name, lock);
    }

    if (locks.size() > 0)
      os << "--" << std::endl;
  }
}

std::ostream& operator<<(std::ostream& os, const GlobalContext& gctx) {
  gctx.print(os);
  return os;
}


} // namespace gitmem