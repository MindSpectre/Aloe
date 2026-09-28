# Usage: cmake -DHOOKS_MODULE=<cmake/git_hooks.cmake> -DWORK_DIR=<scratch dir> -P hooks_path_is_set.cmake
#
# Configuring must point a checkout's core.hooksPath at its tracked .githooks directory, so
# the hooks run without a setup step.

include("${CMAKE_CURRENT_LIST_DIR}/hooks_probe.cmake")
include("${HOOKS_MODULE}")

file(MAKE_DIRECTORY "${WORK_DIR}/repo")
probe_git("${WORK_DIR}/repo" init --quiet)

install_git_hooks("${WORK_DIR}/repo")

probe_git("${WORK_DIR}/repo" config --local --get core.hooksPath)
if (NOT GIT_OUTPUT STREQUAL ".githooks")
    message(FATAL_ERROR "core.hooksPath is [${GIT_OUTPUT}], not [.githooks]")
endif ()
