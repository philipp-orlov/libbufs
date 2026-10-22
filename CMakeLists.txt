# libbufs: header-only zero-copy binary messages, and bufsc, the schema
# compiler that generates their C++ accessors. See README.md.
#
#   add_subdirectory(libbufs)
#   bufs_generate(my_target SCHEMAS a.bufs b.bufs [INCLUDE_DIRS dir...])
#   target_link_libraries(my_target PRIVATE bufs::bufs)
#
# bufs_generate() runs bufsc at build time and puts <stem>.bufs.hpp in a
# directory added to the target's include path.

cmake_minimum_required(VERSION 3.16)
project(libbufs LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

option(BUFS_BUILD_TESTS "Build the libbufs tests" ON)
option(BUFS_BUILD_BENCHMARKS "Build the libbufs benchmarks (see benchmarks/)" OFF)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
endif()

add_library(bufs INTERFACE)
add_library(bufs::bufs ALIAS bufs)
target_include_directories(bufs INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(bufs INTERFACE cxx_std_17)

# bufs/shm.hpp calls shm_open/shm_unlink, which require -lrt on glibc < 2.34.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(BUFS_RT_LIBRARY rt)
    if(BUFS_RT_LIBRARY)
        target_link_libraries(bufs INTERFACE ${BUFS_RT_LIBRARY})
    endif()
endif()

# The schema compiler.
add_executable(bufsc bufsc/main.cpp bufsc/parser.cpp bufsc/generate_cpp.cpp bufsc/check.cpp)
target_compile_options(bufsc PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(bufs::bufsc ALIAS bufsc)
endif()

set(BUFSC_EXECUTABLE "" CACHE FILEPATH "A host bufsc to use when cross-compiling")

function(bufs_generate target)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SCHEMAS;INCLUDE_DIRS" ${ARGN})
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bufs_generated")
    endif()
    if(BUFSC_EXECUTABLE)
        set(bufsc "${BUFSC_EXECUTABLE}")
    else()
        set(bufsc $<TARGET_FILE:bufsc>)
    endif()
    set(includes "")
    foreach(dir ${ARG_INCLUDE_DIRS})
        list(APPEND includes -I "${dir}")
    endforeach()
    set(outputs "")
    foreach(schema ${ARG_SCHEMAS})
        get_filename_component(path "${schema}" ABSOLUTE)
        get_filename_component(stem "${schema}" NAME_WE)
        set(output "${ARG_OUTPUT_DIR}/${stem}.bufs.hpp")
        # Included schemas change the generated layout too: bufsc lists every
        # file it read in a depfile where CMake supports one for this
        # generator; elsewhere every schema of this call is a dependency.
        set(depends "${path}")
        foreach(other ${ARG_SCHEMAS})
            get_filename_component(other "${other}" ABSOLUTE)
            list(APPEND depends "${other}")
        endforeach()
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.20 OR CMAKE_GENERATOR MATCHES "Ninja")
            set(depfile "${ARG_OUTPUT_DIR}/${stem}.bufs.d")
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
                COMMAND ${bufsc} -o "${ARG_OUTPUT_DIR}" ${includes} --depfile "${depfile}" "${path}"
                DEPENDS ${depends} bufsc
                DEPFILE "${depfile}"
                COMMENT "bufsc ${stem}.bufs"
                VERBATIM)
        else()
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
                COMMAND ${bufsc} -o "${ARG_OUTPUT_DIR}" ${includes} "${path}"
                DEPENDS ${depends} bufsc
                COMMENT "bufsc ${stem}.bufs"
                VERBATIM)
        endif()
        list(APPEND outputs "${output}")
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${ARG_OUTPUT_DIR}")
    target_link_libraries(${target} PRIVATE bufs::bufs)
endfunction()

if(BUFS_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(BUFS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
