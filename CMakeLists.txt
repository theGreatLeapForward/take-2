cmake_minimum_required(VERSION 3.25)
project(dojo_bot_take_2)

set(CMAKE_CXX_STANDARD 20)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} -ldpp -lpq -lpqxx -lspdlog -lfmt)