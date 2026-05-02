
#include "Inkplate.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <functional>
#include "secrets.h"

Inkplate inkplate(INKPLATE_1BIT);

// Error levels for consistent error reporting
enum ErrorLevel { ERROR_FATAL, ERROR_RETRYABLE, ERROR_WARNING };

// Track last successful update times
unsigned long lastGraphSuccessTime = 0;
unsigned long lastDashboardSuccessTime = 0;

// Retry function with exponential backoff
// Attempts: 1 to maxAttempts, delay between attempts: baseDelayMs * 2^(attempt-1)
// Example: 5 attempts with 2000ms base -> delays of 2s, 4s, 8s, 16s
bool retry(int maxAttempts, unsigned long baseDelayMs, std::function<bool()> func) {
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    if (func()) return true;
    if (attempt < maxAttempts) {
      unsigned long delayMs = baseDelayMs * (1 << (attempt - 1));
      inkplate.print("\n[RETRY] Attempt " + String(attempt) + "/" + String(maxAttempts) + 
                     " in " + String(delayMs / 1000) + "s");
      inkplate.partialUpdate(true);
      Serial.println("[RETRY] Attempt " + String(attempt) + "/" + String(maxAttempts) + 
                     " - waiting " + String(delayMs) + "ms");
      delay(delayMs);
    }
  }
  return false;
}

// Unified error reporting function
void reportError(String msg, ErrorLevel level = ERROR_WARNING) {
  String prefix;
  switch (level) {
    case ERROR_FATAL: prefix = "[FATAL] "; break;
    case ERROR_RETRYABLE: prefix = "[RETRY] "; break;
    default: prefix = "[WARN] "; break;
  }
  Serial.println(prefix + msg);
  inkplate.print("\n" + prefix + msg);
  inkplate.partialUpdate(true);
}

// Configuration constants
#define BLACK 1
#define WHITE 0
#define MAX_POINTS 100                                 // Maximum number of points to plot
#define DISPLAY_WIDTH 1200                             // Inkplate 10 width
#define DISPLAY_HEIGHT 825                             // Inkplate 10 height
const unsigned long TOKEN_VALIDITY_PERIOD = 43200000;  // 12 hours in milliseconds
const unsigned long UPDATE_INTERVAL = 600000;          // 10 minutes in milliseconds

// Global variables to store token information
String currentToken = "";
unsigned long tokenTimestamp = 0;

// Dashboard data structure
struct DashboardData {
  float gridPower = 0;
  float acLoads = 0;
  float batteryPower = 0;
  float pvPower = 0;
  String systemState = "Unknown";
  float batterySOC = 0;
  float batteryVoltage = 0;
  float batteryCurrent = 0;
  String batteryState = "Idle";
};

// Global variable to store dashboard data
DashboardData currentDashboardData;

// Forward declarations
String formatTime(uint64_t timestampMs, int timezoneOffsetHours);
float findMax(float array[], int length);
void drawCombinedGraph(float percentages[], uint64_t timestamps[], int numPoints,
                       float solarValues[], float gridInValues[], float gridOutValues[],
                       int timezoneOffset);
void drawDashboard(float gridPower, float acLoads, float batteryPower, float pvPower,
                   String systemState, float batterySOC);
String getVictronToken();
String getEndpointData(String token, String endpoint, String instance = "");
void ensureWiFiConnected();
void runDataUpdate();
void parseStatusData(String data);
void parseBatterySummaryData(String data);
void parseSolarChargerSummaryData(String data);
bool parseJsonCommon(JsonDocument &doc, String data, String context, JsonObject &dataObj);
bool handleHttpError(HTTPClient &http, int httpResponseCode, String context, String &errorMsg);

// Helper function to format a timestamp into a time string
String formatTime(uint64_t timestampMs, int timezoneOffsetHours) {
  time_t timestamp = timestampMs / 1000;
  timestamp += timezoneOffsetHours * 3600;
  int secondsInDay = timestamp % 86400L;
  int hours = secondsInDay / 3600;
  int minutes = (secondsInDay % 3600) / 60;
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", hours, minutes);
  return String(timeStr);
}

// Helper function to find the maximum value in an array
float findMax(float array[], int length) {
  float maxVal = 0;
  for (int i = 0; i < length; i++) {
    if (array[i] > maxVal) {
      maxVal = array[i];
    }
  }
  return maxVal;
}

// Helper function to handle HTTP errors
bool handleHttpError(HTTPClient &http, int httpResponseCode, String context, String &errorMsg) {
  if (httpResponseCode <= 0) {
    String err = "HTTP error in " + context + ": " + String(httpResponseCode) + 
                 " (" + String(http.errorToString(httpResponseCode).c_str()) + ")";
    errorMsg = err;
    reportError(err, ERROR_RETRYABLE);
    http.end();
    return false;
  }
  if (httpResponseCode != HTTP_CODE_OK) {
    String err = "HTTP " + String(httpResponseCode) + " in " + context;
    errorMsg = err;
    reportError(err, ERROR_RETRYABLE);
    String response = http.getString();
    if (response.length() > 0) {
      if (response.length() > 50) {
        reportError("Response: " + response.substring(0, 50) + "...", ERROR_WARNING);
      } else {
        reportError("Response: " + response, ERROR_WARNING);
      }
    }
    http.end();
    return false;
  }
  return true;
}

