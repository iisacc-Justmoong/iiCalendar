cmake_minimum_required(VERSION 3.24)
project(iiCalendarIntegration VERSION 0.2.1 LANGUAGES CXX)
include(./cmake/ModuleImports.cmake)
option(BUILD_SHARED_LIBS "Build iiCalendarIntegration shared libraries" ON)
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
option(IICALENDARINTEGRATION_WITH_CURL "Built-in Google/Microsoft HTTPS transport (otherwise inject CalendarHttpGet)" ${calendar_curl_default})
option(IICALENDARINTEGRATION_TEST_IISACC_BRIDGE "Deprecated: build the iisacc bridge as a separate consumer" OFF)
option(IICALENDARINTEGRATION_BUILD_EXAMPLES "Build optional calendar examples" OFF)
option(IICALENDARINTEGRATION_BUILD_PYTHON "Build the optional Python account API" OFF)
if(IICALENDARINTEGRATION_TEST_IISACC_BRIDGE)
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Integration/iiCalendarIntegration.h"
)
foreach(_header IN LISTS _calendar_public_headers)
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiCalendarIntegration/${_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()

add_library(iiCalendarIntegration
    src/Chronology/Date.cpp src/Chronology/Time.cpp src/Chronology/Clock.cpp
    src/Chronology/Calendar.cpp src/Chronology/TimeZone.cpp
    src/Events/Event.cpp src/Events/Recurrence.cpp
    src/Storage/Codec.cpp src/Storage/Store.cpp src/Presentation/View.cpp
    src/Integration/Manager.cpp src/Integration/Google.cpp src/Integration/Microsoft.cpp src/Integration/Accounts.cpp
    src/Security/Sha256.cpp)
add_library(iiCalendarIntegration::iiCalendarIntegration ALIAS iiCalendarIntegration)
target_compile_features(iiCalendarIntegration PUBLIC cxx_std_23)
target_include_directories(iiCalendarIntegration PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(iiCalendarIntegration PRIVATE ICU::i18n ICU::uc ICU::data SQLite3::SQLite3 Threads::Threads)
target_include_directories(iiCalendarIntegration SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)
if(IICALENDARINTEGRATION_WITH_CURL)
    find_package(CURL 7.85 REQUIRED)
    target_sources(iiCalendarIntegration PRIVATE src/Integration/Curl.cpp)
    target_link_libraries(iiCalendarIntegration PRIVATE CURL::libcurl)
else()
    target_sources(iiCalendarIntegration PRIVATE src/Integration/NoCurl.cpp)
endif()
if(APPLE AND CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
    enable_language(OBJCXX)
    target_sources(iiCalendarIntegration PRIVATE src/Integration/Apple.mm)
    set_source_files_properties(src/Integration/Apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(iiCalendarIntegration PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
    find_library(calendar_eventkit EventKit REQUIRED)
    find_library(calendar_foundation Foundation REQUIRED)
    find_library(calendar_coregraphics CoreGraphics REQUIRED)
    target_link_libraries(iiCalendarIntegration PRIVATE ${calendar_eventkit} ${calendar_foundation} ${calendar_coregraphics})
else()
    target_sources(iiCalendarIntegration PRIVATE src/Integration/NoApple.cpp)
endif()
set_target_properties(iiCalendarIntegration PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(iiCalendarIntegration PRIVATE /W4 /permissive-)
else()
    target_compile_options(iiCalendarIntegration PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(BUILD_SHARED_LIBS)
    set_target_properties(iiCalendarIntegration PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(iiCalendarIntegration PROPERTIES FRAMEWORK TRUE FRAMEWORK_VERSION A
            MACOSX_FRAMEWORK_IDENTIFIER "com.iisacc.iiCalendarIntegration")
    elseif(APPLE)
        set_property(TARGET iiCalendarIntegration APPEND PROPERTY INSTALL_RPATH "@loader_path")
    elseif(UNIX AND NOT EMSCRIPTEN)
        set_property(TARGET iiCalendarIntegration APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
endif()

if(IICALENDARINTEGRATION_BUILD_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
    find_package(pybind11 2.12 CONFIG REQUIRED)
    pybind11_add_module(iiCalendarIntegrationPython MODULE src/Integration/PythonAccounts.cpp)
    target_compile_features(iiCalendarIntegrationPython PRIVATE cxx_std_23)
    target_link_libraries(iiCalendarIntegrationPython PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    set_target_properties(iiCalendarIntegrationPython PROPERTIES OUTPUT_NAME iicalendarintegration CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET default)
    set(IICALENDARINTEGRATION_PYTHON_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages" CACHE STRING "Python module path relative to the installation prefix")
    if(IS_ABSOLUTE "${IICALENDARINTEGRATION_PYTHON_INSTALL_DIR}" OR IICALENDARINTEGRATION_PYTHON_INSTALL_DIR MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "IICALENDARINTEGRATION_PYTHON_INSTALL_DIR must be within the installation prefix")
    endif()
    if(UNIX)
        file(RELATIVE_PATH _calendar_python_lib_relative "/${IICALENDARINTEGRATION_PYTHON_INSTALL_DIR}" "/${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(APPLE)
        set_target_properties(iiCalendarIntegrationPython PROPERTIES INSTALL_RPATH "@loader_path/${_calendar_python_lib_relative}")
    elseif(UNIX)
        set_target_properties(iiCalendarIntegrationPython PROPERTIES INSTALL_RPATH "$ORIGIN/${_calendar_python_lib_relative}")
    endif()
    unset(_calendar_python_lib_relative)
    install(TARGETS iiCalendarIntegrationPython LIBRARY DESTINATION "${IICALENDARINTEGRATION_PYTHON_INSTALL_DIR}" RUNTIME DESTINATION "${IICALENDARINTEGRATION_PYTHON_INSTALL_DIR}")
    install(FILES ThirdParty/licenses/pybind11/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendarIntegration/licenses/pybind11)
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_calendar_user_home "$ENV{HOME}")
    if(_calendar_user_home STREQUAL "")
        set(_calendar_user_home "$ENV{USERPROFILE}")
    endif()
    set(IICALENDARINTEGRATION_TEST_PROVIDER_PYTHON_DIR
        "auto" CACHE STRING "Optional Python resource-test adapter path")
    function(calendar_resource_test name)
        add_test(NAME "${name}" COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_runner.py"
            "${IICALENDARINTEGRATION_TEST_PROVIDER_PYTHON_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources" ${ARGN})
    endfunction()
    calendar_resource_test(iiCalendarIntegration.resource_runner "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_runner.py")
    add_test(NAME iiCalendarIntegration.install_prefix COMMAND "${CMAKE_COMMAND}"
        "-DHELPER=${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallPrefix.cmake"
        "-DEXPECTED=${_calendar_user_home}/.local/SDK/iiCalendarIntegration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_prefix.cmake")
    unset(_calendar_user_home)
    calendar_resource_test(iiCalendarIntegration.resource_paths "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_paths.py" "@WORK@")
    calendar_resource_test(iiCalendarIntegration.consumer_isolation "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installed_consumer_policy.py" "@WORK@")
    if(UNIX)
        calendar_resource_test(iiCalendarIntegration.resource_links "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_links.py" "${CMAKE_COMMAND}"
            "${IICALENDARINTEGRATION_TEST_PROVIDER_PYTHON_DIR}" "@WORK@")
        calendar_resource_test(iiCalendarIntegration.install_policy "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_policy.py" "@WORK@")
    endif()
    if(NOT IICALENDARINTEGRATION_BUILD_EXAMPLES)
        add_test(NAME iiCalendarIntegration.production_defaults COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/production_defaults.cmake")
    endif()
    add_test(NAME iiCalendarIntegration.source_layout COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_layout.cmake")
    add_test(NAME iiCalendarIntegration.account_sdk_boundary COMMAND ${CMAKE_COMMAND}
        "-DIICALENDARINTEGRATION_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/account_sdk_boundary.cmake")
    if(IICALENDARINTEGRATION_BUILD_PYTHON)
        add_test(NAME iiCalendarIntegration.python_accounts COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=$<TARGET_FILE_DIR:iiCalendarIntegrationPython>" "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_accounts.py")
        set_tests_properties(iiCalendarIntegration.python_accounts PROPERTIES TIMEOUT 60)
    endif()
    add_executable(iiCalendarIntegrationAccountTests tests/account_tests.cpp)
    target_link_libraries(iiCalendarIntegrationAccountTests PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    add_test(NAME iiCalendarIntegration.accounts COMMAND iiCalendarIntegrationAccountTests)
    set_tests_properties(iiCalendarIntegration.accounts PROPERTIES TIMEOUT 60)
    add_executable(iiCalendarIntegrationHashTests tests/sha256_tests.cpp)
    target_link_libraries(iiCalendarIntegrationHashTests PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    add_test(NAME iiCalendarIntegration.sha256 COMMAND iiCalendarIntegrationHashTests)
    set_tests_properties(iiCalendarIntegration.sha256 PROPERTIES TIMEOUT 60)
    add_executable(iiCalendarIntegrationMicrosoftTests tests/microsoft_calendar_tests.cpp)
    target_link_libraries(iiCalendarIntegrationMicrosoftTests PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    target_include_directories(iiCalendarIntegrationMicrosoftTests SYSTEM PRIVATE ThirdParty)
    add_test(NAME iiCalendarIntegration.external_microsoft COMMAND iiCalendarIntegrationMicrosoftTests)
    set_tests_properties(iiCalendarIntegration.external_microsoft PROPERTIES TIMEOUT 60)
    if(IICALENDARINTEGRATION_WITH_CURL)
        # Explicit network integration executable, kept out of offline CTest.
        add_executable(iiCalendarIntegrationHttpsSmoke tests/https_smoke.cpp)
        target_link_libraries(iiCalendarIntegrationHttpsSmoke PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    endif()
    if(APPLE AND CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
        add_executable(iiCalendarIntegrationAppleTests tests/apple_calendar_tests.mm)
        set_target_properties(iiCalendarIntegrationAppleTests PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_compile_options(iiCalendarIntegrationAppleTests PRIVATE -fobjc-arc)
        target_link_libraries(iiCalendarIntegrationAppleTests PRIVATE iiCalendarIntegration::iiCalendarIntegration ${calendar_foundation})
        add_test(NAME iiCalendarIntegration.external_apple_codec COMMAND iiCalendarIntegrationAppleTests)
        set_tests_properties(iiCalendarIntegration.external_apple_codec PROPERTIES TIMEOUT 60)
    endif()
    add_executable(iiCalendarIntegrationExternalTests tests/external_calendar_tests.cpp)
    target_link_libraries(iiCalendarIntegrationExternalTests PRIVATE iiCalendarIntegration::iiCalendarIntegration)
    foreach(suite manager google apple)
        add_test(NAME iiCalendarIntegration.external_${suite} COMMAND iiCalendarIntegrationExternalTests ${suite})
        set_tests_properties(iiCalendarIntegration.external_${suite} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(iiCalendarIntegrationTests tests/calendar_tests.cpp)
    target_link_libraries(iiCalendarIntegrationTests PRIVATE iiCalendarIntegration::iiCalendarIntegration SQLite3::SQLite3)
    foreach(suite precision calendars zones events recurrence storage_memory views clock)
        add_test(NAME iiCalendarIntegration.${suite} COMMAND iiCalendarIntegrationTests ${suite})
        set_tests_properties(iiCalendarIntegration.${suite} PROPERTIES TIMEOUT 60)
    endforeach()
    if(IICALENDARINTEGRATION_BUILD_PYTHON)
        set(_calendar_python_install_test_dir "${IICALENDARINTEGRATION_PYTHON_INSTALL_DIR}")
    else()
        set(_calendar_python_install_test_dir "")
    endif()
    string(HEX "${CMAKE_PREFIX_PATH}" _calendar_dependency_prefix_hex)
    calendar_resource_test(iiCalendarIntegration.installed_consumer "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_consumer.py"
        "--cmake=${CMAKE_COMMAND}" "--ctest=${CMAKE_CTEST_COMMAND}"
        "--build=${CMAKE_CURRENT_BINARY_DIR}" "--source=${CMAKE_CURRENT_SOURCE_DIR}"
        "--workspace=@WORK@" "--config=$<CONFIG>" "--generator=${CMAKE_GENERATOR}"
        "--dependency-prefix-hex=${_calendar_dependency_prefix_hex}" "--icu-root=${ICU_ROOT}"
        "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--target-triplet=${VCPKG_TARGET_TRIPLET}"
        "--host-triplet=${VCPKG_HOST_TRIPLET}"
        "--python-executable=${Python3_EXECUTABLE}" "--python-module-dir=${_calendar_python_install_test_dir}")
    unset(_calendar_dependency_prefix_hex)
    unset(_calendar_python_install_test_dir)
endif()

if(IICALENDARINTEGRATION_BUILD_EXAMPLES)
    add_executable(iiCalendarIntegrationExample examples/day_details.cpp)
    target_link_libraries(iiCalendarIntegrationExample PRIVATE iiCalendarIntegration::iiCalendarIntegration)
endif()
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiCalendarIntegration")
install(TARGETS iiCalendarIntegration EXPORT iiCalendarIntegrationTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${_calendar_public_headers}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiCalendarIntegration)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendarIntegration)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendarIntegration/docs)
install(FILES ThirdParty/nlohmann/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendarIntegration/licenses/nlohmann-json)
install(FILES ThirdParty/licenses/iisacc-counterpart/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendarIntegration/licenses/iisacc-counterpart)
install(EXPORT iiCalendarIntegrationTargets NAMESPACE iiCalendarIntegration:: DESTINATION ${package_dir})
configure_package_config_file(cmake/iiCalendarIntegrationConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarIntegrationConfig.cmake" INSTALL_DESTINATION ${package_dir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiCalendarIntegrationConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarIntegrationConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarIntegrationConfigVersion.cmake" DESTINATION ${package_dir})

if(BUILD_TESTING AND BUILD_SHARED_LIBS AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME iiCalendarIntegration.shared_link COMMAND "${CMAKE_COMMAND}"
        "-DIMAGE=$<TARGET_FILE:iiCalendarIntegrationTests>" "-DEXPECTED=$<TARGET_FILE:iiCalendarIntegration>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_link.cmake")
endif()
