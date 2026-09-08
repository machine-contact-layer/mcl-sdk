cmake_minimum_required(VERSION 3.16)
project(mcl_developer_sdk VERSION 1.0.0 LANGUAGES C)

option(MCL_DEVELOPER_SDK_BUILD_EXAMPLE "Build the first-contact example" ON)
option(MCL_DEVELOPER_SDK_BUILD_TESTS "Build the machine contract test" ON)

add_library(mcl_sdk STATIC
    src/wire.c
    src/extension.c
    src/link.c
    src/contact.c
    src/endpoint_rendezvous.c
    src/handoff.c
    src/control.c
    src/negotiation.c
    src/sdk.c
    src/conformance.c
    src/rendezvous.c
    src/machine.c
    src/ap_channel.c
    src/ap_modem.c
    src/ap_listen.c
    src/ble_binding.c
    src/ip_binding.c)
target_include_directories(mcl_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(mcl_sdk PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(mcl_sdk PRIVATE /W4 /WX)
else()
    target_compile_options(mcl_sdk PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

if(MCL_DEVELOPER_SDK_BUILD_TESTS)
    enable_testing()
    add_executable(mcl_machine_contract tests/test_machine.c)
    target_link_libraries(mcl_machine_contract PRIVATE mcl_sdk)
    set_target_properties(mcl_machine_contract PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_machine_contract COMMAND mcl_machine_contract)
endif()
if(NOT WIN32)
    target_link_libraries(mcl_sdk PUBLIC m)
endif()

if(MCL_DEVELOPER_SDK_BUILD_EXAMPLE)
    add_executable(mcl_first_contact examples/first_contact.c)
    target_link_libraries(mcl_first_contact PRIVATE mcl_sdk)
    set_target_properties(mcl_first_contact PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_executable(mcl_resource_report examples/resource_report.c)
    target_link_libraries(mcl_resource_report PRIVATE mcl_sdk)
    set_target_properties(mcl_resource_report PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS mcl_sdk EXPORT mcl_sdkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/mcl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT mcl_sdkTargets
    FILE mcl_sdkTargets.cmake
    NAMESPACE mcl::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcl_sdkConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)
