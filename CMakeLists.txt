cmake_minimum_required(VERSION 3.16)
project(mcl_sdk LANGUAGES C)

option(MCL_SDK_BUILD_TESTS "Build host-side test suite and examples" ON)

set(MCL_WIRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-wire" CACHE PATH "Path to mcl-wire repository")
set(MCL_LINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-link" CACHE PATH "Path to mcl-link repository")

# --- Sibling dependency targets (always available) ---

add_library(mcl_wire_dep STATIC
    ${MCL_WIRE_DIR}/src/wire.c
    ${MCL_WIRE_DIR}/src/extension.c)
target_include_directories(mcl_wire_dep PUBLIC ${MCL_WIRE_DIR}/include)
set_target_properties(mcl_wire_dep PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

add_library(mcl_link_dep STATIC
    ${MCL_LINK_DIR}/src/link.c)
target_include_directories(mcl_link_dep PUBLIC ${MCL_LINK_DIR}/include)
set_target_properties(mcl_link_dep PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

# --- SDK library ---

add_library(mcl_sdk STATIC
    src/sdk.c)
target_include_directories(mcl_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
