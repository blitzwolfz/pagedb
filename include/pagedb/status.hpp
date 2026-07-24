#ifndef PAGEDB_STATUS_HPP
#define PAGEDB_STATUS_HPP

#include <string>
#include <utility>

namespace pagedb {

enum class Code {
    Ok = 0,
    NotFound,
    InvalidArgument,
    IoError,
    Corruption,
    PoolExhausted,
    Locked,
    Internal,
};

class Status {
public:
    Status() : code_(Code::Ok) {}
    Status(Code c, std::string msg) : code_(c), msg_(std::move(msg)) {}

    bool ok() const { return code_ == Code::Ok; }
    Code code() const { return code_; }
    const std::string& message() const { return msg_; }

    static Status Ok() { return Status(); }
    static Status NotFound(std::string m) {
        return Status(Code::NotFound, std::move(m));
    }
    static Status InvalidArgument(std::string m) {
        return Status(Code::InvalidArgument, std::move(m));
    }
    static Status IoError(std::string m) {
        return Status(Code::IoError, std::move(m));
    }
    static Status Corruption(std::string m) {
        return Status(Code::Corruption, std::move(m));
    }
    static Status PoolExhausted(std::string m) {
        return Status(Code::PoolExhausted, std::move(m));
    }
    static Status Locked(std::string m) {
        return Status(Code::Locked, std::move(m));
    }
    static Status Internal(std::string m) {
        return Status(Code::Internal, std::move(m));
    }

    std::string to_string() const {
        std::string out = code_name();
        if (!msg_.empty()) {
            out += ": ";
            out += msg_;
        }
        return out;
    }

private:
    const char* code_name() const {
        switch (code_) {
            case Code::Ok:
                return "Ok";
            case Code::NotFound:
                return "NotFound";
            case Code::InvalidArgument:
                return "InvalidArgument";
            case Code::IoError:
                return "IoError";
            case Code::Corruption:
                return "Corruption";
            case Code::PoolExhausted:
                return "PoolExhausted";
            case Code::Locked:
                return "Locked";
            case Code::Internal:
                return "Internal";
        }
        return "Unknown";
    }

    Code code_;
    std::string msg_;
};

// Holds a value or an error. Check ok() before touching value.
template <typename T>
class Result {
public:
    Result(T v) : status_(), value_(std::move(v)) {}
    Result(Status s) : status_(std::move(s)), value_() {}

    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
    T take() { return std::move(value_); }

private:
    Status status_;
    T value_;
};

}  // namespace pagedb

#endif
