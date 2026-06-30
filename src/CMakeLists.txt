find_package(OpenSSL REQUIRED)

add_library(lan_core
    common/parse.cpp
    common/path.cpp
    common/size.cpp
    common/stopwatch.cpp
    fs/file_descriptor.cpp
    fs/file_hash.cpp
    fs/local_copy.cpp
    net/posix_backend.cpp
    net/tcp.cpp
    transfer/block_signature.cpp
    transfer/chunk_codec.cpp
    transfer/delta.cpp
    transfer/file_metadata.cpp
    transfer/manifest.cpp
    transfer/part_file_guard.cpp
    transfer/sync_plan.cpp
    transfer/sync_executor.cpp
    protocol/frame.cpp
    protocol/hello.cpp
    transfer/sync_codec.cpp
    transfer/sync_session.cpp
    transfer/single_file.cpp
    app/sender_config.cpp
    app/sender_transfer.cpp
    app/receiver_config.cpp
    app/receiver_server.cpp
    app/transfer_event.cpp
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
add_executable(lan_tests lan_tests.cpp)

target_link_libraries(sender PRIVATE lan_core)
target_link_libraries(receiver PRIVATE lan_core)
target_link_libraries(local-copy PRIVATE lan_core)
target_link_libraries(lan_tests PRIVATE lan_core)

add_test(NAME lan_tests COMMAND lan_tests)
