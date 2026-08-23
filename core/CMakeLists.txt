idf_component_register(
    SRCS
        "src/espdrop.c"
        "src/airdrop_ask.c"
        "src/airdrop_http.c"
        "src/airdrop_upload.c"
        "src/airdrop_tls.c"
        "src/awdl_frame.c"
        "src/awdl_data.c"
        "src/awdl_netif.c"
        "src/airdrop_mdns.c"
        "src/awdl_tlv.c"
        "src/awdl_service.c"
        "src/awdl_election.c"
        "src/awdl_tx.c"
        "src/awdl_tx_lab.c"
        "src/awdl_probe.c"
        "src/peer_table.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_wifi esp_netif esp_event lwip mbedtls
)

if(CONFIG_ESPDROP_AIRDROP_TLS_LAB)
    target_add_binary_data(
        ${COMPONENT_LIB} "certs/airdrop_lab_certificate.pem" TEXT)
    target_add_binary_data(
        ${COMPONENT_LIB} "certs/airdrop_lab_private_key.pem" TEXT)
endif()
