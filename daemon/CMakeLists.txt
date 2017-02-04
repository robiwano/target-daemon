project(target_daemon)

cmake_minimum_required(VERSION 3.0)

add_subdirectory(externals)

set(SRC
    src/main.cpp)
    
add_executable(target_daemon
    ${SRC})

if (WIN32)
  target_compile_definitions(
    target_daemon PRIVATE _SCL_SECURE_NO_WARNINGS
  )
endif()
    
target_link_libraries(target_daemon asio)

