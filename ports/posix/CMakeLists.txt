# The POSIX (Linux, macOS) port of cpp-oa3: an HTTPS transport over libcurl, the clock and
# log bindings, the oa3-ven-demo program and their tests.
cmake_minimum_required(VERSION 3.18)
project(oa3_posix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OA3_POSIX_WERROR "Treat warnings in the port, the demo and the tests as errors" OFF)
option(OA3_POSIX_TESTS "Build the tests" ON)
option(OA3_POSIX_LIVE_TESTS "Register a test against the live price server" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. oa3)

find_package(CURL REQUIRED)

function(oa3_posix_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    if(OA3_POSIX_WERROR)
        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    endif()
endfunction()

add_library(oa3_posix STATIC
    src/curl_transport.cpp
    src/posix_clock.cpp
    src/stderr_log.cpp
)
target_include_directories(oa3_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oa3_posix PUBLIC oa3 PRIVATE CURL::libcurl)
oa3_posix_warnings(oa3_posix)

add_executable(oa3-ven-demo examples/oa3-ven-demo/main.cpp)
target_link_libraries(oa3-ven-demo PRIVATE oa3_posix CURL::libcurl)
oa3_posix_warnings(oa3-ven-demo)

if(OA3_POSIX_TESTS OR OA3_POSIX_LIVE_TESTS)
    enable_testing()
endif()

if(OA3_POSIX_TESTS)
    find_package(Threads REQUIRED)

    # Starts its own HTTP/1.1 server on an ephemeral loopback port.
    add_executable(test_posix_curl_transport test/test_posix_curl_transport.cpp)
    target_link_libraries(test_posix_curl_transport PRIVATE oa3_posix CURL::libcurl Threads::Threads)
    oa3_posix_warnings(test_posix_curl_transport)
    add_test(NAME test_posix_curl_transport COMMAND test_posix_curl_transport)
    set_tests_properties(test_posix_curl_transport PROPERTIES TIMEOUT 60)

    add_executable(test_posix_clock test/test_posix_clock.cpp)
    target_link_libraries(test_posix_clock PRIVATE oa3_posix)
    oa3_posix_warnings(test_posix_clock)
    add_test(NAME test_posix_clock COMMAND test_posix_clock)
endif()

if(OA3_POSIX_LIVE_TESTS)
    add_test(NAME oa3_posix_live COMMAND oa3-ven-demo --once)
    set_tests_properties(oa3_posix_live PROPERTIES TIMEOUT 120 LABELS live)
endif()
