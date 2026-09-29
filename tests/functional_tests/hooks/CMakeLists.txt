##############################################################################
# install_git_hooks in cmake/git_hooks.cmake, run against throwaway repositories
##############################################################################
foreach (scenario IN ITEMS
        hooks_path_is_set
        hooks_path_of_user_is_kept
        hooks_path_from_global_config_is_kept
        no_checkout_is_left_alone)
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Hooks.Install.${scenario}
            COMMAND ${CMAKE_COMMAND}
            -DHOOKS_MODULE=${CMAKE_SOURCE_DIR}/cmake/git_hooks.cmake
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/${scenario}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/${scenario}.cmake
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Hooks.Install.${scenario} PROPERTIES LABELS "functional")
endforeach ()
##############################################################################

##############################################################################
# .githooks/pre-commit, run against a throwaway repository per scenario
##############################################################################
find_program(BASH_PROGRAM bash REQUIRED)

foreach (scenario IN ITEMS
        rejects_unformatted_staged_files
        accepts_formatted_staged_files
        ignores_files_that_are_not_staged
        ignores_files_outside_the_checked_sources
        judges_the_staged_content_not_the_working_tree
        ignores_working_tree_edits_made_after_staging
        allows_deleting_a_source
        missing_clang_format_blocks_a_source_commit
        missing_clang_format_does_not_block_a_docs_commit
        whole_tree_mode_checks_files_that_are_not_staged)
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Hooks.PreCommit.${scenario}
            COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/pre_commit.sh
            ${scenario} ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/pre_commit_${scenario}
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Hooks.PreCommit.${scenario} PROPERTIES LABELS "functional")
endforeach ()
##############################################################################
