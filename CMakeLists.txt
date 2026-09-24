cmake_minimum_required(VERSION 3.24)
project(iiCalendar VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# A locally bootstrapped ICU is confined to build/, never to system directories.
if(NOT ICU_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/icu/install")
    set(ICU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/icu/install")
endif()
find_package(ICU 69 REQUIRED COMPONENTS i18n uc data)
set(CMAKE_FIND_FRAMEWORK LAST)
find_package(SQLite3 3.37 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(Threads REQUIRED)

add_library(iiCalendar STATIC
    src/Date/Date.cpp src/Time/Time.cpp src/Time/Clock.cpp
    src/Calendar/Calendar.cpp src/Calendar/TimeZone.cpp
    src/Event/Event.cpp src/Event/Recurrence.cpp
    src/Index/Codec.cpp src/Index/Store.cpp src/Calendar/View.cpp)
add_library(iiCalendar::iiCalendar ALIAS iiCalendar)
target_compile_features(iiCalendar PUBLIC cxx_std_23)
target_include_directories(iiCalendar PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(iiCalendar PRIVATE ICU::i18n ICU::uc ICU::data SQLite3::SQLite3 Threads::Threads)
set_target_properties(iiCalendar PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(iiCalendar PRIVATE /W4 /permissive-)
else()
    target_compile_options(iiCalendar PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(BUILD_TESTING)
    add_executable(iiCalendarTests tests/calendar_tests.cpp)
    target_link_libraries(iiCalendarTests PRIVATE iiCalendar::iiCalendar SQLite3::SQLite3)
    foreach(suite precision calendars zones events recurrence storage views clock)
        add_test(NAME iiCalendar.${suite} COMMAND iiCalendarTests ${suite})
        set_tests_properties(iiCalendar.${suite} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME iiCalendar.installed_consumer COMMAND ${CMAKE_COMMAND}
        -DPROJECT_BUILD=${CMAKE_CURRENT_BINARY_DIR} -DPROJECT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}
        -DICU_ROOT=${ICU_ROOT} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_consumer.cmake)
endif()

add_executable(iiCalendarExample examples/day_details.cpp)
target_link_libraries(iiCalendarExample PRIVATE iiCalendar::iiCalendar)
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiCalendar")
install(TARGETS iiCalendar EXPORT iiCalendarTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiCalendar/docs)
install(EXPORT iiCalendarTargets NAMESPACE iiCalendar:: DESTINATION ${package_dir})
configure_package_config_file(cmake/iiCalendarConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfig.cmake" INSTALL_DESTINATION ${package_dir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiCalendarConfigVersion.cmake" DESTINATION ${package_dir})
