cmake_minimum_required(VERSION 3.20)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)  # << 关键，关闭 GNU 扩展
add_executable(main main.cpp)
target_compile_features(main PRIVATE cxx_std_23)

