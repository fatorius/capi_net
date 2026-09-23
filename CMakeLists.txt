cmake_minimum_required(VERSION 3.24)
project(capi_net LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(CAPI_NET_BUILD_TESTS  "Build unit tests" ON)
option(CAPI_NET_BUILD_SERVER "Build the server (requer libpq e zlib)" ON)
option(CAPI_NET_BUILD_CLIENT "Build the client" ON)

include(FetchContent)

# Warnings só nos nossos alvos, não nas dependências baixadas.
add_library(capi_warnings INTERFACE)
target_compile_options(capi_warnings INTERFACE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# common: estatística (pentanomial, GSPRT, Elo) + tipos compartilhados
# ---------------------------------------------------------------------------
add_library(capi_common
    common/penta.cpp
    common/gsprt.cpp
    common/elo.cpp
    common/process.cpp
    common/engine_build.cpp
)
target_include_directories(capi_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(capi_common PRIVATE capi_warnings)

# ---------------------------------------------------------------------------
# HTTP + JSON (server, client e CLI)
# ---------------------------------------------------------------------------
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.58.0
    GIT_SHALLOW    TRUE)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(httplib json)

# CLI (§5.8): usa só cpp-httplib + nlohmann/json.
add_executable(capi_net cli/main.cpp)
target_link_libraries(capi_net PRIVATE httplib::httplib nlohmann_json::nlohmann_json
                                       capi_warnings)

# ---------------------------------------------------------------------------
# client
# ---------------------------------------------------------------------------
if(CAPI_NET_BUILD_CLIENT)
    add_library(capi_client_lib STATIC
        client/config.cpp
        client/topology.cpp
        client/pgn.cpp
        client/api.cpp
        client/runner.cpp
        client/fastchess_setup.cpp
    )
    target_include_directories(capi_client_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(capi_client_lib
        PUBLIC  capi_common httplib::httplib nlohmann_json::nlohmann_json
        PRIVATE capi_warnings)

    add_executable(capi_net_client client/main.cpp)
    target_link_libraries(capi_net_client PRIVATE capi_client_lib capi_warnings)
endif()

# ---------------------------------------------------------------------------
# server
# ---------------------------------------------------------------------------
if(CAPI_NET_BUILD_SERVER)
    # Localiza o libpq via pg_config quando o FindPostgreSQL não acharia sozinho
    # (ex: Homebrew keg-only). Sobrescrevível com -DPostgreSQL_ROOT=...
    find_program(PG_CONFIG pg_config)
    if(PG_CONFIG AND NOT PostgreSQL_ROOT)
        execute_process(COMMAND ${PG_CONFIG} --includedir
                        OUTPUT_VARIABLE _pg_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${PG_CONFIG} --libdir
                        OUTPUT_VARIABLE _pg_lib OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND CMAKE_INCLUDE_PATH ${_pg_inc})
        list(APPEND CMAKE_LIBRARY_PATH ${_pg_lib})
    endif()

    set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libpqxx
        GIT_REPOSITORY https://github.com/jtv/libpqxx.git
        GIT_TAG        7.10.7
        GIT_SHALLOW    TRUE)

    FetchContent_MakeAvailable(libpqxx)
    find_package(ZLIB REQUIRED)

    add_library(capi_server_lib STATIC
        server/config.cpp
        server/db/pool.cpp
        server/gzip.cpp
        server/sanity.cpp
        server/queue.cpp
        server/test_lifecycle.cpp
        server/spec.cpp
        server/books.cpp
        server/github.cpp
        server/smoke.cpp
        server/admin.cpp
        server/http/routes.cpp
        server/http/admin_routes.cpp
        server/http/web_routes.cpp
        server/reports.cpp
    )
    target_include_directories(capi_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(capi_server_lib
        PUBLIC  capi_common pqxx httplib::httplib nlohmann_json::nlohmann_json ZLIB::ZLIB
        PRIVATE capi_warnings)

    add_executable(capi_net_server server/main.cpp)
    target_link_libraries(capi_net_server PRIVATE capi_server_lib capi_warnings)
endif()

# ---------------------------------------------------------------------------
# testes
# ---------------------------------------------------------------------------
if(CAPI_NET_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(common_tests
        common/tests/test_penta.cpp
        common/tests/test_gsprt.cpp
        common/tests/test_elo.cpp
        common/tests/test_process.cpp
    )
    target_link_libraries(common_tests PRIVATE capi_common capi_warnings Catch2::Catch2WithMain)
    catch_discover_tests(common_tests)

    if(CAPI_NET_BUILD_SERVER)
        add_executable(server_tests
            server/tests/test_gzip.cpp
            server/tests/test_sanity.cpp
            server/tests/test_spec.cpp
            server/tests/test_books.cpp
        )
        target_link_libraries(server_tests PRIVATE capi_server_lib capi_warnings Catch2::Catch2WithMain)
        catch_discover_tests(server_tests)
    endif()

    if(CAPI_NET_BUILD_CLIENT)
        add_executable(client_tests
            client/tests/test_pgn.cpp
            client/tests/test_topology.cpp
            client/tests/test_runner.cpp
        )
        target_link_libraries(client_tests PRIVATE capi_client_lib capi_warnings Catch2::Catch2WithMain)
        target_compile_definitions(client_tests PRIVATE
            CAPI_CLIENT_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/client/tests")
        catch_discover_tests(client_tests)
    endif()
endif()
