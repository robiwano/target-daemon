project(target_daemon)

cmake_minimum_required(VERSION 3.0)

add_subdirectory(externals)

set(SRC
  src/main.cpp)
    
add_executable(target_daemon
  ${SRC})

set_target_properties(target_daemon
  PROPERTIES
  CXX_STANDARD 11
)

if (WIN32)
  target_compile_definitions(
    target_daemon PRIVATE _SCL_SECURE_NO_WARNINGS
  )
endif()
    
target_link_libraries(target_daemon asio)

if (UNIX)
  target_link_libraries(target_daemon pthread)
endif()