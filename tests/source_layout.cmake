if(NOT SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
if(EXISTS "${SOURCE_ROOT}/src/iiCalendar")
    message(FATAL_ERROR "The source tree must not repeat the project name")
endif()
foreach(_file Chronology/Date.h Chronology/Date.cpp Chronology/Time.h Chronology/Time.cpp
        Events/Event.h Events/Event.cpp Storage/Store.h Storage/Store.cpp
        Presentation/View.h Presentation/View.cpp Integration/Accounts.h Integration/Accounts.cpp
        Integration/ExternalCalendar.h Integration/Manager.cpp Integration/iiCalendar.h
        Security/Sha256.h Security/Sha256.cpp)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing source in its responsibility directory: ${_file}")
    endif()
endforeach()
