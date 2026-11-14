# Builds the library without PlatformIO, for a host check, embedded Linux or another RTOS.
# Usage from a parent project: add_subdirectory(path/to/cpp-oa3) and link oa3, the umbrella,
# or one component target (oa3_model, oa3_client).
cmake_minimum_required(VERSION 3.18)
project(oa3 VERSION 0.2.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_oa3_top_level ON)
else()
    set(_oa3_top_level OFF)
endif()

if(NOT TARGET ArduinoJson)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(ArduinoJson
        URL https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.3.tar.gz
        URL_HASH SHA256=a2a8a277fc3cd0404499199bf1468682ea3688f354be0eaf6af8648366911257
    )
    FetchContent_MakeAvailable(ArduinoJson)
endif()

add_subdirectory(model)
add_subdirectory(client)

add_library(oa3 INTERFACE)
target_link_libraries(oa3 INTERFACE oa3_model oa3_client)

if(_oa3_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(oa3_model PRIVATE -Wall -Wextra)
    target_compile_options(oa3_client PRIVATE -Wall -Wextra)
endif()

# Compiles every public header in a translation unit of its own, so a header that needs
# something it does not include (or includes a platform header) fails here.
option(OA3_CHECK_HEADERS "Compile each public header on its own" ${_oa3_top_level})
if(OA3_CHECK_HEADERS)
    function(oa3_header_check target include_dir)
        file(GLOB_RECURSE _headers RELATIVE ${include_dir} CONFIGURE_DEPENDS ${include_dir}/*.h)
        set(_tus "")
        foreach(_header IN LISTS _headers)
            string(MAKE_C_IDENTIFIER ${_header} _tu_name)
            set(_tu ${CMAKE_CURRENT_BINARY_DIR}/header_check/${target}/${_tu_name}.cpp)
            file(CONFIGURE OUTPUT ${_tu} CONTENT "#include <${_header}>\n")
            list(APPEND _tus ${_tu})
        endforeach()
        add_library(${target}_header_check OBJECT ${_tus})
        target_link_libraries(${target}_header_check PRIVATE ${target})
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target}_header_check PRIVATE -Wall -Wextra)
        endif()
    endfunction()
    oa3_header_check(oa3_model ${CMAKE_CURRENT_SOURCE_DIR}/model/include)
    oa3_header_check(oa3_client ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
endif()

# The Unity suites in test/, one CTest test each. Off when the library is a subproject.
option(OA3_TESTS "Build the unit tests (fetches Unity)" ${_oa3_top_level})
if(OA3_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
