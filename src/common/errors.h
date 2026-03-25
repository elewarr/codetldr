#pragma once
#include <string>
#include <variant>

namespace codetldr {

enum class ErrorCode {
    kOk = 0,
    kFileNotFound,
    kDatabaseError,
    kWalModeRejected,
    kNotGitRepo,
    kPathError,
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
using Result = std::variant<T, Error>;

template <typename T>
bool is_ok(const Result<T>& r) { return std::holds_alternative<T>(r); }

template <typename T>
const T& get_value(const Result<T>& r) { return std::get<T>(r); }

template <typename T>
const Error& get_error(const Result<T>& r) { return std::get<Error>(r); }

} // namespace codetldr
