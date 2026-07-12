# The r35.x-vintage EDK2 BaseTools are HOST tools and don't build with
# modern host GCC (observed with GCC 16 on CachyOS): C23 became the default
# C standard, which turns the K&R `()` prototypes in the bundled PCCTS
# (antlr/dlg) into hard "too many arguments" errors, and new -Wall warning
# families (e.g. -Wunused-but-set-variable in StringFuncs.c) trip the
# hardcoded -Werror.
#
# Fix the makefiles right before edk2-firmware.inc's do_compile runs make
# on them:
#  - C sources back to gnu17 (BUILD_CFLAGS is C-only; BUILD_CXXFLAGS is
#    separate, so the flag cannot leak into C++ compiles),
#  - pin the aged VfrCompile C++ to gnu++14,
#  - drop -Werror for the host tools (a blanket -Wno-error would lose to
#    the -Werror that follows it on the command line),
#  - PCCTS has its own makefiles with a hardcoded COPT (CRLF line endings,
#    hence the \r handling).
# Every sed is guarded/anchored so re-running do_compile is idempotent.
do_compile:prepend() {
    sed -i \
        -e '/-std=gnu17/! s/^BUILD_CFLAGS = -MD/BUILD_CFLAGS = -std=gnu17 -MD/' \
        -e 's/-Wall -Werror/-Wall/g' \
        -e '/-std=gnu++14/! s/^BUILD_CXXFLAGS = /BUILD_CXXFLAGS = -std=gnu++14 /' \
        ${EDK_TOOLS_PATH}/Source/C/Makefiles/header.makefile
    sed -i \
        -e 's/^COPT=-O\(\r\{0,1\}\)$/COPT=-O -std=gnu17\1/' \
        ${EDK_TOOLS_PATH}/Source/C/VfrCompile/Pccts/antlr/makefile \
        ${EDK_TOOLS_PATH}/Source/C/VfrCompile/Pccts/dlg/makefile
}
