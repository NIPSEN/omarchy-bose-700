cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(bose-700-ctl
        VERSION 0.1.0
        DESCRIPTION "Bose NC Headphones 700 Headless Daemon Control CLI"
        LANGUAGES CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default install prefix to ~/.local" FORCE)
    endif()

    add_compile_options(
        -Wall
        -Wextra
        -pedantic
        -Wno-unused-parameter
    )
endif()

add_executable(bose-700-ctl
    src/main.cpp
)

target_compile_features(bose-700-ctl PRIVATE cxx_std_20)

install(TARGETS bose-700-ctl RUNTIME DESTINATION bin)
