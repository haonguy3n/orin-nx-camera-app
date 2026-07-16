/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Mimic of folly/FileUtil.h (github.com/facebook/folly): readFull() and
 * writeFull() with folly's exact semantics, implemented locally as a
 * header. Dropped: pread/pwrite/readv variants, closeNoInt, fsyncNoInt,
 * readFile/writeFile — add when a caller appears.
 */
#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstddef>

namespace folly {

/// Reads until |count| bytes or EOF. Returns the number of bytes read
/// (less than |count| only on EOF), or -1 on error. Retries EINTR.
inline ssize_t readFull(int fd, void* buf, size_t count) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < count) {
        ssize_t n = ::read(fd, p + got, count - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)  // EOF
            break;
        got += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(got);
}

/// Writes until |count| bytes are sent, handling partial writes.
/// Returns the number of bytes written (== |count| unless ::write
/// returned 0, as folly's wrapFull does), or -1 on error. Retries EINTR.
inline ssize_t writeFull(int fd, const void* buf, size_t count) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < count) {
        ssize_t n = ::write(fd, p + sent, count - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)  // no progress possible; don't spin forever
            break;
        sent += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

}  // namespace folly
