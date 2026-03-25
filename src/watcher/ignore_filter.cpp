#include "watcher/ignore_filter.h"
#include <fnmatch.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace codetldr {

// Default patterns when no .codetldrignore file is present.
// These mirror common gitignore patterns for build artifacts and dependency directories.
static const std::vector<std::string> kDefaultPatterns = {
    "build/",
    ".build/",
    "node_modules/",
    ".git/",
    "vendor/",
    ".codetldr/",
    "dist/",
    "out/",
    "target/",
    "*.o",
    "*.a",
    "*.so",
    "*.dylib",
};

// Parse a raw line into a Pattern struct.
// Returns false if the line is empty or a comment.
static bool parse_line(const std::string& line, IgnoreFilter::Pattern& out) {
    std::string s = line;

    // Trim trailing whitespace/carriage return
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }

    // Skip blank lines and comments
    if (s.empty() || s[0] == '#') {
        return false;
    }

    out.negated = false;
    out.dir_only = false;
    out.anchored = false;

    // Check for negation
    if (s[0] == '!') {
        out.negated = true;
        s = s.substr(1);
    }

    // Check for directory-only pattern (trailing slash)
    if (!s.empty() && s.back() == '/') {
        out.dir_only = true;
        s.pop_back();  // remove trailing slash for matching
    }

    // Check if anchored: pattern contains '/' (other than a leading slash which we strip)
    // A leading '/' means the pattern is anchored to the root — strip it
    if (!s.empty() && s[0] == '/') {
        s = s.substr(1);
        out.anchored = true;
    } else {
        // If the cleaned pattern contains '/', it's anchored (full-path match only)
        out.anchored = (s.find('/') != std::string::npos);
    }

    out.raw = s;
    return !s.empty();
}

// static
IgnoreFilter IgnoreFilter::from_project_root(const std::filesystem::path& root) {
    IgnoreFilter filter;
    std::filesystem::path ignore_file = root / ".codetldrignore";

    if (std::filesystem::exists(ignore_file)) {
        std::ifstream ifs(ignore_file);
        std::string line;
        while (std::getline(ifs, line)) {
            // Use add_pattern to re-use the same parsing logic
            filter.add_pattern(line);
        }
    } else {
        // Use built-in defaults
        for (const auto& p : kDefaultPatterns) {
            filter.add_pattern(p);
        }
    }

    return filter;
}

void IgnoreFilter::add_pattern(const std::string& pattern) {
    Pattern p;
    if (parse_line(pattern, p)) {
        patterns_.push_back(std::move(p));
    }
}

// static
bool IgnoreFilter::match_pattern(const Pattern& p, const std::string& rel) {
    // rel is the full relative path, e.g. "src/main.cpp" or "build/foo.o"

    if (p.dir_only) {
        // Match if rel starts with "dir/" or equals "dir"
        // e.g. pattern "build" (dir_only) matches "build/foo.o"
        // We check if rel starts with p.raw + "/"  or rel == p.raw
        if (rel == p.raw) return true;
        if (rel.size() > p.raw.size() + 1 &&
            rel[p.raw.size()] == '/' &&
            rel.substr(0, p.raw.size()) == p.raw) {
            return true;
        }
        // Also handle nested: "build" matches "project/build/foo" — only if not anchored
        if (!p.anchored) {
            // Check if any path segment equals p.raw
            // e.g. "node_modules" should match "src/node_modules/pkg/file.js"
            std::string seg_prefix = "/" + p.raw + "/";
            std::string checked = "/" + rel;
            if (checked.find(seg_prefix) != std::string::npos) {
                return true;
            }
            // Also check if path starts with the segment
            std::string dir_prefix = p.raw + "/";
            if (rel.size() >= dir_prefix.size() &&
                rel.substr(0, dir_prefix.size()) == dir_prefix) {
                return true;
            }
        }
        return false;
    }

    if (p.anchored) {
        // Anchored patterns: match against full relative path only
        // Try fnmatch on the full path
        if (::fnmatch(p.raw.c_str(), rel.c_str(), FNM_PATHNAME) == 0) {
            return true;
        }
        return false;
    }

    // Unanchored file pattern (e.g., *.o):
    // Match against basename first, then against full path
    std::string basename = rel;
    auto slash = rel.rfind('/');
    if (slash != std::string::npos) {
        basename = rel.substr(slash + 1);
    }

    // Try basename match (no FNM_PATHNAME — allow wildcard to match bare filename)
    if (::fnmatch(p.raw.c_str(), basename.c_str(), 0) == 0) {
        return true;
    }

    // Try full path match with FNM_PATHNAME
    if (::fnmatch(p.raw.c_str(), rel.c_str(), FNM_PATHNAME) == 0) {
        return true;
    }

    return false;
}

bool IgnoreFilter::should_ignore(const std::filesystem::path& rel_path) const {
    // Convert to string with forward slashes, no leading slash
    std::string rel = rel_path.lexically_normal().string();
    // Replace backslashes on Windows (defensive)
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    // Strip leading slash if any
    if (!rel.empty() && rel[0] == '/') {
        rel = rel.substr(1);
    }

    bool ignored = false;

    // Last-match-wins (gitignore semantics)
    for (const auto& p : patterns_) {
        if (match_pattern(p, rel)) {
            ignored = !p.negated;
        }
    }

    return ignored;
}

} // namespace codetldr
