set(schemas ${CMAKE_CURRENT_SOURCE_DIR}/schemas)

add_executable(bufs_test_bufs test_bufs.cpp)
bufs_generate(bufs_test_bufs SCHEMAS ${schemas}/everything.bufs ${schemas}/common.bufs ${schemas}/tricky.bufs)
find_package(Threads REQUIRED)
target_link_libraries(bufs_test_bufs PRIVATE Threads::Threads)
add_test(NAME bufs_round_trip COMMAND bufs_test_bufs)

add_executable(bufs_test_shm test_shm.cpp)
bufs_generate(bufs_test_shm SCHEMAS ${schemas}/ring.bufs)
add_test(NAME bufs_shm COMMAND bufs_test_shm)
set_tests_properties(bufs_shm PROPERTIES TIMEOUT 120)

add_executable(bufs_test_local test_local.cpp)
bufs_generate(bufs_test_local SCHEMAS ${schemas}/ring.bufs)
add_test(NAME bufs_local COMMAND bufs_test_local)
set_tests_properties(bufs_local PROPERTIES TIMEOUT 60)

foreach(version 1 2)
    add_executable(bufs_test_evolution_v${version} test_evolution.cpp)
    target_compile_definitions(bufs_test_evolution_v${version} PRIVATE BUFS_EVOLUTION_VERSION=${version})
    bufs_generate(bufs_test_evolution_v${version} SCHEMAS ${schemas}/evolution_v${version}.bufs
                  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/evolution_v${version})
endforeach()
add_test(NAME bufs_evolution
         COMMAND ${CMAKE_COMMAND} -DV1=$<TARGET_FILE:bufs_test_evolution_v1>
                 -DV2=$<TARGET_FILE:bufs_test_evolution_v2> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/evolution
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/evolution.cmake)

add_test(NAME bufs_check_compatible
         COMMAND bufsc --check ${schemas}/evolution_v1.bufs ${schemas}/evolution_v2.bufs)
add_test(NAME bufs_check_incompatible
         COMMAND bufsc --check ${schemas}/evolution_v1.bufs ${schemas}/evolution_bad.bufs)
set_tests_properties(bufs_check_incompatible PROPERTIES WILL_FAIL TRUE)

# Schemas bufsc must reject, each with a located message naming the problem.
foreach(case "union_type|unions are not supported" "unknown_type|unknown type 'Missing'"
        "vector_of_vectors|vectors of vectors" "struct_with_string|struct fields hold"
        "bad_default|is not an integer" "duplicate_field|duplicate field 'x'"
        "out_of_range|'300' does not fit ubyte" "name_collision|generates the accessor hasName"
        "float_overflow|does not fit float32" "shared_type|only a \\[ubyte\\] or \\[byte\\] field"
        "shared_struct|struct fields cannot be shared" "shared_stream|stream parameter cannot be")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 bad)
    list(GET parts 1 expected)
    add_test(NAME bufs_reject_${bad} COMMAND bufsc -o ${CMAKE_CURRENT_BINARY_DIR} ${schemas}/bad/${bad}.bufs)
    set_tests_properties(bufs_reject_${bad} PROPERTIES PASS_REGULAR_EXPRESSION "bufsc: .*${bad}.bufs:[0-9]+: .*${expected}")
endforeach()

# rpc_service blocks (compiled by librpc's rpcc) are parsed, checked and otherwise ignored.
add_test(NAME bufs_accept_rpc_service COMMAND bufsc -o ${CMAKE_CURRENT_BINARY_DIR} ${schemas}/service.bufs)
add_test(NAME bufs_accept_shared COMMAND bufsc -o ${CMAKE_CURRENT_BINARY_DIR} ${schemas}/shared.bufs)
add_test(NAME bufs_reject_service_type COMMAND bufsc -o ${CMAKE_CURRENT_BINARY_DIR} ${schemas}/bad/service_type.bufs)
set_tests_properties(bufs_reject_service_type PROPERTIES
    PASS_REGULAR_EXPRESSION "bufsc: .*service_type.bufs:[0-9]+: .*'Color' \\(S.M\\) is not a table")

# The same messages in every language: C++ writes, TypeScript, C# and Python read, check, encode and write back, C++ verifies
# (tests/languages/run.cmake). Languages whose tools are missing are skipped.
add_executable(bufs_golden languages/golden.cpp)
bufs_generate(bufs_golden SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/languages/kitchen.bufs)
find_program(BUFS_PYTHON NAMES python3 python)
find_program(BUFS_NODE node)
find_program(BUFS_DOTNET dotnet HINTS $ENV{DOTNET_ROOT} $ENV{HOME}/.dotnet)
set(BUFS_ESBUILD_HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../ts/node_modules/.bin ${CMAKE_CURRENT_SOURCE_DIR}/../../librpc/ts/node_modules/.bin)
find_program(BUFS_ESBUILD esbuild HINTS ${BUFS_ESBUILD_HINTS} NO_DEFAULT_PATH)
add_test(NAME bufs_languages
         COMMAND ${CMAKE_COMMAND} -DBUFSC=$<TARGET_FILE:bufsc> -DGOLDEN=$<TARGET_FILE:bufs_golden>
                 -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/.. -DWORK=${CMAKE_CURRENT_BINARY_DIR}/languages
                 -DPYTHON=${BUFS_PYTHON} -DNODE=${BUFS_NODE} -DESBUILD=${BUFS_ESBUILD} -DDOTNET=${BUFS_DOTNET}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/languages/run.cmake)
set_tests_properties(bufs_languages PROPERTIES TIMEOUT 300)
