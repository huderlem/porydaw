cmake_minimum_required(VERSION 3.21)
project(porydaw VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# AddressSanitizer build for the harness sweep (tools/run_checks.sh): memory
# bugs abort with a stack trace instead of corrupting silently. GCC/Clang
# only. Use a separate build dir, e.g.:
#   cmake -B build-asan -DPORYDAW_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
option(PORYDAW_ASAN "Build with AddressSanitizer" OFF)
if(PORYDAW_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
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
qt_add_library(porydaw_app STATIC
    src/mainwindow.cpp
    src/mainwindow.h
    src/songsession.h
    src/ui/layout.cpp
    src/ui/layout.h
    src/ui/typography.cpp
    src/ui/typography.h
    # Editable swatches and color policy stay separate from Qt runtime mapping;
    # paint code consumes only fully resolved roles.
    src/ui/theme/theme.h
    src/ui/theme/themeruntime.cpp
    src/ui/theme/themeruntime.h
    src/ui/theme/theme_roles.h
    src/ui/theme/color_math.cpp
    src/ui/theme/color_math.h
    src/ui/theme/presetcolors.h
    src/ui/theme/trackidentitycolors.h
    src/ui/theme/trackidentitycolors.cpp
    src/ui/theme/themeresolver.cpp
    src/ui/theme/themeresolver.h
    src/ui/theme/themecontroller.cpp
    src/ui/theme/themecontroller.h
    src/ui/theme/oklchpicker.cpp
    src/ui/theme/oklchpicker.h
    src/ui/theme/themepage.cpp
    src/ui/theme/themepage.h

    src/audio/audioengine.cpp
    src/audio/audioengine.h
    src/audio/auditionslots.cpp
    src/audio/auditionslots.h
    src/audio/miniaudio_impl.c
    src/audio/stb_vorbis_impl.c
    src/audio/wavexport.cpp
    src/audio/wavexport.h
    src/audio/sampledata.h
    src/audio/sampledsp.cpp
    src/audio/sampledsp.h
    src/audio/sampleimport.cpp
    src/audio/sampleimport.h
    src/audio/sampledoc.cpp
    src/audio/sampledoc.h
    src/audio/samplewav.cpp
    src/audio/samplewav.h
    src/audio/sf2reader.cpp
    src/audio/sf2reader.h
    src/core/mid2agbtables.cpp
    src/core/mid2agbtables.h
    src/core/miditimeline.cpp
    src/core/miditimeline.h
    src/core/noteid.h
    src/core/smf.cpp
    src/core/smf.h
    src/core/songdocument.cpp
    src/core/songdocument.h
    src/core/timelineplayer.cpp
    src/core/timelineplayer.h
    src/core/velocitymodel.cpp
    src/core/velocitymodel.h
    src/core/midiimport.cpp
    src/core/midiimport.h
    src/project/decompproject.cpp
    src/project/decompproject.h
    src/project/songregistry.cpp
    src/project/songregistry.h
    src/project/songsmk.cpp
    src/project/songsmk.h
    src/project/samplereg.cpp
    src/project/samplereg.h
    src/project/sidecar.cpp
    src/project/sidecar.h
    src/project/voicegroupsource.cpp
    src/project/voicegroupsource.h
    src/ui/m4asemantics.cpp
    src/ui/m4asemantics.h
    src/ui/applicationstartup.cpp
    src/ui/applicationstartup.h
    src/ui/newsongwizard.cpp
    src/ui/newsongwizard.h
    src/ui/sampleeditordialog.cpp
    src/ui/sampleeditordialog.h
    src/ui/sf2zonepicker.cpp
    src/ui/sf2zonepicker.h
    src/ui/waveformview.cpp
    src/ui/waveformview.h
    src/ui/samplepicker.cpp
    src/ui/samplepicker.h
    src/ui/voicegroupbrowser.cpp
    src/ui/voicegroupbrowser.h
    src/ui/keymap.cpp
    src/ui/keymap.h
    src/ui/keyboardshortcutspage.cpp
    src/ui/keyboardshortcutspage.h
    src/ui/enginesettings.cpp
    src/ui/enginesettings.h
    src/ui/audiosettingspage.cpp
    src/ui/audiosettingspage.h
    src/ui/appearancesettingspage.cpp
    src/ui/appearancesettingspage.h
    src/ui/settingsdialog.cpp
    src/ui/settingsdialog.h
    src/ui/songsettingsdialog.cpp
    src/ui/songsettingsdialog.h
    src/ui/polyphonypanel.cpp
    src/ui/polyphonypanel.h
    src/ui/songlistpanel.cpp
    src/ui/songlistpanel.h
    src/ui/songviewmodel.cpp
    src/ui/songviewmodel.h
    src/ui/timelinesurface.cpp
    src/ui/timelinesurface.h
    src/ui/velocityaxis.cpp
    src/ui/velocityaxis.h
    src/ui/velocitygesturemodel.cpp
    src/ui/velocitygesturemodel.h
    src/ui/contextmenu.cpp
    src/ui/contextmenu.h
    src/ui/songview.cpp
    src/ui/songview.h
    src/ui/playheadoverlay.cpp
    src/ui/playheadoverlay.h
    src/ui/eventlistview.cpp
    src/ui/eventlistview.h
    src/ui/viewsidecar.cpp
    src/ui/viewsidecar.h
)
if(APPLE)
    target_sources(porydaw_app PRIVATE
        src/ui/playheadrenderer_macos.mm
    )
    target_compile_definitions(porydaw_app PUBLIC PORYDAW_USE_DIRECT_PLAYHEAD)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(porydaw_app PRIVATE
        ${APPKIT_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
    )
endif()
target_include_directories(porydaw_app PUBLIC
    src
    ${PORYAAAA_DIR}/third_party
    external/dr_libs
    external/stb
)
# The project() version is the single source of truth (docs/RELEASING.md);
# release CI refuses a tag that doesn't match it.
target_compile_definitions(porydaw_app PUBLIC
    PORYDAW_VERSION="${PROJECT_VERSION}")
target_link_libraries(porydaw_app PUBLIC Qt6::Widgets poryaaaa_engine)
if(WIN32)
    target_sources(porydaw_app PRIVATE src/ui/playheadrenderer_dcomp.cpp)
    target_compile_definitions(porydaw_app PUBLIC PORYDAW_USE_DIRECT_PLAYHEAD)
    target_link_libraries(porydaw_app PRIVATE d2d1 d3d11 dcomp dxgi)
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(porydaw_app PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

qt_add_executable(porydaw
    src/main.cpp
    src/tabcheck.cpp
    src/eventviewcheck.cpp
    src/rollcheck.cpp
    src/rollcheckplayhead.cpp
    src/rollcheckplayhead.h
    src/viewcheck.cpp
    src/roundtrip.cpp
    src/editcheck.cpp
    src/noteidcheck.cpp
    src/savecheck.cpp
    src/onboardcheck.cpp
    src/vgcheck.cpp
    src/vgsavecheck.cpp
    src/exportcheck.cpp
    src/loopcheck.cpp
    src/polycheck.cpp
    src/primecheck.cpp
    src/smfcheck.cpp
    src/transportcheck.cpp
    src/audiocheck.cpp
    src/mkcheck.cpp
    src/ignorecheck.cpp
    src/samplecheck.cpp
    src/sessioncheck.cpp
    src/settingscheck.cpp
    src/keymapcheck.cpp
    src/velcheck.cpp
    resources/fonts.qrc
)
if(APPLE)
    target_sources(porydaw PRIVATE
        src/rollcheckplayhead_macos.mm
    )
endif()
qt_add_resources(porydaw "app_icon"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/porydaw-16.png
        resources/porydaw-32.png
        resources/porydaw-48.png
        resources/porydaw-128.png
        resources/porydaw-256.png
)
qt_add_resources(porydaw "cursor_images"
    PREFIX "/cursors"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES
        resources/left-drag.png
        resources/left-drag@2x.png
        resources/pencil.png
        resources/pencil@2x.png
        resources/right-drag.png
        resources/right-drag@2x.png
)
target_link_libraries(porydaw PRIVATE porydaw_app)
if(WIN32)
    # Stage the Qt DLLs beside porydaw.exe so the build tree runs directly.
    # Static Qt links everything into the exe, leaving windeployqt nothing
    # to stage — it fails with "does not seem to be a Qt executable" — so
    # the step only runs against a shared Qt. The imported windeployqt
    # target ships with Qt 6.3+; without it the build still succeeds and
    # the DLLs must come from PATH.
    get_target_property(porydaw_qt_core_type Qt6::Core TYPE)
    if(porydaw_qt_core_type STREQUAL "STATIC_LIBRARY")
        # Nothing to deploy.
    elseif(TARGET Qt6::windeployqt)
        add_custom_command(TARGET porydaw POST_BUILD
            COMMAND Qt6::windeployqt
                    --no-translations
                    --no-opengl-sw
                    --no-system-d3d-compiler
                    "$<TARGET_FILE:porydaw>"
            VERBATIM
        )
    else()
        message(WARNING "Qt6::windeployqt not available (needs Qt >= 6.3); "
                        "porydaw.exe will need the Qt DLLs on PATH")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    # Font and theme checks link the real application library but stay out of
    # the shipping executable and its command-line surface.
    qt_add_executable(porydaw_themechecks
        src/ui/theme/themechecks_main.cpp
        src/fontcheck.cpp
        src/ui/theme/themecheck.cpp
        src/ui/theme/themecheck.h
        resources/fonts.qrc
    )
    target_link_libraries(porydaw_themechecks PRIVATE porydaw_app)

    add_test(NAME porydaw_theme_checks
             COMMAND porydaw_themechecks --themecheck)
    add_test(NAME porydaw_fontcheck
             COMMAND porydaw_themechecks --fontcheck)
    add_test(NAME porydaw_darkbase_check
             COMMAND porydaw_themechecks --darkbasecheck)
    set_tests_properties(porydaw_theme_checks porydaw_darkbase_check
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # fontcheck runs on the native platform on Windows so it exercises the
    # real font stack the TTF hinting targets; everywhere else offscreen
    # keeps headless runs (CI, WSL) working.
    if(NOT WIN32)
        set_tests_properties(porydaw_fontcheck
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

if(WIN32)
    set_target_properties(porydaw PROPERTIES WIN32_EXECUTABLE TRUE)
    # Embeds resources/porydaw.ico as the executable's Explorer/taskbar icon.
    target_sources(porydaw PRIVATE resources/porydaw.rc)
    target_link_libraries(porydaw_app PRIVATE dwmapi)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # MinGW: statically link libgcc/libstdc++/winpthread so the exe
        # doesn't depend on libgcc_s_seh-1.dll etc. at runtime.
        target_link_options(porydaw PRIVATE -static)
    endif()
endif()
if(APPLE)
    set_target_properties(porydaw PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_GUI_IDENTIFIER com.huderlem.porydaw
        MACOSX_BUNDLE_ICON_FILE porydaw.icns)
    target_sources(porydaw PRIVATE resources/porydaw.icns)
    set_source_files_properties(resources/porydaw.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
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
