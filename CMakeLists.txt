cmake_minimum_required(VERSION 3.5)
project(lswasm-proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wasm-proxy src/main.cpp)

if(UNIX)
  target_link_libraries(wasm-proxy PRIVATE pthread)
endif()
