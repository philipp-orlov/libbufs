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
        "float_overflow|does not fit float32")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 bad)
    list(GET parts 1 expected)
    add_test(NAME bufs_reject_${bad} COMMAND bufsc -o ${CMAKE_CURRENT_BINARY_DIR} ${schemas}/bad/${bad}.bufs)
    set_tests_properties(bufs_reject_${bad} PROPERTIES PASS_REGULAR_EXPRESSION "bufsc: .*${bad}.bufs:[0-9]+: .*${expected}")
endforeach()