// Helper function for common JSON parsing steps
bool parseJsonCommon(JsonDocument &doc, String data, String context, JsonObject &dataObj) {
  DeserializationError error = deserializeJson(doc, data);
  if (error) {
    reportError("JSON parse error in " + context + ": " + String(error.c_str()), ERROR_RETRYABLE);
    return false;
  }
  if (!doc["success"].as<bool>()) {
    reportError(context + " request was not successful", ERROR_RETRYABLE);
    return false;
  }
  JsonObject records = doc["records"];
  if (records.isNull()) {
    reportError("No records in " + context + " data", ERROR_RETRYABLE);
    return false;
  }
  dataObj = records["data"];
  if (dataObj.isNull()) {
    reportError("No data in " + context + " records", ERROR_RETRYABLE);
    return false;
  }
  return true;
}

// Function to authenticate and get the token
String getVictronToken() {
  // If we have a valid token in memory, return it
  if (currentToken != "" && millis() - tokenTimestamp < TOKEN_VALIDITY_PERIOD) {
    Serial.println("Using cached token");
    return currentToken;
  }

  // Try to refresh token with retry
  bool success = retry(5, 2000, [&]() {
    ensureWiFiConnected();
    HTTPClient http;
    String payload = "{\"username\":\"" + String(victronUsername) + "\",\"password\":\"" + String(victronPassword) + "\"}";
    String url = "https://vrmapi.victronenergy.com/v2/auth/login";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("Pragma", "no-cache");
    http.setTimeout(10000);
    
    int httpResponseCode = http.POST(payload);
    String errorMsg;
    
    if (!handleHttpError(http, httpResponseCode, "getVictronToken", errorMsg)) {
      return false;
    }
    
    String response = http.getString();
    http.end();
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
      reportError("Token JSON parse error: " + String(error.c_str()), ERROR_RETRYABLE);
      return false;
    }
    
    if (!doc.containsKey("token")) {
      if (doc.containsKey("message")) {
        String errorMsg = doc["message"].as<String>();
        reportError("Token error: " + errorMsg, ERROR_RETRYABLE);
      } else {
        reportError("No token in response", ERROR_RETRYABLE);
      }
      return false;
    }
    
    // Success - store token
    currentToken = doc["token"].as<String>();
    tokenTimestamp = millis();
    Serial.println("New token received and stored in memory");
    return true;
  });

  // Fallback: return expired token if we had one before
  if (!success && currentToken != "") {
    reportError("Token refresh failed, using old token", ERROR_WARNING);
    return currentToken;
  }
  
  // No token available at all
  if (!success) {
    currentToken = "";
    tokenTimestamp = 0;
    reportError("No token available", ERROR_FATAL);
  }
  
  return currentToken;
}

// Function to get data from any endpoint
String getEndpointData(String token, String endpoint, String instance) {
  if (token == "") {
    reportError("No token provided for " + endpoint, ERROR_RETRYABLE);
    return "";
  }

  // Retry the endpoint fetch up to 5 times
  String result = "";
  bool success = retry(5, 2000, [&]() {
    HTTPClient http;
    String url = "https://vrmapi.victronenergy.com/v2/installations/" + String(installationId) + endpoint;
    if (instance != "") {
      url += "?instance=" + instance;
    }
    
    ensureWiFiConnected();
    Serial.println("Attempting to get data from: " + url);
    
    http.begin(url);
    http.addHeader("X-Authorization", "Bearer " + token);
    http.setTimeout(15000);  // 15 second timeout
    
    int httpResponseCode = http.GET();
    String errorMsg;
    
    if (!handleHttpError(http, httpResponseCode, "getEndpointData(" + endpoint + ")", errorMsg)) {
      return false;
    }
    
    result = http.getString();
    http.end();
    
    if (result.length() == 0) {
      reportError("Empty response from " + endpoint, ERROR_RETRYABLE);
      return false;
    }
    
    Serial.println("Data received successfully from " + endpoint);
    return true;
  });
  
  if (!success) {
    result = "";
  }
  return result;
}

// Function to ensure WiFi is connected
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  
  reportError("WiFi disconnected, reconnecting...", ERROR_RETRYABLE);
  WiFi.disconnect(true);
  delay(1000);
  
  WiFi.begin(ssid, pass);
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    reportError("WiFi connection failed", ERROR_RETRYABLE);
  } else {
    Serial.println("WiFi connected");
    reportError("WiFi reconnected", ERROR_WARNING);
  }
}

// Function to parse Status data
void parseStatusData(String data) {
  JsonDocument doc;
  JsonObject dataObj;
  if (!parseJsonCommon(doc, data, "Status", dataObj)) {
    return;
  }

  // Calculate AC loads from output power phases
  float acLoads = 0;
  if (dataObj.containsKey("29") && dataObj.containsKey("30") && dataObj.containsKey("31")) {
    acLoads += dataObj["29"]["valueFloat"].as<float>();  // OP1
    acLoads += dataObj["30"]["valueFloat"].as<float>();  // OP2
    acLoads += dataObj["31"]["valueFloat"].as<float>();  // OP3
    currentDashboardData.acLoads = acLoads;
  }

  // Calculate grid power from input power phases
  if (dataObj.containsKey("17") && dataObj.containsKey("18") && dataObj.containsKey("19")) {
    float gridPower = 0;
    gridPower += dataObj["17"]["valueFloat"].as<float>();  // IP1
    gridPower += dataObj["18"]["valueFloat"].as<float>();  // IP2
    gridPower += dataObj["19"]["valueFloat"].as<float>();  // IP3
    currentDashboardData.gridPower = gridPower;
  }

  if (dataObj.containsKey("40")) {
    currentDashboardData.systemState = dataObj["40"]["value"].as<String>();
  }
}

