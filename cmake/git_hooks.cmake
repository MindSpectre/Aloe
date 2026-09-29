# install_git_hooks(<checkout>)
#
# Points the checkout's core.hooksPath at its tracked .githooks directory, so a developer
# gets the hooks from the configure step they run anyway. It does nothing when
#   - <checkout> has no .git of its own: `git -C` would find an enclosing repository and
#     edit that one instead;
#   - core.hooksPath is already set, in any configuration file: a setting here would win
#     over a global one and silently switch the developer's own hooks off;
#   - git is missing or refuses to touch the checkout, as in a container that runs as a
#     different user than the one who owns the sources.
# The write is --local so that it can never land in the global configuration.
function(install_git_hooks checkout)
    find_program(_igh_git git)
    if (NOT _igh_git OR NOT EXISTS "${checkout}/.git")
        message("Git hooks: not a git checkout, nothing to install")
        return()
    endif ()

    execute_process(
            COMMAND "${_igh_git}" -C "${checkout}" config --get core.hooksPath
            OUTPUT_VARIABLE _igh_current
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _igh_result
            ERROR_QUIET
    )
    if (_igh_result EQUAL 0)
        message("Git hooks: core.hooksPath is already ${_igh_current}, left alone")
        return()
    endif ()
    # `git config --get` exits 1 for a key that is not set; anything else is a refusal.
    if (NOT _igh_result EQUAL 1)
        message("Git hooks: git would not read the checkout's configuration, nothing installed")
        return()
    endif ()

    execute_process(
            COMMAND "${_igh_git}" -C "${checkout}" config --local core.hooksPath .githooks
            RESULT_VARIABLE _igh_result
            ERROR_QUIET
    )
    if (_igh_result EQUAL 0)
        message("Git hooks: core.hooksPath set to .githooks")
    else ()
        message("Git hooks: git would not write the checkout's configuration, nothing installed")
    endif ()
endfunction()
