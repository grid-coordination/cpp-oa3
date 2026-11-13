# The POSIX (Linux, macOS) port of cpp-oa3: an HTTPS transport over libcurl, an MQTT session
# over Paho MQTT C, the clock and log bindings, the oa3-ven-demo program and their tests.
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

if(APPLE AND NOT OPENSSL_ROOT_DIR AND NOT DEFINED ENV{OPENSSL_ROOT_DIR})
    find_program(_brew brew)
    if(_brew)
        execute_process(COMMAND ${_brew} --prefix openssl@3 OUTPUT_VARIABLE _openssl_prefix
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _brew_rc)
        if(_brew_rc EQUAL 0 AND IS_DIRECTORY "${_openssl_prefix}")
            set(OPENSSL_ROOT_DIR "${_openssl_prefix}")
        endif()
    endif()
endif()
find_package(OpenSSL REQUIRED)

# Eclipse Paho MQTT C with SSL. `auto` uses an installed package (its CMake config) when there
# is one and otherwise fetches the pinned release; `system` and `fetch` force either.
set(OA3_POSIX_PAHO "auto" CACHE STRING "Paho MQTT C: auto, system or fetch")
set_property(CACHE OA3_POSIX_PAHO PROPERTY STRINGS auto system fetch)
set(_paho_target "")
if(NOT OA3_POSIX_PAHO STREQUAL "fetch")
    find_package(eclipse-paho-mqtt-c CONFIG QUIET)
    if(TARGET eclipse-paho-mqtt-c::paho-mqtt3cs)
        set(_paho_target eclipse-paho-mqtt-c::paho-mqtt3cs)
    elseif(TARGET eclipse-paho-mqtt-c::paho-mqtt3cs-static)
        set(_paho_target eclipse-paho-mqtt-c::paho-mqtt3cs-static)
    elseif(OA3_POSIX_PAHO STREQUAL "system")
        message(FATAL_ERROR "OA3_POSIX_PAHO=system but no eclipse-paho-mqtt-c package with SSL")
    endif()
endif()
if(_paho_target)
    message(STATUS "Paho MQTT C: system package ${eclipse-paho-mqtt-c_VERSION}")
else()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(PAHO_WITH_SSL ON CACHE BOOL "" FORCE)
    set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(PAHO_ENABLE_CPACK OFF CACHE BOOL "" FORCE)
    set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(PAHO_HIGH_PERFORMANCE ON CACHE BOOL "" FORCE)
    FetchContent_Declare(paho_mqtt_c
        URL https://github.com/eclipse-paho/paho.mqtt.c/archive/refs/tags/v1.3.16.tar.gz
        URL_HASH SHA256=8b960f51edc7e03507637d987882bc486d8f4be6e79431bf99e2763344fd14c5
    )
    FetchContent_MakeAvailable(paho_mqtt_c)
    set(_paho_target paho-mqtt3cs-static)
    message(STATUS "Paho MQTT C: fetched v1.3.16")
endif()

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
    src/paho_mqtt_session.cpp
    src/posix_clock.cpp
    src/stderr_log.cpp
)
target_include_directories(oa3_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(oa3_posix PUBLIC oa3
    PRIVATE CURL::libcurl ${_paho_target} OpenSSL::SSL OpenSSL::Crypto)
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

    # Starts mosquitto on an ephemeral loopback port; registered only when it is installed.
    find_program(OA3_POSIX_MOSQUITTO mosquitto
                 PATHS /usr/sbin /usr/local/sbin /opt/homebrew/sbin NO_CACHE)
    if(OA3_POSIX_MOSQUITTO)
        add_executable(test_posix_paho_loopback test/test_posix_paho_loopback.cpp)
        target_link_libraries(test_posix_paho_loopback PRIVATE oa3_posix ${_paho_target})
        oa3_posix_warnings(test_posix_paho_loopback)
        add_test(NAME test_posix_paho_loopback
                 COMMAND test_posix_paho_loopback ${OA3_POSIX_MOSQUITTO})
        set_tests_properties(test_posix_paho_loopback PROPERTIES TIMEOUT 60)
    else()
        message(STATUS "mosquitto not found: test_posix_paho_loopback not registered")
    endif()
endif()

if(OA3_POSIX_LIVE_TESTS)
    add_test(NAME oa3_posix_live COMMAND oa3-ven-demo --once)
    add_test(NAME oa3_posix_live_mqtt COMMAND oa3-ven-demo --notifications mqtt --once)
    add_test(NAME oa3_posix_live_mqtt_exclusive
             COMMAND oa3-ven-demo --notifications mqtt --exclusive-tls --once)
    set_tests_properties(oa3_posix_live oa3_posix_live_mqtt oa3_posix_live_mqtt_exclusive
                         PROPERTIES TIMEOUT 120 LABELS live)

    add_executable(test_posix_paho_mqtt_session test/test_posix_paho_mqtt_session.cpp)
    target_link_libraries(test_posix_paho_mqtt_session PRIVATE oa3_posix)
    oa3_posix_warnings(test_posix_paho_mqtt_session)
    add_test(NAME test_posix_paho_mqtt_session COMMAND test_posix_paho_mqtt_session)
    set_tests_properties(test_posix_paho_mqtt_session PROPERTIES TIMEOUT 120 LABELS live)
endif()
