qt_add_executable(hoverbench
    hoverbench.cpp
)

target_link_libraries(hoverbench PRIVATE
    Qt6::Gui Qt6::Multimedia
    itub_defaults
)

set_target_properties(hoverbench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
