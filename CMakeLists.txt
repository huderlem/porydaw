cmake_minimum_required(VERSION 3.21)
project(porydaw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---- poryaaaa engine core (MIT, statically linked) ----
set(PORYAAAA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/poryaaaa)
add_library(poryaaaa_engine STATIC
    ${PORYAAAA_DIR}/plugin/m4a_engine.c
    ${PORYAAAA_DIR}/plugin/m4a_channel.c
    ${PORYAAAA_DIR}/plugin/m4a_tables.c
    ${PORYAAAA_DIR}/plugin/m4a_reverb.c
    ${PORYAAAA_DIR}/plugin/voicegroup_loader.c
)
target_include_directories(poryaaaa_engine PUBLIC ${PORYAAAA_DIR}/plugin)
if(UNIX)
    target_link_libraries(poryaaaa_engine PUBLIC m)
endif()

# ---- porydaw application ----
qt_add_executable(porydaw
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/songsession.h
    src/tabcheck.cpp
    src/eventviewcheck.cpp
    src/rollcheck.cpp
    src/viewcheck.cpp
    src/roundtrip.cpp
    src/editcheck.cpp
    src/savecheck.cpp
    src/onboardcheck.cpp
    src/vgcheck.cpp
    src/vgsavecheck.cpp
    src/exportcheck.cpp
    src/loopcheck.cpp
    src/primecheck.cpp
    src/transportcheck.cpp
    src/mkcheck.cpp
    src/sessioncheck.cpp
    src/audio/audioengine.cpp
    src/audio/audioengine.h
    src/audio/miniaudio_impl.c
    src/audio/wavexport.cpp
    src/audio/wavexport.h
    src/core/mid2agbtables.cpp
    src/core/mid2agbtables.h
    src/core/miditimeline.cpp
    src/core/miditimeline.h
    src/core/smf.cpp
    src/core/smf.h
    src/core/songdocument.cpp
    src/core/songdocument.h
    src/core/timelineplayer.cpp
    src/core/timelineplayer.h
    src/core/midiimport.cpp
    src/core/midiimport.h
    src/project/decompproject.cpp
    src/project/decompproject.h
    src/project/songregistry.cpp
    src/project/songregistry.h
    src/project/songsmk.cpp
    src/project/songsmk.h
    src/project/voicegroupsource.cpp
    src/project/voicegroupsource.h
    src/ui/m4asemantics.cpp
    src/ui/m4asemantics.h
    src/ui/newsongwizard.cpp
    src/ui/newsongwizard.h
    src/ui/voicegroupbrowser.cpp
    src/ui/voicegroupbrowser.h
    src/ui/enginesettingsdialog.cpp
    src/ui/enginesettingsdialog.h
    src/ui/songsettingsdialog.cpp
    src/ui/songsettingsdialog.h
    src/ui/songlistpanel.cpp
    src/ui/songlistpanel.h
    src/ui/songviewmodel.cpp
    src/ui/songviewmodel.h
    src/ui/songview.cpp
    src/ui/songview.h
    src/ui/eventlistview.cpp
    src/ui/eventlistview.h
    src/ui/viewsidecar.cpp
    src/ui/viewsidecar.h
)

qt_add_resources(porydaw "cursor_images"
    PREFIX "/cursors"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/left-drag.png
        resources/left-drag@2x.png
        resources/right-drag.png
        resources/right-drag@2x.png
)
target_include_directories(porydaw PRIVATE
    src
    ${PORYAAAA_DIR}/third_party
)
target_link_libraries(porydaw PRIVATE Qt6::Widgets poryaaaa_engine)
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(porydaw PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    set_target_properties(porydaw PROPERTIES WIN32_EXECUTABLE TRUE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # MinGW: statically link libgcc/libstdc++/winpthread so the exe
        # doesn't depend on libgcc_s_seh-1.dll etc. at runtime.
        target_link_options(porydaw PRIVATE -static)
    endif()
endif()
if(APPLE)
    set_target_properties(porydaw PROPERTIES MACOSX_BUNDLE TRUE)
endif()

# ---- Offline render CLI (development/testing: parity vs poryaaaa_render) ----
add_executable(porydaw_render_cli
    tools/porydaw_render_cli.cpp
    src/core/mid2agbtables.cpp
    src/core/miditimeline.cpp
    src/core/smf.cpp
    src/core/timelineplayer.cpp
)
target_include_directories(porydaw_render_cli PRIVATE src)
target_link_libraries(porydaw_render_cli PRIVATE Qt6::Core poryaaaa_engine)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(porydaw_render_cli PRIVATE -static)
endif()
