cmake_minimum_required(VERSION 3.20)
project(BluetoothBatteryTray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link the C runtime statically so the portable build runs on a clean Windows
# installation without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(BluetoothBatteryTray WIN32
    src/main.cpp
    src/BatteryMonitor.h
    src/BatteryMonitor.cpp
    resources/resource.rc
)

target_include_directories(BluetoothBatteryTray PRIVATE src)

target_link_libraries(BluetoothBatteryTray PRIVATE
    runtimeobject
    shell32
    user32
    advapi32
    ole32
    oleaut32
    comctl32
    setupapi
    windowscodecs
    comdlg32
)

target_compile_definitions(BluetoothBatteryTray PRIVATE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
