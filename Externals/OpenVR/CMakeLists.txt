# CMakeLists.txt for OpenVR SDK
cmake_minimum_required(VERSION 3.13)
project(OpenVR)

# Define the OpenVR library
add_library(openvr_api INTERFACE)

# Specify the include directory for the library
target_include_directories(openvr_api INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Specify the library file
# Note: In a real scenario, you would link against the actual .lib or .so/.a file.
# For this placeholder, we ensure CMake knows where to look.
# CMake will expect a file named openvr_api or libopenvr_api.a/so/lib etc.
# We created a dummy libopenvr_api.a
target_link_libraries(openvr_api INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/libopenvr_api.a"
)

# Create a target that can be used by the main project
add_library(OpenVR::OpenVR ALIAS openvr_api)
