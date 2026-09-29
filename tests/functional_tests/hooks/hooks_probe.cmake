# Shared by the install_git_hooks scenarios: an isolated git and a helper to run it in a
# scratch repository. Expects WORK_DIR to be set by the including script.

find_program(GIT git REQUIRED)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# The developer's own git configuration must not change what a scenario sees, and the
# scenarios must be able to prove that the global configuration was left alone.
set(GLOBAL_GITCONFIG "${WORK_DIR}/global.gitconfig")
file(WRITE "${GLOBAL_GITCONFIG}" "")
set(ENV{GIT_CONFIG_GLOBAL} "${GLOBAL_GITCONFIG}")
set(ENV{GIT_CONFIG_NOSYSTEM} 1)

# probe_git(<repository> <git arguments>...)
# Runs git in <repository>; the caller sees GIT_RESULT and GIT_OUTPUT.
macro (probe_git repository)
    execute_process(
            COMMAND "${GIT}" -C "${repository}" ${ARGN}
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_OUTPUT
            ERROR_VARIABLE GIT_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endmacro ()
