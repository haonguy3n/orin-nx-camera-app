/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Trimmed copy of folly/Synchronized.h (github.com/facebook/folly).
 * Kept: the core guarded-data idiom — wlock()/rlock() returning RAII
 * locked pointers, withWLock()/withRLock(), copy() — over
 * std::shared_mutex. Dropped: LockTraits, timed/upgrade locks, and the
 * multi-object acquireLocked()/synchronized() helpers.
 */
#pragma once

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace folly {

/// Couples data with the mutex that guards it. The data is reachable
/// only through a held lock, so unsynchronized access won't compile.
template <typename T, typename Mutex = std::shared_mutex>
class Synchronized {
    template <typename Data, typename Lock>
    class LockedPtr {
    public:
        LockedPtr(Data* data, Mutex& mutex) : lock_(mutex), data_(data) {}
        Data* operator->() const noexcept { return data_; }
        Data& operator*() const noexcept { return *data_; }

    private:
        Lock lock_;
        Data* data_;
    };

public:
    Synchronized() = default;
    explicit Synchronized(T data) : data_(std::move(data)) {}

    Synchronized(const Synchronized&) = delete;
    Synchronized& operator=(const Synchronized&) = delete;

    /// Exclusive (write) access; holds the lock while the pointer lives.
    auto wlock() {
        return LockedPtr<T, std::unique_lock<Mutex>>(&data_, mutex_);
    }

    /// Shared (read) access; holds the lock while the pointer lives.
    auto rlock() const {
        return LockedPtr<const T, std::shared_lock<Mutex>>(&data_, mutex_);
    }

    /// Runs |function| with exclusive access; returns its result.
    template <typename Function>
    auto withWLock(Function&& function) {
        return std::forward<Function>(function)(*wlock());
    }

    /// Runs |function| with shared access; returns its result.
    template <typename Function>
    auto withRLock(Function&& function) const {
        return std::forward<Function>(function)(*rlock());
    }

    /// Snapshot copy of the guarded data.
    T copy() const { return *rlock(); }

private:
    mutable Mutex mutex_;
    T data_;
};

}  // namespace folly
