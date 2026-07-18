/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Mimic of folly/Expected.h (github.com/facebook/folly): a value-or-error
 * discriminated union with folly's core API — implicit construction from
 * Value or Unexpected<Error>, hasValue()/hasError(), value()/error(),
 * operator bool, dereference/arrow, value_or(), makeUnexpected(). Storage
 * is std::variant (boring, correct) instead of folly's hand-rolled union.
 * Dropped: then()/thenOrThrow() monadic chaining, exception-throwing
 * accessors (we build without exceptions in mind: misuse asserts).
 */
#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

namespace camera::base {

template <class Error>
class Unexpected {
public:
    explicit constexpr Unexpected(Error error) : error_(std::move(error)) {}

    Error& error() & { return error_; }
    const Error& error() const& { return error_; }
    Error&& error() && { return std::move(error_); }

private:
    Error error_;
};

template <class Error>
constexpr Unexpected<std::decay_t<Error>> makeUnexpected(Error&& error) {
    return Unexpected<std::decay_t<Error>>(std::forward<Error>(error));
}

template <class Value, class Error>
class Expected {
public:
    /* implicit */ Expected(Value value)
        : storage_(std::in_place_index<0>, std::move(value)) {}

    template <class E>
    /* implicit */ Expected(Unexpected<E> unexpected)
        : storage_(std::in_place_index<1>, std::move(unexpected).error()) {}

    bool hasValue() const noexcept { return storage_.index() == 0; }
    bool hasError() const noexcept { return !hasValue(); }
    explicit operator bool() const noexcept { return hasValue(); }

    Value& value() & { assert(hasValue()); return std::get<0>(storage_); }
    const Value& value() const& {
        assert(hasValue());
        return std::get<0>(storage_);
    }
    Value&& value() && {
        assert(hasValue());
        return std::get<0>(std::move(storage_));
    }

    Error& error() & { assert(hasError()); return std::get<1>(storage_); }
    const Error& error() const& {
        assert(hasError());
        return std::get<1>(storage_);
    }
    Error&& error() && {
        assert(hasError());
        return std::get<1>(std::move(storage_));
    }

    Value& operator*() & { return value(); }
    const Value& operator*() const& { return value(); }
    Value&& operator*() && { return std::move(*this).value(); }
    Value* operator->() { return &value(); }
    const Value* operator->() const { return &value(); }

    template <class U>
    Value value_or(U&& dflt) const& {
        return hasValue() ? value()
                          : static_cast<Value>(std::forward<U>(dflt));
    }

private:
    std::variant<Value, Error> storage_;
};

}  // namespace camera::base
