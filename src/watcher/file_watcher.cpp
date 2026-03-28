#include "watcher/file_watcher.h"
#include <unordered_set>
#include <string>
#include <filesystem>

namespace codetldr {

// Set of source file extensions that trigger events
static const std::unordered_set<std::string> kSourceExtensions = {
    ".cpp", ".h", ".py", ".js", ".ts", ".java", ".kt", ".swift",
    ".m", ".rs", ".c", ".cc", ".cxx", ".hpp", ".tsx", ".jsx",
    ".rb", ".rake", ".gemspec", ".ru",
    ".lua"
};

// ---- WatcherListener ----

FileWatcher::WatcherListener::WatcherListener(
    std::function<void(std::string)> on_event,
    std::function<void()> on_wakeup)
    : on_event_(std::move(on_event))
    , on_wakeup_(std::move(on_wakeup)) {}

bool FileWatcher::WatcherListener::is_source_file(const std::string& filename) {
    std::filesystem::path p(filename);
    std::string ext = p.extension().string();
    return kSourceExtensions.count(ext) > 0;
}

void FileWatcher::WatcherListener::handleFileAction(
    efsw::WatchID /*watchid*/,
    const std::string& dir,
    const std::string& filename,
    efsw::Action action,
    std::string /*oldFilename*/)
{
    // CRITICAL: Must return immediately — no blocking I/O, no analysis, no database access.

    // Handle Modified, Add (create), and Delete events for source files
    if (action == efsw::Actions::Modified ||
        action == efsw::Actions::Add ||
        action == efsw::Actions::Delete)
    {
        // Check extension for source files
        if (!is_source_file(filename)) {
            return;
        }

        // Construct full path from dir + filename
        std::string full_path = dir;
        // Ensure trailing slash
        if (!full_path.empty() && full_path.back() != '/') {
            full_path += '/';
        }
        full_path += filename;

        // Check ignore filter if set — compute relative path from project root
        if (ignore_filter_ && !project_root_.empty()) {
            std::filesystem::path abs_path(full_path);
            std::filesystem::path rel_path;
            // Attempt to make relative — if not under project root, skip
            try {
                rel_path = std::filesystem::relative(abs_path, project_root_);
            } catch (...) {
                rel_path = abs_path;
            }
            if (ignore_filter_->should_ignore(rel_path)) {
                return;
            }
        }

        // Invoke callbacks — expected to be thread-safe (pipe write or concurrent queue push)
        on_event_(full_path);
        on_wakeup_();
    }
}

// ---- FileWatcher ----

FileWatcher::FileWatcher(std::function<void(std::string)> on_event,
                         std::function<void()> on_wakeup)
    : on_event_(std::move(on_event))
    , on_wakeup_(std::move(on_wakeup)) {}

FileWatcher::~FileWatcher() {
    stop();
}

void FileWatcher::set_ignore_filter(const IgnoreFilter* filter) {
    ignore_filter_ = filter;
}

void FileWatcher::start(const std::filesystem::path& project_root) {
    listener_ = std::make_unique<WatcherListener>(on_event_, on_wakeup_);

    // Wire ignore filter and project root into listener
    listener_->set_project_root(project_root);
    if (ignore_filter_) {
        listener_->set_ignore_filter(ignore_filter_);
    }

    efsw_watcher_ = std::make_unique<efsw::FileWatcher>();

    // Add recursive watch
    watch_id_ = efsw_watcher_->addWatch(project_root.string(), listener_.get(), /*recursive=*/true);

    // Start watching in background thread
    efsw_watcher_->watch();
}

void FileWatcher::stop() {
    if (efsw_watcher_ && watch_id_ > 0) {
        efsw_watcher_->removeWatch(watch_id_);
        watch_id_ = 0;
    }
    efsw_watcher_.reset();
    listener_.reset();
}

} // namespace codetldr
