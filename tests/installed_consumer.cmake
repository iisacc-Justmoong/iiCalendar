set(prefix "${PROJECT_BUILD}/install")
if(NOT ICU_ROOT)
    set(ICU_ROOT "$ENV{ICU_ROOT}")
endif()
set(consumer_build "${PROJECT_BUILD}/consumer/build")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BUILD}" --config "${CONFIG}" --prefix "${prefix}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Install failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --fresh -G "${GENERATOR}" -S "${PROJECT_SOURCE}/tests/consumer" -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${prefix};${DEPENDENCY_PREFIX}" "-DICU_ROOT=${ICU_ROOT}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}" "-DVCPKG_TARGET_TRIPLET=${TARGET_TRIPLET}"
    "-DVCPKG_HOST_TRIPLET=${HOST_TRIPLET}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${CONFIG}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}" -C "${CONFIG}" --output-on-failure
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed consumer test failed")
endif()
