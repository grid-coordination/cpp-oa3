# The Unity suites, built and run without PlatformIO. Each test_<name>/ directory is one
# executable and one CTest test. A test_model_<name>/ suite links oa3_model alone.

# Unity 2.6.1, the release PlatformIO's `test_framework = unity` resolves to, so a suite
# behaves the same under `pio test` and under CTest. To build offline, pass
# -DFETCHCONTENT_SOURCE_DIR_UNITY=<checkout>.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(Unity
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.1.tar.gz
    URL_HASH SHA256=b41a66d45a6b99758fb3202ace6178177014d52fc524bf1f72687d93e9867292
)
FetchContent_MakeAvailable(Unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

file(GLOB _oa3_suites RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*)
foreach(_suite IN LISTS _oa3_suites)
    if(NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${_suite})
        continue()
    endif()
    file(GLOB _sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_suite}/*.cpp)
    add_executable(${_suite} ${_sources})
    target_link_libraries(${_suite} PRIVATE oa3_model unity)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${_suite} PRIVATE -Wall -Wextra)
    endif()
    string(REGEX REPLACE "^test_" "" _name ${_suite})
    add_test(NAME oa3_${_name} COMMAND ${_suite})
endforeach()
