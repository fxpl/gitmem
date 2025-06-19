#include "interpreter.hh"

namespace gitmem
{
    using namespace trieste;

    /**
     * A TraceNode represents a point in the space of possible schedulings. A
     * path in a tree of TraceNodes represents a scheduling, with the thread ID
     * of each node being the thread that was scheduled at that point. When
     * there are no more children to explore, or when one thread has crashed,
     * the TraceNode is marked as complete so that the next run will not explore
     * it again.
     *
     */
    struct TraceNode
    {
        size_t tid_;
        bool complete;
        std::vector<std::shared_ptr<TraceNode>> children;

        TraceNode(const size_t tid) : tid_(tid), complete(false) {}

        std::shared_ptr<TraceNode> extend(ThreadID tid)
        {
            children.push_back(std::make_shared<TraceNode>(tid));
            return children.back();
        }

        bool is_leaf() const
        {
            return children.empty();
        }
    };

    /**
     * Print the traces of the program, one trace per line. Each trace is a
     * sequence of thread IDs that were scheduled in that order.
     */
    template <typename S>
    void print_traces(S &stream, const std::vector<std::vector<ThreadID>> &traces)
    {
        for (const auto &trace : traces)
        {
            for (const auto &tid : trace)
            {
                stream << tid << " ";
            }
            stream << std::endl;
        }
    }

    /** Build an output path for the execution graph, appending an index to the
     * filename to avoid overwriting previous graphs. */
    std::filesystem::path build_output_path(const std::filesystem::path &output_path, const size_t idx)
    {
        auto parent = output_path.parent_path();
        auto name = output_path.stem().string();
        auto ext = output_path.extension().string();
        return parent / (name + "_" + std::to_string(idx) + ext);
    }

    /**
     * Explore all possible execution paths of the program, printing one trace
     * for each distinct final state that led to an error.
     */
    int model_check(const Node ast, const std::filesystem::path &output_path)
    {
        GlobalContext gctx(ast);

        auto final_contexts = std::vector<GlobalContext>{};
        auto failing_contexts = std::vector<GlobalContext>{};
        auto deadlocked_contexts = std::vector<GlobalContext>{};

        auto final_traces = std::vector<std::vector<size_t>>{};
        auto failing_traces = std::vector<std::vector<size_t>>{};
        auto deadlocked_traces = std::vector<std::vector<size_t>>{};

        const auto root = std::make_shared<TraceNode>(0);
        auto cursor = root;
        auto current_trace = std::vector<size_t>{0}; // Start with the main thread
        verbose << "==== Thread " << cursor->tid_ << " ====" << std::endl;
        progress_thread(gctx, cursor->tid_, gctx.threads[cursor->tid_]);

        while (!root->complete)
        {
            while (!cursor->children.empty() && !cursor->children.back()->complete)
            {
                // We have a child that is not complete, we can extend that trace
                cursor = cursor->children.back();
                current_trace.push_back(cursor->tid_);
                verbose << "==== Thread " << cursor->tid_ << " (replay) ====" << std::endl;
                progress_thread(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
            }

            // Try to find a thread to schedule next
            size_t start_idx = cursor->children.empty() ? 0 : cursor->children.back()->tid_ + 1;
            size_t no_threads = gctx.threads.size();
            bool made_progress = false;
            for (size_t i = start_idx; i < no_threads && !made_progress; ++i)
            {
                auto thread = gctx.threads[i];
                if (!thread->terminated)
                {
                    // Run the thread to the next sync point
                    verbose << "==== Thread " << i << " ====" << std::endl;
                    auto prog_or_term = progress_thread(gctx, i, thread);
                    if (std::holds_alternative<TerminationStatus>(prog_or_term))
                    {
                        // Thread terminated, we can extend the trace
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                        if (std::get<TerminationStatus>(prog_or_term) != TerminationStatus::completed)
                        {
                            // Thread terminated with an error, we can stop here
                            verbose << "Thread " << i << " terminated with an error" << std::endl;
                            cursor->complete = true;
                        }
                    }
                    else if (std::get<ProgressStatus>(prog_or_term) == ProgressStatus::progress)
                    {
                        // Thread made progress, we can continue
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                    }
                }
            }

            if (!made_progress)
            {
                // No threads made progress, we can stop here
                cursor->complete = true;
            }

            bool all_completed = std::all_of(gctx.threads.begin(), gctx.threads.end(),
                                             [](const auto &thread)
                                             { return thread->terminated && *thread->terminated == TerminationStatus::completed; });
            bool any_crashed =
                std::any_of(gctx.threads.begin(), gctx.threads.end(),
                            [](const auto &thread)
                            { return thread->terminated && *thread->terminated != TerminationStatus::completed; });

            bool is_deadlock = !all_completed && !made_progress && cursor->is_leaf();

            if (all_completed || any_crashed || is_deadlock)
            {
                // Remember final state if it is new
                if (!std::any_of(final_contexts.begin(), final_contexts.end(),
                                 [&gctx](const GlobalContext &state)
                                 { return state == gctx; }))
                {
                    final_contexts.push_back(gctx);
                    final_traces.push_back(current_trace);
                    if (any_crashed)
                    {
                        failing_traces.push_back(current_trace);
                        failing_contexts.push_back(gctx);
                    }
                    else if (is_deadlock)
                    {
                        deadlocked_traces.push_back(current_trace);
                        deadlocked_contexts.push_back(gctx);
                    }
                }

                cursor->complete = true;
            }

            if (cursor->complete && !root->complete)
            {
                // Reset the cursor to the root and start a new trace
                verbose << std::endl
                        << "Restarting trace..." << std::endl;
                gctx = GlobalContext(ast);

                cursor = root;
                current_trace.clear();
                current_trace.push_back(0); // Start with the main thread again
                verbose << "==== Thread " << cursor->tid_ << " (replay) ====" << std::endl;
                progress_thread(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
            }
        }

        verbose << "Found a total of " << final_traces.size() << " trace(s) with distinct final states:" << std::endl;
        print_traces(verbose, final_traces);

        size_t idx = 0;
        if (!failing_traces.empty())
        {
            std::cout << "Found " << failing_traces.size() << " trace(s) with errors:" << std::endl;
            print_traces(std::cout, failing_traces);

            for (const auto &ctx : failing_contexts)
            {
                auto path = build_output_path(output_path, idx++);
                ctx.print_execution_graph(path);
            }
        }

        if (!deadlocked_traces.empty())
        {
            std::cout << "Found " << deadlocked_traces.size() << " trace(s) leading to deadlock:" << std::endl;
            print_traces(std::cout, deadlocked_traces);

            for (const auto &ctx : deadlocked_contexts)
            {
                auto path = build_output_path(output_path, idx++);
                ctx.print_execution_graph(path);
            }
        }

        return deadlocked_traces.empty() && failing_traces.empty() ? 0 : 1;
    }
}
