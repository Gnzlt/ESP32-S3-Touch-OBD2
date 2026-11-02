file(GLOB UI_SRCS "ui/*.c" "ui/assets/*.c" "ui/assets/fonts/*.c")

idf_component_register(
    SRCS "app_main.c" "app_shared.c" "mem_report.c" "obd_task.c" "sensor_task.c" "sd_log.c" "board_power.c" "log_export.c" "tpms_service.c"
         "ble/obd_link.c" "ble/ble_scan.c"
         "elm/elm_reply.c" "elm/elm327.c"
         "obd/pids.c" "obd/obd_state.c" "obd/poller.c" "obd/dtc.c" "obd/alerts.c" "obd/board_guard.c" "obd/gear.c" "obd/trip.c" "obd/fuel.c" "obd/ride_summary.c"
         "sensors/battery.c" "sensors/beeper.c" "sensors/motion.c" "sensors/held_peak.c" "sensors/wallclock.c" "sensors/rtc_regs.c" "sensors/chip_temp.c"
         "tpms/tpms_adv.c" "tpms/tpms_formats.c" "tpms/tpms_tyres.c" "tpms/tpms_alerts.c" "tpms/tpms_pair.c" "tpms/tpms_cfgfile.c"
         "demo/demo.c"
         ${UI_SRCS}
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_tsens esp_timer fatfs mbedtls esp_app_format esp_driver_usb_serial_jtag esp32_s3_touch_lcd_1_85b
)
# The bike profile picked in menuconfig (main/bike/bike.h includes it).
set(BIKE_PROFILE_FILE "${COMPONENT_DIR}/bike/profiles/${CONFIG_DASH_BIKE_PROFILE}.h")
if(NOT EXISTS "${BIKE_PROFILE_FILE}")
    message(FATAL_ERROR "Bike profile '${CONFIG_DASH_BIKE_PROFILE}' not found: no ${BIKE_PROFILE_FILE}. "
                        "Set it in menuconfig (OBD2 dashboard -> Bike profile).")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE BIKE_PROFILE_H="bike/profiles/${CONFIG_DASH_BIKE_PROFILE}.h")
# The generated fonts include "lvgl.h" this way.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
# LVGL calls its allocator (ui/lv_mem_psram.c) but links after main; pull it in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
