cmake_minimum_required(VERSION 3.25)

# Version number, shared with the shell through Directory.Build.props
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" clinicavt_release_version LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(clinicavt
    VERSION ${clinicavt_release_version}
    DESCRIPTION "ClinicAVT: clinical AI, entirely on-device"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A compiler cache (CI sets the launcher) cannot cache objects that share one PDB
if(CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

enable_testing()

# Applies to every target below; the tests exempt googletest around its fetch
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

add_library(clinicavt_warnings INTERFACE)
target_compile_options(clinicavt_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>)

add_subdirectory(engine)
add_subdirectory(launcher)

# Developer weight acquisition: verified download into the repo model store
add_custom_target(fetch-models
    COMMAND dotnet run --project "${clinicavt_SOURCE_DIR}/tools/ClinicAVT.FetchModels" -c Release --
            fetch "${clinicavt_SOURCE_DIR}/weights" "${clinicavt_SOURCE_DIR}"
    VERBATIM)
