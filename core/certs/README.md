# AirDrop TLS lab credentials

These files contain one public, self-signed diagnostic identity for the
explicit `CONFIG_ESPDROP_AIRDROP_TLS_LAB` build. The private key is intentionally
public and must never be used as a device identity, trust anchor, production
credential, or Contacts Only credential.

CMake embeds these files only when the TLS lab option is enabled. Normal,
OTA, and web-flasher firmware exclude both files and the TLS probe path.

The first hardware slice mirrors OpenDrop's minimum Everyone-mode behavior so
the TLS transport can be characterized independently. A production sender
must create and protect a per-device or per-session identity instead.
