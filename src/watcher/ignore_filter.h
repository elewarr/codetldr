#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace codetldr {

/// Pattern-based file path filter using gitignore-style syntax.
/// Supports:
///   - Glob patterns (fnmatch):  *.o, build/*.cpp
///   - Directory patterns:       build/  (trailing slash)
///   - Negation patterns:        !important.o  (last match wins)
///   - Anchored patterns:        /root-only.txt (leading slash)
///
/// Pattern evaluation follows gitignore semantics:
///   - Patterns are evaluated in order; last match wins.
///   - Negated patterns (!) un-ignore previously ignored paths.
///   - Directory patterns only match paths inside that directory.
class IgnoreFilter {
public:
    IgnoreFilter() = default;

    /// Read .codetldrignore from project_root/.codetldrignore.
    /// If the file exists, its patterns replace the built-in defaults.
    /// If the file does not exist, the built-in default patterns are used.
    static IgnoreFilter from_project_root(const std::filesystem::path& root);

    /// Returns true if rel_path (relative to project root) should be ignored.
    /// rel_path must use forward slashes (no leading slash).
    bool should_ignore(const std::filesystem::path& rel_path) const;

    /// Add a single gitignore-style pattern string.
    void add_pattern(const std::string& pattern);

    /// Internal parsed pattern representation (public for use by parse_line helper).
    struct Pattern {
        std::string raw;      // original pattern text (stripped of leading ! and trailing /)
        bool negated;         // true if started with '!'
        bool dir_only;        // true if ended with '/'
        bool anchored;        // true if contained a '/' (other than trailing) => full-path match only
    };

private:

    /// Match a single pattern against a relative path string.
    static bool match_pattern(const Pattern& p, const std::string& rel);

    std::vector<Pattern> patterns_;
};

} // namespace codetldr
