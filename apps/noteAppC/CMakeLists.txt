qt_add_executable(noteAppC main.cpp)

target_compile_definitions(noteAppC PRIVATE NOTEAPPC_VERSION="${PROJECT_VERSION}")
target_link_libraries(noteAppC PRIVATE noteAppC_ui)
# Loadable modules (libs/modules) resolve the editor, theme and vault symbols
# from this executable at load time, so they must be exported.
if(NOT ANDROID)
    set_target_properties(noteAppC PROPERTIES ENABLE_EXPORTS ON)
endif()
# The product name; the target keeps the code name every build script uses.
if(NOT ANDROID)
    set_target_properties(noteAppC PROPERTIES OUTPUT_NAME notolith)
endif()

# Qt's generated manifest would request RECORD_AUDIO, MODIFY_AUDIO_SETTINGS and
# BLUETOOTH purely because Qt Multimedia is linked (audio/video embeds play
# back, never record), and the package/label would otherwise be
# org.qtproject.example.noteAppC / noteAppC - fine as a label, wrong as an id.
# The checked-in android/AndroidManifest.xml fixes both; see docs/PLATFORMS.md.
if(ANDROID)
    set_property(TARGET noteAppC PROPERTY
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/android)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_VERSION_NAME ${PROJECT_VERSION})
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_VERSION_CODE 1)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_MIN_SDK_VERSION 26)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)
endif()
