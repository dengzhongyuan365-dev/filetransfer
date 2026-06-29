add_library(lan_core
    common/parse.cpp
    common/path.cpp
    common/size.cpp
    common/stopwatch.cpp
    app/sender_config.cpp
    app/receiver_config.cpp
)

target_include_directories(lan_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

add_executable(sender sender_main.cpp)
add_executable(receiver receiver_main.cpp)

target_link_libraries(sender PRIVATE lan_core)
target_link_libraries(receiver PRIVATE lan_core)
