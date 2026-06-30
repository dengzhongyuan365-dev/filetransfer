find_package(GTest REQUIRED)

add_executable(unit_tests
    common_tests.cpp
    transfer_tests.cpp
)

target_link_libraries(unit_tests PRIVATE
    lan_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
