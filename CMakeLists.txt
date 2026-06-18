cmake_minimum_required(VERSION 3.16)
project(sensor_manager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# nlohmann/json — prefer system install, fall back to FetchContent
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Mosquitto C client (libmosquitto-dev)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

set(SOURCES
    src/main.cpp
    src/Logger.cpp
    src/Config.cpp
    src/Transport.cpp
    src/MqttClient.cpp
    src/SensorManager.cpp
)

add_executable(sensor_manager ${SOURCES})
target_include_directories(sensor_manager PRIVATE src)
target_include_directories(sensor_manager PRIVATE ${MOSQUITTO_INCLUDE_DIRS})
target_link_libraries(sensor_manager PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    ${MOSQUITTO_LIBRARIES}
)

install(TARGETS sensor_manager DESTINATION bin)
install(FILES config/sensor_config.json DESTINATION etc/sensor_manager)
install(FILES config/sensor_config.ini  DESTINATION etc/sensor_manager)