// Function to parse Battery Summary data
void parseBatterySummaryData(String data) {
  JsonDocument doc;
  JsonObject dataObj;
  if (!parseJsonCommon(doc, data, "Battery Summary", dataObj)) {
    return;
  }
  // Parse Battery SOC
  if (dataObj.containsKey("51")) {
    currentDashboardData.batterySOC = dataObj["51"]["valueFloat"].as<float>();
  }
  // Parse Battery Voltage
  if (dataObj.containsKey("47")) {
    currentDashboardData.batteryVoltage = dataObj["47"]["valueFloat"].as<float>();
  }
  // Parse Battery Current (positive = charging, negative = discharging)
  if (dataObj.containsKey("49")) {
    currentDashboardData.batteryCurrent = dataObj["49"]["valueFloat"].as<float>();
    currentDashboardData.batteryPower = currentDashboardData.batteryVoltage * currentDashboardData.batteryCurrent;
    Serial.println(currentDashboardData.batteryPower);
  }
  // Determine if battery is charging, discharging, or idle
  if (abs(currentDashboardData.batteryCurrent) < 0.5) {
    currentDashboardData.batteryState = "Idle";
  } else if (currentDashboardData.batteryCurrent > 0) {
    currentDashboardData.batteryState = "Charging";
  } else {
    currentDashboardData.batteryState = "Discharging";
  }
}

// Function to parse Solar Charger Summary data
void parseSolarChargerSummaryData(String data) {
  JsonDocument doc;
  JsonObject dataObj;
  if (!parseJsonCommon(doc, data, "Solar Charger Summary", dataObj)) {
    return;
  }
  // Parse PV Power (Battery watts)
  if (dataObj.containsKey("107")) {
    currentDashboardData.pvPower = dataObj["107"]["valueFloat"].as<float>();
  }
}

