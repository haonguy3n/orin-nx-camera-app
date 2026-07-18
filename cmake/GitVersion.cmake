# Derive a build version string from git so a running binary names the exact
# commit it was built from. Produces "<base>+<git-describe>", e.g.
# "0.4.0+538a293" or "0.4.0+538a293-dirty"; falls back to "<base>+unknown"
# when git or the worktree is unavailable (e.g. a source tarball build).
#
# Captured at configure time -- a new commit is reflected on the next CMake
# reconfigure, which CMake runs automatically when any CMakeLists.txt changes.
function(derive_git_version base out_var)
    set(describe "")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=8
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
            OUTPUT_VARIABLE describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    if(describe STREQUAL "")
        set(${out_var} "${base}+unknown" PARENT_SCOPE)
    else()
        set(${out_var} "${base}+${describe}" PARENT_SCOPE)
    endif()
endfunction()
