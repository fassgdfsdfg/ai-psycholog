cmake_minimum_required(VERSION 3.20)
project(ai_psychologist_server CXX)

set(CMAKE_CXX_STANDARD 17)

# Ищем библиотеки через vcpkg
find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(crow CONFIG REQUIRED)

# Собираем сервер
add_executable(server main.cpp)
target_link_libraries(server PRIVATE Crow::Crow CURL::libcurl nlohmann_json::nlohmann_json)