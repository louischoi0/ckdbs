#pragma once

#include <optional>
#include <string>
#include <utility>

// RocksDB-Status-style explicit error type. rules.md #1: `throw` is
// forbidden everywhere in the engine; every fallible function returns one
// of these instead, and constructors must not fail (fallible construction
// goes through a static factory returning StatusOr<T>).

namespace kds {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kOutOfSpace,
    kNotFound,
    kAlreadyExists,
    kOutOfRange,
    kCorruption,
    kIoError,
};

class [[nodiscard]] Status {
public:
    Status() noexcept : code_(StatusCode::kOk) {}

    static Status OK() { return Status(); }
    static Status InvalidArgument(std::string msg) {
        return Status(StatusCode::kInvalidArgument, std::move(msg));
    }
    static Status OutOfSpace(std::string msg) {
        return Status(StatusCode::kOutOfSpace, std::move(msg));
    }
    static Status NotFound(std::string msg) {
        return Status(StatusCode::kNotFound, std::move(msg));
    }
    static Status AlreadyExists(std::string msg) {
        return Status(StatusCode::kAlreadyExists, std::move(msg));
    }
    static Status OutOfRange(std::string msg) {
        return Status(StatusCode::kOutOfRange, std::move(msg));
    }
    static Status Corruption(std::string msg) {
        return Status(StatusCode::kCorruption, std::move(msg));
    }
    static Status IoError(std::string msg) { return Status(StatusCode::kIoError, std::move(msg)); }

    bool ok() const noexcept { return code_ == StatusCode::kOk; }
    StatusCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    StatusCode code_;
    std::string message_;
};

// Holds either a T or a non-ok Status, never both. Mirrors absl/RocksDB
// StatusOr: fallible constructors/factories return this instead of
// throwing or returning a half-valid T.
template <typename T>
class [[nodiscard]] StatusOr {
public:
    StatusOr(Status status) : status_(std::move(status)) {
        // A StatusOr built from a Status must carry an error: constructing
        // one from Status::OK() with no value would leave value() UB-prone.
    }
    StatusOr(T value) : status_(Status::OK()), value_(std::move(value)) {}

    bool ok() const noexcept { return status_.ok(); }
    const Status& status() const noexcept { return status_; }

    T& value() { return *value_; }
    const T& value() const { return *value_; }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace kds
