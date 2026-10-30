# The team example in C++; the other languages' versions (python/, ts/, cs/) are run against it by run_examples.py.
add_executable(bufs_example_team cpp/team.cpp)
bufs_generate(bufs_example_team SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/team.bufs)
set_target_properties(bufs_example_team PROPERTIES OUTPUT_NAME team)

if(BUFS_BUILD_TESTS)
    find_program(BUFS_EXAMPLES_PYTHON NAMES python3 python)
    find_program(BUFS_EXAMPLES_NODE node)
    find_program(BUFS_EXAMPLES_DOTNET dotnet HINTS $ENV{DOTNET_ROOT} $ENV{HOME}/.dotnet)
    find_program(BUFS_EXAMPLES_ESBUILD esbuild NO_DEFAULT_PATH
                 HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../ts/node_modules/.bin ${CMAKE_CURRENT_SOURCE_DIR}/../../librpc/ts/node_modules/.bin)
    if(BUFS_EXAMPLES_PYTHON)
        set(args --bufsc $<TARGET_FILE:bufsc> --cpp $<TARGET_FILE:bufs_example_team> --python ${BUFS_EXAMPLES_PYTHON}
                 --work ${CMAKE_CURRENT_BINARY_DIR}/run)
        if(BUFS_EXAMPLES_NODE AND BUFS_EXAMPLES_ESBUILD)
            list(APPEND args --node ${BUFS_EXAMPLES_NODE} --esbuild ${BUFS_EXAMPLES_ESBUILD})
        endif()
        if(BUFS_EXAMPLES_DOTNET)
            list(APPEND args --dotnet ${BUFS_EXAMPLES_DOTNET})
        endif()
        add_test(NAME bufs_examples COMMAND ${BUFS_EXAMPLES_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/run_examples.py ${args})
        set_tests_properties(bufs_examples PROPERTIES TIMEOUT 300)
    endif()
endif()
