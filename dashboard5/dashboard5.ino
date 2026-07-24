#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h> // For black and white displays
#include "icons.h" // Include all icon bitmaps (room icons and weather icons)
#include <time.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include "secrets.h" // WiFi credentials and Home Assistant IP - see secrets.h.example for the template

// Custom fonts - uncomment when files are added to the project
#include "Roboto_Bold40pt7b.h"      // XXLarge font for time
#include "Roboto_Bold20pt7b.h"      // XLarge font for outdoor temp
#include "Roboto_Regular14pt7b.h"   // Medium font for values
#include "Roboto_Regular12pt7b.h"   // Small font for labels
#include "Roboto_Bold14pt7b.h"

// Font definitions - uncomment when font files are added
const GFXfont* font_xxlarge = &Roboto_Bold40pt7b;     // 60pt - Time display
const GFXfont* font_xlarge = &Roboto_Bold20pt7b;      // 40pt - Outdoor temperature
//const GFXfont* font_medium = &Roboto_Regular14pt7b;   // 20pt - Room temps, values
const GFXfont* font_medium = &Roboto_Bold14pt7b;   // 20pt - Room temps, values
const GFXfont* font_small = &Roboto_Regular12pt7b;    // 12pt - Labels, date, times

// When fonts are NULL, the display will use default font with setTextSize()
//const GFXfont* font_xxlarge = nullptr;
//const GFXfont* font_xlarge = nullptr;
//const GFXfont* font_medium = nullptr;
//const GFXfont* font_small = nullptr;

#define BAT_ADC_PIN 34   // GPIO34 (A2)
#define ADC_MAX 4095     // 12-bit ADC
#define ADC_REF_VOLTAGE 1100  // mV (internal reference voltage, typically 1100mV)

// NTP Server settings
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600;  // GMT+1 for Budapest (winter time)
const int DAYLIGHT_OFFSET_SEC = 3600;  // +1 hour for daylight saving time
// The timezone string automatically handles DST transitions
const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Europe/Budapest timezone with automatic DST

//Firebeetle 2 ESP32-E (CS, DC, RST, BUSY)
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> display(GxEPD2_750_GDEY075T7(13, 22, 21, 14));

// Sensor entity IDs we are interested in
const char* SENSORS[] = {
  "sensor.balcony_humidity",
  "sensor.balcony_pressure",
  "sensor.balcony_temperature",
  "sensor.bathroom_big_humidity",
  "sensor.bathroom_big_temperature",
  "sensor.bathroom_small_humidity",
  "sensor.bathroom_small_temperature",
  "sensor.bedroom_humidity",
  "sensor.bedroom_temperature",
  "sensor.entrance_humidity",
  "sensor.entrance_temperature",
  "sensor.living_room_humidity",
  "sensor.living_room_temperature",
  "sensor.second_floor_humidity",
  "sensor.second_floor_temperature",
  "sensor.working_room_humidity",
  "sensor.working_room_temperature",
  "sensor.pantry_temperature",
  "sensor.pantry_humidity",
  "sensor.sun_next_setting",
  "sensor.sun_next_rising",
  "sensor.pilisszentivan_condition",
  "sensor.pilisszentivan_temperature",
  "sensor.inverter_daily_yield"
};

const int NUM_SENSORS = sizeof(SENSORS) / sizeof(SENSORS[0]);
const int FORECAST_HOURS = 10;

// Structure to hold sensor data
struct SensorData {
  String name;
  String value;
  String unit; // e.g., "°C", "%", "hPa", "kWh"
};

// Structure to hold forecast data
struct ForecastData {
    String time;
    String condition;
    float temperature;
};

SensorData sensorValues[NUM_SENSORS]; // Global array to store fetched sensor data
ForecastData hourlyForecast[FORECAST_HOURS]; // Global array to store fetched forecast data

// Daily power consumption
float consumption = 0;
int totalHeatingMinutes = 0;

int batteryPercent = 0;

// Function to connect to Wi-Fi
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi.");
  }
}

