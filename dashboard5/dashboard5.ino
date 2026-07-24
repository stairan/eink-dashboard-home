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
#define BAT_ADC_SAMPLES 16  // number of samples averaged per reading, to smooth out ADC noise

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

// --- Sleep / refresh cadence configuration ---
// The ESP32 wakes every CLOCK_TICK_SECONDS to update just the clock via a
// fast partial refresh (light sleep, so the display's partial-update state
// survives between ticks). Every DAY_FULL_REFRESH_TICKS (day) or
// NIGHT_FULL_REFRESH_TICKS (night) ticks, it instead reconnects WiFi, pulls
// fresh sensor/weather data, and does a full-window refresh.
#define CLOCK_TICK_SECONDS 60
#define DAY_FULL_REFRESH_TICKS 5     // 5 * 60s = 5 minutes
#define NIGHT_FULL_REFRESH_TICKS 10  // 10 * 60s = 10 minutes
#define NIGHT_START_HOUR 1
#define NIGHT_END_HOUR 5

// Bounding box of the clock (time + date) within the header, used for
// partial-window refreshes so the rest of the dashboard stays untouched.
const int16_t CLOCK_X = 280;
const int16_t CLOCK_Y = 0;
const int16_t CLOCK_W = 296;
const int16_t CLOCK_H = 100;

int ticksSinceFullRefresh = 999; // force a full refresh on first boot

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

// Single-cell LiPo discharge curve (voltage -> percentage). LiPo voltage sags
// non-linearly - it stays fairly flat through most of the usable capacity and
// then drops off sharply near empty, so a straight 3.0-4.2V line under- or
// over-reports charge depending on where on the curve the battery sits.
// Values between points are linearly interpolated.
struct BatteryCurvePoint { float voltage; float percent; };
const BatteryCurvePoint BATTERY_CURVE[] = {
  {4.20, 100.0}, {4.15, 95.0}, {4.11, 90.0}, {4.08, 85.0}, {4.02, 80.0},
  {3.98, 75.0},  {3.95, 70.0}, {3.91, 65.0}, {3.87, 60.0}, {3.85, 55.0},
  {3.84, 50.0},  {3.82, 45.0}, {3.80, 40.0}, {3.79, 35.0}, {3.77, 30.0},
  {3.75, 25.0},  {3.73, 20.0}, {3.71, 15.0}, {3.69, 10.0}, {3.61, 5.0},
  {3.27, 0.0}
};
const int BATTERY_CURVE_POINTS = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);

float getBatteryPercentage(float voltage) {
  if (voltage >= BATTERY_CURVE[0].voltage) return 100.0;
  if (voltage <= BATTERY_CURVE[BATTERY_CURVE_POINTS - 1].voltage) return 0.0;

  for (int i = 0; i < BATTERY_CURVE_POINTS - 1; i++) {
    float vHigh = BATTERY_CURVE[i].voltage;
    float vLow = BATTERY_CURVE[i + 1].voltage;
    if (voltage <= vHigh && voltage >= vLow) {
      float pHigh = BATTERY_CURVE[i].percent;
      float pLow = BATTERY_CURVE[i + 1].percent;
      return pLow + (voltage - vLow) / (vHigh - vLow) * (pHigh - pLow);
    }
  }
  return 0.0; // unreachable
}

// Reads the battery voltage - averaged over several samples to smooth out
// ADC noise - and converts it to a percentage using the LiPo curve above.
// Call this before any WiFi activity: WiFi's current draw can sag the
// reading if the battery/regulator has any real internal resistance.
int readBatteryPercent() {
  long mvSum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    mvSum += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  int mv = mvSum / BAT_ADC_SAMPLES;

  // FireBeetle 2 ESP32-E has a 1/2 voltage divider, so multiply by 2
  float batteryVoltage = mv * 2 / 1000.0; // in Volts
  float percent = getBatteryPercentage(batteryVoltage);

  Serial.print("Measured Voltage: ");
  Serial.print(batteryVoltage, 2);
  Serial.println(" V");
  Serial.print("Battery: ");
  Serial.print(percent, 1);
  Serial.println("%");
  Serial.println("-----------------");

  return (int) round(percent);
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

// True during the reduced-frequency overnight window. Defaults to false
// (day cadence) if NTP time isn't known yet, e.g. on first boot.
bool inNightWindow() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return false;
  }
  int hour = timeinfo.tm_hour;
  return (hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR);
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

