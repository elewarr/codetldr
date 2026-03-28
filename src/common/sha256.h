#pragma once
// Cross-platform SHA256 file and string digest.
// Returns 64-character lowercase hex string, or "" on error.

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>

inline std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        CC_SHA256_Update(&ctx, buf, static_cast<CC_LONG>(f.gcount()));
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    std::string hex;
    hex.reserve(64);
    for (auto b : digest) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        hex += h;
    }
    return hex;
}

// Returns 64-char hex SHA256 of the input string.
inline std::string sha256_string(const std::string& input) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, input.data(), static_cast<CC_LONG>(input.size()));
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    std::string hex;
    hex.reserve(64);
    for (auto b : digest) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        hex += h;
    }
    return hex;
}

#else

inline std::string sha256_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return "";
    std::string escaped = path.string();
    // Escape single quotes in path for shell safety
    std::string safe;
    safe.reserve(escaped.size() + 2);
    safe += '\'';
    for (char c : escaped) {
        if (c == '\'') {
            safe += "'\\''";
        } else {
            safe += c;
        }
    }
    safe += '\'';
    std::string cmd = "sha256sum " + safe + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[256] = {};
    if (!fgets(buf, sizeof(buf), p)) {
        pclose(p);
        return "";
    }
    pclose(p);
    std::string result(buf);
    auto space = result.find(' ');
    return space != std::string::npos ? result.substr(0, space) : "";
}

inline std::string sha256_string(const std::string& input) {
    char tmppath[] = "/tmp/codetldr-sha256-XXXXXX";
    int fd = ::mkstemp(tmppath);
    if (fd < 0) return "";
    ::write(fd, input.data(), input.size());
    ::close(fd);
    std::string result = sha256_file(tmppath);
    ::unlink(tmppath);
    return result;
}

#endif
