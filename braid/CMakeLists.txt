cmake_minimum_required(VERSION 2.8)

find_package(MPI COMPONENTS C REQUIRED)
find_package(HYPRE REQUIRED)

include_directories( ${MPI_C_INCLUDE_DIRS} )

file(GLOB BRAID_SRC "*.c")
add_library(braid ${BRAID_SRC})

target_link_libraries(braid ${MPI_C_LIBRARIES})
