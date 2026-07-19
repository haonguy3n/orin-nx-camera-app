/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Mimic of folly/File.h (github.com/facebook/folly): a move-only RAII
 * owner of a file descriptor with folly's API — fd(), release(), close(),
 * closeNoThrow(), swap(), operator bool. Dropped: the path-opening
 * constructor (throws), temporary(), dup(), lock helpers.
 */
#pragma once

#include <unistd.h>

#include <utility>

namespace camera::base {

class File {
public:
    constexpr File() noexcept = default;

    /// Takes control of |fd|; closes it on destruction iff |ownsFd|.
    explicit File(int fd, bool ownsFd = false) noexcept
        : fd_(fd), ownsFd_(ownsFd) {}

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept
        : fd_(other.fd_), ownsFd_(other.ownsFd_) {
        other.fd_ = -1;
        other.ownsFd_ = false;
    }

    File& operator=(File&& other) noexcept {
        File(std::move(other)).swap(*this);
        return *this;
    }

    ~File() { closeNoThrow(); }

    /// The underlying fd, or -1 if empty.
    int fd() const noexcept { return fd_; }

    explicit operator bool() const noexcept { return fd_ != -1; }

    /// Releases ownership; the caller must close the returned fd.
    int release() noexcept {
        int released = fd_;
        fd_ = -1;
        ownsFd_ = false;
        return released;
    }

    /// Closes (if owned). Returns false if ::close failed.
    bool closeNoThrow() noexcept {
        int r = ownsFd_ ? ::close(fd_) : 0;
        release();
        return r == 0;
    }

    /// Folly's close() throws on failure; without exceptions we just
    /// forward to closeNoThrow() and ignore the result.
    void close() noexcept { closeNoThrow(); }

    void swap(File& other) noexcept {
        std::swap(fd_, other.fd_);
        std::swap(ownsFd_, other.ownsFd_);
    }

private:
    int fd_ = -1;
    bool ownsFd_ = false;
};

inline void swap(File& a, File& b) noexcept { a.swap(b); }

}  // namespace camera::base
