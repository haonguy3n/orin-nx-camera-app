// Self-check for the vendored folly subset (src/camera/base/).
// Run from embedded/ (one line):
//   g++ -std=c++17 -Wall -Wextra -I src $(pkg-config --cflags --libs
//   glib-2.0) src/camera/base/test/base_check.cpp -o /tmp/base_check && /tmp/base_check
#include <cassert>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <fcntl.h>

#include "camera/base/Expected.h"
#include "camera/base/File.h"
#include "camera/base/FileUtil.h"
#include "camera/base/ScopeGuard.h"
#include "camera/base/Synchronized.h"
#include "camera/base/Unit.h"
#include "camera/base/logging/xlog.h"

int main() {
    // SCOPE_EXIT runs at scope end, in reverse order.
    int order = 0, first = 0, second = 0;
    {
        SCOPE_EXIT { first = ++order; };
        SCOPE_EXIT { second = ++order; };
    }
    assert(second == 1 && first == 2);

    // dismiss() cancels the guard.
    bool ran = false;
    {
        auto g = camera::base::makeGuard([&] { ran = true; });
        g.dismiss();
    }
    assert(!ran);

    // Synchronized: hammer a string from writers while readers copy.
    // Under the old plain-string members this was the data race; with
    // the lock every copy must observe a complete string.
    struct Status { int n = 0; std::string name; };
    camera::base::Synchronized<Status> status;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 10000; ++i) {
                status.withWLock([&](Status& s) {
                    s.n = i;
                    s.name = (t % 2) ? "a-fairly-long-artifact-name.swu"
                                     : "x.swu";
                });
                Status snap = status.copy();
                assert(snap.name == "a-fairly-long-artifact-name.swu" ||
                       snap.name == "x.swu");
            }
        });
    }
    for (auto& th : threads)
        th.join();
    assert(status.rlock()->n == 9999);

    // FileUtil: writeFull/readFull round-trip through a pipe; readFull
    // returns a short count only at EOF.
    int fds[2];
    assert(pipe(fds) == 0);
    const char msg[] = "0123456789abcdef";
    assert(camera::base::writeFull(fds[1], msg, sizeof(msg)) ==
           static_cast<ssize_t>(sizeof(msg)));
    char in[sizeof(msg)];
    assert(camera::base::readFull(fds[0], in, sizeof(in)) ==
           static_cast<ssize_t>(sizeof(in)));
    assert(std::string(in) == msg);
    close(fds[1]);  // EOF on the read side
    assert(camera::base::readFull(fds[0], in, sizeof(in)) == 0);
    close(fds[0]);

    // xlog: basename extraction + the macro expands and logs.
    using camera::base::detail::xlogBasename;
    assert(std::string(xlogBasename("src/camera/core/Watchdog.cpp")) ==
           "Watchdog.cpp");
    assert(std::string(xlogBasename("no_dirs.cpp")) == "no_dirs.cpp");
    XLOGF(INFO, "base_check: xlog smoke test, answer=%d", 42);

    // Expected: value and error paths, value_or, same-type Value/Error.
    auto ok = [](int v) -> camera::base::Expected<int, std::string> { return v; };
    auto bad = [](std::string e) -> camera::base::Expected<int, std::string> {
        return camera::base::makeUnexpected(std::move(e));
    };
    assert(ok(7) && *ok(7) == 7 && ok(7).value_or(-1) == 7);
    assert(!bad("nope") && bad("nope").error() == "nope");
    assert(bad("x").value_or(-1) == -1);
    camera::base::Expected<std::string, std::string> same("v");
    assert(same.hasValue() && same.value() == "v");
    camera::base::Expected<camera::base::Unit, int> u(camera::base::unit);
    assert(u.hasValue());

    // File: RAII close, move transfers ownership, release disowns.
    int raw = -1;
    {
        camera::base::File f(open("/dev/null", O_RDONLY), /*ownsFd=*/true);
        assert(f && f.fd() >= 0);
        camera::base::File g(std::move(f));
        assert(!f && g);
        raw = g.release();
        assert(!g && raw >= 0);
    }  // g's dtor must NOT close raw (released)
    assert(fcntl(raw, F_GETFD) != -1);  // still open
    {
        camera::base::File h(raw, /*ownsFd=*/true);
    }  // h closes raw
    assert(fcntl(raw, F_GETFD) == -1 && errno == EBADF);  // now closed

    // File: self-move must not close or lose the fd.
    {
        camera::base::File s(open("/dev/null", O_RDONLY), /*ownsFd=*/true);
        int sfd = s.fd();
        assert(sfd >= 0);
        camera::base::File& alias = s;  // dodge -Wself-move
        s = std::move(alias);
        assert(s.fd() == sfd && fcntl(sfd, F_GETFD) != -1);
    }

    return 0;
}
