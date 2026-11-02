# Host tests for the protocol core (pure C, no ESP-IDF): runs on the Mac.
#   cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ./build-host/host_tests
cmake_minimum_required(VERSION 3.16)
project(obd2_dash_host_tests C)

set(CMAKE_C_STANDARD 11)
set(MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

add_executable(host_tests
    test_main.c
    test_tpms.c
    ${MAIN}/elm/elm_reply.c
    ${MAIN}/elm/elm327.c
    ${MAIN}/obd/pids.c
    ${MAIN}/obd/obd_state.c
    ${MAIN}/obd/poller.c
    ${MAIN}/obd/dtc.c
    ${MAIN}/obd/alerts.c
    ${MAIN}/obd/board_guard.c
    ${MAIN}/obd/gear.c
    ${MAIN}/obd/trip.c
    ${MAIN}/obd/fuel.c
    ${MAIN}/obd/ride_summary.c
    ${MAIN}/demo/demo.c
    ${MAIN}/sensors/motion.c
    ${MAIN}/sensors/held_peak.c
    ${MAIN}/sensors/rtc_regs.c
    ${MAIN}/tpms/tpms_adv.c
    ${MAIN}/tpms/tpms_formats.c
    ${MAIN}/tpms/tpms_tyres.c
    ${MAIN}/tpms/tpms_alerts.c
    ${MAIN}/tpms/tpms_pair.c
    ${MAIN}/tpms/tpms_cfgfile.c
)
target_include_directories(host_tests PRIVATE ${MAIN})
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_compile_definitions(host_tests PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
    # The tests' numbers (gears, fuel, alerts) are the example bike's.
    BIKE_PROFILE_H="bike/profiles/honda_cb500x.h")
target_link_libraries(host_tests PRIVATE m)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
