# The one exe at the top of the release folder. It starts the app from app\ beside it,
# and carries its own C runtime because the redistributable ships inside app\
enable_language(RC)
add_executable(clinicavt_launcher WIN32 launcher.cpp launcher.rc)
set_target_properties(clinicavt_launcher PROPERTIES
    OUTPUT_NAME ClinicAVT
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(clinicavt_launcher PRIVATE clinicavt_warnings)
