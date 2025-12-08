# Rotary SIP Phone Firmware

Firmware skeleton for embedding an ESP32-ADF inside a rotary phone to operate as a SIP client with a minimalist configuration UI.

## Features
- Rotary dial pulses captured on GPIO and translated to digits.
- Simple HTTP configuration page (Wi-Fi and SIP credentials) optimized for low memory usage.
- NVS-backed persistent storage for configuration.
- FreeRTOS task to buffer dialed numbers and trigger SIP calls (stub for integration with ESP-ADF/PJSIP).

## Building
1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (the repo includes a known-good clone in `/workspace/esp-idf` if you followed the setup from this branch):
   ```bash
   git clone --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   ./install.sh esp32
   source export.sh
   ```
   Ensure `IDF_PATH` points to the cloned directory (for example, `/workspace/esp-idf`).
   
   > **Troubleshooting build errors**
   > * If `idf.py set-target` fails with `components/heap/tlsf/include is not a directory`, the ESP-IDF clone is incomplete. Run `git submodule update --init --recursive` inside your `esp-idf` clone, or reclone with `--recursive` to pull TLSF and other bundled components.
   > * Avoid pointing `IDF_PATH` at the `esp-adf/esp-idf` directory that ships inside ESP-ADF; it omits several submodules needed by this project. Use a full ESP-IDF clone (v5.x or v6.x) instead.
2. From this repository root, run:
   ```bash
   idf.py set-target esp32
   idf.py menuconfig   # optional, to adjust GPIO pins or Wi-Fi defaults
   idf.py build
   ```
3. Flash to the ESP32-ADF:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

If you plan to add custom drivers or SIP stacks, drop them into the `components/` directory referenced by `EXTRA_COMPONENT_DIRS` in the top-level `CMakeLists.txt`.

## Hardware Wiring (defaults in `main/main.c`)
- Rotary pulse output -> GPIO4 (falling-edge pulses counted).
- Hook switch -> GPIO5 (high when handset lifted).
- Adjust `PULSE_GPIO` and `HOOK_GPIO` in `main/main.c` if your wiring differs.

## Web Configuration
After the device joins Wi-Fi, browse to its IP. The form allows updating:
- Wi-Fi SSID/password
- SIP username/password
- SIP domain
- SIP extension (for display/dial plan)

Submitting the form stores values in NVS and reloads the page. Integrate the `sip_place_call` stub with your preferred SIP stack to place calls when dialing completes.
