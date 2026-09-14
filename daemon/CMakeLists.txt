cmake_minimum_required(VERSION 3.20)

# Package Resolution via PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Target 1: bose_700_lib (Static Protocol, State & Bluetooth Transport Library)
# ---------------------------------------------------------------------------
set(PROTOCOL_LIB_SOURCES
    src/BluetoothManager.cpp
    src/BluezWatcher.cpp
    src/BmapLink.cpp
    src/StateEngine.cpp
    src/IpcServer.cpp
)

add_library(bose_700_lib STATIC
    ${PROTOCOL_LIB_SOURCES}
)

target_include_directories(bose_700_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(bose_700_lib PUBLIC
    PkgConfig::BLUEZ
    PkgConfig::SDBUS
    Threads::Threads
)

target_compile_features(bose_700_lib PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Target 2: test_protocol (Protocol, Framing, StateEngine & IpcServer Tests)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.cpp")
        add_executable(test_protocol
            tests/test_protocol.cpp
        )

        target_link_libraries(test_protocol PRIVATE
            bose_700_lib
        )

        target_include_directories(test_protocol PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        add_test(NAME ProtocolTest COMMAND test_protocol)
    endif()
endif()

# ---------------------------------------------------------------------------
# Target 3: bose-700-daemon (Headless Daemon Executable)
# ---------------------------------------------------------------------------
add_executable(bose-700-daemon
    src/main.cpp
)

target_link_libraries(bose-700-daemon PRIVATE
    bose_700_lib
)

target_include_directories(bose-700-daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

install(TARGETS bose-700-daemon RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Target 4: systemd User Socket & Service Units
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bose-700.socket")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/bose-700.socket"
        DESTINATION "lib/systemd/user"
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bose-700.service")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/bose-700.service"
        DESTINATION "lib/systemd/user"
    )
endif()
