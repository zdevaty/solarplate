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

// Forward declarations
String formatTime(uint64_t timestampMs, int timezoneOffsetHours);
float findMax(float array[], int length);
void drawCombinedGraph(float percentages[], uint64_t timestamps[], int numPoints,
                       float solarValues[], float gridInValues[], float gridOutValues[],
                       int timezoneOffset);
String getVictronToken();
String getBatteryData(String token);
void ensureWiFiConnected();
void runDataUpdate();

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

// Function to get battery data
String getBatteryData(String token) {
  HTTPClient http;
  String batteryData = "";
  String url = "https://vrmapi.victronenergy.com/v2/installations/" + String(installationId) + "/stats/battery";

  ensureWiFiConnected();

  if (token == "") {
    Serial.println("Error: No token provided");
    inkplate.print("\nError: No token");
    inkplate.partialUpdate(true);
    return batteryData;
  }

  Serial.println("Attempting to get battery data...");
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
    return batteryData;
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
    return batteryData;
  }

  batteryData = http.getString();
  http.end();

  if (batteryData.length() == 0) {
    Serial.println("Empty response received");
    inkplate.print("\nEmpty response");
    inkplate.partialUpdate(true);
    return "";
  }

  Serial.println("Battery data received successfully");
  return batteryData;
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

// Function to draw the combined graph
void drawCombinedGraph(float percentages[], uint64_t timestamps[], int numPoints,
                       float solarValues[], float gridInValues[], float gridOutValues[],
                       int timezoneOffset) {
  // Clear the display for the graph
  inkplate.clearDisplay();

  // Define graph dimensions and margins
  int margin = 43;
  int displayWidth = 1200;                          // Inkplate 10 width
  int displayHeight = 825;                          // Inkplate 10 height
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
    inkplate.setCursor(graphX - 40, yPos - 5 );
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

  // Display the graph
  inkplate.display();
}

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

  // Get battery data with retries
  String batteryData = "";
  int maxRetries = 3;
  int retryDelay = 5000;

  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Serial.print("Attempt ");
    Serial.print(attempt);
    Serial.println(" to get battery data");
    inkplate.print("\nGetting data (attempt ");
    inkplate.print(attempt);
    inkplate.print("/");
    inkplate.print(maxRetries);
    inkplate.print(")");
    inkplate.partialUpdate(true);

    batteryData = getBatteryData(token);
    if (batteryData != "") {
      Serial.println("Successfully got battery data");
      inkplate.print("\nGot data");
      inkplate.partialUpdate(true);
      break;
    }
    if (attempt < maxRetries) {
      delay(retryDelay);
    }
  }

  if (batteryData != "") {
    JsonDocument batteryDoc;
    DeserializationError batteryError = deserializeJson(batteryDoc, batteryData);

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

      if (actualNumPoints > 0) {
        drawCombinedGraph(percentages, timestamps, actualNumPoints,
                          solarValues, gridInValues, gridOutValues, 2);
      } else {
        Serial.println("Error: No valid data points to display");
        inkplate.print("\nError: No data");
        inkplate.partialUpdate(true);
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
