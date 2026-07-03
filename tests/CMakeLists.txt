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
        ../src/gui/transfer_record_repository.cpp
        ../src/gui/transfer_record_store.cpp
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

find_package(Qt6 COMPONENTS Widgets Network QUIET)

if(Qt6Widgets_FOUND AND Qt6Network_FOUND)
    add_executable(gui_widget_tests
        gui_transfer_card_tests.cpp
        ../src/gui/qt_utils.cpp
        ../src/gui/transfer_card.cpp
    )

    target_include_directories(gui_widget_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(gui_widget_tests PRIVATE
        lan_core
        Qt6::Widgets
        Qt6::Network
        GTest::gtest
    )

    gtest_discover_tests(gui_widget_tests)
endif()
