# Usage: cmake -DHOOKS_MODULE=<cmake/git_hooks.cmake> -DWORK_DIR=<scratch dir> -P hooks_path_from_global_config_is_kept.cmake
#
# A developer's own hooks directory may come from their global configuration. A setting in
# the checkout would win over it and silently switch those hooks off for this repository,
# so configuring must leave things as they are.

include("${CMAKE_CURRENT_LIST_DIR}/hooks_probe.cmake")
include("${HOOKS_MODULE}")

file(WRITE "${GLOBAL_GITCONFIG}" "[core]\n\thooksPath = /global/hooks\n")
file(MAKE_DIRECTORY "${WORK_DIR}/repo")
probe_git("${WORK_DIR}/repo" init --quiet)

install_git_hooks("${WORK_DIR}/repo")

probe_git("${WORK_DIR}/repo" config --local --get core.hooksPath)
if (GIT_RESULT EQUAL 0)
    message(FATAL_ERROR "the checkout was given core.hooksPath [${GIT_OUTPUT}] over the global one")
endif ()
