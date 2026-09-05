cmake_minimum_required(VERSION 3.16)
project(mcl_sdk LANGUAGES C)

set(MCL_SDK_VERSION 1.0.0)

option(MCL_SDK_BUILD_TESTS "Build host-side test suite and examples" ON)
option(MCL_SDK_BUILD_TOOLS "Build the dual-transport host harness shared library" OFF)

set(MCL_WIRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-wire" CACHE PATH "Path to mcl-wire repository")
set(MCL_LINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-link" CACHE PATH "Path to mcl-link repository")
set(MCL_IP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-ip" CACHE PATH "Path to mcl-ip repository")
set(MCL_BLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-ble" CACHE PATH "Path to mcl-ble repository")

# --- Dependencies: installed packages first, sibling sources as a fallback ---
#
# Two consumers exist and they need different things.
#
# An EXTERNAL consumer has an install prefix and no idea these repositories are
# siblings; it must resolve mcl_wire and mcl_link through find_package. That is
# also the only configuration from which this package can be installed, because
# an exported target may not reference a target that was never installed.
#
# A DEVELOPER has the eight repositories checked out side by side and wants to
# build without installing anything first. That path compiles the sibling
# sources directly, exactly as before.
#
# find_package first, so the external case is the one that is exercised whenever
# it is available rather than being a rarely-used branch.
find_package(mcl_wire QUIET CONFIG)
find_package(mcl_link QUIET CONFIG)

if(mcl_wire_FOUND AND mcl_link_FOUND)
    set(MCL_SDK_DEPS_INSTALLED TRUE)
    add_library(mcl_wire_dep INTERFACE)
    target_link_libraries(mcl_wire_dep INTERFACE mcl::mcl_wire)
    add_library(mcl_link_dep INTERFACE)
    target_link_libraries(mcl_link_dep INTERFACE mcl::mcl_link)
    message(STATUS "mcl-sdk: using installed mcl_wire and mcl_link")
else()
    set(MCL_SDK_DEPS_INSTALLED FALSE)
    foreach(dep_dir "${MCL_WIRE_DIR}" "${MCL_LINK_DIR}")
        if(NOT EXISTS "${dep_dir}/include/mcl")
            message(FATAL_ERROR
                "mcl-sdk needs either installed mcl_wire/mcl_link packages or "
                "the sibling repositories. Neither was found; missing: ${dep_dir}")
        endif()
    endforeach()

    add_library(mcl_wire_dep STATIC
        ${MCL_WIRE_DIR}/src/wire.c
        ${MCL_WIRE_DIR}/src/extension.c)
    target_include_directories(mcl_wire_dep PUBLIC ${MCL_WIRE_DIR}/include)
    set_target_properties(mcl_wire_dep PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_library(mcl_link_dep STATIC
        ${MCL_LINK_DIR}/src/link.c
        ${MCL_LINK_DIR}/src/contact.c
        ${MCL_LINK_DIR}/src/rendezvous.c
        ${MCL_LINK_DIR}/src/handoff.c
        ${MCL_LINK_DIR}/src/control.c
        ${MCL_LINK_DIR}/src/negotiation.c)
    target_include_directories(mcl_link_dep PUBLIC ${MCL_LINK_DIR}/include)
    set_target_properties(mcl_link_dep PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    message(STATUS "mcl-sdk: building mcl_wire and mcl_link from sibling sources")
endif()

# --- SDK library ---

add_library(mcl_sdk STATIC
    src/sdk.c
    src/conformance.c
    src/rendezvous.c)
# Build-tree include path. The install-interface path is added in the install
# section, which is the only configuration that can export this target.
target_include_directories(mcl_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mcl_sdk PUBLIC
    mcl_wire_dep
    mcl_link_dep)
set_target_properties(mcl_sdk PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

# --- Tests and examples ---

if(MCL_SDK_BUILD_TESTS)
    enable_testing()

    add_executable(mcl_sdk_test_conformance tests/test_conformance.c)
    target_link_libraries(mcl_sdk_test_conformance PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_conformance PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_conformance COMMAND mcl_sdk_test_conformance)

    add_executable(mcl_sdk_test_rendezvous tests/test_rendezvous.c)
    target_link_libraries(mcl_sdk_test_rendezvous PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_rendezvous PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_rendezvous COMMAND mcl_sdk_test_rendezvous)

    add_executable(mcl_sdk_test tests/test_sdk.c)
    target_link_libraries(mcl_sdk_test PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_test COMMAND mcl_sdk_test)

    add_executable(mcl_sdk_test_framed tests/test_sdk_framed.c)
    target_link_libraries(mcl_sdk_test_framed PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_framed PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_framed COMMAND mcl_sdk_test_framed)

    add_executable(mcl_sdk_test_handoff tests/test_sdk_handoff.c)
    target_link_libraries(mcl_sdk_test_handoff PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_handoff PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_handoff COMMAND mcl_sdk_test_handoff)

    add_executable(mcl_sdk_test_multi_contact tests/test_multi_contact.c)
    target_link_libraries(mcl_sdk_test_multi_contact PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_multi_contact PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_multi_contact COMMAND mcl_sdk_test_multi_contact)

    add_executable(mcl_sdk_test_headers tests/test_headers.c)
    target_link_libraries(mcl_sdk_test_headers PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_test_headers PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_test_headers COMMAND mcl_sdk_test_headers)

    # Pins the transport-id registry against the binding headers implementing
    # it. These drifted apart once and nothing caught it; see the test header.
    # Bindings are optional siblings, so each is compiled in only if present.
    add_executable(mcl_sdk_test_transport_registry tests/test_transport_registry_binding.c)
    target_link_libraries(mcl_sdk_test_transport_registry PRIVATE mcl_sdk)
    target_compile_definitions(mcl_sdk_test_transport_registry PRIVATE
        "MCL_TRANSPORT_REGISTRY_PATH=\"${MCL_LINK_DIR}/registries/transport-ids-v0.1.json\"")
    foreach(binding IP BLE UWB)
        string(TOLOWER ${binding} binding_lower)
        set(binding_dir "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-${binding_lower}")
        if(EXISTS "${binding_dir}/include/mcl/${binding_lower}_binding.h")
            target_include_directories(mcl_sdk_test_transport_registry PRIVATE
                "${binding_dir}/include")
            target_compile_definitions(mcl_sdk_test_transport_registry PRIVATE
                MCL_HAVE_${binding}_BINDING)
        endif()
    endforeach()
    # Each binding's profile registry restates its transport_id, so it is a
    # second place the assignment can drift.
    foreach(binding AP IP BLE UWB)
        string(TOLOWER ${binding} binding_lower)
        set(profile_reg
            "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-${binding_lower}/registries/${binding_lower}-profiles-v0.1.json")
        if(EXISTS "${profile_reg}")
            target_compile_definitions(mcl_sdk_test_transport_registry PRIVATE
                "MCL_${binding}_PROFILE_REGISTRY_PATH=\"${profile_reg}\"")
        endif()
    endforeach()
    set_target_properties(mcl_sdk_test_transport_registry PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_sdk_transport_registry COMMAND mcl_sdk_test_transport_registry)

    add_executable(mcl_sdk_hello_world examples/hello_world.c)
    target_link_libraries(mcl_sdk_hello_world PRIVATE mcl_sdk)
    set_target_properties(mcl_sdk_hello_world PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endif()

# --- Host harness shared library ---
#
# The dual-transport experiment needs a host peer holding a BLE link and a UDP
# socket at the same time. Windows exposes BLE only through WinRT, so that
# harness is written in C# -- and a harness that reimplemented framing,
# fragmentation or the migration state machine in C# would be measuring itself
# against the board rather than measuring the shipped code. Instead the whole
# stack is compiled into one shared library and the harness calls into it
# through tools/dual_host_shim.c, which adds no protocol logic of its own.
if(MCL_SDK_BUILD_TOOLS)
    foreach(pair "mcl-ip;${MCL_IP_DIR}/src/ip_binding.c" "mcl-ble;${MCL_BLE_DIR}/src/ble_binding.c")
        list(GET pair 0 dep_name)
        list(GET pair 1 dep_src)
        if(NOT EXISTS "${dep_src}")
            message(FATAL_ERROR
                "MCL_SDK_BUILD_TOOLS requires ${dep_name}. Expected ${dep_src}.")
        endif()
    endforeach()

    add_library(mcl_dual_host SHARED
        tools/dual_host_shim.c
        "${MCL_IP_DIR}/src/ip_binding.c"
        "${MCL_BLE_DIR}/src/ble_binding.c")
    target_include_directories(mcl_dual_host PRIVATE
        "${MCL_IP_DIR}/include"
        "${MCL_BLE_DIR}/include")
    target_link_libraries(mcl_dual_host PRIVATE mcl_sdk)
    set_target_properties(mcl_dual_host PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
endif()

# --- Installation and package export ---
#
# Only possible when the dependencies came from installed packages: an exported
# target may not reference a target that was never installed, and mcl_wire_dep
# built from sibling sources is exactly such a target. Refusing loudly beats
# producing a package whose Targets.cmake names something the consumer cannot
# resolve.
if(MCL_SDK_DEPS_INSTALLED)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    target_include_directories(mcl_sdk PUBLIC
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

    install(TARGETS mcl_sdk mcl_wire_dep mcl_link_dep
        EXPORT mcl_sdkTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/mcl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT mcl_sdkTargets
        FILE mcl_sdkTargets.cmake
        NAMESPACE mcl::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfigVersion.cmake"
        VERSION ${MCL_SDK_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcl_sdkConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/mcl_sdkConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcl_sdk)
endif()
