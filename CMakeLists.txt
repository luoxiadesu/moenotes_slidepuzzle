cmake_minimum_required(VERSION 3.16)
project(moenotes-slidepuzzle VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(MOENOTES_BUILD_CLI "Build the SlidePuzzle command line tool" ON)
option(MOENOTES_BUILD_TESTS "Build C regression tests" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(moenotes_slidepuzzle STATIC
    src/sp_rng.c
    src/sp_puzzle.c
    src/sp_ida.c
    src/sp_pdb.c
    src/sp_image.c
    src/sp_board.c
    src/sp_match.c
    src/sp_solver.c
    src/sp_jacket.c
    src/sp_jacket_public.c)
add_library(moenotes::slidepuzzle ALIAS moenotes_slidepuzzle)
set_target_properties(moenotes_slidepuzzle PROPERTIES EXPORT_NAME slidepuzzle)
target_include_directories(moenotes_slidepuzzle
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_definitions(moenotes_slidepuzzle PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(moenotes_slidepuzzle PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
    target_link_libraries(moenotes_slidepuzzle PUBLIC m)
endif()

if(MOENOTES_BUILD_CLI)
    add_executable(moenotes_slidepuzzle_cli
        cli/moenotes_slidepuzzle_cli.c
        cli/moenotes_slidepuzzle_export.c)
    target_link_libraries(moenotes_slidepuzzle_cli PRIVATE moenotes_slidepuzzle)
    # The batch exporter writes gzip'd JSONL. This is the only third-party dependency
    # in the project and it is confined to the CLI: the library itself stays
    # dependency-free, so downstream users of moenotes::slidepuzzle are unaffected.
    find_package(ZLIB REQUIRED)
    target_link_libraries(moenotes_slidepuzzle_cli PRIVATE ZLIB::ZLIB)
    target_include_directories(moenotes_slidepuzzle_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(moenotes_slidepuzzle_cli PROPERTIES OUTPUT_NAME moenotes_slidepuzzle)
    if(NOT MSVC)
        target_compile_options(moenotes_slidepuzzle_cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS moenotes_slidepuzzle_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS moenotes_slidepuzzle ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/moenotes_slidepuzzle.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(MOENOTES_BUILD_TESTS)
    enable_testing()
    foreach(test_name rng puzzle read jacket pdb_api)
        add_executable(test_${test_name} tests/test_${test_name}.c)
        target_link_libraries(test_${test_name} PRIVATE moenotes_slidepuzzle)
        target_include_directories(test_${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(test_${test_name} PRIVATE
            MNSP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        if(NOT MSVC)
            target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test_name} COMMAND test_${test_name})
    endforeach()
    set_tests_properties(jacket PROPERTIES
        ENVIRONMENT "MOENOTES_SLIDEPUZZLE_CACHE=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
    if(MOENOTES_BUILD_CLI)
        add_test(NAME cli_version COMMAND moenotes_slidepuzzle_cli --version)
        set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "moenotes_slidepuzzle 0\\.1\\.0")

        # The exporter is a file producer, so its test drives the real binary and then
        # reads what it wrote. It needs zlib to read the gzip'd output back.
        add_executable(test_export tests/test_export.c)
        target_link_libraries(test_export PRIVATE moenotes_slidepuzzle ZLIB::ZLIB)
        target_compile_definitions(test_export PRIVATE
            MNSP_EXPORT_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-export"
            MNSP_EXPORT_BINARY="$<TARGET_FILE:moenotes_slidepuzzle_cli>")
        if(NOT MSVC)
            target_compile_options(test_export PRIVATE -UNDEBUG)
        endif()
        add_test(NAME export COMMAND test_export)
    endif()
endif()
