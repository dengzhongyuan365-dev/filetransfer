#pragma once

#include <cassert>
#include <utility>
#include <variant>

#include "lan/common/error.h"

namespace lan {

template <typename T>
class Result {
public:
    static Result success(T value) {
        return Result(std::move(value));
    }

    static Result failure(Error error) {
        return Result(std::move(error));
    }

    bool has_value() const {
        return std::holds_alternative<T>(data_);
    }

    explicit operator bool() const {
        return has_value();
    }

    const T& value() const& {
        assert(has_value());
        return std::get<T>(data_);
    }

    T& value() & {
        assert(has_value());
        return std::get<T>(data_);
    }

    T&& value() && {
        assert(has_value());
        return std::move(std::get<T>(data_));
    }

    const Error& error() const {
        assert(!has_value());
        return std::get<Error>(data_);
    }

private:
    explicit Result(T value) : data_(std::move(value)) {}
    explicit Result(Error error) : data_(std::move(error)) {}

    std::variant<T, Error> data_;
};

}  // namespace lan
