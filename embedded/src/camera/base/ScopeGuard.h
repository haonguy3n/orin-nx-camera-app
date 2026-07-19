/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Trimmed copy of folly/ScopeGuard.h (github.com/facebook/folly).
 * Kept: makeGuard() + SCOPE_EXIT. Dropped: SCOPE_FAIL / SCOPE_SUCCESS
 * (need uncaught-exception tracking we have no use for) and the
 * throwing-copy failsafe (our guards capture by reference only).
 */
#pragma once

#include <type_traits>
#include <utility>

namespace camera::base {
namespace detail {

class ScopeGuardImplBase {
public:
    /// Cancels the guard: the function will not run at scope exit.
    void dismiss() noexcept { dismissed_ = true; }

protected:
    ScopeGuardImplBase() noexcept = default;
    bool dismissed_ = false;
};

template <typename FunctionType>
class ScopeGuardImpl : public ScopeGuardImplBase {
public:
    explicit ScopeGuardImpl(FunctionType fn) noexcept(
        std::is_nothrow_move_constructible<FunctionType>::value)
        : function_(std::move(fn)) {}

    ScopeGuardImpl(ScopeGuardImpl&& other) noexcept(
        std::is_nothrow_move_constructible<FunctionType>::value)
        : function_(std::move(other.function_)) {
        dismissed_ = std::exchange(other.dismissed_, true);
    }

    ScopeGuardImpl(const ScopeGuardImpl&) = delete;
    ScopeGuardImpl& operator=(const ScopeGuardImpl&) = delete;

    ~ScopeGuardImpl() {
        if (!dismissed_)
            function_();
    }

private:
    FunctionType function_;
};

enum class ScopeGuardOnExit {};

template <typename FunctionType>
ScopeGuardImpl<std::decay_t<FunctionType>> operator+(ScopeGuardOnExit,
                                                     FunctionType&& fn) {
    return ScopeGuardImpl<std::decay_t<FunctionType>>(
        std::forward<FunctionType>(fn));
}

}  // namespace detail

/// Returns an object that invokes |fn| when it leaves scope, unless
/// dismiss() was called on it first.
template <typename FunctionType>
detail::ScopeGuardImpl<std::decay_t<FunctionType>> makeGuard(
    FunctionType&& fn) {
    return detail::ScopeGuardImpl<std::decay_t<FunctionType>>(
        std::forward<FunctionType>(fn));
}

}  // namespace camera::base

#define FOLLY_CONCATENATE_IMPL(s1, s2) s1##s2
#define FOLLY_CONCATENATE(s1, s2) FOLLY_CONCATENATE_IMPL(s1, s2)
#define FOLLY_ANONYMOUS_VARIABLE(str) FOLLY_CONCATENATE(str, __COUNTER__)

/// Runs the block when the enclosing scope exits, on every path:
///     SCOPE_EXIT { close(fd); };
#define SCOPE_EXIT                                    \
    auto FOLLY_ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) = \
        ::camera::base::detail::ScopeGuardOnExit() + [&]() noexcept