// Draws the current time + date in the header's middle section. Shared by
// the full dashboard draw and the clock-only partial refresh tick, so both
// paths always draw the clock at the exact same position.
void drawClock() {
  display.setFont(font_xxlarge);  // XXLarge (60pt) for time
  display.setCursor(300, 70);  // Adjusted Y position for custom font baseline
  display.print(getCurrentTime());

  display.setFont(font_small);  // Small (16pt) for date
  display.setCursor(300, 90);  // Adjusted Y position
  display.print(getCurrentDate());
}

void drawHeader() {
  // Layout constants
  const int HEADER_HEIGHT = 100;

  // Get data
  String sunset = convertISOToLocalHHMM(getSensorValue("sensor.sun_next_setting"));
  String sunrise = convertISOToLocalHHMM(getSensorValue("sensor.sun_next_rising"));
  String outdoorTemp = getSensorValue("sensor.pilisszentivan_temperature");
  String weatherCondition = getSensorValue("sensor.pilisszentivan_condition");

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
  drawClock();

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

void drawFullDashboard() {
  display.setFullWindow(); // Set full window for full screen updates

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
  display.powerOff(); // drop driving voltage between updates, don't fade the panel

  Serial.println("Full dashboard refresh complete");
}

// Fast partial refresh of just the clock area - used for the once-a-minute
// tick between full dashboard refreshes, so bumping the displayed minute
// doesn't need WiFi or a full, more visible screen flash.
void drawClockPartial() {
  display.setPartialWindow(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H);

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    drawClock();
  } while (display.nextPage());
  display.powerOff(); // drop driving voltage between updates, don't fade the panel

  Serial.println("Clock partial refresh complete");
}

// Reconnects WiFi, syncs time, pulls fresh sensor/weather data, and does a
// full dashboard redraw. This is the expensive path - only run periodically
// (see ticksSinceFullRefresh in loop()), not on every clock tick.
void doFullRefreshCycle() {
  // Measure battery before any WiFi activity, so the reading isn't skewed
  // by voltage sag from the radio's current draw.
  batteryPercent = readBatteryPercent();
  Serial.printf("Battery percentage: %d\n", batteryPercent);

  connectToWiFi();

  // Sync time with NTP server (only if WiFi is connected)
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeWithNTP();
  }

  fetchAllSensorStates();

  drawFullDashboard();

  // Done with WiFi until the next full-refresh cycle - no need to keep the
  // radio on through the clock-only ticks in between.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 E-Ink Home Assistant Sensor Display");

  analogReadResolution(12);

  // The display is only initialized once here - unlike deep sleep, light
  // sleep (used in loop() below) keeps RAM intact, so there's no need to
  // (and no benefit to) re-init the panel on every tick.
  display.init(115200);
  display.setRotation(0); // Set to horizontal landscape (0 or 2 for landscape)
}

void loop() {
  bool isNight = inNightWindow();
  int fullRefreshInterval = isNight ? NIGHT_FULL_REFRESH_TICKS : DAY_FULL_REFRESH_TICKS;

  if (ticksSinceFullRefresh >= fullRefreshInterval) {
    Serial.println("--- Full refresh cycle ---");
    doFullRefreshCycle();
    ticksSinceFullRefresh = 0;
  } else {
    Serial.println("--- Clock tick ---");
    drawClockPartial();
    ticksSinceFullRefresh++;
  }

  Serial.printf("Light sleep for %d seconds...\n", CLOCK_TICK_SECONDS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)CLOCK_TICK_SECONDS * 1000000ULL);
  esp_light_sleep_start();
}
