cmake_minimum_required(VERSION 3.16)
project(remote_cpp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 指定 Qt5 的安装路径 (请根据你的实际安装路径修改！)
set(CMAKE_PREFIX_PATH "D:/Qt/Qt5.14.2/5.14.2/mingw73_64")

# 查找 Qt5
find_package(Qt5 REQUIRED COMPONENTS Widgets Gui)
find_package(Threads REQUIRED)

# 客户端
set(CMAKE_AUTOMOC ON)
add_executable(remote_client WIN32 client/main.cpp client/app.rc)
target_link_libraries(remote_client PRIVATE Qt5::Widgets Qt5::Gui ws2_32 Threads::Threads)

# 服务端（Windows）
if(WIN32)
    add_executable(remote_server_win WIN32 server_win/main.cpp)
    target_link_libraries(remote_server_win PRIVATE Qt5::Gui ws2_32 shell32 Threads::Threads)
endif()
