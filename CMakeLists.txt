cmake_minimum_required(VERSION 3.16)
project(RemotePointer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(remote_pointer remote_pointer.cpp)

if(WIN32)
    target_link_libraries(remote_pointer PRIVATE ws2_32 user32)
endif()
