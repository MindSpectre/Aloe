##############################################################################
# The vcpkg bootstrap shim, exercised on a throwaway copy of itself
##############################################################################
foreach (scenario IN ITEMS explicit_root_is_used failed_provisioning_is_legible foreign_directory_is_kept)
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Bootstrap.${scenario}
            COMMAND ${CMAKE_COMMAND}
            -DSHIM=${CMAKE_SOURCE_DIR}/cmake/vcpkg-bootstrap.cmake
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/${scenario}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/${scenario}.cmake
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Bootstrap.${scenario} PROPERTIES LABELS "functional")
endforeach ()
##############################################################################
