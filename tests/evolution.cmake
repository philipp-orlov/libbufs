# Runs the two schema versions against each other's messages.
#   cmake -DV1=... -DV2=... -DDIR=... -P evolution.cmake

file(MAKE_DIRECTORY "${DIR}")
foreach(step "${V1};write;${DIR}/v1.bin" "${V2};write;${DIR}/v2.bin" "${V1};read;${DIR}/v2.bin"
        "${V2};read;${DIR}/v1.bin")
    list(GET step 0 program)
    list(GET step 1 mode)
    list(GET step 2 file)
    execute_process(COMMAND "${program}" ${mode} "${file}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${program} ${mode} ${file} failed: ${result}")
    endif()
endforeach()
