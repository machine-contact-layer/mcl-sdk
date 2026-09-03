cmake_minimum_required(VERSION 3.16)
project(mcl_external_consumer LANGUAGES C)

# Deliberately NOTHING here knows where the MCL repositories live.
#
# No ../mcl-wire, no MCL_*_DIR, no add_subdirectory. If find_package cannot
# resolve the SDK from CMAKE_PREFIX_PATH, this project must fail -- that failure
# is the test. A consumer project that reached into a sibling source tree would
# be proving the sibling layout works, which is the opposite of what the release
# gate asks.
find_package(mcl_sdk REQUIRED CONFIG)

add_executable(mcl_external_consumer main.c)
target_link_libraries(mcl_external_consumer PRIVATE mcl::mcl_sdk)
set_target_properties(mcl_external_consumer PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

enable_testing()
add_test(NAME mcl_external_consumer COMMAND mcl_external_consumer)
