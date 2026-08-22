# Application API

Applications include `espdrop/espdrop.h`. Protocol implementation details
stay behind this boundary. Discovery and send calls currently return
`ESP_ERR_NOT_SUPPORTED`; this is intentional fail-closed behavior until an
AWDL netif exists. Receive handlers may be registered now and will become
active with the receiver backend.
