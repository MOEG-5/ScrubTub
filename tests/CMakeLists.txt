find_file(SCRUBTUB_TEST_FIXTURE testvideo1.mp4
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../vids"
    NO_DEFAULT_PATH)

# The read-only test corpus: the repository's own vids/ folder, scanned as-is
# (AGENTS.md). Overridable per machine or per worktree at configure time
# (-DSCRUBTUB_TEST_VIDS_DIR=...) or at run time (SCRUBTUB_TEST_VIDS_DIR env);
# suites skip with a clear message when it is missing.
set(SCRUBTUB_TEST_VIDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vids" CACHE PATH
    "Directory scanned as-is by media-dependent tests")

add_library(scrubtub_testsupport STATIC
    testsupport/TreeSnapshot.cpp
    testsupport/FixtureCorpus.cpp
    testsupport/MediaFixtures.cpp
)
target_link_libraries(scrubtub_testsupport PUBLIC Qt6::Core)
target_compile_definitions(scrubtub_testsupport PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_compile_definitions(scrubtub_testsupport PUBLIC
    SCRUBTUB_TEST_VIDS_DIR_DEFAULT="${SCRUBTUB_TEST_VIDS_DIR}")

add_executable(tst_filesafety
    tst_filesafety.cpp
)
target_link_libraries(tst_filesafety PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_filesafety PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()

add_test(NAME filesafety
    COMMAND tst_filesafety
)

add_executable(tst_probe
    tst_probe.cpp
)
target_link_libraries(tst_probe PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_probe PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()
add_test(NAME probe
    COMMAND tst_probe
)

add_executable(tst_catalogue
    tst_catalogue.cpp
)
target_link_libraries(tst_catalogue PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_catalogue PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()
add_test(NAME catalogue
    COMMAND tst_catalogue
)

add_executable(tst_extract
    tst_extract.cpp
)
target_link_libraries(tst_extract PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_extract PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()
add_test(NAME extract
    COMMAND tst_extract
)

add_executable(tst_preview
    tst_preview.cpp
)
target_link_libraries(tst_preview PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Quick Qt6::Test scrubtub_defaults
)
add_test(NAME preview
    COMMAND tst_preview
)

add_executable(tst_hoversession
    tst_hoversession.cpp
)
target_link_libraries(tst_hoversession PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_hoversession PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()
add_test(NAME hoversession
    COMMAND tst_hoversession
)

add_executable(tst_search
    tst_search.cpp
)
target_link_libraries(tst_search PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_search PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
endif()
add_test(NAME search
    COMMAND tst_search
)

add_executable(tst_fileops
    tst_fileops.cpp
)
target_link_libraries(tst_fileops PRIVATE
    scrubtub_testsupport scrubtub_core Qt6::Test scrubtub_defaults
)
if(SCRUBTUB_TEST_FIXTURE)
    target_compile_definitions(tst_fileops PRIVATE
        SCRUBTUB_DEFAULT_SOURCE_FIXTURE="${SCRUBTUB_TEST_FIXTURE}")
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

# Media-free regression fences for the large-library hot paths (model
# bookkeeping, fuzzy search). No corpus, no subprocesses.
add_executable(tst_performance tst_performance.cpp)
target_link_libraries(tst_performance PRIVATE scrubtub_core Qt6::Test scrubtub_defaults)
set_target_properties(tst_performance PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME performance COMMAND tst_performance)
set_tests_properties(performance PROPERTIES TIMEOUT 180)
set_property(TEST performance APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Media-free regressions for asynchronous paging and folder removal.
add_executable(tst_catalogueupdates tst_catalogueupdates.cpp)
target_link_libraries(tst_catalogueupdates PRIVATE scrubtub_core Qt6::Test scrubtub_defaults)
set_target_properties(tst_catalogueupdates PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME catalogueupdates COMMAND tst_catalogueupdates)
set_tests_properties(catalogueupdates PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Explicit private-build validation; ordinary runs skip without the directory.
add_executable(tst_mediarelease tst_mediarelease.cpp)
target_link_libraries(tst_mediarelease PRIVATE scrubtub_testsupport scrubtub_core Qt6::Test)
add_test(NAME mediarelease COMMAND tst_mediarelease)
set_tests_properties(mediarelease PROPERTIES TIMEOUT 1800)

add_executable(tst_mediatools tst_mediatools.cpp)
target_link_libraries(tst_mediatools PRIVATE scrubtub_core Qt6::Test)
add_test(NAME mediatools COMMAND tst_mediatools)

# CI uses newly generated original clips, never an uploaded copy of vids/.
add_executable(tst_releasesmoke tst_releasesmoke.cpp)
target_link_libraries(tst_releasesmoke PRIVATE scrubtub_testsupport scrubtub_core Qt6::Test)
add_test(NAME releasesmoke COMMAND tst_releasesmoke)
set_tests_properties(releasesmoke PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
