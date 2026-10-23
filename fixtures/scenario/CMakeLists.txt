set(SCENARIO ${FIXTURES}.Scenario)

##############################################################################
# The script language: the model, the parser and the notation
##############################################################################
add_library(${SCENARIO}.Script STATIC
        script/script.hpp
        script/parse_script.hpp
        script/parse_script.cpp
        script/notation.hpp
        script/notation.cpp
)
target_include_directories(${SCENARIO}.Script PUBLIC
        script/
)
target_link_libraries(${SCENARIO}.Script PUBLIC
        Aloe::Common::Core
        Aloe::Common::Wire
)
##############################################################################

##############################################################################
# The run side: the peer's numbers and the matcher
##############################################################################
add_library(${SCENARIO}.Run STATIC
        run/peer.hpp
        run/peer.cpp
        run/match.hpp
        run/match.cpp
        run/harness.hpp
        run/harness.cpp
        run/runner.hpp
        run/runner.cpp
)
target_include_directories(${SCENARIO}.Run PUBLIC
        run/
)
target_link_libraries(${SCENARIO}.Run PUBLIC
        ${SCENARIO}.Script
        Aloe::Fixtures::Frames
        Aloe::Fixtures::Fabric
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Loop
)
##############################################################################

##############################################################################
# Scenario exported library
##############################################################################
add_combined_library(${SCENARIO}
        DIRECTORIES
        export
        SOURCES
        export/aloe/scenario
        LIBRARIES
        ${SCENARIO}.Script
        ${SCENARIO}.Run
)

add_library(Aloe::Fixtures::Scenario ALIAS ${SCENARIO})
##############################################################################
