# Core library: non-GUI logic shared by the app and tests.
add_library(scrubtub_core STATIC
    app/ProfilePaths.h app/ProfilePaths.cpp
    app/MediaTools.h
    catalogue/SourceScanner.h catalogue/SourceScanner.cpp
    catalogue/Database.h catalogue/Database.cpp
    catalogue/Catalogue.h catalogue/Catalogue.cpp
    catalogue/CatalogueModel.h catalogue/CatalogueModel.cpp
    catalogue/CatalogueProxy.h catalogue/CatalogueProxy.cpp
    catalogue/VideoRow.h
    catalogue/TagEngine.h catalogue/TagEngine.cpp
    media/Probe.h media/Probe.cpp
    media/Extract.h media/Extract.cpp
    media/HoverSession.h media/HoverSession.cpp
    media/HoverFrameItem.h media/HoverFrameItem.cpp
    media/PreviewProvider.h media/PreviewProvider.cpp
)

target_include_directories(scrubtub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(scrubtub_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick
           rapidfuzz::rapidfuzz SQLite3::SQLite3
    INTERFACE scrubtub_defaults)

# GUI application
qt_add_executable(scrubtub WIN32
    main.cpp
)

qt_add_qml_module(scrubtub
    URI scrubtub.ui
    VERSION 1.0
    QML_FILES
        ui/Main.qml
        ui/VideoCard.qml
        ui/ActionButton.qml
        ui/RangeFilter.qml
    RESOURCES
        ui/icons/library.svg
        ui/icons/sort.svg
        ui/icons/sidebar.svg
        ui/icons/folder.svg
        ui/icons/star.svg
        ui/icons/plus.svg
        ui/icons/close.svg
        ui/icons/search.svg
        ui/icons/filter.svg
        ui/icons/settings.svg
        ui/icons/chevron.svg
        ui/icons/arrow.svg
        ui/icons/refresh.svg
        ui/icons/play.svg
        ui/icons/info.svg
        ui/icons/check.svg
        ui/icons/unrated.svg
        ui/icons/warning.svg
)

target_link_libraries(scrubtub PRIVATE
    scrubtub_core
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network
    scrubtub_defaults
)

set_target_properties(scrubtub scrubtub_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS scrubtub RUNTIME DESTINATION bin)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md"
    DESTINATION share/scrubtub)
option(SCRUBTUB_DEPLOY_QT "Deploy Qt runtime and QML modules at install time" OFF)
if(SCRUBTUB_DEPLOY_QT)
    set(scrubtub_deploy_options)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Distribution-provided Qt also lives in /usr/lib. Override Qt's default
        # exclusion of that entire directory, while leaving glibc to the OS.
        list(APPEND scrubtub_deploy_options POST_EXCLUDE_REGEXES
            "/libc[.]so[.]" "/libm[.]so[.]" "/libdl[.]so[.]"
            "/libpthread[.]so[.]" "/librt[.]so[.]" "/libresolv[.]so[.]"
            "/ld-linux[^/]*[.]so")
    elseif(WIN32)
        # qt.conf supplies relative paths. Keep DLL bytes matchable to MSYS2,
        # and let our DLL closure collector supply the MinGW runtime.
        list(APPEND scrubtub_deploy_options NO_TRANSLATIONS NO_COMPILER_RUNTIME
            DEPLOY_TOOL_OPTIONS --no-patchqt --no-system-d3d-compiler --no-opengl-sw)
    endif()
    qt_generate_deploy_qml_app_script(
        TARGET scrubtub
        OUTPUT_SCRIPT scrubtub_deploy_script
        ${scrubtub_deploy_options}
    )
    install(SCRIPT ${scrubtub_deploy_script})
endif()
