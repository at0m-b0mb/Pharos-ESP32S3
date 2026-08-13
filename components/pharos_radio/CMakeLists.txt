idf_component_register(
    SRCS
        "pharos_radio.c"
        "pharos_region.c"
        "tx_fence.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_engine
    PRIV_REQUIRES esp_wifi esp_timer nvs_flash bt
)

# The transmit fence, mechanism 2 of 4 (see include/pharos_radio.h).
#
# Every Espressif/NimBLE transmit primitive is redirected to an abort trap in
# tx_fence.c. --wrap=sym makes the linker resolve calls to sym as __wrap_sym
# and expose the original as __real_sym. tx_fence.c defines every __wrap_ here
# and calls no __real_ except the mode setter, which it screens.
#
# Adding a symbol to this list requires adding it to tx_fence.c and to
# tools/check_tx_fence.sh as well - three edits, so the fence cannot be
# widened by forgetting a spot. CI runs check_tx_fence.sh.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_wifi_80211_tx"
    "-Wl,--wrap=esp_wifi_deauth_sta"
    "-Wl,--wrap=esp_now_send"
    "-Wl,--wrap=esp_now_init"
    "-Wl,--wrap=esp_wifi_set_mode"
)
