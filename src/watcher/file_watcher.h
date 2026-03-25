#pragma once
#include "watcher/ignore_filter.h"
#include <efsw/efsw.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace codetldr {

// Wraps efsw::FileWatcher with non-blocking callbacks.
// Callbacks are called on efsw's internal watcher thread — they MUST return immediately.
// Thread safety: on_event and on_wakeup callbacks are expected to be thread-safe
//               (e.g., write to a pipe or push to a concurrent queue).
class FileWatcher {
public:
    // on_event:   called with the full path of a changed source file
    // on_wakeup:  called to wake the coordinator's poll() (e.g., writes to wakeup pipe)
    FileWatcher(std::function<void(std::string)> on_event,
                std::function<void()> on_wakeup);

    ~FileWatcher();

    // Non-copyable, non-movable
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Start watching project_root recursively. Creates efsw::FileWatcher and starts watch thread.
    void start(const std::filesystem::path& project_root);

    // Remove watch and stop efsw watcher thread.
    void stop();

    // Set the ignore filter. Must be called before start(). The pointer must remain valid
    // for the lifetime of the FileWatcher (owned by Coordinator).
    void set_ignore_filter(const IgnoreFilter* filter);

    // Returns true if the watcher is currently active (started and not stopped).
    bool is_active() const { return efsw_watcher_ != nullptr; }

private:
    // Inner listener — implements efsw::FileWatchListener.
    // CRITICAL: handleFileAction MUST return immediately (no blocking I/O, no analysis, no DB access).
    class WatcherListener : public efsw::FileWatchListener {
    public:
        WatcherListener(std::function<void(std::string)> on_event,
                        std::function<void()> on_wakeup);

        void handleFileAction(efsw::WatchID watchid,
                              const std::string& dir,
                              const std::string& filename,
                              efsw::Action action,
                              std::string oldFilename = "") override;

        // Set the project root (to compute relative paths for ignore checks)
        void set_project_root(const std::filesystem::path& root) { project_root_ = root; }

        // Set the ignore filter (non-owning pointer)
        void set_ignore_filter(const IgnoreFilter* filter) { ignore_filter_ = filter; }

    private:
        // Return true if the extension is a supported source file type
        static bool is_source_file(const std::string& filename);

        std::function<void(std::string)> on_event_;
        std::function<void()> on_wakeup_;
        std::filesystem::path project_root_;
        const IgnoreFilter* ignore_filter_ = nullptr;
    };

    std::function<void(std::string)> on_event_;
    std::function<void()> on_wakeup_;
    std::unique_ptr<efsw::FileWatcher> efsw_watcher_;
    std::unique_ptr<WatcherListener> listener_;
    efsw::WatchID watch_id_ = 0;
    const IgnoreFilter* ignore_filter_ = nullptr;
};

} // namespace codetldr
