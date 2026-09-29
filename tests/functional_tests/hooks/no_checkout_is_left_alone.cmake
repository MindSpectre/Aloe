# Usage: cmake -DHOOKS_MODULE=<cmake/git_hooks.cmake> -DWORK_DIR=<scratch dir> -P no_checkout_is_left_alone.cmake
#
# Sources that are not a git checkout of their own - a release tarball unpacked inside some
# other repository, say - must not make configuring edit that other repository's settings,
# nor the developer's global ones.

include("${CMAKE_CURRENT_LIST_DIR}/hooks_probe.cmake")
include("${HOOKS_MODULE}")

file(MAKE_DIRECTORY "${WORK_DIR}/outer/vendor/aloe")
probe_git("${WORK_DIR}/outer" init --quiet)

install_git_hooks("${WORK_DIR}/outer/vendor/aloe")

probe_git("${WORK_DIR}/outer" config --local --get core.hooksPath)
if (GIT_RESULT EQUAL 0)
    message(FATAL_ERROR "the enclosing repository was given core.hooksPath [${GIT_OUTPUT}]")
endif ()

file(READ "${GLOBAL_GITCONFIG}" global_config)
if (NOT global_config STREQUAL "")
    message(FATAL_ERROR "the global git configuration was edited:\n${global_config}")
endif ()
