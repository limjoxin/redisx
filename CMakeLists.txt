cmake_minimum_required(VERSION 3.20)
project(redisx CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Asio (standalone) include path
set(ASIO_DIR "${CMAKE_SOURCE_DIR}/deps/asio/include")
add_library(asio_iface INTERFACE)
target_include_directories(asio_iface INTERFACE "${ASIO_DIR}")
target_compile_definitions(asio_iface INTERFACE ASIO_STANDALONE)

if (WIN32)
  target_compile_definitions(asio_iface INTERFACE _WIN32_WINNT=0x0A00)
endif()

# -------- Sources
file(GLOB_RECURSE REDISX_HEADERS "${CMAKE_SOURCE_DIR}/include/redisx/*.hpp")
file(GLOB_RECURSE REDISX_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")

# -------- Core library
add_library(redisx-core ${REDISX_SOURCES} ${REDISX_HEADERS})
target_include_directories(redisx-core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(redisx-core PUBLIC asio_iface)

if (NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(redisx-core PUBLIC Threads::Threads)
endif()

# -------- Server app
add_executable(redisx-server "${CMAKE_SOURCE_DIR}/app/main.cpp")
target_link_libraries(redisx-server PRIVATE redisx-core)

# -------- CLI app (standalone)
add_executable(redis-cli "${CMAKE_SOURCE_DIR}/app/redis-cli.cpp")
target_link_libraries(redis-cli PRIVATE asio_iface)
if (NOT WIN32)
  target_link_libraries(redis-cli PRIVATE Threads::Threads)
endif()
