find_package(OpenSSL REQUIRED)
find_package(Qt6 COMPONENTS Widgets Network)

add_library(lan_core
    common/error.cpp
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
    app/transfer_scheduler.cpp
    app/transfer_snapshot.cpp
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

install(TARGETS sender receiver local-copy
    RUNTIME DESTINATION bin
)

if(Qt6_FOUND)
    find_package(Qt6LinguistTools QUIET)

    add_executable(lan-gui
        gui/control_message.cpp
        gui/discovery_controller.cpp
        gui/drop_panel.cpp
        gui/device_manager.cpp
        gui/main_window.cpp
        gui/qt_utils.cpp
        gui/target_dialogs.cpp
        gui/transfer_card.cpp
        gui/transfer_events.cpp
        gui/transfer_list_model.cpp
        gui_main.cpp
    )
    target_include_directories(lan-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lan-gui PRIVATE lan_core Qt6::Widgets Qt6::Network)

    target_compile_definitions(lan-gui PRIVATE
        LAN_TRANSLATION_DIR="${CMAKE_INSTALL_FULL_DATADIR}/lan-file-transfer/translations"
    )

    install(TARGETS lan-gui
        RUNTIME DESTINATION bin
    )

    if(Qt6LinguistTools_FOUND)
        qt_add_translation(LAN_GUI_QM_FILES
            ${PROJECT_SOURCE_DIR}/translations/lan-file-transfer_zh_CN.ts
        )
        add_custom_target(lan-gui-translations ALL DEPENDS ${LAN_GUI_QM_FILES})
        install(FILES ${LAN_GUI_QM_FILES}
            DESTINATION share/lan-file-transfer/translations
        )
    else()
        message(STATUS "Qt6 LinguistTools not found; translation .qm files will not be generated")
    endif()
endif()