// Function to draw the dashboard below the graph
void drawDashboard(float gridPower, float acLoads, float batteryPower, float pvPower, String systemState, float batterySOC) {
  int margin = 43;
  int displayWidth = DISPLAY_WIDTH;             // Inkplate 10 width
  int displayHeight = DISPLAY_HEIGHT;           // Inkplate 10 height
  int graphHeight = 400;                        // Height of the graph above
  int dashboardY = margin + graphHeight + 135;  // Start Y position for dashboard (below graph)

  // Set font size for dashboard
  inkplate.setTextSize(2);

  // Define box dimensions and positions
  int boxHeight = 65;
  int boxSpacing = 20;
  int startX = margin;
  int startY = dashboardY;

  // We'll use a 3x3 grid with System in the center
  int colWidth = (displayWidth - 2 * margin) / 3 - boxSpacing;
  int rowHeight = boxHeight + boxSpacing;

  // Define positions for each component in the grid layout
  // First row: Grid (left), empty (middle), Load (right)
  int gridBoxX = startX;
  int gridBoxY = startY;
  int loadBoxX = startX + 2 * (colWidth + boxSpacing);  // Right column
  int loadBoxY = startY;

  // Second row: empty (left), System (middle), empty (right)
  int systemBoxX = startX + colWidth + boxSpacing;  // Middle column
  int systemBoxY = startY + boxHeight + boxSpacing;

  // Third row: Battery (left), empty (middle), PV (right)
  int batteryBoxX = startX;
  int batteryBoxY = startY + 2 * (boxHeight + boxSpacing);
  int pvBoxX = startX + 2 * (colWidth + boxSpacing);  // Right column
  int pvBoxY = batteryBoxY;

  // All boxes will be the same width (colWidth)
  int boxWidth = colWidth;

  // Helper function to draw an arrow on a line (thicker version)
  auto drawArrowOnLine = [](int x, int y, int direction, int size = 12) {
    // Thickness of the arrow
    const int thickness = 3;
    const int halfThickness = thickness / 2;
    // Draw the main line with thickness
    switch (direction) {
      case 0:  // right
        // Draw a thick horizontal line
        for (int i = -halfThickness; i <= halfThickness; i++) {
          inkplate.drawLine(x, y + i, x + size, y + i, BLACK);
        }
        // Draw arrowhead (triangle)
        for (int i = 0; i <= thickness; i++) {
          inkplate.drawLine(x + size - i, y, x + size - 5 - i, y - 5, BLACK);
          inkplate.drawLine(x + size - i, y, x + size - 5 - i, y + 5, BLACK);
        }
        break;
      case 1:  // down
        // Draw a thick vertical line
        for (int i = -halfThickness; i <= halfThickness; i++) {
          inkplate.drawLine(x + i, y, x + i, y + size, BLACK);
        }
        // Arrowhead
        for (int i = 1; i <= thickness; i++) {
          inkplate.drawLine(x, y + size - i, x - 5, y + size - 5 - i, BLACK);
          inkplate.drawLine(x, y + size - i, x + 5, y + size - 5 - i, BLACK);
        }
        break;
      case 2:  // left
        // Draw a thick horizontal line
        for (int i = -halfThickness; i <= halfThickness; i++) {
          inkplate.drawLine(x, y + i, x - size, y + i, BLACK);
        }
        // Draw arrowhead (triangle)
        for (int i = 1; i <= thickness; i++) {
          inkplate.drawLine(x - size + i, y, x - size + 5 + i, y - 5, BLACK);
          inkplate.drawLine(x - size + i, y, x - size + 5 + i, y + 5, BLACK);
        }
        break;
      case 3:  // up
        // Draw a thick vertical line
        for (int i = -halfThickness; i <= halfThickness; i++) {
          inkplate.drawLine(x + i, y, x + i, y - size, BLACK);
        }
        // Draw arrowhead (triangle)
        for (int i = 1; i <= thickness; i++) {
          inkplate.drawLine(x, y - size + i, x - 5, y - size + 5 + i, BLACK);
          inkplate.drawLine(x, y - size + i, x + 5, y - size + 5 + i, BLACK);
        }
        break;
    }
  };

  // Draw all boxes
  // Draw Grid box (top left)
  inkplate.drawRect(gridBoxX, gridBoxY, boxWidth, boxHeight, BLACK);
  inkplate.setCursor(gridBoxX + 10, gridBoxY + 10);
  inkplate.print("Sit");  // Grid
  int gridArrowX = gridBoxX + boxWidth / 2;
  int gridArrowY = gridBoxY + 30;
  inkplate.setCursor(gridBoxX + 10, gridBoxY + 40);
  inkplate.setTextSize(3);
  if (gridPower < 0) {
    inkplate.print(abs(gridPower), 0);
    inkplate.print(" W Pretok");
  } else {
    inkplate.print(gridPower, 0);
    inkplate.print(" W");
  }
  inkplate.setTextSize(2);

  // Draw AC Loads box (top right)
  inkplate.drawRect(loadBoxX, loadBoxY, boxWidth, boxHeight, BLACK);
  inkplate.setCursor(loadBoxX + 10, loadBoxY + 10);
  inkplate.print("Zatez");  // Load
  inkplate.setTextSize(3);
  inkplate.setCursor(loadBoxX + 10, loadBoxY + 40);
  inkplate.print(acLoads, 0);
  inkplate.print(" W");
  inkplate.setTextSize(2);

  // Draw System box (middle center)
  inkplate.drawRect(systemBoxX, systemBoxY, boxWidth, boxHeight, BLACK);
  inkplate.setCursor(systemBoxX + 10, systemBoxY + 10);
  inkplate.print("System");  // System state
  inkplate.setCursor(systemBoxX + 10, systemBoxY + 40);
  // Translate system state to Czech
  String translatedState;
  if (systemState == "External control") {
    translatedState = "Externi rizeni";
  } else if (systemState == "Absorption") {
    translatedState = "Absorpce";
  } else if (systemState == "Float") {
    translatedState = "Udrzovani";
  } else if (systemState == "Bulk") {
    translatedState = "Hromadeni";
  } else {
    translatedState = systemState;
  }
  inkplate.print(translatedState);

  // Draw Battery box (bottom left)
  inkplate.drawRect(batteryBoxX, batteryBoxY, boxWidth, boxHeight, BLACK);
  inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 10);
  inkplate.print("Baterie");  // Battery
  int batteryArrowX = batteryBoxX + boxWidth / 2;
  int batteryArrowY = batteryBoxY + 30;
  if (abs(batteryPower) < 10) {
    inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
    inkplate.print(currentDashboardData.batteryState);
  } else {
    inkplate.setTextSize(3);
    if (batteryPower > 0) {
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
      inkplate.print(abs(batteryPower), 0);
      inkplate.print(" W Nabijeni");
    } else {
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
      inkplate.print(abs(batteryPower), 0);
      inkplate.print(" W Vybijeni");
    }
    inkplate.setTextSize(2);
  }
  inkplate.setTextSize(3);
  inkplate.setCursor(batteryBoxX + boxWidth - 90, batteryBoxY + 10);
  inkplate.print(batterySOC, 1);
  inkplate.print("%");
  inkplate.setTextSize(2);

  // Draw PV Charger box (bottom right)
  inkplate.drawRect(pvBoxX, pvBoxY, boxWidth, boxHeight, BLACK);
  inkplate.setCursor(pvBoxX + 10, pvBoxY + 10);
  inkplate.print("Solarni nabijec");  // Solar charger
  inkplate.setTextSize(3);
  inkplate.setCursor(pvBoxX + 10, pvBoxY + 40);
  inkplate.print(pvPower, 0);
  inkplate.print(" W");
  inkplate.setTextSize(2);

  // Calculate connection points for each box
  int gridRightX = gridBoxX + boxWidth;
  int gridCenterY = gridBoxY + boxHeight / 2;
  int loadLeftX = loadBoxX;
  int loadCenterY = loadBoxY + boxHeight / 2;
  int systemLeftX = systemBoxX;
  int systemRightX = systemBoxX + boxWidth;
  int systemTopY = systemBoxY;
  int systemBottomY = systemBoxY + boxHeight;
  int systemCenterX = systemBoxX + boxWidth / 2;
  int systemCenterY = systemBoxY + boxHeight / 2;
  int batteryTopX = batteryBoxX + boxWidth / 2;
  int batteryTopY = batteryBoxY;
  int batteryCenterY = batteryBoxY + boxHeight / 2;
  int pvTopX = pvBoxX + boxWidth / 2;
  int pvTopY = pvBoxY;
  int pvCenterY = pvBoxY + boxHeight / 2;

  // Helper function to draw an orthogonal connection between two points with an arrow
  auto drawOrthogonalConnection = [&](int startX, int startY, int endX, int endY, int arrowDir) {
    // Draw the first segment (horizontal)
    inkplate.drawLine(startX, startY, endX, startY, BLACK);

    // Draw the second segment (vertical)
    inkplate.drawLine(endX, startY, endX, endY, BLACK);
    int arrowX = (startX + endX) / 2;
    int arrowY = startY;
    drawArrowOnLine(arrowX, arrowY, arrowDir);
  };

  // 1. Grid to System connection (if importing)
  if (gridPower > 10) {
    drawOrthogonalConnection(
      gridBoxX + boxWidth, gridCenterY,  // Start at right center of grid box
      systemCenterX - 30, systemBoxY,       // End at left center of system box
      0                                  // Arrow direction: right
    );
  }

  // 2. System to Grid connection (if exporting)
  if (gridPower < -10) {
    drawOrthogonalConnection(
      gridBoxX + boxWidth, gridCenterY,  // End at right center of grid box
      systemCenterX - 30, systemBoxY,       // Start at left center of system box
      2                                  // Arrow direction: left
    );
  }

  // 3. PV to System connection (always when PV is producing)
  if (pvPower > 10) {
    drawOrthogonalConnection(
      pvBoxX, pvCenterY,                           // Start at top center of PV box
      systemCenterX + 30, systemBoxY + boxHeight,  // End at bottom center of system box
      2                                            // Arrow direction: left
    );
  }

  // 4. Battery to/from System connection
  if (abs(batteryPower) > 10) {
    int dir = 2;
    if (batteryPower < -10) {  // Discharging - battery to system
      dir = 0;
    }
    drawOrthogonalConnection(
      batteryBoxX + boxWidth, batteryCenterY,
      systemCenterX - 30, systemBoxY + boxHeight,
      dir);
  }

  // 5. System to Load connection (always when loads are active)
  if (acLoads > 10) {
    drawOrthogonalConnection(
      loadBoxX, loadCenterY,
      systemCenterX + 30, systemBoxY,
      0  // Arrow direction: right
    );
  }

  // Display last update timestamp if data is stale
  if (lastDashboardSuccessTime > 0) {
    unsigned long ageMinutes = (millis() - lastDashboardSuccessTime) / 1000 / 60;
    if (ageMinutes > 1) {
      inkplate.setCursor(DISPLAY_WIDTH - 150, DISPLAY_HEIGHT - 20);
      inkplate.setTextSize(1);
      inkplate.print("Dashboard: " + String(ageMinutes) + "min ago");
      inkplate.setTextSize(2);
    }
  }

  Serial.println("updated dashboard");

  // Update display
  inkplate.partialUpdate(true);
}