int getBatteryV2() {
  int adcValue = analogRead(BAT_ADC_PIN);
  int mv = analogReadMilliVolts(BAT_ADC_PIN); // if supported by your ESP32 core

  // FireBeetle 2 ESP32-E has a 1/2 voltage divider, so multiply by 2
  float batteryVoltage = mv * 2 / 1000.0; // in Volts

  Serial.print("ADC Value: ");
  Serial.println(adcValue);
  Serial.print("Measured Voltage: ");
  Serial.print(batteryVoltage);
  Serial.println(" V");

  // Compute battery percentage
  float batteryPercent = getBatteryPercentage(batteryVoltage);
  Serial.print("Battery: ");
  Serial.print(batteryPercent, 1);
  Serial.println("%");

  Serial.println("-----------------");

  return (int) batteryPercent;
}

float getBatteryPercentage(float voltage) {
  if (voltage >= 4.2) return 100.0;
  if (voltage <= 3.0) return 0.0;
  // Linear approximation (you can refine this with a lookup table)
  return (voltage - 3.0) / (4.2 - 3.0) * 100.0;
}

void fetchAllSensorStates() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot fetch sensor data.");
    return;
  }

  HTTPClient http;
  String url = "http://" + String(HA_IP) + ":8100/api/v1/dashboard";
  Serial.print("Fetching all states from: ");
  Serial.println(url);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.printf("HTTP Code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient* stream = http.getStreamPtr();
      if (!stream) {
        Serial.println("Error: Could not get HTTP stream pointer.");
        http.end();
        return;
      }

      const size_t capacity = 50 * 1024;
      DynamicJsonDocument doc(capacity);

      Serial.println("Deserializing JSON from stream...");
      DeserializationError error = deserializeJson(doc, *stream);

      if (error) {
        Serial.print(F("deserializeJson() failed for full states: "));
        Serial.println(error.f_str());
        http.end();
        return;
      }

      JsonObject root = doc.as<JsonObject>();

      // Process sensor_readings
      JsonArray sensorReadings = root["sensor_readings"];
      for (int i = 0; i < NUM_SENSORS; i++) {
        for (JsonObject sensor : sensorReadings) {
          if (String(SENSORS[i]) == sensor["entity_id"].as<String>()) {
            sensorValues[i].name = SENSORS[i];
            sensorValues[i].value = sensor["state"].as<String>();
            break;
          }
        }
      }

      // Process weather_forecast
      JsonArray forecastData = root["weather_forecast"]["forecast_data"];
      int count = 0;
      for (JsonObject forecast : forecastData) {
        if (count >= FORECAST_HOURS) break;
        String dateTime = forecast["datetime"].as<String>();
        hourlyForecast[count].time = dateTime.substring(dateTime.indexOf('T') + 1, dateTime.indexOf('T') + 6);
        hourlyForecast[count].condition = forecast["condition"].as<String>();
        hourlyForecast[count].temperature = forecast["temperature"].as<float>();
        count++;
      }

      // Process daily_power_usage
      consumption = root["daily_power_usage"]["daily_usage"].as<float>();

      // Process daily_thermostat_stats
      totalHeatingMinutes = root["daily_thermostat_stats"]["total_heating_minutes"].as<int>();

      // You can add print statements here to verify the data
      Serial.println("Successfully parsed all data.");

    }
  } else {
    Serial.printf("HTTP GET failed for all states, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Helper function to get sensor value by entity_id
String getSensorValue(const char* entityId) {
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (sensorValues[i].name == entityId) {
      return sensorValues[i].value;
    }
  }
  return "N/A";
}

// Helper function to parse ISO datetime to HH:MM
String parseTimeToHHMM(String isoTime) {
  // isoTime format: "2025-01-27T16:30:00+00:00"
  int tIndex = isoTime.indexOf('T');
  if (tIndex > 0) {
    return isoTime.substring(tIndex + 1, tIndex + 6); // Extract HH:MM
  }
  return "N/A";
}

String convertISOToLocalHHMM(String isoTime) {
  if (isoTime == "N/A") {
    return "N/A";
  }

  struct tm tm;
  // Example: 2025-01-27T16:30:00+00:00
  if (sscanf(isoTime.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    
    // Temporarily set the timezone to UTC to use mktime as mkgmtime
    setenv("TZ", "UTC", 1);
    tzset();

    time_t utc_time = mktime(&tm);

    // Restore the original timezone
    setenv("TZ", TZ_INFO, 1);
    tzset();

    struct tm* local_tm = localtime(&utc_time);
    char timeStr[6];
    strftime(timeStr, sizeof(timeStr), "%H:%M", local_tm);
    return String(timeStr);
  }
  return "N/A";
}

// Helper function to get room temperature
String getRoomTemp(const char* room) {
  String entityId = "sensor." + String(room) + "_temperature";
  return getSensorValue(entityId.c_str());
}

// Helper function to get room humidity
String getRoomHumidity(const char* room) {
  String entityId = "sensor." + String(room) + "_humidity";
  return getSensorValue(entityId.c_str());
}

// Initialize and sync time with NTP server
void syncTimeWithNTP() {
  Serial.println("Syncing time with NTP server...");

  // Configure time with timezone string (handles DST automatically)
  configTzTime(TZ_INFO, NTP_SERVER);

  // Wait for time to be set
  int retries = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo) && retries < 10) {
    Serial.print(".");
    delay(500);
    retries++;
  }

  if (retries < 10) {
    Serial.println("\nTime synchronized successfully!");
    Serial.print("Current Budapest time: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
  } else {
    Serial.println("\nFailed to sync time with NTP server");
  }
}

// Get current time in HH:MM format (Budapest timezone)
String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00:00";
  }

  char timeStr[6];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  return String(timeStr);
}

