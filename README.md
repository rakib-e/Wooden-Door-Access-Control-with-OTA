# Wooden-Door-Access-Control-with-OTA
Smart ESP32 wooden door access controller with RFID validation, WiFi connectivity, MQTT commands, OTA updates, and secure door-opening logic.
# Wooden Door Access Control with OTA

## Project Description
This project is an ESP32-based smart wooden door access control system designed for RFID-based entry and exit management. It reads RFID card numbers, validates access through a remote server, controls a door lock or motorized mechanism, and plays audio feedback using a buzzer or sound module. The system also supports over-the-air (OTA) firmware updates and MQTT-based remote commands.

The project is intended for secure physical access systems such as offices, hostels, rooms, or gated doors. It is built to work with ESP32 hardware and uses a local file system for storing configuration and card data, along with remote cloud synchronization.

## Main Features
- RFID-based access using RDM6300
- Smart wooden door opening and closing control
- Remote card verification and sync logic
- WiFi connectivity with WiFiManager fallback portal
- MQTT command handling for open/close/reset and other device commands
- OTA firmware update support
- Local LittleFS storage for card data and MQTT files
- Audio feedback and buzzer tones for access and denial events
- Automatic reconnect and retry logic for WiFi and server access
- Support for master card validation and server-managed card list updates

## Hardware Components
- ESP32 microcontroller
- RDM6300 RFID reader module
- Lock or motor driver circuit
- Buzzer / audio output
- LED indicators
- Door lock actuator or DC motor control hardware
- Power monitoring and signal control pins

## Pin Configuration
The firmware defines the main pins for the door system:

- RFID reader:
  - `RDM6300_RX_PIN = 4`
- LEDs:
  - `READ_LED_PIN = 18`
  - `GREN_LED_PIN = 19`
- Buzzer:
  - `BUZZER_PIN = 5`
- Door lock / motor:
  - `LOCK_PIN = 13`
  - `AIN1 = 25`
  - `AIN2 = 33`
  - `PWMA = 32`
  - `STBY = 26`
- Status LED:
  - `ST_LED = 2`

## Software Libraries Used
The project uses the following Arduino libraries:
- `WiFi.h`
- `WiFiManager.h`
- `HTTPClient.h`
- `LittleFS.h`
- `ArduinoJson.h`
- `rdm6300.h`
- `esp_task_wdt.h`
- `esp_wifi.h`
- `WiFiClientSecure.h`
- `Update.h`
- `PubSubClient.h`

## Core System Behavior
1. The ESP32 boots and initializes the RFID reader, WiFi, filesystem, and MQTT client.
2. It tries to connect to WiFi using saved settings or starts a WiFiManager setup portal if required.
3. The device reads RFID tags and validates them against a configured card list or remote server response.
4. If the card is authorized, the door unlocks or opens.
5. If the card is unauthorized or the network fails, the system triggers a denial tone and logs the event.
6. The device periodically checks for scheduled sync and OTA update tasks.
7. MQTT commands can trigger open/close actions or device resets remotely.

## Access Control Logic
The project supports:
- authorized card validation using server responses
- master card validation through a static list
- door open/close request handling
- deny tone and event logging for unauthorized access
- re-sync after failed card verification

## MQTT and OTA Features
The device subscribes to MQTT topics and publishes status updates such as:
- device status topic
- device event topic
- filesystem status topic
- global OTA update command topic

The OTA update system checks a remote API to determine whether a newer firmware version is available and downloads it to the ESP32 if allowed.

## Local Storage
The project stores data in the ESP32 filesystem using LittleFS:
- `/p.txt`
- `/mqtt.txt`

These files are used to store card data and device-related information locally.

## Security Note
This documentation intentionally does not include secret values, private API keys, MQTT credentials, or server URLs. Those must be configured securely before deployment in your own environment.

## Important Notes
- Replace all placeholder values before deployment.
- `api_secret` and server access values should never be committed to a public GitHub repository.
- The code uses `WiFiManager` for setup and fallback configuration.
- `WiFiClientSecure` is enabled for OTA and HTTPS, but production installations should use proper certificate validation and secure deployment practices.
- Physical access devices should be tested thoroughly before use in live environments.

## Upload Instructions
1. Open `Wooden_door_newerp_OTA.ino` in the Arduino IDE.
2. Install the required libraries.
3. Select the correct ESP32 board and COM port.
4. Fill in your own WiFi, API, MQTT, and OTA configuration values.
5. Compile and upload the firmware.
6. Open the serial monitor to observe RFID, WiFi, MQTT, and OTA status.

## File Structure
- `Wooden_door_newerp_OTA.ino` — main firmware
- `README.md` — project documentation

## Example Use Cases
- Smart wooden door lock system
- Office room access control
- Hostel room entry system
- Secure gate or door access automation

## Disclaimer
This project is intended for embedded access control and security automation experiments. It should be configured carefully and tested in the actual deployment environment before use in production.