// Function to draw the combined graph
void drawCombinedGraph(float percentages[], uint64_t timestamps[], int numPoints,
                       float solarValues[], float gridInValues[], float gridOutValues[],
                       int timezoneOffset) {
  // Clear the display
  inkplate.clearDisplay();

  // Define graph dimensions and margins
  int margin = 43;
  int displayWidth = DISPLAY_WIDTH;    // Inkplate 10 width
  int displayHeight = DISPLAY_HEIGHT;  // Inkplate 10 height
  int graphWidth = displayWidth - 2 * margin - 5;
  int graphHeight = 350;
  int graphX = margin;
  int graphY = margin;

  // Calculate scaling for the right y-axis (power in kW)
  float maxSolar = findMax(solarValues, numPoints);
  float maxGridIn = findMax(gridInValues, numPoints);
  float maxGridOut = findMax(gridOutValues, numPoints);
  float gridMax = max(maxGridIn, maxGridOut);
  float rightAxisMax = max(maxSolar, gridMax);

  // Round up to a nice number
  if (rightAxisMax <= 5) rightAxisMax = 5;
  else if (rightAxisMax <= 10) rightAxisMax = 10;
  else if (rightAxisMax <= 20) rightAxisMax = 20;
  else rightAxisMax = ceil(rightAxisMax / 10) * 10;

  // Draw graph axes
  inkplate.drawLine(graphX, graphY + graphHeight, graphX + graphWidth, graphY + graphHeight, BLACK);
  inkplate.drawLine(graphX, graphY, graphX, graphY + graphHeight, BLACK);
  int rightAxisX = graphX + graphWidth;
  inkplate.drawLine(rightAxisX, graphY, rightAxisX, graphY + graphHeight, BLACK);

  // Draw left y-axis labels (battery %)
  inkplate.setTextSize(2);
  for (int i = 0; i <= 100; i += 25) {
    int yPos = graphY + graphHeight - (i * graphHeight / 100);
    inkplate.drawLine(graphX - 3, yPos, graphX, yPos, BLACK);
    inkplate.setCursor(graphX - 40, yPos - 5);
    inkplate.print(i);
  }

  // Draw right y-axis labels (for kW values)
  int numRightLabels = 10;
  for (int i = 0; i <= numRightLabels; i++) {
    int labelValue = round((i * 2 * rightAxisMax) / numRightLabels);
    int yPos = graphY + graphHeight - (i * graphHeight) / numRightLabels;
    inkplate.drawLine(rightAxisX, yPos, rightAxisX + 3, yPos, BLACK);
    inkplate.setCursor(rightAxisX + 10, yPos - 3);
    inkplate.print(labelValue, 1);
  }

  // Draw x-axis labels (times)
  for (int i = 0; i < numPoints; i += max(1, numPoints / 10)) {
    int xPos = graphX + (i * graphWidth) / (numPoints - 1);
    String timeStr = formatTime(timestamps[i], timezoneOffset);
    inkplate.setCursor(xPos - 20, graphY + graphHeight + 10);
    inkplate.print(timeStr);
  }

  // Draw battery percentage line graph
  for (int i = 0; i < numPoints - 1; i++) {
    int x1 = graphX + (i * graphWidth) / (numPoints - 1);
    int y1 = graphY + graphHeight - (percentages[i] * graphHeight / 100);
    int x2 = graphX + ((i + 1) * graphWidth) / (numPoints - 1);
    int y2 = graphY + graphHeight - (percentages[i + 1] * graphHeight / 100);
    drawThickLineWithCircles(x1, y1, x2, y2, 5, BLACK);
  }

  // Calculate bar widths
  int barWidth = max(5, min(20, graphWidth / numPoints / 4));
  int barSpacing = barWidth + 2;

  // Create combined grid values
  float gridValues[MAX_POINTS];
  for (int i = 0; i < numPoints; i++) {
    gridValues[i] = gridInValues[i] - gridOutValues[i];
  }

  // Find maximum absolute grid value for scaling
  float maxAbsGrid = 0;
  for (int i = 0; i < numPoints; i++) {
    float absValue = abs(gridValues[i]);
    if (absValue > maxAbsGrid) {
      maxAbsGrid = absValue;
    }
  }

  if (maxAbsGrid > rightAxisMax) {
    rightAxisMax = maxAbsGrid;
    if (rightAxisMax <= 5) rightAxisMax = 5;
    else if (rightAxisMax <= 10) rightAxisMax = 10;
    else if (rightAxisMax <= 20) rightAxisMax = 20;
    else rightAxisMax = ceil(rightAxisMax / 10) * 10;
  }

  // Draw bars for each metric at each time point
  for (int i = 0; i < numPoints; i++) {
    int xPosBase = graphX + (i * graphWidth) / (numPoints - 1);
    int solarXPos = xPosBase - barWidth - 2;
    if (solarXPos < graphX) solarXPos = graphX;
    int gridXPos = xPosBase + 2;

    // Draw solar production bars
    if (solarValues[i] > 0) {
      int barHeight = (solarValues[i] / rightAxisMax) * graphHeight;
      int yPos = graphY + graphHeight - barHeight;
      inkplate.fillRect(solarXPos, yPos, barWidth, barHeight, BLACK);
    }

    // Draw grid bar
    float gridValue = gridValues[i];
    if (gridValue != 0) {
      int barHeight;
      int yPos;
      if (gridValue > 0) {  // Grid consumption
        barHeight = (gridValue / rightAxisMax) * graphHeight;
        yPos = graphY + graphHeight - barHeight;
      } else {  // Grid overflow
        barHeight = (abs(gridValue) / rightAxisMax) * graphHeight;
        yPos = graphY + graphHeight;
      }
      if (gridValue > 0) {
        inkplate.drawRect(gridXPos, yPos, barWidth, barHeight, BLACK);
        // Diagonal hatching
        for (int y = yPos; y < yPos + barHeight; y += 3) {
          inkplate.drawLine(gridXPos, y, gridXPos + barWidth, y + 3, BLACK);
        }
      } else {
        inkplate.drawRect(gridXPos, yPos, barWidth, barHeight, BLACK);
        // Vertical lines fill
        for (int x = gridXPos; x < gridXPos + barWidth; x += 2) {
          inkplate.drawLine(x, yPos, x, yPos + barHeight, BLACK);
        }
      }
    }
  }

  // Add legend
  int legendX = 10;
  int legendY = 15;
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Baterie [%]");
  legendX = 700;
  inkplate.fillRect(legendX, legendY, 20, 10, BLACK);
  legendX += 23;
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Solar");
  legendX += 70;
  inkplate.drawRect(legendX, legendY, 20, 10, BLACK);
  for (int y = legendY; y < legendY + 10; y += 3) {
    inkplate.drawLine(legendX, y, legendX + 20, y + 3, BLACK);
  }
  legendX += 23;
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Sit (vstup/pretoky) [kWh]");

  // Add title and axis labels
  inkplate.setTextSize(3);
  inkplate.setCursor(graphX + graphWidth / 2 - 100, graphY - 30);
  inkplate.print("Solaary");

  // Display last update timestamp if data is stale
  if (lastGraphSuccessTime > 0) {
    unsigned long ageMinutes = (millis() - lastGraphSuccessTime) / 1000 / 60;
    if (ageMinutes > 1) {
      inkplate.setTextSize(1);
      inkplate.setCursor(DISPLAY_WIDTH - 150, graphY - 10);
      inkplate.print("Graph: " + String(ageMinutes) + "min ago");
      inkplate.setTextSize(3);
    }
  }
}