// Get current date formatted as "Weekday, YYYY.MM.DD" (Budapest timezone)
String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Unknown";
  }

  // Day names in English
  const char* dayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

  char dateStr[30];
  sprintf(dateStr, "%s, %04d.%02d.%02d",
          dayNames[timeinfo.tm_wday],
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday);

  return String(dateStr);
}

// Helper function to get weather icon based on condition string
const unsigned char* getWeatherIcon(String condition) {
  // Convert condition to lowercase for comparison
  condition.toLowerCase();

  // Map weather conditions to icons
  if (condition == "clear-night") {
    return epd_bitmap_clear_night;
  } else if (condition == "cloudy") {
    return epd_bitmap_cloudy;
  } else if (condition == "fog") {
    return epd_bitmap_fog;
  } else if (condition == "lightning-rainy" || condition == "lightning_rainy") {
    return epd_bitmap_lightning_rainy;
  } else if (condition == "lightning") {
    return epd_bitmap_lightning;
  } else if (condition == "pouring") {
    return epd_bitmap_pouring;
  } else if (condition == "snowy-rainy" || condition == "snowy_rainy") {
    return epd_bitmap_snowy_rainy;
  } else if (condition == "sunny" || condition == "clear-day") {
    return epd_bitmap_sunny;
  } else if (condition == "partlycloudy" || condition == "partly-cloudy" || condition == "partly_cloudy") {
    return epd_bitmap_partlycloudy;
  } else if (condition == "hail") {
    return epd_bitmap_hail;
  } else if (condition == "snow" || condition == "snowy") {
    return epd_bitmap_snow;
  } else if (condition == "rainy" || condition == "rain") {
    return epd_bitmap_rain;
  } else if (condition == "windy-variant" || condition == "windy_variant") {
    return epd_bitmap_windy_variant;
  } else if (condition == "windy") {
    return epd_bitmap_windy;
  }

  // Default to cloudy if condition not recognized
  return epd_bitmap_cloudy;
}

// Helper function to get weather icon based on condition string
const unsigned char* getWeatherForecastIcon(String condition) {
  // Convert condition to lowercase for comparison
  condition.toLowerCase();

  // Map weather conditions to icons
  if (condition == "clear-night") {
    return epd_bitmap_45_clear_night;
  } else if (condition == "cloudy") {
    return epd_bitmap_45_cloudy;
  } else if (condition == "fog") {
    return epd_bitmap_45_fog;
  } else if (condition == "lightning-rainy" || condition == "lightning_rainy") {
    return epd_bitmap_45_lightning_rainy;
  } else if (condition == "lightning") {
    return epd_bitmap_45_lightning;
  } else if (condition == "pouring") {
    return epd_bitmap_45_pouring;
  } else if (condition == "snowy-rainy" || condition == "snowy_rainy") {
    return epd_bitmap_45_snowy_rainy;
  } else if (condition == "sunny" || condition == "clear-day") {
    return epd_bitmap_45_sunny;
  } else if (condition == "partlycloudy" || condition == "partly-cloudy" || condition == "partly_cloudy") {
    return epd_bitmap_45_partlycloudy;
  } else if (condition == "hail") {
    return epd_bitmap_45_hail;
  } else if (condition == "snow" || condition == "snowy") {
    return epd_bitmap_45_snow;
  } else if (condition == "rainy" || condition == "rain") {
    return epd_bitmap_45_rain;
  } else if (condition == "windy-variant" || condition == "windy_variant") {
    return epd_bitmap_45_windy_variant;
  } else if (condition == "windy") {
    return epd_bitmap_45_windy;
  }

  // Default to cloudy if condition not recognized
  return epd_bitmap_45_cloudy;
}

