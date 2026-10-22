# libbufs benchmarks: libbufs vs FlatBuffers vs a JSON baseline, and the
# shared-memory ring vs sockets. See README.md.
#
#   cmake -S libbufs -B build-bench -DCMAKE_BUILD_TYPE=Release \
#         -DBUFS_BUILD_BENCHMARKS=ON -DFLATBUFFERS_DIR=/path/to/flatbuffers
#
# FLATBUFFERS_DIR is a FlatBuffers source checkout with flatc built in it
# (FLATBUFFERS_FLATC overrides where flatc is). Without it the FlatBuffers
# rows are left out. The JSON rows need libhttp's json from the surrounding
# tree (external/libhttp) and the minimal JsonLite parser kept in baseline/;
# they are left out when those sources are not there.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "libbufs benchmarks: no CMAKE_BUILD_TYPE, numbers will be unoptimized")
endif()

set(FLATBUFFERS_DIR "" CACHE PATH "FlatBuffers source checkout (with flatc built) for the benchmark rows")
set(FLATBUFFERS_FLATC "" CACHE FILEPATH "flatc to use (default: FLATBUFFERS_DIR/build/flatc or FLATBUFFERS_DIR/flatc)")

set(bench_schema ${CMAKE_CURRENT_SOURCE_DIR}/bench.bufs)

add_executable(bufs_bench bench_main.cpp bench_alloc.cpp bench_bufs.cpp)
bufs_generate(bufs_bench SCHEMAS ${bench_schema})
target_include_directories(bufs_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bufs_bench PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
find_package(Threads REQUIRED)
target_link_libraries(bufs_bench PRIVATE Threads::Threads)

# --------------------------------------------------------------- FlatBuffers --
if(FLATBUFFERS_DIR)
    if(NOT FLATBUFFERS_FLATC)
        foreach(candidate ${FLATBUFFERS_DIR}/build/flatc ${FLATBUFFERS_DIR}/flatc)
            if(EXISTS ${candidate} AND NOT FLATBUFFERS_FLATC)
                set(FLATBUFFERS_FLATC ${candidate})
            endif()
        endforeach()
    endif()
    if(NOT EXISTS "${FLATBUFFERS_DIR}/include/flatbuffers/flatbuffers.h" OR NOT FLATBUFFERS_FLATC)
        message(FATAL_ERROR "FLATBUFFERS_DIR=${FLATBUFFERS_DIR}: need include/flatbuffers/flatbuffers.h and a built "
                            "flatc (build/flatc, or set FLATBUFFERS_FLATC)")
    endif()
    set(fb_out ${CMAKE_CURRENT_BINARY_DIR}/flatc_generated)
    add_custom_command(
        OUTPUT ${fb_out}/bench_generated.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${fb_out}
        COMMAND ${FLATBUFFERS_FLATC} --cpp -o ${fb_out} ${bench_schema}
        DEPENDS ${bench_schema} ${FLATBUFFERS_FLATC}
        COMMENT "flatc bench.bufs"
        VERBATIM)
    target_sources(bufs_bench PRIVATE bench_flatbuffers.cpp ${fb_out}/bench_generated.h)
    set_source_files_properties(bench_flatbuffers.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${fb_out};${FLATBUFFERS_DIR}/include")
    target_compile_definitions(bufs_bench PRIVATE BENCH_HAVE_FLATBUFFERS=1)
    message(STATUS "libbufs benchmarks: FlatBuffers from ${FLATBUFFERS_DIR}, flatc ${FLATBUFFERS_FLATC}")
else()
    message(STATUS "libbufs benchmarks: no FLATBUFFERS_DIR, FlatBuffers rows skipped")
endif()

# ----------------------------------------------------------------------- JSON --
# The sources are compiled in directly (they depend on nothing else), so this
# works whether or not the libhttp target exists in this build.
get_filename_component(nns_root ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(libhttp_dir ${nns_root}/external/libhttp)
set(jsonlite_dir ${CMAKE_CURRENT_SOURCE_DIR}/baseline)
if(EXISTS ${libhttp_dir}/src/json.cpp AND EXISTS ${libhttp_dir}/src/base64.cpp
   AND EXISTS ${jsonlite_dir}/internal/JsonLite.cpp)
    add_library(bufs_bench_json STATIC bench_json.cpp ${libhttp_dir}/src/json.cpp ${libhttp_dir}/src/base64.cpp
                                       ${jsonlite_dir}/internal/JsonLite.cpp)
    target_include_directories(bufs_bench_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${libhttp_dir}/include
                                                       ${jsonlite_dir})
    target_compile_features(bufs_bench_json PRIVATE cxx_std_17)
    target_link_libraries(bufs_bench PRIVATE bufs_bench_json)
    target_compile_definitions(bufs_bench PRIVATE BENCH_HAVE_JSON=1)
else()
    message(STATUS "libbufs benchmarks: libhttp json / JsonLite sources not found, JSON rows skipped")
endif()

# ----------------------------------------------------- shared-memory transport --
add_executable(bufs_bench_shm bench_shm.cpp)
bufs_generate(bufs_bench_shm SCHEMAS ${bench_schema} OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/bufs_generated_shm)
target_include_directories(bufs_bench_shm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bufs_bench_shm PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
target_link_libraries(bufs_bench_shm PRIVATE Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(bufs_bench_shm PRIVATE rt)
endif()
