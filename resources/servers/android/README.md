# scrcpy-server 5.0.1

- Upstream: https://github.com/Genymobile/scrcpy
- Release: https://github.com/Genymobile/scrcpy/releases/tag/v5.0.1
- Unmodified binary: https://github.com/Genymobile/scrcpy/releases/download/v5.0.1/scrcpy-server-v5.0.1
- SHA-256: `764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536`
- License: Apache-2.0; see `LICENSE.txt` in this directory.

HarmonyCast implements the host-side protocol for this exact server version. Video is configured as H.264 with device/stream/frame metadata enabled and audio disabled. Video and control use separate connections to the same uniquely named abstract device socket. ADB uploads a temporary per-session JAR without installing an application; shutdown removes only that session's forwarding rule and JAR.
