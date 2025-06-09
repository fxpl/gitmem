#include "interpreter.hh"

namespace gitmem
{
    using namespace trieste;

    // TODO: Document
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
    };

    int model_check(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);
        GlobalContext gctx{{main_thread}, {}, {}};

        auto final_states = std::vector<GlobalContext>{};
        auto traces = std::vector<std::vector<size_t>>{};
        auto bad_traces = std::vector<std::vector<size_t>>{};

        const auto root = std::make_shared<TraceNode>(0);
        auto cursor = root;
        auto current_trace = std::vector<size_t>{0}; // Start with the main thread
        run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);

        if (main_thread->terminated)
        {
            // If the main thread is already terminated, we don't need to explore further
            final_states.push_back(gctx);
            traces.push_back(current_trace);
            cursor->complete = true;
            if (*main_thread->terminated != TerminationStatus::completed)
                bad_traces.push_back(current_trace);
        }

        while (!root->complete)
        {
            // Step through the trace
            while (!cursor->children.empty() && !cursor->children.back()->complete)
            {
                // We have a child that is not complete, we can extend the trace
                cursor = cursor->children.back();
                current_trace.push_back(cursor->tid_);
                run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
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
                    auto prog_or_term = run_thread_to_sync(gctx, i, thread);
                    if (std::holds_alternative<TerminationStatus>(prog_or_term))
                    {
                        // Thread terminated, we can extend the trace
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                        if (std::get<TerminationStatus>(prog_or_term) != TerminationStatus::completed)
                        {
                            // Thread terminated with an error, we can stop here
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
                    else if (gctx.threads.size() > no_threads)
                    {
                        // Thread spawned a new thread, which counts as progress
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                    }
                }
            }

            if (!made_progress)
            {
                // No further threads made progress, we can stop here
                cursor->complete = true;
            }

            bool all_completed = std::all_of(gctx.threads.begin(), gctx.threads.end(),
                                             [](const auto &thread)
                                             { return thread->terminated && *thread->terminated == TerminationStatus::completed; });
            bool any_crashed =
                std::any_of(gctx.threads.begin(), gctx.threads.end(),
                            [](const auto &thread)
                            { return thread->terminated && *thread->terminated != TerminationStatus::completed; });

            if (all_completed || any_crashed)
            {
                // Remember final result if it is new
                if (!std::any_of(final_states.begin(), final_states.end(),
                                 [&gctx](const GlobalContext &state)
                                 { return state == gctx; }))
                {
                    final_states.push_back(gctx);
                    traces.push_back(current_trace);
                    if (any_crashed)
                        bad_traces.push_back(current_trace);
                }

                cursor->complete = true;
            }

            if (cursor->complete)
            {
                ThreadContext new_starting_ctx = {};
                auto new_main_thread = std::make_shared<Thread>(new_starting_ctx, starting_block);
                gctx = {{new_main_thread}, {}, {}};

                cursor = root;
                current_trace.clear();
                current_trace.push_back(0); // Start with the main thread again
                run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
            }
        }

        logging::Info log;
        log << "Found a total of " << traces.size() << " trace(s) with distinct final states:" << std::endl;
        for (auto &trace : traces)
        {
            for (size_t i = 0; i < trace.size(); ++i)
            {
                log << trace[i] << " ";
            }
            log << std::endl;
        }

        if (!bad_traces.empty())
        {
            std::cout << "Found " << bad_traces.size() << " trace(s) with errors:" << std::endl;
            for (auto &bad_trace : bad_traces)
            {
                for (auto tid : bad_trace)
                {
                    std::cout << tid << " ";
                }
                std::cout << std::endl;
            }
            return 1;
        }

        return 0;
    }
}
