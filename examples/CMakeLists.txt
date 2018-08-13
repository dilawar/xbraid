cmake_minimum_required(VERSION 2.8)

find_package(HYPRE REQUIRED)
find_package(MPI COMPONENTS C REQUIRED)
include_directories( ${HYPRE_INCLUDE_DIRS} ${MPI_C_INCLUDE_DIRS} )
include_directories( ${CMAKE_CURRENT_SOURCE_DIR}/../braid )

set(EXAMPLE_SRCS
    # ex-01-adjoint.c
    ex-01.c
    # ex-01-expanded-bdf2.c
    # ex-01-expanded.c
    ex-01-optimization.c
    # ex-01-refinement.c
    ex-02.c
    # ex-02-lib.c
    ex-02-serial.c
    # ex-03.c
    # ex-03-lib.c
    # ex-03-serial.c
    # ex-04.c
    ex-04-lib.c
    ex-04-serial.c
    # vis.c
    )

# message(STATUS "Found ${EXAMPLE_SRCS}")
foreach( _exfile ${EXAMPLE_SRCS} )
    get_filename_component(TGTNAME ${_exfile} NAME_WE)
    message(STATUS "Compiling ${_exfile}")
    add_executable( ${TGTNAME} ${_exfile} )
    target_link_libraries(${TGTNAME} braid ${HYPRE_LIBRARIES} m )
endforeach()


