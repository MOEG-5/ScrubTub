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

add_executable(tst_extract
    tst_extract.cpp
)
target_link_libraries(tst_extract PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_extract PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME extract
    COMMAND tst_extract
)

add_executable(tst_preview
    tst_preview.cpp
)
target_link_libraries(tst_preview PRIVATE
    itub_testsupport itub_core Qt6::Quick Qt6::Test itub_defaults
)
add_test(NAME preview
    COMMAND tst_preview
)

add_executable(tst_hoversession
    tst_hoversession.cpp
)
target_link_libraries(tst_hoversession PRIVATE
    itub_testsupport itub_core Qt6::Multimedia Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_hoversession PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME hoversession
    COMMAND tst_hoversession
)

add_executable(tst_search
    tst_search.cpp
)
target_link_libraries(tst_search PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_search PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME search
    COMMAND tst_search
)

add_executable(tst_fileops
    tst_fileops.cpp
)
target_link_libraries(tst_fileops PRIVATE
    itub_testsupport itub_core Qt6::Test itub_defaults
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
    target_compile_definitions(tst_fileops PRIVATE
        ITUB_DEFAULT_SOURCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../testvideo1.mp4")
endif()
add_test(NAME fileops
    COMMAND tst_fileops
)

set_target_properties(tst_filesafety tst_probe tst_catalogue tst_extract tst_preview tst_hoversession tst_search tst_fileops PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)



# Headless platforms for every Qt-based test; GUI tests use their own
# disposable displays per AGENTS.md.
set_tests_properties(filesafety probe catalogue extract preview hoversession search fileops PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
