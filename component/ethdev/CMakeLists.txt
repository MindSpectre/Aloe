set(ETHDEV ${COMPONENT}.Ethdev)

##############################################################################
# The environment abstraction layer, started once per process
##############################################################################
add_library(${ETHDEV}.Eal STATIC
        eal/eal.hpp
        eal/eal.cpp
)
target_include_directories(${ETHDEV}.Eal PUBLIC
        eal/
)
target_link_libraries(${ETHDEV}.Eal PUBLIC
        Aloe::Dpdk
)
##############################################################################

##############################################################################
# Ethdev exported library
##############################################################################
add_combined_library(${ETHDEV}
        DIRECTORIES
        export
        SOURCES
        export/aloe/ethdev
        LIBRARIES
        ${ETHDEV}.Eal
)

add_library(Aloe::Component::Ethdev ALIAS ${ETHDEV})
##############################################################################