// Helper function to set font - uses custom font if available, otherwise uses default
void setFont(const GFXfont* font, int defaultSize = 1) {
  if (font != nullptr) {
    display.setFont(font);
  } else {
    display.setFont();  // Reset to default font
    display.setTextSize(defaultSize);
  }
}

void drawHeader() {
  // Layout constants
  const int HEADER_HEIGHT = 100;

  // Get data
  String sunset = convertISOToLocalHHMM(getSensorValue("sensor.sun_next_setting"));
  String sunrise = convertISOToLocalHHMM(getSensorValue("sensor.sun_next_rising"));
  String outdoorTemp = getSensorValue("sensor.pilisszentivan_temperature");
  String weatherCondition = getSensorValue("sensor.pilisszentivan_condition");

  // Get current time and date from NTP
  String currentTime = getCurrentTime();
  String currentDate = getCurrentDate();

  // Left side: Sunset and Sunrise icons + times (x=10, y=10)
  // Draw sunset icon at (10, 10) size 40x40
  display.drawBitmap(10, 10, epd_bitmap_sunset, 40, 40, GxEPD_BLACK);
  display.setFont(font_medium);
  display.setCursor(60, 35);
  display.print(sunset);

  // Draw sunrise icon at (10, 55) size 40x40
  display.drawBitmap(10, 55, epd_bitmap_sunrise, 40, 40, GxEPD_BLACK);
  display.setFont(font_medium);
  display.setCursor(60, 80);
  display.print(sunrise);

  // Middle: Time and Date (centered around x=300)
  display.setFont(font_xxlarge);  // XXLarge (60pt) for time
  display.setCursor(300, 70);  // Adjusted Y position for custom font baseline
  display.print(currentTime);

  display.setFont(font_small);  // Small (16pt) for date
  display.setCursor(300, 90);  // Adjusted Y position
  display.print(currentDate);

  // Right side: Weather icon and outdoor temperature
  // Draw weather icon at (570, 10) size 90x90 (actual icon size)
  const unsigned char* weatherIcon = getWeatherIcon(weatherCondition);
  display.drawBitmap(570, 10, weatherIcon, 90, 90, GxEPD_BLACK);

  setFont(font_xlarge, 3);  // XLarge (40pt) for temperature
  display.setCursor(680, 50);  // Adjusted Y position for custom font baseline
  display.print(outdoorTemp);
  display.print(" C");

  setFont(font_small, 1);  // Small for "Outside" label
  display.setCursor(670, 80);
  display.print("Outside");

  // --- Battery Percentage (Top-Right corner) ---
  char battStr[10];
  sprintf(battStr, "%d%%", batteryPercent);

  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(745, 13);
  display.print(battStr);
    
  // Switch back to black text for the rest of the display
  display.setTextColor(GxEPD_BLACK);

  // Draw separator line
  display.drawLine(0, HEADER_HEIGHT, 800, HEADER_HEIGHT, GxEPD_BLACK);
}

