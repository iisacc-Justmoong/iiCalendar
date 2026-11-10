if(NOT BUILD_ROOT)
    message(FATAL_ERROR "BUILD_ROOT is required")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --target iiCalendarExample
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
if(status EQUAL 0)
    message(FATAL_ERROR "The optional example target must be excluded by default")
endif()
