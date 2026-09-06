# Core library: non-GUI logic shared by the app, tests, and benchmarks.
add_library(itub_core STATIC
    app/ProfilePaths.h app/ProfilePaths.cpp
    catalogue/SourceScanner.h catalogue/SourceScanner.cpp
    catalogue/Database.h catalogue/Database.cpp
    catalogue/Catalogue.h catalogue/Catalogue.cpp
    catalogue/CatalogueModel.h catalogue/CatalogueModel.cpp
    catalogue/CatalogueProxy.h catalogue/CatalogueProxy.cpp
    catalogue/VideoRow.h
    media/Probe.h media/Probe.cpp
    media/Extract.h media/Extract.cpp
)

target_include_directories(itub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(itub_core
    PUBLIC Qt6::Core Qt6::Gui rapidfuzz::rapidfuzz SQLite3::SQLite3
    INTERFACE itub_defaults)

# GUI application
qt_add_executable(itub WIN32
    main.cpp
)

qt_add_qml_module(itub
    URI itub.ui
    VERSION 1.0
    QML_FILES
        ui/Main.qml
)

target_link_libraries(itub PRIVATE
    itub_core
    Qt6::Qml Qt6::Quick Qt6::QuickControls2
    itub_defaults
)

set_target_properties(itub itub_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
