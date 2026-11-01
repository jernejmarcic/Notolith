# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# Remote images in notes (docs/OFFLINE.md). Built only with NOTEAPPC_WITH_NETWORK: the default build links
# none of this.
#
# noteAppC_imagewire: the helper's wire format and the app's check of it. Gui only.
add_library(noteAppC_imagewire STATIC ImageWire.h ImageWire.cpp)
target_include_directories(noteAppC_imagewire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_imagewire PUBLIC Qt6::Gui)

# The helper: decodes one image, run only under bubblewrap (ImageDecoder.cpp). It links Gui and the wire
# format and nothing else, and no RUNPATH: it needs only what /usr has. Next to the executable in a build
# tree, in libexec when installed.
qt_add_executable(notolith-imagedecode ImageDecodeMain.cpp)
target_link_libraries(notolith-imagedecode PRIVATE noteAppC_imagewire Qt6::Core Qt6::Gui)
set_target_properties(notolith-imagedecode PROPERTIES
    WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/noteAppC
    SKIP_BUILD_RPATH ON INSTALL_RPATH "")
install(TARGETS notolith-imagedecode RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/notolith)

add_library(noteAppC_remoteimage STATIC
    RemoteImagePolicy.h RemoteImagePolicy.cpp
    ImageStore.h ImageStore.cpp
    ImageFetcher.h ImageFetcher.cpp
    ImageDecoder.h ImageDecoder.cpp
    RemoteImages.h RemoteImages.cpp)
set_target_properties(noteAppC_remoteimage PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_remoteimage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(noteAppC_remoteimage PRIVATE NOTEAPPC_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}/notolith")
target_link_libraries(noteAppC_remoteimage PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent noteAppC_imagewire noteAppC_sandbox
    PRIVATE noteAppC_xdg)
