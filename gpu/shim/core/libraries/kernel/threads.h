// gow3: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("gow3:hle");
            func(stop);
            done->store(true);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // gow3: these are called from guest code, which a C++ exception cannot unwind: a failed
    // join is logged and the thread detached instead.
    void Join() {
        // Linux fork: one join at a time. AvPlayer's demuxer joins its decoders when the movie
        // ends while sceAvPlayerStop joins them from the game's thread: two joins of one thread,
        // and the second waited on a thread that was gone (the game hung on the world map after
        // a level until a signal woke it). The second caller now waits for the first join here,
        // then finds the thread no longer joinable.
        // A thread stopping its own Thread object never waits for the lock: whoever holds it is
        // joining this very thread and would wait for it forever.
        if (thread.joinable() && thread.get_id() == std::this_thread::get_id()) {
            std::unique_lock self_lock{join_mutex, std::try_to_lock};
            if (self_lock && thread.joinable()) thread.detach();
            return;
        }
        std::scoped_lock join_lock{join_mutex};
        if (!thread.joinable()) return;
        try {
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
            else thread.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            if (thread.get_id() != std::this_thread::get_id()) {
                for (int waited = 0; finished && !finished->load() && waited < 5000; ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (thread.joinable()) thread.detach();
        }
    }
    bool Joinable() const { return thread.joinable(); }
    // Linux fork: ask the thread to stop without waiting, so whatever it sleeps on can be woken
    // before the join (AvPlayerSource::Stop).
    void RequestStop() {
        if (thread.joinable()) thread.request_stop();
    }
    void Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            Join();
        }
    }

private:
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
    std::mutex join_mutex; ///< Linux fork: Join() from several threads at once
};
} // namespace Libraries::Kernel