// Function to draw thick lines with circles for smoother appearance
void drawThickLineWithCircles(int x1, int y1, int x2, int y2, int thickness, int color) {
  // Calculate the distance between the two points
  float distance = sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));

  // If the line is very short, just draw a circle at the midpoint
  if (distance < thickness) {
    int midX = (x1 + x2) / 2;
    int midY = (y1 + y2) / 2;
    inkplate.fillCircle(midX, midY, thickness / 2, color);
    return;
  }

  // Calculate the number of circles we need to draw (one every thickness pixels)
  int numCircles = distance / (thickness * 0.5);  // Some overlap between circles

  // Draw circles along the line
  for (int i = 0; i <= numCircles; i++) {
    float t = (float)i / numCircles;
    int x = x1 + (x2 - x1) * t;
    int y = y1 + (y2 - y1) * t;
    inkplate.fillCircle(x, y, thickness / 2, color);
  }

  // Also draw circles at the endpoints to ensure full coverage
  inkplate.fillCircle(x1, y1, thickness / 2, color);
  inkplate.fillCircle(x2, y2, thickness / 2, color);
}

// Add these to your global variables
unsigned long lastGraphUpdateTime = 0;
unsigned long lastDashboardUpdateTime = 0;
const unsigned long GRAPH_UPDATE_INTERVAL = 120000;  // 2 minutes
const unsigned long DASHBOARD_UPDATE_INTERVAL = 30000;  // 30 seconds

