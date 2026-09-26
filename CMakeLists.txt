cmake_minimum_required(VERSION 3.22)

project(klaudiuszoweVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Finished plugins are copied here after every build.
set(KLAUD_PLUGIN_OUTPUT_DIR "C:/gen plugins" CACHE PATH "Where built plugins are copied")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# Look and feel shared by all plugins.
add_library(klaud_shared INTERFACE)
target_include_directories(klaud_shared INTERFACE shared)

add_subdirectory(plugins/StereoScale)
