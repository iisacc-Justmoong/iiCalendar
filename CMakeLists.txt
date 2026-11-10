cmake_minimum_required(VERSION 3.24)
project(iiCalendar VERSION 0.2.1 LANGUAGES CXX)
option(BUILD_SHARED_LIBS "Build iiCalendar shared libraries" ON)
include(cmake/InstallPrefix.cmake)
calendar_set_install_prefix()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
if(PROJECT_IS_TOP_LEVEL AND UNIX AND CMAKE_CURRENT_BINARY_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/build")
    include(cmake/ResourceLinks.cmake)
    calendar_compat_links("${CMAKE_CURRENT_SOURCE_DIR}/resources" "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# A locally bootstrapped ICU and its source are project-owned resources.
if(NOT ICU_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/dependencies/icu/install")
    set(ICU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources/dependencies/icu/install")
endif()
find_package(ICU 69 REQUIRED COMPONENTS i18n uc data)
set(CMAKE_FIND_FRAMEWORK LAST)
find_package(SQLite3 3.37 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(Threads REQUIRED)
set(calendar_curl_default ON)
if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS|visionOS)$")
    set(calendar_curl_default OFF)
endif()
option(IICALENDAR_WITH_CURL "Built-in Google/Microsoft HTTPS transport (otherwise inject CalendarHttpGet)" ${calendar_curl_default})
option(IICALENDAR_TEST_IISACC_BRIDGE "Deprecated: build the iisacc bridge as a separate consumer" OFF)
option(IICALENDAR_BUILD_EXAMPLES "Build optional calendar examples" OFF)
if(IICALENDAR_TEST_IISACC_BRIDGE)
    message(FATAL_ERROR "The iisacc bridge is a separate consumer: configure examples/iisacc-bridge independently")
endif()

set(_calendar_public_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Integration/Accounts.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Chronology/Calendar.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Chronology/Date.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Events/Event.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Integration/ExternalCalendar.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Integration/IisaccAccount.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Storage/Store.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Chronology/Time.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Chronology/TimeZone.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Presentation/View.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Integration/iiCalendar.h"
)
foreach(_header IN LISTS _calendar_public_headers)
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiCalendar/${_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()

add_library(iiCalendar
    src/Chronology/Date.cpp src/Chronology/Time.cpp src/Chronology/Clock.cpp
    src/Chronology/Calendar.cpp src/Chronology/TimeZone.cpp
    src/Events/Event.cpp src/Events/Recurrence.cpp
    src/Storage/Codec.cpp src/Storage/Store.cpp src/Presentation/View.cpp
    src/Integration/Manager.cpp src/Integration/Google.cpp src/Integration/Microsoft.cpp src/Integration/Accounts.cpp
    src/Security/Sha256.cpp)
add_library(iiCalendar::iiCalendar ALIAS iiCalendar)
target_compile_features(iiCalendar PUBLIC cxx_std_23)
target_include_directories(iiCalendar PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(iiCalendar PRIVATE ICU::i18n ICU::uc ICU::data SQLite3::SQLite3 Threads::Threads)
target_include_directories(iiCalendar SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)
if(IICALENDAR_WITH_CURL)
    find_package(CURL 7.85 REQUIRED)
    target_sources(iiCalendar PRIVATE src/Integration/Curl.cpp)
    target_link_libraries(iiCalendar PRIVATE CURL::libcurl)
else()
    target_sources(iiCalendar PRIVATE src/Integration/NoCurl.cpp)
endif()
if(APPLE AND CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
    enable_language(OBJCXX)
    target_sources(iiCalendar PRIVATE src/Integration/Apple.mm)
    set_source_files_properties(src/Integration/Apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(iiCalendar PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    find_library(calendar_eventkit EventKit REQUIRED)
    find_library(calendar_foundation Foundation REQUIRED)
    find_library(calendar_coregraphics CoreGraphics REQUIRED)
    target_link_libraries(iiCalendar PRIVATE ${calendar_eventkit} ${calendar_foundation} ${calendar_coregraphics})
else()
    target_sources(iiCalendar PRIVATE src/Integration/NoApple.cpp)
endif()
set_target_properties(iiCalendar PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(iiCalendar PRIVATE /W4 /permissive-)
else()
    target_compile_options(iiCalendar PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(BUILD_SHARED_LIBS)
    set_target_properties(iiCalendar PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(iiCalendar PROPERTIES FRAMEWORK TRUE FRAMEWORK_VERSION A
            MACOSX_FRAMEWORK_IDENTIFIER "com.iisacc.iiCalendar")
    elseif(APPLE)
        set_property(TARGET iiCalendar APPEND PROPERTY INSTALL_RPATH "@loader_path")
    elseif(UNIX AND NOT EMSCRIPTEN)
        set_property(TARGET iiCalendar APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_calendar_user_home "$ENV{HOME}")
    if(_calendar_user_home STREQUAL "")
        set(_calendar_user_home "$ENV{USERPROFILE}")
    endif()
    set(IICALENDAR_TEST_PROVIDER_PYTHON_DIR
        "auto" CACHE STRING "Optional Python resource-test adapter path")
    function(calendar_resource_test name)
        add_test(NAME "${name}" COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_runner.py"
            "${IICALENDAR_TEST_PROVIDER_PYTHON_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources" ${ARGN})
    endfunction()
    calendar_resource_test(iiCalendar.resource_runner "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_runner.py")
    add_test(NAME iiCalendar.install_prefix COMMAND "${CMAKE_COMMAND}"
        "-DHELPER=${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallPrefix.cmake"
        "-DEXPECTED=${_calendar_user_home}/.local/SDK/iiCalendar"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_prefix.cmake")
    unset(_calendar_user_home)
    calendar_resource_test(iiCalendar.resource_paths "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_paths.py" "@WORK@")
    calendar_resource_test(iiCalendar.consumer_isolation "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installed_consumer_policy.py" "@WORK@")
    if(UNIX)
        calendar_resource_test(iiCalendar.resource_links "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_links.py" "${CMAKE_COMMAND}"
            "${IICALENDAR_TEST_PROVIDER_PYTHON_DIR}" "@WORK@")
        calendar_resource_test(iiCalendar.install_policy "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_policy.py" "@WORK@")
    endif()
    if(NOT IICALENDAR_BUILD_EXAMPLES)
        add_test(NAME iiCalendar.production_defaults COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/production_defaults.cmake")
    endif()
    add_test(NAME iiCalendar.source_layout COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_layout.cmake")
    add_test(NAME iiCalendar.account_sdk_boundary COMMAND ${CMAKE_COMMAND}
        "-DIICALENDAR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/account_sdk_boundary.cmake")
    add_executable(iiCalendarAccountTests tests/account_tests.cpp)
    target_link_libraries(iiCalendarAccountTests PRIVATE iiCalendar::iiCalendar)
    add_test(NAME iiCalendar.accounts COMMAND iiCalendarAccountTests)
    set_tests_properties(iiCalendar.accounts PROPERTIES TIMEOUT 60)
    add_executable(iiCalendarHashTests tests/sha256_tests.cpp)
    target_link_libraries(iiCalendarHashTests PRIVATE iiCalendar::iiCalendar)
    add_test(NAME iiCalendar.sha256 COMMAND iiCalendarHashTests)
    set_tests_properties(iiCalendar.sha256 PROPERTIES TIMEOUT 60)
    add_executable(iiCalendarMicrosoftTests tests/microsoft_calendar_tests.cpp)
    target_link_libraries(iiCalendarMicrosoftTests PRIVATE iiCalendar::iiCalendar)
    target_include_directories(iiCalendarMicrosoftTests SYSTEM PRIVATE ThirdParty)
    add_test(NAME iiCalendar.external_microsoft COMMAND iiCalendarMicrosoftTests)
    set_tests_properties(iiCalendar.external_microsoft PROPERTIES TIMEOUT 60)
    if(IICALENDAR_WITH_CURL)
        # Explicit network integration executable, kept out of offline CTest.
        add_executable(iiCalendarHttpsSmoke tests/https_smoke.cpp)
        target_link_libraries(iiCalendarHttpsSmoke PRIVATE iiCalendar::iiCalendar)
    endif()
    if(APPLE AND CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
        add_executable(iiCalendarAppleTests tests/apple_calendar_tests.mm)
        set_target_properties(iiCalendarAppleTests PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_compile_options(iiCalendarAppleTests PRIVATE -fobjc-arc)
        target_link_libraries(iiCalendarAppleTests PRIVATE iiCalendar::iiCalendar ${calendar_foundation})
        add_test(NAME iiCalendar.external_apple_codec COMMAND iiCalendarAppleTests)
        set_tests_properties(iiCalendar.external_apple_codec PROPERTIES TIMEOUT 60)
    endif()
    add_executable(iiCalendarExternalTests tests/external_calendar_tests.cpp)
    target_link_libraries(iiCalendarExternalTests PRIVATE iiCalendar::iiCalendar)
    foreach(suite manager google apple)
        add_test(NAME iiCalendar.external_${suite} COMMAND iiCalendarExternalTests ${suite})
        set_tests_properties(iiCalendar.external_${suite} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(iiCalendarTests tests/calendar_tests.cpp)
    target_link_libraries(iiCalendarTests PRIVATE iiCalendar::iiCalendar SQLite3::SQLite3)
    foreach(suite precision calendars zones events recurrence storage_memory views clock)
        add_test(NAME iiCalendar.${suite} COMMAND iiCalendarTests ${suite})
        set_tests_properties(iiCalendar.${suite} PROPERTIES TIMEOUT 60)
    endforeach()
    string(HEX "${CMAKE_PREFIX_PATH}" _calendar_dependency_prefix_hex)
    calendar_resource_test(iiCalendar.installed_consumer "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_consumer.py"
        "--cmake=${CMAKE_COMMAND}" "--ctest=${CMAKE_CTEST_COMMAND}"
        "--build=${CMAKE_CURRENT_BINARY_DIR}" "--source=${CMAKE_CURRENT_SOURCE_DIR}"
        "--workspace=@WORK@" "--config=$<CONFIG>" "--generator=${CMAKE_GENERATOR}"
        "--dependency-prefix-hex=${_calendar_dependency_prefix_hex}" "--icu-root=${ICU_ROOT}"
        "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--target-triplet=${VCPKG_TARGET_TRIPLET}"
        "--host-triplet=${VCPKG_HOST_TRIPLET}")
    unset(_calendar_dependency_prefix_hex)
endif()

if(IICALENDAR_BUILD_EXAMPLES)
    add_executable(iiCalendarExample examples/day_details.cpp)
    target_link_libraries(iiCalendarExample PRIVATE iiCalendar::iiCalendar)
endif()
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiCalendar")
install(TARGETS iiCalendar EXPORT iiCalendarTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${_calendar_public_headers}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiCalendar)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar/docs)
install(FILES ThirdParty/nlohmann/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar/licenses/nlohmann-json)
install(FILES ThirdParty/licenses/iisacc-counterpart/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar/licenses/iisacc-counterpart)
install(EXPORT iiCalendarTargets NAMESPACE iiCalendar:: DESTINATION ${package_dir})
configure_package_config_file(cmake/iiCalendarConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfig.cmake" INSTALL_DESTINATION ${package_dir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfigVersion.cmake" DESTINATION ${package_dir})

if(BUILD_TESTING AND BUILD_SHARED_LIBS AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME iiCalendar.shared_link COMMAND "${CMAKE_COMMAND}"
        "-DIMAGE=$<TARGET_FILE:iiCalendarTests>" "-DEXPECTED=$<TARGET_FILE:iiCalendar>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_link.cmake")
endif()
