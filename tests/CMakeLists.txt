add_library(itub_testsupport STATIC
    testsupport/TreeSnapshot.cpp
    testsupport/FixtureCorpus.cpp
)
target_link_libraries(itub_testsupport PUBLIC Qt6::Core)
target_compile_definitions(itub_testsupport PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

add_executable(tst_filesafety
    tst_filesafety.cpp
)
target_link_libraries(tst_filesafety PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_filesafety PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()

add_test(NAME filesafety
    COMMAND tst_filesafety
)

add_executable(tst_probe
    tst_probe.cpp
)
target_link_libraries(tst_probe PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_probe PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME probe
    COMMAND tst_probe
)

add_executable(tst_catalogue
    tst_catalogue.cpp
)
target_link_libraries(tst_catalogue PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_catalogue PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME catalogue
    COMMAND tst_catalogue
)

set_target_properties(tst_filesafety tst_probe tst_catalogue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
