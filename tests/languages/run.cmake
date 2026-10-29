# The cross-language test: C++ writes the reference messages of kitchen.bufs; TypeScript, C# and Python (each of them that
# is installed) read them, check every field, encode the same values themselves and write them back; C++ verifies and reads
# what they wrote. Every codec must also produce C++'s bytes, which the checkers compare.
#   cmake -DBUFSC=… -DGOLDEN=… -DSOURCE=<libbufs> -DWORK=<dir> [-DPYTHON=…] [-DNODE=… -DESBUILD=…] [-DDOTNET=…] -P run.cmake
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/gen)
set(schema ${SOURCE}/tests/languages/kitchen.bufs)

function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "${what} failed (${rc})")
    endif()
endfunction()

run("golden write" ${GOLDEN} write ${WORK})
set(ran "")

if(PYTHON)
    run("bufsc --py" ${BUFSC} --py --no-cpp -o ${WORK}/gen ${schema})
    run("python check" ${PYTHON} -I ${SOURCE}/tests/languages/check.py ${WORK}/gen ${WORK})
    list(APPEND ran py)
endif()

if(NODE AND ESBUILD)
    run("bufsc --ts" ${BUFSC} --ts --no-cpp -o ${WORK}/gen ${schema})
    file(COPY ${SOURCE}/tests/languages/check.ts DESTINATION ${WORK}/gen)
    run("esbuild" ${ESBUILD} ${WORK}/gen/check.ts --bundle --platform=node --format=esm
        --alias:@bufs/runtime=${SOURCE}/ts/src/bufs.ts --outfile=${WORK}/gen/check.mjs --log-level=warning)
    run("typescript check" ${NODE} ${WORK}/gen/check.mjs ${WORK})
    list(APPEND ran ts)
endif()

if(DOTNET)
    set(ENV{DOTNET_CLI_TELEMETRY_OPTOUT} 1)
    set(ENV{DOTNET_NOLOGO} 1)
    set(ENV{DOTNET_SYSTEM_GLOBALIZATION_INVARIANT} 1)
    set(tests ${SOURCE}/cs/Bufs.Tests)
    run("bufsc --cs" ${BUFSC} --cs --no-cpp -o ${tests}/gen ${schema})
    run("c# check" ${DOTNET} run --project ${tests} -- ${WORK})
    list(APPEND ran cs)
endif()

run("golden verify" ${GOLDEN} verify ${WORK})
message(STATUS "checked: cpp ${ran}")
