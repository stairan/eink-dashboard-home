# GEMINI.md

## Project Overview

This is an Arduino project for a home dashboard displayed on an e-ink screen. It is designed to fetch and display data from a Home Assistant instance, providing a low-power, always-on overview of smart home sensors, weather, and other information.

The dashboard is built for an ESP32-based board (specifically, a Firebeetle 2 ESP32-E) and a large 7.5-inch black and white e-ink display (GxEPD2_750_GDEY075T7).

### Key Features:

*   **Home Assistant Integration:** Connects to a local Home Assistant server to pull sensor data and forecasts.
*   **E-Ink Display:** Uses a GxEPD2 e-ink display for a crisp, low-power interface.
*   **Comprehensive Dashboard:** Displays a variety of information, including:
    *   Time and date
    *   Sunrise and sunset times
    *   Outdoor temperature and weather conditions
    *   Hourly weather forecast
    *   Room-specific temperature and humidity
    *   Solar power yield, power usage, and heating statistics
*   **Customizable:** The project uses custom fonts and icons, which can be modified or extended.

## Building and Running

### Prerequisites:

1.  **Arduino IDE or PlatformIO:** You will need an environment to build and upload the Arduino sketch.
2.  **ESP32 Board Support:** Ensure you have the ESP32 board support package installed in your Arduino IDE or PlatformIO.
3.  **Libraries:** Install the following libraries through the Arduino Library Manager or by adding them to your `platformio.ini`:
    *   `WiFi`
    *   `HTTPClient`
    *   `ArduinoJson`
    *   `GxEPD2`
    *   `Adafruit GFX Library`
    *   `U8g2` (as a dependency for GxEPD2)

### Configuration:

Before uploading the sketch, you must configure the following in `dashboard5.ino`:

1.  **WiFi Credentials:**
    ```cpp
    const char* WIFI_SSID = "your_wifi_ssid";
    const char* WIFI_PASSWORD = "your_wifi_password";
    ```

2.  **Home Assistant IP Address:**
    ```cpp
    const char* HA_IP = "your_home_assistant_ip";
    ```

### Uploading:

1.  Connect your Firebeetle 2 ESP32-E board to your computer.
2.  Select the correct board and port in your Arduino IDE or PlatformIO configuration.
3.  Upload the `dashboard5.ino` sketch.

## Development Conventions

*   **Code Structure:** The main application logic is in `dashboard5.ino`. All custom fonts and icon bitmaps are stored in `.h` header files.
*   **Icons:** Icons are stored as byte arrays in `icons.h`. To add new icons, you will need to convert them to the appropriate format and add them to this file.
*   **Fonts:** Custom fonts are included as `.h` files. These are generated from font files (like TTF) using a tool like `fontconvert`.
*   **Data Fetching:** All data is fetched from a custom Home Assistant endpoint (`/api/v1/dashboard`). This endpoint should be configured in your Home Assistant instance to provide the necessary sensor and forecast data in a single JSON response.
*   **Display Logic:** The display is updated in sections (`drawHeader`, `drawMiddleSection`, `drawRoomsSection`, `drawFooter`). This modular approach makes it easier to modify the layout.
