cmake_minimum_required(VERSION 3.20)

# Configurable on its own, so `setup --uninstall` can build just this helper.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(bose-700-deploy LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# bose-700-deploy: the no-follow file placement helper setup runs. It is built
# alongside the daemon but never installed.
add_executable(bose-700-deploy
    deploy.cpp
)

target_compile_features(bose-700-deploy PRIVATE cxx_std_20)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_deploy.sh")
    add_test(NAME test_deploy
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_deploy.sh $<TARGET_FILE:bose-700-deploy>
    )
endif()
