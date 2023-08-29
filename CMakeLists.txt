cmake_minimum_required(VERSION 3.24)
project(ocijail)

set(CMAKE_CXX_STANDARD 20)
include_directories(${PROJECT_SOURCE_DIR}/)
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/ocijail/*.cpp)
add_executable(ocijail ${SRC_FILES})

find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)
target_link_libraries(ocijail PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(ocijail PRIVATE CLI11::CLI11)

install(TARGETS ocijail)
