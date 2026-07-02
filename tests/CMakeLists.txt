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

find_package(Qt6 COMPONENTS Core QUIET)

if(Qt6_FOUND)
    add_executable(gui_unit_tests
        gui_device_manager_tests.cpp
        gui_transfer_list_model_tests.cpp
        ../src/gui/device_manager.cpp
        ../src/gui/transfer_list_model.cpp
        ../src/gui/transfer_record_matcher.cpp
    )

    target_include_directories(gui_unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(gui_unit_tests PRIVATE
        Qt6::Core
        GTest::gtest_main
    )

    gtest_discover_tests(gui_unit_tests)
endif()
