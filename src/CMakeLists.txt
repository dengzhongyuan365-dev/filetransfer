find_package(OpenSSL REQUIRED)

add_library(lan_core
    common/parse.cpp
    common/path.cpp
    common/size.cpp
    common/stopwatch.cpp
    fs/file_descriptor.cpp
    fs/file_hash.cpp
    fs/local_copy.cpp
    app/sender_config.cpp
    app/receiver_config.cpp
)

target_include_directories(lan_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(lan_core PUBLIC
    OpenSSL::Crypto
)

add_executable(sender sender_main.cpp)
add_executable(receiver receiver_main.cpp)
add_executable(local-copy local_copy_main.cpp)

target_link_libraries(sender PRIVATE lan_core)
target_link_libraries(receiver PRIVATE lan_core)
target_link_libraries(local-copy PRIVATE lan_core)
