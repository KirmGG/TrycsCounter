cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
project(TrycsFrameWindows VERSION 0.1.2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TC_BUILD_TESTS "Build unit tests of the pure logic (no game needed)" OFF)

if (TC_BUILD_TESTS)
    enable_testing()
    foreach (test_name window_math_test probe_planner_test precision_test macro_test)
        add_executable(${test_name} tests/${test_name}.cpp)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    return()
endif()

add_library(${PROJECT_NAME} SHARED
    src/Analysis.cpp
    src/Cbf.cpp
    src/Common.cpp
    src/DeepCount.cpp
    src/Display.cpp
    src/EdgeCheck.cpp
    src/Graph.cpp
    src/LayoutMode.cpp
    src/Library.cpp
    src/LiveCounter.cpp
    src/Macros.cpp
    src/Recording.cpp
    src/ReplayFixes.cpp
    src/Replay.cpp
    src/Results.cpp
    src/Trace.cpp
    src/Training.cpp
    src/Ui.cpp
    src/hooks/BaseLayerHooks.cpp
    src/hooks/MenuHooks.cpp
    src/hooks/EditorHooks.cpp
    src/hooks/ObjectHooks.cpp
    src/hooks/PauseHooks.cpp
    src/hooks/PlayLayerHooks.cpp
    src/hooks/SchedulerHooks.cpp
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK is not set. Run: geode sdk install")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# iOS refuses to load a dylib without a code signature ("missing code signature"),
# so sign it ad-hoc before Geode packages it into the .geode.
if (IOS)
    find_program(TC_CODESIGN codesign)
    if (TC_CODESIGN)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${TC_CODESIGN} --force --sign - --identifier trycs.counter $<TARGET_FILE:${PROJECT_NAME}>
            COMMENT "Ad-hoc signing the iOS dylib")
    else()
        message(WARNING "codesign not found: the iOS dylib will be unsigned and iOS will not load it")
    endif()
endif()