void drawMiddleSection() {
  // Layout constants
  const int Y_START = 100;
  const int MIDDLE_HEIGHT = 90;
  const int FORECAST_X = 10;
  const int FORECAST_Y = Y_START + 5;
  const int ITEM_WIDTH = 98; // 800px / 8 items ≈ 100px per item

  // Draw 8-hour forecast
  for (int i = 0; i < 8 && i < FORECAST_HOURS; i++) {
    int itemX = FORECAST_X + (i * ITEM_WIDTH);

    // Draw hour
    setFont(font_small);  // Small (16pt) for time labels
    display.setCursor(itemX + 10, FORECAST_Y + 15);
    display.print(hourlyForecast[i].time);

    // Draw weather icon at (itemX + 20, FORECAST_Y + 20) size 64x64
    const unsigned char* forecastIcon = getWeatherForecastIcon(hourlyForecast[i].condition);
    // Center the 45x45 icon in the 98px wide space
    display.drawBitmap(itemX + 17, FORECAST_Y + 15, forecastIcon, 45, 45, GxEPD_BLACK);

    // Draw temperature
    setFont(font_medium);  // Small (16pt) for temperature
    display.setCursor(itemX + 15, FORECAST_Y + 75);
    display.print(hourlyForecast[i].temperature, 0);
    display.print(" C");
  }

  // Draw separator line
  display.drawLine(0, Y_START + MIDDLE_HEIGHT, 800, Y_START + MIDDLE_HEIGHT, GxEPD_BLACK);
}

void drawRoomsSection() {
  // Layout constants
  const int Y_START = 190; // 100 + 90
  const int BOX_WIDTH = 260;
  const int BOX_HEIGHT = 65;
  const int SPACING_X = 5;
  const int SPACING_Y = 5;

  // Room names matching the sensor entity IDs
  const char* rooms[] = {
    "balcony",
    "bathroom_big",
    "bathroom_small",
    "bedroom",
    "entrance",
    "living_room",
    "pantry",
    "second_floor",
    "working_room"
  };

  const char* roomDisplayNames[] = {
    "Balcony",
    "Bathroom Big",
    "Bathroom Small",
    "Bedroom",
    "Entrance",
    "Living Room",
    "Pantry",
    "Second Floor",
    "Working Room"
  };

  // Room icon bitmaps (40x40)
  const unsigned char* roomIcons[] = {
    epd_bitmap_balcony,
    epd_bitmap_bathroom_big,
    epd_bitmap_bathroom_small,
    epd_bitmap_bedroom,
    epd_bitmap_entrance,
    epd_bitmap_living_room,
    epd_bitmap_pantry,
    epd_bitmap_second_floor,
    epd_bitmap_working_room
  };

  const int NUM_ROOMS = 9;

  // Draw 3x3 grid
  for (int i = 0; i < NUM_ROOMS; i++) {
    int col = i % 3;
    int row = i / 3;

    // Calculate position
    int x = col * (BOX_WIDTH + SPACING_X) + 5;
    int y = Y_START + row * (BOX_HEIGHT + SPACING_Y) + 5;

    // Get room data
    String temp = getRoomTemp(rooms[i]);
    String humidity = getRoomHumidity(rooms[i]);
    int humidity_int = humidity.toFloat();

    // Draw box border
    display.drawRect(x, y, BOX_WIDTH, BOX_HEIGHT, GxEPD_BLACK);

    // Draw room icon at (x + 5, y + 15) size 40x40
    display.drawBitmap(x + 5, y + 15, roomIcons[i], 40, 40, GxEPD_BLACK);

    // Draw room name
    setFont(font_small, 1);  // Small (16pt) for room names
    display.setCursor(x + 60, y + 25);  // Adjusted Y for custom font baseline
    display.print(roomDisplayNames[i]);

    // Draw temperature
    setFont(font_medium, 2);  // Medium (20pt) for temperature values
    display.setCursor(x + 60, y + 50);  // Adjusted Y for custom font baseline
    display.print(temp);
    display.print(" C");

    // Draw humidity
    setFont(font_medium, 2);  // Medium (20pt) for humidity values
    display.setCursor(x + 185, y + 50);  // Adjusted Y for custom font baseline
    display.print(humidity_int);
    display.print("%");
  }

  // Draw separator line
  const int Y_BOTTOM = Y_START + 215;
  display.drawLine(0, Y_BOTTOM, 800, Y_BOTTOM, GxEPD_BLACK);
}

