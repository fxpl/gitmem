#include <trieste/trieste.h>
#include "lang.hh"
#include "graph.hh"

namespace gitmem
{
    /* For debug printing */
    inline struct Verbose
    {
        bool enabled = false;

        template <typename T>
        const Verbose& operator<<(const T &msg) const {
            if (enabled) {
                std::cout << msg;
            }
            return *this;
        }

        const Verbose& operator<<(std::ostream& (*manip)(std::ostream&)) const {
            if (enabled) {
                std::cout << manip;
            }
            return *this;
        }
    } verbose;

    /* A 'Global' is a structure to capture the current synchronising objects
     * representation of a global variable. The structure is the current value,
     * the current commit id for the variable, and the history of commited ids.
     */

    using Commit = size_t;
    using CommitHistory = std::vector<Commit>;

    struct Global {
        size_t val;
        std::optional<Commit> commit;
        CommitHistory history;
    };

    using Globals = std::unordered_map<std::string, Global>;

    enum class TerminationStatus {
        completed,
        datarace_exception,
        unlock_exception,
        assertion_failure_exception,
        unassigned_variable_read_exception,
    };

    using Locals = std::unordered_map<std::string, size_t>;

    struct ThreadContext
    {
        Locals locals;
        Globals globals;
        std::shared_ptr<graph::Node> tail;
    };

    using ThreadStatus = std::optional<TerminationStatus>;

    struct Thread
    {
        ThreadContext ctx;
        Node block;
        size_t pc = 0;
        ThreadStatus terminated = std::nullopt;

        bool operator==(const Thread& other) const {
            // Globals have a history that we don't care about, so we only
            // compare values
            if (ctx.globals.size() != other.ctx.globals.size())
                return false;
            for (const auto& [var, global] : ctx.globals) {
                if (!other.ctx.globals.contains(var) ||
                    ctx.globals.at(var).val != other.ctx.globals.at(var).val)
                {
                    return false;
                }
            }
            return ctx.locals == other.ctx.locals &&
                   block == other.block &&
                   pc == other.pc &&
                   terminated == other.terminated;
        }
    };

    using ThreadID = size_t;

    struct Lock {
        Globals globals;
        std::optional<ThreadID> owner = std::nullopt;
        std::shared_ptr<graph::Node> last;
    };

    using Threads = std::vector<std::shared_ptr<Thread>>;

    using Locks = std::unordered_map<std::string, struct Lock>;

    struct GlobalContext {
        Threads threads;
        Locks locks;
        NodeMap<size_t> cache;
        std::unordered_map<Commit, std::shared_ptr<graph::Node>> commit_map;
        Commit uuid = 0;

        bool operator==(const GlobalContext& other) const {
            if (threads.size() != other.threads.size() || locks.size() != other.locks.size())
                return false;

            // Threads may have been spawned in a different order, so we
            // find the thread with the same block in the other context
            for (auto& thread : threads) {
                auto it = std::find_if(other.threads.begin(), other.threads.end(),
                                       [&thread](auto& t) { return t->block == thread->block; });
                if (it == other.threads.end() || !(*thread == **it))
                    return false;
            }

            for (auto& [name, lock] : locks) {
                if (!other.locks.contains(name))
                    return false;
                auto& other_lock = other.locks.at(name);
                if (lock.owner != other_lock.owner)
                    return false;
            }
            return true;
        }
    };

    enum class ProgressStatus {
        progress,
        no_progress
    };

    inline bool operator!(ProgressStatus p) { return p == ProgressStatus::no_progress; }

    inline ProgressStatus operator||(const ProgressStatus& p1, const ProgressStatus& p2)
    {
        return (p1 == ProgressStatus::progress || p2 == ProgressStatus::progress) ? ProgressStatus::progress : ProgressStatus::no_progress;
    }

    inline void operator|=(ProgressStatus& p1, const ProgressStatus& p2) {  p1 = (p1 || p2); }

    // Entry functions
    int interpret(const Node);
    int interpret_interactive(const Node);
    int model_check(const Node);

    // Internal functions
    int run_threads(GlobalContext&);
    std::variant<ProgressStatus, TerminationStatus>
    run_thread_to_sync(GlobalContext&, const ThreadID, std::shared_ptr<Thread>);
}
