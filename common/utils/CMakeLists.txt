set(UTILS ${COMMON}.Utils)

##############################################################################
# Values: discarding values on purpose, and choosing a fallback
##############################################################################
add_library(${UTILS}.Values INTERFACE
        values/values.hpp
)
target_include_directories(${UTILS}.Values INTERFACE
        values/
)
##############################################################################

##############################################################################
# Guards: static assertions that pin a member function as mutating or as an instance function
##############################################################################
add_library(${UTILS}.Guards INTERFACE
        guards/guards.hpp
)
target_include_directories(${UTILS}.Guards INTERFACE
        guards/
)
##############################################################################

##############################################################################
# Utils exported library
##############################################################################
add_combined_library(${UTILS}
        DIRECTORIES
        export
        SOURCES
        export/aloe/utils
        LIBRARIES
        ${UTILS}.Values
        ${UTILS}.Guards
)

add_library(Aloe::Common::Utils ALIAS ${UTILS})
##############################################################################
