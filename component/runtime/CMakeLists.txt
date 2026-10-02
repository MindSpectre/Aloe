set(RUNTIME ${COMPONENT}.Runtime)

##############################################################################
# Work: the intrusive node, the same-shard run queue and the cross-thread inbox
##############################################################################
add_library(${RUNTIME}.Work INTERFACE
        work/work.hpp
        work/run_queue.hpp
        work/inbox.hpp
)
target_include_directories(${RUNTIME}.Work INTERFACE
        work/
)
##############################################################################

##############################################################################
# Runtime exported library
##############################################################################
add_combined_library(${RUNTIME}
        DIRECTORIES
        export
        SOURCES
        export/aloe/runtime
        LIBRARIES
        ${RUNTIME}.Work
)

add_library(Aloe::Component::Runtime ALIAS ${RUNTIME})
##############################################################################
