# Usage: cmake -DHOOKS_MODULE=<cmake/git_hooks.cmake> -DWORK_DIR=<scratch dir> -P hooks_path_of_user_is_kept.cmake
#
# A checkout that already names a hooks directory keeps it: configuring must not replace a
# setup the developer chose.

include("${CMAKE_CURRENT_LIST_DIR}/hooks_probe.cmake")
include("${HOOKS_MODULE}")

file(MAKE_DIRECTORY "${WORK_DIR}/repo")
probe_git("${WORK_DIR}/repo" init --quiet)
probe_git("${WORK_DIR}/repo" config --local core.hooksPath ".husky")

install_git_hooks("${WORK_DIR}/repo")

probe_git("${WORK_DIR}/repo" config --local --get core.hooksPath)
if (NOT GIT_OUTPUT STREQUAL ".husky")
    message(FATAL_ERROR "the developer's core.hooksPath was replaced by [${GIT_OUTPUT}]")
endif ()
