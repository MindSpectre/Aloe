set(SCENARIO ${FIXTURES}.Scenario)

##############################################################################
# The script language: the model, the parser and the notation
##############################################################################
add_library(${SCENARIO}.Script STATIC
        script/script.hpp
        script/parse_script.hpp
        script/parse_script.cpp
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
# Scenario exported library
##############################################################################
add_combined_library(${SCENARIO}
        DIRECTORIES
        export
        SOURCES
        export/aloe/scenario
        LIBRARIES
        ${SCENARIO}.Script
)

add_library(Aloe::Fixtures::Scenario ALIAS ${SCENARIO})
##############################################################################
