set(prefix "${PROJECT_BUILD}/install")
set(consumer_build "${PROJECT_BUILD}/consumer/build")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BUILD}" --prefix "${prefix}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Install failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE}/tests/consumer" -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DICU_ROOT=${ICU_ROOT}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" --output-on-failure
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed consumer test failed")
endif()
