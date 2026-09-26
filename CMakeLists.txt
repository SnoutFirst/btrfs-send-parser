cmake_minimum_required(VERSION 3.16)

project(btrfs-send-parser
        VERSION 0.1.0
        DESCRIPTION "Standalone parser for the Btrfs send stream format"
        HOMEPAGE_URL "https://github.com/SnoutFirst/btrfs-send-parser"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BTRFS_SEND_PARSER_BUILD_CLI "Build the btrfs-send-dump command line tool" ON)
option(BTRFS_SEND_PARSER_BUILD_TESTS "Build the test suite" ON)
option(BTRFS_SEND_PARSER_BUILD_EXAMPLES "Build the example programs" ON)
option(BTRFS_SEND_PARSER_BUILD_INTEGRATION_TESTS "Register tests that need root and a btrfs filesystem" OFF)
option(BTRFS_SEND_PARSER_INSTALL "Generate install rules" ON)
option(BTRFS_SEND_PARSER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(btrfs-send-parser
    src/byte_source.cpp
    src/crc32c.cpp
    src/error.cpp
    src/flags.cpp
    src/operations.cpp
    src/parser.cpp
    src/stream.cpp
    src/types.cpp)
add_library(btrfs::send ALIAS btrfs-send-parser)

target_compile_features(btrfs-send-parser PUBLIC cxx_std_17)
target_include_directories(btrfs-send-parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(btrfs-send-parser PROPERTIES
    OUTPUT_NAME btrfs-send-parser
    #The exported target has to match the alias used inside the build tree, so find_package() consumers link
    #btrfs::send exactly like this project's own targets do.
    EXPORT_NAME send
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    target_compile_options(btrfs-send-parser PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
else()
    target_compile_options(btrfs-send-parser PRIVATE /W4)
endif()

if(BTRFS_SEND_PARSER_WARNINGS_AS_ERRORS AND NOT MSVC)
    target_compile_options(btrfs-send-parser PRIVATE -Werror)
endif()

if(BTRFS_SEND_PARSER_BUILD_CLI)
    add_executable(btrfs-send-dump cli/dump.cpp cli/main.cpp)
    target_link_libraries(btrfs-send-dump PRIVATE btrfs::send)
    target_compile_definitions(btrfs-send-dump PRIVATE
        BTRFS_SEND_PARSER_VERSION="${PROJECT_VERSION}")
    if(NOT MSVC)
        target_compile_options(btrfs-send-dump PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    endif()
endif()

if(BTRFS_SEND_PARSER_BUILD_EXAMPLES)
    foreach(example walk_stream changed_extents parse_to_memory)
        add_executable(btrfs-send-example-${example} examples/${example}.cpp)
        target_link_libraries(btrfs-send-example-${example} PRIVATE btrfs::send)
        if(NOT MSVC)
            target_compile_options(btrfs-send-example-${example} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
        endif()
    endforeach()
endif()

if(BTRFS_SEND_PARSER_BUILD_TESTS)
    enable_testing()

    add_executable(btrfs-send-parser-tests
        tests/test_main.cpp
        tests/test_crc32c.cpp
        tests/test_types.cpp
        tests/test_parser_operations.cpp
        tests/test_parser_errors.cpp
        tests/test_stream_api.cpp
        tests/test_short_reads.cpp
        tests/test_fixtures.cpp)
    target_link_libraries(btrfs-send-parser-tests PRIVATE btrfs::send)
    target_include_directories(btrfs-send-parser-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    #The fixture tests read the streams committed under tests/fixtures from the source tree, so the tests
    #do not depend on the fixtures being copied next to the binary.
    target_compile_definitions(btrfs-send-parser-tests PRIVATE
        BTRFS_SEND_PARSER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(NOT MSVC)
        target_compile_options(btrfs-send-parser-tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    endif()

    add_test(NAME unit COMMAND btrfs-send-parser-tests)

    #The command line tool is exercised against the fixtures directly: one test per stream, so a failure names
    #the file that broke and no shell is needed to feed the tool.
    if(BTRFS_SEND_PARSER_BUILD_CLI)
        foreach(fixture
                clone-v2.bin
                compressed-v2.bin
                fallocate-fileattr-v2.bin
                full-v1.bin
                full-v2.bin
                incremental-v1.bin
                incremental-v2.bin
                minimal-v1.bin
                minimal-v2.bin
                multi-subvol-omit-end-v1.bin
                multi-subvol-v1.bin
                nodata-v1.bin
                nodata-v2.bin)
            add_test(NAME cli-${fixture}
                     COMMAND btrfs-send-dump --stats ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture})
        endforeach()
        #Input that is not a send stream has to be refused.
        add_test(NAME cli-rejects-non-streams
                 COMMAND btrfs-send-dump ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
        set_tests_properties(cli-rejects-non-streams PROPERTIES WILL_FAIL TRUE)
    endif()

    if(BTRFS_SEND_PARSER_BUILD_INTEGRATION_TESTS AND BTRFS_SEND_PARSER_BUILD_CLI)
        add_test(NAME integration
                 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run.sh $<TARGET_FILE:btrfs-send-dump>)
        set_tests_properties(integration PROPERTIES LABELS integration TIMEOUT 600)
    endif()
endif()

if(BTRFS_SEND_PARSER_INSTALL)
    install(TARGETS btrfs-send-parser EXPORT btrfs-send-parserTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/btrfs DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(BTRFS_SEND_PARSER_BUILD_CLI)
        install(TARGETS btrfs-send-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(EXPORT btrfs-send-parserTargets
            FILE btrfs-send-parserTargets.cmake
            NAMESPACE btrfs::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btrfs-send-parser)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/btrfs-send-parserConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/btrfs-send-parserConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btrfs-send-parser)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/btrfs-send-parserConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/btrfs-send-parserConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/btrfs-send-parserConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btrfs-send-parser)
endif()
