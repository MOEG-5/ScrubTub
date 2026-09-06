qt_add_executable(hoverbench
    hoverbench.cpp
)

qt_add_executable(framecheck /tmp/framecheck.cpp)
target_link_libraries(framecheck PRIVATE itub_core Qt6::Multimedia itub_defaults)
target_compile_definitions(framecheck PRIVATE
    ITUB_DEBUG_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")


target_link_libraries(hoverbench PRIVATE
    Qt6::Gui Qt6::Multimedia
    itub_defaults
)

set_target_properties(hoverbench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
