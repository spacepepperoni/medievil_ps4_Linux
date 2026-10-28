// gow3: optimizations that can be switched off while the game runs (GOW3_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
// gow3 (Windows): no signals. The loader's exception handler resumes the faulting thread
// in gow3_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int gow3_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) gow3_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace Gow3Toggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41,
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace Gow3Toggle

namespace Gow3Stats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (GOW3_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<unsigned long> gpu_thread_id{0}; ///< Windows thread id of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0}, t_gc{0}, t_readback_wait{0};
/// Diagnostics are collected only with GOW3_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("GOW3_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
/// Linux fork: host copy waits by caller (GOW3_FRAME_STATS prints them): which fences and writes
/// the GPU thread waits at. A caller sets its site for the scope of the wait (WaitSite).
enum WaitSiteId : int { SiteSubmit, SiteEop, SiteComputeFence, SiteWriteData, SiteEos, SiteAscWriteData,
                        SiteAscReleaseMem, SiteDownload, SiteSignalInline, SiteFlip, SiteCount };
inline const char* const wait_site_names[SiteCount] = {"submit/other", "eop", "compute fence", "write data", "eos",
                                                       "asc write data", "asc release mem", "download",
                                                       "signal inline", "flip"};
inline std::atomic<std::uint64_t> site_wait_ns[SiteCount]{}, site_waits[SiteCount]{};
inline thread_local int wait_site = SiteSubmit;
struct WaitSite {
    int previous;
    explicit WaitSite(int site) : previous{wait_site} { wait_site = site; }
    ~WaitSite() { wait_site = previous; }
};
/// Linux fork: GPU-written buffer data copied back for the CPU (a CPU access to a GPU-modified
/// page): each one waits for the GPU to finish all submitted work. Busiest 64 KiB blocks kept.
inline std::atomic<std::uint64_t> buffer_downloads{0}, buffer_download_bytes{0}, buffer_download_wait_ns{0},
    buffer_downloads_mapped{0}, buffer_downloads_deferred{0};
/// Linux fork: the CPU reads mapped GPU memory: each submission ends with a barrier making its
/// writes visible to the host (BufferCache::DownloadMapped).
inline std::atomic<bool> host_read_barrier{false};
/// Set by a buffer write since the last submission: only then is the barrier needed.
inline std::atomic<bool> host_read_pending{false};
struct DownloadBlock { std::uint64_t block, count; };
/// A spin lock: downloads may run inside a fault handler.
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() { while (flag.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { flag.clear(std::memory_order_release); }
};
inline SpinLock download_blocks_mutex;
inline DownloadBlock download_blocks[8]{};
inline void NoteDownloadBlock(std::uint64_t address) {
    std::lock_guard lock{download_blocks_mutex};
    const std::uint64_t block = address >> 16;
    DownloadBlock* least = &download_blocks[0];
    for (auto& b : download_blocks) {
        if (b.block == block && b.count) { ++b.count; return; }
        if (b.count < least->count) least = &b;
    }
    if (!least->count) *least = {block, 1};
    else ++least->count; // stays approximate: the least busy slot absorbs new blocks
}
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace Gow3Stats
