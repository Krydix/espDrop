idf_component_register(
    SRCS
        "src/espdrop.c"
        "src/awdl_frame.c"
        "src/awdl_tlv.c"
        "src/awdl_tx.c"
        "src/awdl_tx_lab.c"
        "src/awdl_probe.c"
        "src/peer_table.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_wifi esp_netif esp_event
)
