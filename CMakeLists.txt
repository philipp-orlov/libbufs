# libbufs: header-only zero-copy binary messages, and bufsc, the schema
# compiler that generates their C++ accessors. See README.md.
#
#   add_subdirectory(libbufs)
#   bufs_generate(my_target SCHEMAS a.bufs b.bufs [INCLUDE_DIRS dir...])
#   target_link_libraries(my_target PRIVATE bufs::bufs)
#
# bufs_generate() runs bufsc at build time and puts <stem>.bufs.hpp in a
# directory added to the target's include path. Add TS, CS or PY to write the
# messages for TypeScript (<stem>.bufs.ts), C# (<stem>.bufs.cs) and Python
# (<stem>_bufs.py) next to it; their runtimes are in ts/, cs/ and python/.

# The range turns on the newer policies where CMake has them: CMP0116, for the
# DEPFILEs of the generators (the paths in them are absolute, so either reading
# agrees).
cmake_minimum_required(VERSION 3.16...3.28)
project(libbufs LANGUAGES CXX)

# A build configured without a type gets Release: optimized code, and smaller binaries.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

option(BUFS_BUILD_TESTS "Build the libbufs tests" ON)
option(BUFS_BUILD_FUZZ "Build the fuzz targets (fuzz/)" OFF)
option(BUFS_BUILD_BENCHMARKS "Build the libbufs benchmarks (see benchmarks/)" OFF)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
endif()

# The schema compiler. Its front end (parser, checker, schema model) is libcore's libschema: a sibling checkout by
# default, ignored when the including project already provides the target.
set(BUFS_LIBCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libcore" CACHE PATH "Path to a libcore checkout")
if(NOT TARGET core::schema)
    set(CORE_BUILD_TESTS OFF CACHE BOOL "")
    add_subdirectory("${BUFS_LIBCORE_DIR}" libcore)
endif()
add_library(bufs INTERFACE)
# Shared memory, descriptors and futex waits come from libcore's libsys.
target_link_libraries(bufs INTERFACE $<BUILD_INTERFACE:core::mem> $<BUILD_INTERFACE:core::sys>)
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

# The schema compiler. Its front end (parser, checker, schema model) is libcore's libschema.
add_executable(bufsc bufsc/main.cpp bufsc/generate_cpp.cpp bufsc/generators.cpp bufsc/gen_ts.cpp bufsc/gen_cs.cpp bufsc/gen_py.cpp)
target_link_libraries(bufsc PRIVATE core::schema)
target_compile_options(bufsc PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(bufs::bufsc ALIAS bufsc)
endif()

set(BUFSC_EXECUTABLE "" CACHE FILEPATH "A host bufsc to use when cross-compiling")

function(bufs_generate target)
    cmake_parse_arguments(ARG "TS;CS;PY" "OUTPUT_DIR" "SCHEMAS;INCLUDE_DIRS" ${ARGN})
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
    # TS, CS and PY also write the same messages for TypeScript, C# and Python next to the header.
    set(languages "")
    set(extra_suffixes "")
    if(ARG_TS)
        list(APPEND languages --ts)
        list(APPEND extra_suffixes .bufs.ts)
    endif()
    if(ARG_CS)
        list(APPEND languages --cs)
        list(APPEND extra_suffixes .bufs.cs)
    endif()
    if(ARG_PY)
        list(APPEND languages --py)
        list(APPEND extra_suffixes _bufs.py)
    endif()
    set(outputs "")
    foreach(schema ${ARG_SCHEMAS})
        get_filename_component(path "${schema}" ABSOLUTE)
        get_filename_component(stem "${schema}" NAME_WE)
        set(output "${ARG_OUTPUT_DIR}/${stem}.bufs.hpp")
        set(also "")
        foreach(suffix ${extra_suffixes})
            list(APPEND also "${ARG_OUTPUT_DIR}/${stem}${suffix}")
        endforeach()
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
                OUTPUT "${output}" ${also}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
                COMMAND ${bufsc} -o "${ARG_OUTPUT_DIR}" ${includes} ${languages} --depfile "${depfile}" "${path}"
                DEPENDS ${depends} bufsc
                DEPFILE "${depfile}"
                COMMENT "bufsc ${stem}.bufs"
                VERBATIM)
        else()
            add_custom_command(
                OUTPUT "${output}" ${also}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
                COMMAND ${bufsc} -o "${ARG_OUTPUT_DIR}" ${includes} ${languages} "${path}"
                DEPENDS ${depends} bufsc
                COMMENT "bufsc ${stem}.bufs"
                VERBATIM)
        endif()
        list(APPEND outputs "${output}" ${also})
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${ARG_OUTPUT_DIR}")
    target_link_libraries(${target} PRIVATE bufs::bufs)
endfunction()

if(BUFS_BUILD_TESTS)
    add_subdirectory(tests)
endif()

option(BUFS_BUILD_EXAMPLES "Build the examples (examples/: the same messages in C++, Python, TypeScript and C#)" ON)
if(BUFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUFS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Fuzz targets (fuzz/): libFuzzer executables with Clang, otherwise a small mutation driver that doubles as a ctest
# smoke test (and finds what libFuzzer would in its first minutes under -fsanitize=address,undefined).
if(BUFS_BUILD_FUZZ)
    add_executable(fuzz_verify fuzz/fuzz_verify.cpp)
    bufs_generate(fuzz_verify SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/everything.bufs
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/common.bufs
                  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(fuzz_verify PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz_verify PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        target_sources(fuzz_verify PRIVATE fuzz/fuzz_driver.cpp)
        if(BUILD_TESTING)
            add_test(NAME bufs_fuzz_verify COMMAND fuzz_verify --iterations=20000 --seed=1)
            set_tests_properties(bufs_fuzz_verify PROPERTIES TIMEOUT 300)
        endif()
    endif()
endif()