void drawFooter() {
  // Layout constants
  const int Y_START = 410; // 480 - 70
  const int SECTION_WIDTH = 800 / 3; // ~266px per section
  const int ICON_SIZE = 50; // Icons are 40x40
  const int ICON_Y = Y_START + 15; // Adjust Y position for 40px icons

  // Section 1: Solar Yield
  // Draw solar icon at (10, ICON_Y) size 40x40
  display.drawBitmap(10, ICON_Y, epd_bitmap_solar_panel, ICON_SIZE, ICON_SIZE, GxEPD_BLACK);

  String solarYield = getSensorValue("sensor.inverter_daily_yield");
  setFont(font_small, 1);  // Small (16pt) for label
  display.setCursor(65, Y_START + 28);  // Adjusted Y for custom font baseline
  display.print("Solar");
  setFont(font_medium, 2);  // Medium (20pt) for value
  display.setCursor(65, Y_START + 52);  // Adjusted Y for custom font baseline
  display.print(solarYield);
  display.print(" kWh");

  // Section 2: Power Usage
  // Draw power usage icon at (SECTION_WIDTH + 10, ICON_Y) size 40x40
  display.drawBitmap(SECTION_WIDTH + 10, ICON_Y, epd_bitmap_power_usage, ICON_SIZE, ICON_SIZE, GxEPD_BLACK);

  setFont(font_small, 1);  // Small (16pt) for label
  display.setCursor(SECTION_WIDTH + 65, Y_START + 28);
  display.print("Usage");
  setFont(font_medium, 2);  // Medium (20pt) for value
  display.setCursor(SECTION_WIDTH + 65, Y_START + 52);
  display.print(consumption, 1);
  display.print(" kWh");

  // Section 3: Heating Minutes
  // Draw heating icon at (2 * SECTION_WIDTH + 10, ICON_Y) size 40x40
  display.drawBitmap(2 * SECTION_WIDTH + 10, ICON_Y, epd_bitmap_heating, ICON_SIZE, ICON_SIZE, GxEPD_BLACK);

  int hours = totalHeatingMinutes / 60;
  int minutes = totalHeatingMinutes % 60;

  setFont(font_small, 1);  // Small (16pt) for label
  display.setCursor(2 * SECTION_WIDTH + 65, Y_START + 28);
  display.print("Heating");
  setFont(font_medium, 2);  // Medium (20pt) for value
  display.setCursor(2 * SECTION_WIDTH + 65, Y_START + 52);
  display.print(hours);
  display.print("h ");
  display.print(minutes);
  display.print("m");
}

void displaySensorData() {
  display.init(115200); // Initialize display with a baud rate
  display.setRotation(0); // Set to horizontal landscape (0 or 2 for landscape)
  display.setFullWindow(); // Set full window for full screen updates

  // Clear display
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK); // Set text color to black

    // Draw all sections
    drawHeader();
    drawMiddleSection();
    drawRoomsSection();
    drawFooter();

  } while (display.nextPage());

  Serial.println("Display updated successfully");
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 E-Ink Home Assistant Sensor Display");

  analogReadResolution(12);


  // Check if we woke up from deep sleep
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from deep sleep (timer).");
  } else {
    Serial.println("First boot or external reset.");
  }
}

void loop() {
  Serial.println("--- Starting new cycle ---");
  connectToWiFi(); // Connect to WiFi

  // Sync time with NTP server (only if WiFi is connected)
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeWithNTP();
  }

  fetchAllSensorStates();

  batteryPercent = getBatteryV2();
  //batteryPercent = batteryPercentage(voltage);
  Serial.printf("Battery percentage: %d\n", batteryPercent);

  displaySensorData();

  // Put the e-paper controller to sleep so it doesn't keep drawing power
  // (and the panel doesn't fade) while the ESP32 is in deep sleep.
  display.hibernate();

  // Check the current hour for night mode
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int current_hour = timeinfo.tm_hour;
    if (current_hour >= 1 && current_hour < 5) {
      // Night mode: update every 5 minutes
      Serial.println("Entering deep sleep for 300 seconds (night mode)...");
      esp_sleep_enable_timer_wakeup(300 * 1000000); // 300 seconds in microseconds
    } else {
      // Day mode: update every 60 seconds
      Serial.println("Entering deep sleep for 60 seconds...");
      esp_sleep_enable_timer_wakeup(60 * 1000000); // 60 seconds in microseconds
    }
  } else {
    // Fallback to 60 seconds if time is not available
    Serial.println("Time not available, entering deep sleep for 60 seconds...");
    esp_sleep_enable_timer_wakeup(60 * 1000000); // 60 seconds in microseconds
  }
  
  esp_deep_sleep_start();
}
