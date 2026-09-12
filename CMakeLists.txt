cmake_minimum_required(VERSION 3.20)

project(omarchy-bose-700
    VERSION 0.1.0
    DESCRIPTION "Bose NC Headphones 700 Headless Bluetooth Daemon, CLI, and Omarchy Widget"
    LANGUAGES CXX C
)

# Standard C++20 enforcement
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd and language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default installation directory to ~/.local
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default install prefix to ~/.local" FORCE)
endif()

# Compiler options: Strict warnings as required
add_compile_options(
    -Wall
    -Wextra
    -pedantic
    -Wno-unused-parameter
)

# Position independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Testing configuration
include(CTest)
enable_testing()

# Daemon & Protocol Library Subdirectory
add_subdirectory(daemon)

# CLI Subdirectory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
endif()

# Installer/deploy helper (written separately; build works before it lands).
# Defines the bose-700-deploy target from installer/deploy.cpp.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/installer/CMakeLists.txt")
    add_subdirectory(installer)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/installer/deploy.cpp")
    add_executable(bose-700-deploy installer/deploy.cpp)
    target_compile_features(bose-700-deploy PRIVATE cxx_std_20)
    install(TARGETS bose-700-deploy RUNTIME DESTINATION bin)
endif()