struct GraphData {
  float percentages[MAX_POINTS];
  uint64_t timestamps[MAX_POINTS];
  float solarValues[MAX_POINTS];
  float gridInValues[MAX_POINTS];
  float gridOutValues[MAX_POINTS];
  int numPoints = 0;
  int timezoneOffset = 2;
};

GraphData currentGraphData;

// Function to update graph data (runs every 2 minutes)
void updateGraphData() {
  Serial.println("Updating graph data...");
  reportError("Updating graph...", ERROR_WARNING);

  // Get token
  String token = getVictronToken();
  if (token == "") {
    reportError("Cannot update graph: no token", ERROR_RETRYABLE);
    return;
  }

  // Get stats data for the graph
  String statsData = getEndpointData(token, "/stats");
  if (statsData == "") {
    reportError("No graph data received", ERROR_RETRYABLE);
    return;
  }

  JsonDocument batteryDoc;
  DeserializationError batteryError = deserializeJson(batteryDoc, statsData);
  if (batteryError) {
    reportError("Graph JSON parse error: " + String(batteryError.c_str()), ERROR_RETRYABLE);
    return;
  }

  if (!batteryDoc["records"].containsKey("bs")) {
    reportError("'bs' not found in graph records", ERROR_RETRYABLE);
    return;
  }

  JsonArray bsArray = batteryDoc["records"]["bs"];
  int numPoints = bsArray.size();
  if (numPoints == 0) {
    reportError("No data points in graph array", ERROR_RETRYABLE);
    return;
  }

  float percentages[MAX_POINTS];
  uint64_t timestamps[MAX_POINTS];
  float solarValues[MAX_POINTS];
  float gridInValues[MAX_POINTS];
  float gridOutValues[MAX_POINTS];

  for (int i = 0; i < MAX_POINTS; i++) {
    solarValues[i] = 0;
    gridInValues[i] = 0;
    gridOutValues[i] = 0;
  }

  int step = 1;
  int actualNumPoints;
  if (numPoints > MAX_POINTS) {
    step = numPoints / MAX_POINTS + 1;
    actualNumPoints = MAX_POINTS;
  } else {
    actualNumPoints = numPoints;
  }

  // Extract battery data
  for (int i = 0, pointIndex = 0; i < numPoints && pointIndex < MAX_POINTS; i += step, pointIndex++) {
    if (i < bsArray.size()) {
      JsonArray bsEntry = bsArray[i];
      if (bsEntry.size() >= 2) {
        timestamps[pointIndex] = bsEntry[0].as<uint64_t>();
        percentages[pointIndex] = bsEntry[1].as<float>();
      }
    }
  }

  // Extract solar data
  if (batteryDoc["records"].containsKey("Pdc")) {
    JsonArray pdcArray = batteryDoc["records"]["Pdc"];
    for (int i = 0, pointIndex = 0; i < pdcArray.size() && pointIndex < actualNumPoints; i += step, pointIndex++) {
      if (i < pdcArray.size()) {
        JsonArray pdcEntry = pdcArray[i];
        if (pdcEntry.size() >= 2) {
          float powerW = pdcEntry[1].as<float>();
          solarValues[pointIndex] = powerW / 1000.0;  // Convert W to kW
        }
      }
    }
  } else {
    reportError("No solar data in graph records", ERROR_WARNING);
  }

  // Extract grid data
  if (batteryDoc["records"].containsKey("grid_history_from")) {
    JsonArray gridFromArray = batteryDoc["records"]["grid_history_from"];
    for (int i = 0, pointIndex = 0; i < gridFromArray.size() && pointIndex < actualNumPoints; i += step, pointIndex++) {
      if (i < gridFromArray.size()) {
        JsonArray gridEntry = gridFromArray[i];
        if (gridEntry.size() >= 2) {
          gridInValues[pointIndex] = gridEntry[1].as<float>();
        }
      }
    }
  }

  if (batteryDoc["records"].containsKey("grid_history_to")) {
    JsonArray gridToArray = batteryDoc["records"]["grid_history_to"];
    for (int i = 0, pointIndex = 0; i < gridToArray.size() && pointIndex < actualNumPoints; i += step, pointIndex++) {
      if (i < gridToArray.size()) {
        JsonArray gridEntry = gridToArray[i];
        if (gridEntry.size() >= 2) {
          gridOutValues[pointIndex] = gridEntry[1].as<float>();
        }
      }
    }
  }

  // Store the data in the global graph data structure
  for (int i = 0; i < actualNumPoints; i++) {
    currentGraphData.percentages[i] = percentages[i];
    currentGraphData.timestamps[i] = timestamps[i];
    currentGraphData.solarValues[i] = solarValues[i];
    currentGraphData.gridInValues[i] = gridInValues[i];
    currentGraphData.gridOutValues[i] = gridOutValues[i];
  }
  currentGraphData.numPoints = actualNumPoints;

  // Update the graph display
  updateGraphDisplay();
  lastGraphSuccessTime = millis();
}

