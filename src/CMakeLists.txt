cmake_minimum_required(VERSION 3.22)
project(HighFPS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add shared library
add_library(highfps SHARED
    cpp/main.cpp
    cpp/log.cpp
    cpp/config.cpp
    cpp/timing.cpp
    cpp/ui.cpp
    cpp/interpolation.cpp
    external/TEFKernel/include/tef_api_imp.c
)

# Output filename: libhighfps.android.arm64.so
set_target_properties(highfps PROPERTIES
    OUTPUT_NAME "highfps.android.arm64"
    PREFIX "lib"
    SUFFIX ".so"
)

# Include directories
target_include_directories(highfps PRIVATE
    cpp
    includes
    external/TEFKernel/include
)

target_link_libraries(highfps PRIVATE dl log android)
