#include "Inkplate.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

Inkplate inkplate(INKPLATE_1BIT);

// Configuration constants
#define BLACK 1
#define WHITE 0
#define MAX_POINTS 100                                 // Maximum number of points to plot
const unsigned long TOKEN_VALIDITY_PERIOD = 43200000;  // 12 hours in milliseconds
const unsigned long UPDATE_INTERVAL = 600000;          // 10 minutes in milliseconds

// Global variables to store token information
String currentToken = "";
unsigned long tokenTimestamp = 0;
unsigned long lastUpdateTime = 0;

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
void parseStatsData(String data);

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

// Function to authenticate and get the token
String getVictronToken() {
  // If we have a valid token in memory, return it
  if (currentToken != "" && millis() - tokenTimestamp < TOKEN_VALIDITY_PERIOD) {
    Serial.println("Using cached token");
    return currentToken;
  }
  HTTPClient http;
  String token = "";
  ensureWiFiConnected();
  String payload = "{\"username\":\"" + String(victronUsername) + "\",\"password\":\"" + String(victronPassword) + "\"}";
  String url = "https://vrmapi.victronenergy.com/v2/auth/login";
  Serial.println("Attempting to connect to Victron API...");
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  http.setTimeout(10000);  // 10 second timeout
  int httpResponseCode = http.POST(payload);
  // Error handling
  if (httpResponseCode <= 0) {
    Serial.print("HTTP error: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: " + String(httpResponseCode) + " (" + String(http.errorToString(httpResponseCode).c_str()) + ")";
    Serial.println(errorMsg);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    http.end();
    return token;
  }
  if (httpResponseCode != HTTP_CODE_OK) {
    Serial.print("Unexpected HTTP response: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: " + String(httpResponseCode);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    String response = http.getString();
    Serial.println("Response: " + response);
    if (response.length() > 0) {
      inkplate.print("\nResponse: ");
      if (response.length() > 50) {
        inkplate.print(response.substring(0, 50) + "...");
      } else {
        inkplate.print(response);
      }
      inkplate.partialUpdate(true);
    }
    http.end();
    return token;
  }
  String response = http.getString();
  http.end();
  // Parse JSON response
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (!error && doc.containsKey("token")) {
    token = doc["token"].as<String>();
    // Store the token and timestamp in memory
    currentToken = token;
    tokenTimestamp = millis();
    Serial.println("New token received and stored in memory");
    return token;
  } else {
    if (error) {
      Serial.print("JSON parsing error: ");
      Serial.println(error.c_str());
      String errorMsg = "JSON Error: " + String(error.c_str());
      inkplate.print("\n" + errorMsg);
      inkplate.partialUpdate(true);
    }
    if (!doc.containsKey("token")) {
      Serial.println("No token found in response");
      inkplate.print("\nNo token in response");
      if (doc.containsKey("message")) {
        String errorMsg = doc["message"].as<String>();
        Serial.println("Error message: " + errorMsg);
        inkplate.print("\nError: " + errorMsg);
      }
      inkplate.partialUpdate(true);
    }
  }
  return token;
}

// Function to get data from any endpoint
String getEndpointData(String token, String endpoint, String instance) {
  HTTPClient http;
  String result = "";
  String url = "https://vrmapi.victronenergy.com/v2/installations/" + String(installationId) + endpoint;

  if (instance != "") {
    url += "?instance=" + instance;
  }

  ensureWiFiConnected();
  if (token == "") {
    Serial.println("Error: No token provided");
    inkplate.print("\nError: No token");
    inkplate.partialUpdate(true);
    return result;
  }

  Serial.println("Attempting to get data from: " + url);
  http.begin(url);
  http.addHeader("X-Authorization", "Bearer " + token);
  http.setTimeout(15000);  // 15 second timeout

  int httpResponseCode = http.GET();

  // Error handling
  if (httpResponseCode <= 0) {
    Serial.print("HTTP error: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: " + String(httpResponseCode) + " (" + String(http.errorToString(httpResponseCode).c_str()) + ")";
    Serial.println(errorMsg);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    http.end();
    return result;
  }

  if (httpResponseCode != HTTP_CODE_OK) {
    Serial.print("Unexpected HTTP response: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: " + String(httpResponseCode);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    String response = http.getString();
    Serial.println("Response: " + response);
    if (response.length() > 0) {
      inkplate.print("\nResponse: ");
      if (response.length() > 50) {
        inkplate.print(response.substring(0, 50) + "...");
      } else {
        inkplate.print(response);
      }
      inkplate.partialUpdate(true);
    }
    http.end();
    return result;
  }

  result = http.getString();
  http.end();

  if (result.length() == 0) {
    Serial.println("Empty response received from " + endpoint);
    inkplate.print("\nEmpty response from " + endpoint);
    inkplate.partialUpdate(true);
    return "";
  }

  Serial.println("Data received successfully from " + endpoint);
  return result;
}

// Function to ensure WiFi is connected
void ensureWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, (re)connecting...");
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Failed to reconnect WiFi");
    } else {
      Serial.println("WiFi connected");
    }
  }
}

// Function to parse Status data
void parseStatusData(String data) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, data);

  if (error) {
    Serial.print("Error parsing Status data: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["success"].as<bool>()) {
    Serial.println("Status data request was not successful");
    return;
  }

  JsonObject records = doc["records"];
  if (records.isNull()) {
    Serial.println("No records in Status data");
    return;
  }

  JsonObject dataObj = records["data"];
  if (dataObj.isNull()) {
    Serial.println("No data in Status records");
    return;
  }

  // Parse AC Loads (sum of output powers)
  float acLoads = 0;
  if (dataObj.containsKey("29") && dataObj.containsKey("30") && dataObj.containsKey("31")) {
    acLoads += dataObj["29"]["valueFloat"].as<float>();  // OP1
    acLoads += dataObj["30"]["valueFloat"].as<float>();  // OP2
    acLoads += dataObj["31"]["valueFloat"].as<float>();  // OP3
    currentDashboardData.acLoads = acLoads;
  }

  // Parse System State (VE.Bus state)
  if (dataObj.containsKey("40")) {
    currentDashboardData.systemState = dataObj["40"]["value"].as<String>();
  }
}