// Function to update dashboard data (runs every 5 seconds)
void updateDashboardData() {
  Serial.println("Updating dashboard data...");
  reportError("Updating dashboard...", ERROR_WARNING);

  // Get token
  String token = getVictronToken();
  if (token == "") {
    reportError("Cannot update dashboard: no token", ERROR_RETRYABLE);
    return;
  }

  // Get dashboard data from endpoints
  String statusData = getEndpointData(token, "/widgets/Status", "276");
  if (statusData != "") {
    parseStatusData(statusData);
  } else {
    reportError("No status data received", ERROR_WARNING);
  }

  String batterySummaryData = getEndpointData(token, "/widgets/BatterySummary", "512");
  if (batterySummaryData != "") {
    parseBatterySummaryData(batterySummaryData);
  } else {
    reportError("No battery data received", ERROR_WARNING);
  }

  // Get data from both solar chargers
  String solarCharger1Data = getEndpointData(token, "/widgets/SolarChargerSummary", "278");
  if (solarCharger1Data != "") {
    parseSolarChargerSummaryData(solarCharger1Data);
  } else {
    reportError("No solar charger 1 data", ERROR_WARNING);
  }

  String solarCharger2Data = getEndpointData(token, "/widgets/SolarChargerSummary", "279");
  if (solarCharger2Data != "") {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, solarCharger2Data);
    if (!error && doc["success"].as<bool>()) {
      JsonObject records = doc["records"];
      if (!records.isNull()) {
        JsonObject dataObj = records["data"];
        if (!dataObj.isNull() && dataObj.containsKey("107")) {
          float additionalPvPower = dataObj["107"]["valueFloat"].as<float>();
          currentDashboardData.pvPower += additionalPvPower;
        }
      }
    } else if (error) {
      reportError("Solar charger 2 parse error: " + String(error.c_str()), ERROR_WARNING);
    }
  } else {
    reportError("No solar charger 2 data", ERROR_WARNING);
  }

  Serial.println("Got all data, updating dashboard...");
  updateDashboardDisplay();
  lastDashboardSuccessTime = millis();
}

// Function to draw only the graph area
void updateGraphDisplay() {
  int margin = 43;
  int graphHeight = 400;

  // Clear only the graph area
  inkplate.fillRect(0, margin, DISPLAY_WIDTH, graphHeight, WHITE);

  // Draw the graph with current data
  drawCombinedGraph(
    currentGraphData.percentages,
    currentGraphData.timestamps,
    currentGraphData.numPoints,
    currentGraphData.solarValues,
    currentGraphData.gridInValues,
    currentGraphData.gridOutValues,
    currentGraphData.timezoneOffset
  );
  inkplate.display();
}

// Function to draw only the dashboard area
void updateDashboardDisplay() {
  int margin = 43;
  int graphHeight = 400;
  int dashboardY = margin + graphHeight + 135;

  // Clear only the dashboard area
  inkplate.fillRect(0, dashboardY, DISPLAY_WIDTH, DISPLAY_HEIGHT - dashboardY, WHITE);

  // Draw the dashboard with current data
  drawDashboard(
    currentDashboardData.gridPower,
    currentDashboardData.acLoads,
    currentDashboardData.batteryPower,
    currentDashboardData.pvPower,
    currentDashboardData.systemState,
    currentDashboardData.batterySOC
  );
}

// Modified setup function
void setup() {
  inkplate.begin();
  inkplate.clearDisplay();
  inkplate.display();
  Serial.begin(9600);
  Serial.println("Starting Victron display...");

  // Initialize watchdog timer (60 second timeout)
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog initialized (60s timeout)");

  // Connect to WiFi
  WiFi.begin(ssid, pass);
  inkplate.print("Connecting to WiFi...");
  inkplate.partialUpdate(true);
  unsigned long wifiStartTime = millis();
  const unsigned long wifiTimeout = 30000;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    esp_task_wdt_reset(); // Reset watchdog while connecting
    inkplate.print('.');
    inkplate.partialUpdate(true);
    if (millis() - wifiStartTime > wifiTimeout) {
      reportError("WiFi connection timeout", ERROR_RETRYABLE);
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    inkplate.println("\nWiFi connected");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    reportError("WiFi connection failed", ERROR_RETRYABLE);
  }
  inkplate.partialUpdate(true);

  // Initial updates
  updateGraphData();
  updateDashboardData();

  lastGraphUpdateTime = millis();
  lastDashboardUpdateTime = millis();
}

// Modified loop function
void loop() {
  // Reset watchdog timer on every iteration
  esp_task_wdt_reset();

  unsigned long currentTime = millis();

  // Handle millis() overflow
  if (currentTime < lastGraphUpdateTime) {
    lastGraphUpdateTime = 0;
  }
  if (currentTime < lastDashboardUpdateTime) {
    lastDashboardUpdateTime = 0;
  }

  // Check if it's time for a graph update
  if ((currentTime - lastGraphUpdateTime) >= GRAPH_UPDATE_INTERVAL) {
    lastGraphUpdateTime = currentTime;
    updateGraphData();
  }

  // Check if it's time for a dashboard update
  if ((currentTime - lastDashboardUpdateTime) >= DASHBOARD_UPDATE_INTERVAL) {
    lastDashboardUpdateTime = currentTime;
    updateDashboardData();
  }

  // Small delay to prevent watchdog issues
  delay(100);
}

