cmake_minimum_required(VERSION 3.16)

project(mongreldb_c
    VERSION 0.58.1
    DESCRIPTION "Pure C99 HTTP client for MongrelDB"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MONGRELDB_C_BUILD_TESTS "Build the MongrelDB C client tests" ON)

# libcurl is the only runtime dependency. On Debian/Ubuntu install
# libcurl4-openssl-dev; on Fedora curl-devel; on macOS it ships with the system.
find_package(CURL REQUIRED)

add_library(mongreldb_c SHARED
    src/mongreldb.c
)
target_include_directories(mongreldb_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mongreldb_c PRIVATE CURL::libcurl)
target_compile_definitions(mongreldb_c PRIVATE MONGRELDB_C_EXPORTS)
set_target_properties(mongreldb_c PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
)

if(MONGRELDB_C_BUILD_TESTS)
    enable_testing()
    add_executable(test_mongreldb tests/test_mongreldb.c)
    target_link_libraries(test_mongreldb PRIVATE mongreldb_c CURL::libcurl)
    add_test(NAME test_mongreldb COMMAND test_mongreldb)

    # Offline wire-shape conformance test. Includes mongreldb.c directly to
    # reach the file-local json_serialize_column helper; does not link against
    # the public mongreldb_c library (avoids duplicate-symbol errors).
    add_executable(test_wire_shape tests/test_wire_shape.c)
    target_include_directories(test_wire_shape PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_wire_shape PRIVATE CURL::libcurl)
    add_test(NAME test_wire_shape COMMAND test_wire_shape)
endif()

# Install rules: library, headers, and pkg-config for packagers.
include(GNUInstallDirs)

install(TARGETS mongreldb_c
    EXPORT MongrelDBCTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/mongreldb.h include/mongreldb_engine.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Convenience static target is not provided; embed the sources directly if a
# static link is required. The header is dual-licensed MIT OR Apache-2.0.
