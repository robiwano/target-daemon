
# ASIO CMake stuff ##################
add_library(asio INTERFACE)
    
target_include_directories(
    asio
    INTERFACE asio/asio/include
)

target_compile_definitions(
    asio
    INTERFACE ASIO_STANDALONE=1
)

if (WIN32)
    target_compile_definitions(
        asio
        INTERFACE _WIN32_WINNT=0x0601
    )
endif()
#####################################