// Function to parse Battery Summary data
void parseBatterySummaryData(String data) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, data);

  if (error) {
    Serial.print("Error parsing Battery Summary data: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["success"].as<bool>()) {
    Serial.println("Battery Summary request was not successful");
    return;
  }

  JsonObject records = doc["records"];
  if (records.isNull()) {
    Serial.println("No records in Battery Summary data");
    return;
  }

  JsonObject dataObj = records["data"];
  if (dataObj.isNull()) {
    Serial.println("No data in Battery Summary records");
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
  DeserializationError error = deserializeJson(doc, data);

  if (error) {
    Serial.print("Error parsing Solar Charger Summary data: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["success"].as<bool>()) {
    Serial.println("Solar Charger Summary request was not successful");
    return;
  }

  JsonObject records = doc["records"];
  if (records.isNull()) {
    Serial.println("No records in Solar Charger Summary data");
    return;
  }

  JsonObject dataObj = records["data"];
  if (dataObj.isNull()) {
    Serial.println("No data in Solar Charger Summary records");
    return;
  }

  // Parse PV Power (Battery watts)
  if (dataObj.containsKey("107")) {
    currentDashboardData.pvPower = dataObj["107"]["valueFloat"].as<float>();
  }
}

// Function to parse Stats data
void parseStatsData(String data) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, data);

  if (error) {
    Serial.print("Error parsing Stats data: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["success"].as<bool>()) {
    Serial.println("Stats request was not successful");
    return;
  }

  // This function is already handled in drawCombinedGraph
  // We'll keep it here for completeness but not implement parsing again
}

// Function to draw the dashboard below the graph
void drawDashboard(float gridPower, float acLoads, float batteryPower, float pvPower, String systemState, float batterySOC) {
  int margin = 43;
  int displayWidth = 1200;                     // Inkplate 10 width
  int displayHeight = 825;                     // Inkplate 10 height
  int graphHeight = 350;                       // Height of the graph above
  int dashboardY = margin + graphHeight + 50;  // Start Y position for dashboard (below graph)

  // Set font size for dashboard
  inkplate.setTextSize(2);

  // Define box dimensions and positions
  int boxHeight = 90;
  int boxSpacing = 15;
  int startX = margin;
  int startY = dashboardY;

  // We'll use a 3x2 grid with System in the middle
  int colWidth = (displayWidth - 2 * margin) / 3 - boxSpacing;
  int rowHeight = boxHeight + boxSpacing;

  // Define positions for each component in the new layout

  // First row (Grid and Load)
  int gridBoxX = startX;
  int gridBoxY = startY;
  int loadBoxX = startX + 2*(colWidth + boxSpacing);  // Right column
  int loadBoxY = startY;

  // Second row (System only, centered)
  int systemBoxX = startX + colWidth + boxSpacing;  // Middle column
  int systemBoxY = startY + boxHeight + boxSpacing;

  // Third row (Battery and PV)
  int batteryBoxX = startX;
  int batteryBoxY = startY + 2*(boxHeight + boxSpacing);
  int pvBoxX = startX + 2*(colWidth + boxSpacing);  // Right column
  int pvBoxY = batteryBoxY;

  // All boxes will be the same width (colWidth) except PV and Battery which are in outer columns
  int outerBoxWidth = colWidth;
  int centerBoxWidth = colWidth;

  // Helper function to draw an arrow
  auto drawArrow = [](int x, int y, int direction, int size = 10) {
    // direction: 0=right, 1=down, 2=left, 3=up
    //            4=down-right, 5=down-left, 6=up-right, 7=up-left
    switch(direction) {
      case 0: // right
        inkplate.drawLine(x, y, x+size, y, BLACK);
        inkplate.drawLine(x+size, y, x+size-5, y-5, BLACK);
        inkplate.drawLine(x+size, y, x+size-5, y+5, BLACK);
        break;
      case 1: // down
        inkplate.drawLine(x, y, x, y+size, BLACK);
        inkplate.drawLine(x, y+size, x-5, y+size-5, BLACK);
        inkplate.drawLine(x, y+size, x+5, y+size-5, BLACK);
        break;
      case 2: // left
        inkplate.drawLine(x, y, x-size, y, BLACK);
        inkplate.drawLine(x-size, y, x-size+5, y-5, BLACK);
        inkplate.drawLine(x-size, y, x-size+5, y+5, BLACK);
        break;
      case 3: // up
        inkplate.drawLine(x, y, x, y-size, BLACK);
        inkplate.drawLine(x, y-size, x-5, y-size+5, BLACK);
        inkplate.drawLine(x, y-size, x+5, y-size+5, BLACK);
        break;
      case 4: // down-right diagonal
        inkplate.drawLine(x, y, x+size, y+size, BLACK);
        inkplate.drawLine(x+size, y+size, x+size-5, y+size-5, BLACK);
        inkplate.drawLine(x+size, y+size, x+size-5, y+size+5, BLACK);
        break;
      case 5: // down-left diagonal
        inkplate.drawLine(x, y, x-size, y+size, BLACK);
        inkplate.drawLine(x-size, y+size, x-size+5, y+size-5, BLACK);
        inkplate.drawLine(x-size, y+size, x-size+5, y+size+5, BLACK);
        break;
      case 6: // up-right diagonal
        inkplate.drawLine(x, y, x+size, y-size, BLACK);
        inkplate.drawLine(x+size, y-size, x+size-5, y-size+5, BLACK);
        inkplate.drawLine(x+size, y-size, x+size-5, y-size-5, BLACK);
        break;
      case 7: // up-left diagonal
        inkplate.drawLine(x, y, x-size, y-size, BLACK);
        inkplate.drawLine(x-size, y-size, x-size+5, y-size+5, BLACK);
        inkplate.drawLine(x-size, y-size, x-size+5, y-size-5, BLACK);
        break;
    }
  };

  // Draw Grid box (top left)
  inkplate.drawRect(gridBoxX, gridBoxY, outerBoxWidth, boxHeight, BLACK);
  inkplate.setCursor(gridBoxX + 10, gridBoxY + 10);
  inkplate.print("Sit");  // Grid

  int gridArrowX = gridBoxX + outerBoxWidth/2;
  int gridArrowY = gridBoxY + 30;
  if (gridPower < 0) {
    // Exporting to grid (power flowing out)
    drawArrow(gridArrowX, gridArrowY, 2);  // left arrow (out to grid)
    inkplate.setCursor(gridBoxX + 10, gridBoxY + 40);
    inkplate.print(abs(gridPower), 0);
    inkplate.print(" W");
    inkplate.setCursor(gridBoxX + 10, gridBoxY + 60);
    inkplate.print("Vyvoz");  // Export
  } else {
    // Importing from grid (power flowing in)
    drawArrow(gridArrowX, gridArrowY, 0);  // right arrow (in from grid)
    inkplate.setCursor(gridBoxX + 10, gridBoxY + 40);
    inkplate.print(gridPower, 0);
    inkplate.print(" W");
    inkplate.setCursor(gridBoxX + 10, gridBoxY + 60);
    inkplate.print("Dovoz");  // Import
  }

  // Draw AC Loads box (top right)
  inkplate.drawRect(loadBoxX, loadBoxY, outerBoxWidth, boxHeight, BLACK);
  inkplate.setCursor(loadBoxX + 10, loadBoxY + 10);
  inkplate.print("Zatez");  // Load
  // Right arrow shows power flowing to loads
  drawArrow(loadBoxX + outerBoxWidth/2, loadBoxY + 30, 0);
  inkplate.setCursor(loadBoxX + 10, loadBoxY + 40);
  inkplate.print(acLoads, 0);
  inkplate.print(" W");

  // Draw System box (middle center)
  inkplate.drawRect(systemBoxX, systemBoxY, centerBoxWidth, boxHeight, BLACK);
  inkplate.setCursor(systemBoxX + 10, systemBoxY + 10);
  inkplate.print("System stav");  // System state
  inkplate.setCursor(systemBoxX + 10, systemBoxY + 40);

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
  inkplate.drawRect(batteryBoxX, batteryBoxY, outerBoxWidth, boxHeight, BLACK);
  inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 10);
  inkplate.print("Baterie");  // Battery

  int batteryArrowX = batteryBoxX + outerBoxWidth/2;
  int batteryArrowY = batteryBoxY + 30;
  if (abs(batteryPower) < 10) {
    inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
    inkplate.print(currentDashboardData.batteryState);
  } else {
    if (batteryPower > 0) {
      // Charging - arrow pointing up (into system)
      drawArrow(batteryArrowX, batteryArrowY, 3);
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
      inkplate.print("Nabijeni");
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 60);
      inkplate.print(abs(batteryPower), 0);
      inkplate.print(" W");
    } else {
      // Discharging - arrow pointing down (out to system)
      drawArrow(batteryArrowX, batteryArrowY, 1);
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 40);
      inkplate.print("Vybijeni");
      inkplate.setCursor(batteryBoxX + 10, batteryBoxY + 60);
      inkplate.print(abs(batteryPower), 0);
      inkplate.print(" W");
    }
  }
  inkplate.setCursor(batteryBoxX + outerBoxWidth - 60, batteryBoxY + 10);
  inkplate.print(batterySOC, 1);
  inkplate.print("%");

  // Draw PV Charger box (bottom right)
  inkplate.drawRect(pvBoxX, pvBoxY, outerBoxWidth, boxHeight, BLACK);
  inkplate.setCursor(pvBoxX + 10, pvBoxY + 10);
  inkplate.print("Solarni nabijec");  // Solar charger
  // Up arrow shows power flowing into system
  drawArrow(pvBoxX + outerBoxWidth/2, pvBoxY + 30, 3);
  inkplate.setCursor(pvBoxX + 10, pvBoxY + 40);
  inkplate.print(pvPower, 0);
  inkplate.print(" W");

  // Calculate center points of each box for arrow positioning
  int gridCenterX = gridBoxX + outerBoxWidth/2;
  int gridCenterY = gridBoxY + boxHeight/2;
  int loadCenterX = loadBoxX + outerBoxWidth/2;
  int loadCenterY = loadBoxY + boxHeight/2;
  int systemCenterX = systemBoxX + centerBoxWidth/2;
  int systemCenterY = systemBoxY + boxHeight/2;
  int batteryCenterX = batteryBoxX + outerBoxWidth/2;
  int batteryCenterY = batteryBoxY + boxHeight/2;
  int pvCenterX = pvBoxX + outerBoxWidth/2;
  int pvCenterY = pvBoxY + boxHeight/2;

  // Draw arrows between components to show power flow

  // 1. Grid to System (if importing)
  if (gridPower > 10) {
    // From grid (left) to system (center)
    inkplate.drawLine(gridBoxX + outerBoxWidth, gridCenterY,
                     systemBoxX, systemCenterY, BLACK);
    drawArrow((gridBoxX + outerBoxWidth + systemBoxX)/2,
              (gridCenterY + systemCenterY)/2, 4);  // down-right arrow
  }

  // 2. System to Grid (if exporting)
  if (gridPower < -10) {
    // From system (center) to grid (left)
    inkplate.drawLine(systemBoxX, systemCenterY,
                     gridBoxX + outerBoxWidth, gridCenterY, BLACK);
    drawArrow((gridBoxX + outerBoxWidth + systemBoxX)/2,
              (gridCenterY + systemCenterY)/2, 6);  // up-right arrow (but actually down-left from system)
    // Actually should be up-left diagonal (7) because we're going from system to grid (up and left)
    // Redraw line and arrow with correct direction
    inkplate.drawLine(systemBoxX, systemCenterY,
                     gridBoxX + outerBoxWidth, gridCenterY, BLACK);
    drawArrow((gridBoxX + outerBoxWidth + systemBoxX)/2,
              (gridCenterY + systemCenterY)/2, 7);  // up-left arrow
  }

  // 3. PV to System (always when PV is producing)
  if (pvPower > 10) {
    // From PV (bottom right) to system (middle center)
    inkplate.drawLine(pvCenterX, pvBoxY,  // Start at top of PV box
                     systemCenterX, systemBoxY + boxHeight,  // End at bottom of system box
                     BLACK);
    drawArrow((pvCenterX + systemCenterX)/2,
              (pvBoxY + systemBoxY + boxHeight)/2, 5);  // down-left arrow
    // Actually should be up-left diagonal (7) because we're going from PV to system (up and left)
    // Redraw line and arrow with correct direction
    inkplate.drawLine(pvCenterX, pvBoxY,
                     systemCenterX, systemBoxY + boxHeight, BLACK);
    drawArrow((pvCenterX + systemCenterX)/2,
              (pvBoxY + systemBoxY + boxHeight)/2, 7);  // up-left arrow
  }

  // 4. Battery to System (if discharging) or System to Battery (if charging)
  if (batteryPower < -10) {
    // From battery (bottom left) to system (middle center)
    inkplate.drawLine(batteryCenterX, batteryBoxY,
                     systemCenterX, systemBoxY + boxHeight, BLACK);
    drawArrow((batteryCenterX + systemCenterX)/2,
              (batteryBoxY + systemBoxY + boxHeight)/2, 6);  // up-right arrow
  } else if (batteryPower > 10) {
    // From system (middle center) to battery (bottom left)
    inkplate.drawLine(systemCenterX, systemBoxY + boxHeight,
                     batteryCenterX, batteryBoxY, BLACK);
    drawArrow((systemCenterX + batteryCenterX)/2,
              (systemBoxY + boxHeight + batteryBoxY)/2, 4);  // down-right arrow
  }

  // 5. System to Load (always when loads are active)
  if (acLoads > 10) {
    // From system (middle center) to load (top right)
    inkplate.drawLine(systemBoxX + centerBoxWidth, systemCenterY,
                     loadBoxX, loadCenterY, BLACK);
    drawArrow((systemBoxX + centerBoxWidth + loadBoxX)/2,
              (systemCenterY + loadCenterY)/2, 4);  // down-right arrow
    // Actually should be up-right diagonal (6) because we're going from system to load (up and right)
    // Redraw line and arrow with correct direction
    inkplate.drawLine(systemBoxX + centerBoxWidth, systemCenterY,
                     loadBoxX, loadCenterY, BLACK);
    drawArrow((systemBoxX + centerBoxWidth + loadBoxX)/2,
              (systemCenterY + loadCenterY)/2, 6);  // up-right arrow
  }

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
  int displayWidth = 1200;  // Inkplate 10 width
  int displayHeight = 825;  // Inkplate 10 height
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
  inkplate.print("Sit (vstup/pretoky) [kWh / h]");

  // Add title and axis labels
  inkplate.setTextSize(3);
  inkplate.setCursor(graphX + graphWidth / 2 - 100, graphY - 30);
  inkplate.print("Solaary");
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
// Main update function
void runDataUpdate() {
  Serial.println("Running data update...");
  inkplate.print("\nUpdating data...");
  inkplate.partialUpdate(true);
  ensureWiFiConnected();
  // Get token
  String token = getVictronToken();
  if (token == "") {
    Serial.println("Failed to get Victron token");
    inkplate.print("\nFailed to get token");
    inkplate.partialUpdate(true);
    return;
  }
  // Initialize dashboard data
  currentDashboardData = DashboardData();
  // Get data from all relevant endpoints
  String statusData = getEndpointData(token, "/widgets/Status", "276");
  if (statusData != "") {
    parseStatusData(statusData);
  }
  String batterySummaryData = getEndpointData(token, "/widgets/BatterySummary", "512");
  if (batterySummaryData != "") {
    parseBatterySummaryData(batterySummaryData);
  }
  // Get data from both solar chargers (instances 278 and 279)
  String solarCharger1Data = getEndpointData(token, "/widgets/SolarChargerSummary", "278");
  if (solarCharger1Data != "") {
    parseSolarChargerSummaryData(solarCharger1Data);
  }
  String solarCharger2Data = getEndpointData(token, "/widgets/SolarChargerSummary", "279");
  if (solarCharger2Data != "") {
    // For the second solar charger, we'll just add its power to the total PV power
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
    }
  }
  // Get stats data for the graph
  String statsData = getEndpointData(token, "/stats");
  if (statsData != "") {
    JsonDocument batteryDoc;
    DeserializationError batteryError = deserializeJson(batteryDoc, statsData);
    if (!batteryError) {
      if (!batteryDoc["records"].containsKey("bs")) {
        Serial.println("Error: 'bs' not found in records");
        inkplate.print("\nError: Missing 'bs'");
        inkplate.partialUpdate(true);
        return;
      }

      JsonArray bsArray = batteryDoc["records"]["bs"];
      int numPoints = bsArray.size();

      if (numPoints == 0) {
        Serial.println("Error: No data points in bs array");
        inkplate.print("\nError: No data points");
        inkplate.partialUpdate(true);
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
        Serial.println("Warning: 'Pdc' not found in records");
        inkplate.print("\nWarning: No solar data");
        inkplate.partialUpdate(true);
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

      // Get grid power from the latest grid data for dashboard
      if (actualNumPoints > 0) {
        float lastGridIn = gridInValues[actualNumPoints - 1];
        float lastGridOut = gridOutValues[actualNumPoints - 1];
        currentDashboardData.gridPower = lastGridIn - lastGridOut;

        if (actualNumPoints > 0) {
          drawCombinedGraph(percentages, timestamps, actualNumPoints,
                            solarValues, gridInValues, gridOutValues, 2);

          // Draw the dashboard below the graph
          drawDashboard(
            currentDashboardData.gridPower,
            currentDashboardData.acLoads,
            currentDashboardData.batteryPower,
            currentDashboardData.pvPower,
            currentDashboardData.systemState,
            currentDashboardData.batterySOC);
        } else {
          Serial.println("Error: No valid data points to display");
          inkplate.print("\nError: No data");
          inkplate.partialUpdate(true);
        }
      }
    } else {
      Serial.println("JSON parsing error:");
      Serial.println(batteryError.c_str());
      inkplate.print("\nFailed to parse data");
      inkplate.partialUpdate(true);
    }
  } else {
    inkplate.print("\nNo data received");
    inkplate.partialUpdate(true);
  }
}
void setup() {
  inkplate.begin();
  inkplate.clearDisplay();
  inkplate.display();
  Serial.begin(9600);
  Serial.println("Starting Victron display...");
  // Connect to WiFi
  WiFi.begin(ssid, pass);
  inkplate.print("Connecting to WiFi...");
  inkplate.partialUpdate(true);
  unsigned long wifiStartTime = millis();
  const unsigned long wifiTimeout = 30000;  // 30 second timeout
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    inkplate.print('.');
    inkplate.partialUpdate(true);
    if (millis() - wifiStartTime > wifiTimeout) {
      Serial.println("WiFi connection timeout");
      inkplate.print("\nWiFi timeout");
      inkplate.partialUpdate(true);
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    inkplate.println("\nWiFi connected");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    inkplate.println("\nWiFi failed");
    Serial.println("WiFi connection failed");
  }
  inkplate.partialUpdate(true);
  // Run initial data update
  runDataUpdate();
  lastUpdateTime = millis();
}
void loop() {
  // Check if it's time for an update
  unsigned long currentTime = millis();
  // Handle millis() overflow (every ~50 days)
  if (currentTime < lastUpdateTime) {
    lastUpdateTime = 0;
  }
  if ((currentTime - lastUpdateTime) >= UPDATE_INTERVAL) {
    lastUpdateTime = currentTime;
    runDataUpdate();
  }
  // Small delay to prevent watchdog issues
  delay(1000);
}
