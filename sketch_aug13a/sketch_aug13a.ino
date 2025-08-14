#include "Inkplate.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

Inkplate inkplate(INKPLATE_1BIT);

String currentToken = "";
unsigned long tokenTimestamp = 0;
const unsigned long TOKEN_VALIDITY_PERIOD = 43200000; // 12 hours in milliseconds

#define BLACK 1
#define WHITE 0
#define MAX_POINTS 100  // Maximum number of points to plot

// Helper function to format a timestamp into a time string
String formatTime(uint64_t timestampMs, int timezoneOffsetHours = 0) {
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

// Modified function to draw the combined graph with grid in/out as a single bar
void drawCombinedGraph(
  float percentages[], uint64_t timestamps[], int numPoints,
  float solarValues[], float gridInValues[], float gridOutValues[],
  int timezoneOffset = 0) {

  // Clear the display for the graph
  inkplate.clearDisplay();

  // Define graph dimensions and margins
  int margin = 70;
  int displayWidth = 1200;  // Inkplate 10 width
  int displayHeight = 825;  // Inkplate 10 height
  int graphWidth = displayWidth - 2 * margin;
  int graphHeight = 400;
  int graphX = margin;
  int graphY = margin;

  // Calculate scaling for the right y-axis (power in kW)
  // Find maximum values for each metric to determine y-axis scale
  float maxSolar = findMax(solarValues, numPoints);
  float maxGridIn = findMax(gridInValues, numPoints);
  float maxGridOut = findMax(gridOutValues, numPoints);

  // For the grid, we need to consider both positive (in) and negative (out) values
  // So our max should be the larger of maxGridIn or maxGridOut
  float gridMax = max(maxGridIn, maxGridOut);

  // The right y-axis will scale based on the maximum of solar and grid values
  float rightAxisMax = max(maxSolar, gridMax);
  // Round up to a nice number
  if (rightAxisMax <= 5) rightAxisMax = 5;
  else if (rightAxisMax <= 10) rightAxisMax = 10;
  else if (rightAxisMax <= 20) rightAxisMax = 20;
  else rightAxisMax = ceil(rightAxisMax / 10) * 10;

  // Draw graph axes
  inkplate.drawLine(graphX, graphY + graphHeight, graphX + graphWidth, graphY + graphHeight, BLACK);  // X-axis
  inkplate.drawLine(graphX, graphY, graphX, graphY + graphHeight, BLACK);                             // Left Y-axis (battery %)

  // Right y-axis (for kW values)
  int rightAxisX = graphX + graphWidth;
  inkplate.drawLine(rightAxisX, graphY, rightAxisX, graphY + graphHeight, BLACK);

  // Draw left y-axis labels (0%, 25%, 50%, 75%, 100%) - Battery %
  inkplate.setTextSize(1);
  for (int i = 0; i <= 100; i += 25) {
    int yPos = graphY + graphHeight - (i * graphHeight / 100);
    inkplate.drawLine(graphX - 3, yPos, graphX, yPos, BLACK);  // Small tick mark
    inkplate.setCursor(graphX - 30, yPos - 3);
    inkplate.print(i);
    inkplate.print("%");
  }

  // Draw right y-axis labels (for kW values)
  // Since we have both positive and negative values, we'll center the axis at zero
  // The axis will range from -rightAxisMax to +rightAxisMax
  int numRightLabels = 10;
  for (int i = 0; i <= numRightLabels; i++) {
    float labelValue = (i * 2 * rightAxisMax) / numRightLabels - rightAxisMax;  // From -max to +max
    int yPos = graphY + graphHeight - (i * graphHeight) / numRightLabels;
    inkplate.drawLine(rightAxisX, yPos, rightAxisX + 3, yPos, BLACK);  // Small tick mark
    inkplate.setCursor(rightAxisX + 10, yPos - 3);
    inkplate.print(labelValue, 1);  // 1 decimal place
    inkplate.print("kW");
  }

  // Draw x-axis labels (times)
  for (int i = 0; i < numPoints; i += max(1, numPoints / 10)) {
    int xPos = graphX + (i * graphWidth) / (numPoints - 1);
    String timeStr = formatTime(timestamps[i], timezoneOffset);
    inkplate.setCursor(xPos - 20, graphY + graphHeight + 10);
    inkplate.print(timeStr);
  }

  // Draw the battery percentage line graph (on left side)
  for (int i = 0; i < numPoints - 1; i++) {
    int x1 = graphX + (i * graphWidth) / (numPoints - 1);
    int y1 = graphY + graphHeight - (percentages[i] * graphHeight / 100);
    int x2 = graphX + ((i + 1) * graphWidth) / (numPoints - 1);
    int y2 = graphY + graphHeight - (percentages[i + 1] * graphHeight / 100);
    inkplate.drawLine(x1, y1, x2, y2, BLACK);
  }

  // Calculate bar widths
  int barWidth = max(5, min(20, graphWidth / numPoints / 4));  // Narrower bars to fit both solar and grid
  int barSpacing = barWidth + 2;                               // Space between bars

  // Create a combined grid value array where:
  // positive values = grid in (consumption from grid)
  // negative values = grid out (overflow to grid)
  float gridValues[MAX_POINTS];
  for (int i = 0; i < numPoints; i++) {
    // Grid in is positive, grid out is negative
    gridValues[i] = gridInValues[i] - gridOutValues[i];
  }

  // Find the maximum absolute grid value for scaling
  float maxAbsGrid = 0;
  for (int i = 0; i < numPoints; i++) {
    float absValue = abs(gridValues[i]);
    if (absValue > maxAbsGrid) {
      maxAbsGrid = absValue;
    }
  }
  // Update our right axis max if needed
  if (maxAbsGrid > rightAxisMax) {
    rightAxisMax = maxAbsGrid;
    // Round up again
    if (rightAxisMax <= 5) rightAxisMax = 5;
    else if (rightAxisMax <= 10) rightAxisMax = 10;
    else if (rightAxisMax <= 20) rightAxisMax = 20;
    else rightAxisMax = ceil(rightAxisMax / 10) * 10;
  }

  // Draw bars for each metric at each time point
  for (int i = 0; i < numPoints; i++) {
    int xPosBase = graphX + (i * graphWidth) / (numPoints - 1);

    // Position for solar bar (left half)
    int solarXPos = xPosBase - barWidth - 2;
    if (solarXPos < graphX) solarXPos = graphX;

    // Position for grid bar (right half)
    int gridXPos = xPosBase + 2;

    // Draw solar production bars (solid fill) - always positive
    if (solarValues[i] > 0) {
      int barHeight = (solarValues[i] / rightAxisMax) * graphHeight;
      int yPos = graphY + graphHeight - barHeight;
      inkplate.drawRect(solarXPos, yPos, barWidth, barHeight, BLACK);
      // Fill with horizontal lines (solid fill)
      for (int y = yPos; y < yPos + barHeight; y += 2) {
        inkplate.drawLine(solarXPos, y, solarXPos + barWidth, y, BLACK);
      }
    }

    // Draw combined grid bar
    // The height depends on whether it's positive (grid in) or negative (grid out)
    float gridValue = gridValues[i];
    if (gridValue != 0) {
      int barHeight;
      int yPos;

      if (gridValue > 0) {  // Grid consumption (positive)
        barHeight = (gridValue / rightAxisMax) * graphHeight;
        yPos = graphY + graphHeight - barHeight;
      } else {  // Grid overflow (negative)
        barHeight = (abs(gridValue) / rightAxisMax) * graphHeight;
        yPos = graphY + graphHeight;  // Starts at baseline and goes downward
      }

      // Draw the grid bar
      if (gridValue > 0) {
        // Positive grid value (consumption) - extends upward
        inkplate.drawRect(gridXPos, yPos, barWidth, barHeight, BLACK);
        // Fill with diagonal hatching
        for (int y = yPos; y < yPos + barHeight; y += 3) {
          inkplate.drawLine(gridXPos, y, gridXPos + barWidth, y + 3, BLACK);
        }
      } else {
        // Negative grid value (overflow) - extends downward
        inkplate.drawRect(gridXPos, yPos, barWidth, barHeight, BLACK);
        // Fill with vertical lines
        for (int x = gridXPos; x < gridXPos + barWidth; x += 2) {
          inkplate.drawLine(x, yPos, x, yPos + barHeight, BLACK);
        }
      }
    }
  }

  // Add legend
  int legendX = graphX + graphWidth + 20;
  int legendY = graphY;

  // Battery line legend
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Battery:");
  inkplate.drawLine(legendX - 10, legendY + 5, legendX - 10 + 20, legendY + 5, BLACK);
  legendY += 20;

  // Solar legend
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Solar:");
  inkplate.drawRect(legendX - 15, legendY - 8, 20, 10, BLACK);
  for (int y = legendY - 8; y < legendY - 8 + 10; y += 2) {
    inkplate.drawLine(legendX - 15, y, legendX - 15 + 20, y, BLACK);
  }
  legendY += 20;

  // Grid legend
  inkplate.setCursor(legendX, legendY);
  inkplate.print("Grid:");
  // Draw sample grid in bar (positive)
  inkplate.drawRect(legendX - 15, legendY - 8, 20, 10, BLACK);
  for (int y = legendY - 8; y < legendY + 2; y += 3) {
    inkplate.drawLine(legendX - 15, y, legendX - 15 + 20, y + 3, BLACK);
  }
  // Draw sample grid out bar (negative) below
  inkplate.drawRect(legendX - 15, legendY + 10, 20, 10, BLACK);
  for (int x = legendX - 15; x < legendX + 5; x += 2) {
    inkplate.drawLine(x, legendY + 10, x, legendY + 20, BLACK);
  }

  legendY += 30;  // Skip down past the sample bars

  inkplate.setCursor(legendX + 5, legendY - 20);
  inkplate.print("= In (above)");
  legendY += 10;
  inkplate.setCursor(legendX + 5, legendY - 10);
  inkplate.print("= Out (below)");
  legendY += 20;

  // Add title and axis labels
  inkplate.setTextSize(2);
  inkplate.setCursor(graphX + graphWidth / 2 - 100, graphY - 20);
  inkplate.print("Solaary");

  inkplate.setTextSize(1);
  inkplate.setCursor(graphX + graphWidth / 2 - 20, graphY + graphHeight + 30);
  inkplate.print("Time");

  // Label Y axes
  inkplate.setCursor(graphX - 25, graphY + graphHeight / 2);
  inkplate.print("Battery %");
  inkplate.setCursor(rightAxisX + 20, graphY + graphHeight / 2);
  inkplate.print("Power (kW)");
  inkplate.setCursor(rightAxisX + 20, graphY + graphHeight / 2 + 15);
  inkplate.print("(+in, -out)");

  // Display the graph
  inkplate.display();
}

// Function to authenticate and get the token
String getVictronToken() {
  HTTPClient http;
  String token = "";

  ensureWiFiConnected();

  String payload = "{\"username\":\"" + String(victronUsername) + "\",\"password\":\"" + String(victronPassword) + "\"}";
  String url = "https://vrmapi.victronenergy.com/v2/auth/login";

  Serial.println("Attempting to connect to Victron API...");

  // Begin HTTP connection with timeout
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");
  http.setTimeout(10000);  // 10 second timeout

  int httpResponseCode = http.POST(payload);

  // Check HTTP response
  if (httpResponseCode <= 0) {
    Serial.print("HTTP error: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: ";
    errorMsg += httpResponseCode;
    errorMsg += " (";
    errorMsg += http.errorToString(httpResponseCode).c_str();
    errorMsg += ")";
    Serial.println(errorMsg);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    http.end();
    return token;
  }

  // Check if we got a successful response
  if (httpResponseCode != HTTP_CODE_OK) {
    Serial.print("Unexpected HTTP response: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: ";
    errorMsg += httpResponseCode;
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);

    // Try to get error message from response
    String response = http.getString();
    Serial.println("Response: " + response);

    // Display first 50 chars of response if available
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

  // If we got here, we have a successful response
  String response = http.getString();
  http.end();

  // Parse JSON response
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    Serial.print("JSON parsing error: ");
    Serial.println(error.c_str());
    String errorMsg = "JSON Error: ";
    errorMsg += error.c_str();
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    return token;
  }

  // Check if token exists in response
  if (doc.containsKey("token")) {
    token = doc["token"].as<String>();
    Serial.println("Token received successfully");
    return token;
  } else {
    Serial.println("No token found in response");
    inkplate.print("\nNo token in response");

    // Try to get error message from response
    if (doc.containsKey("message")) {
      String errorMsg = doc["message"].as<String>();
      Serial.println("Error message: " + errorMsg);
      inkplate.print("\nError: " + errorMsg);
    }

    inkplate.partialUpdate(true);
    return token;
  }
}

// Function to get battery data with improved error handling
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
  http.setTimeout(15000);  // 15 second timeout for potentially large responses

  int httpResponseCode = http.GET();

  // Check HTTP response
  if (httpResponseCode <= 0) {
    Serial.print("HTTP error: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: ";
    errorMsg += httpResponseCode;
    errorMsg += " (";
    errorMsg += http.errorToString(httpResponseCode).c_str();
    errorMsg += ")";
    Serial.println(errorMsg);
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);
    http.end();
    return batteryData;
  }

  // Check if we got a successful response
  if (httpResponseCode != HTTP_CODE_OK) {
    Serial.print("Unexpected HTTP response: ");
    Serial.println(httpResponseCode);
    String errorMsg = "HTTP Error: ";
    errorMsg += httpResponseCode;
    inkplate.print("\n" + errorMsg);
    inkplate.partialUpdate(true);

    // Try to get error message from response
    String response = http.getString();
    Serial.println("Response: " + response);

    // Display first 50 chars of response if available
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

  // If we got here, we have a successful response
  batteryData = http.getString();
  http.end();

  // Check if response is empty
  if (batteryData.length() == 0) {
    Serial.println("Empty response received");
    inkplate.print("\nEmpty response");
    inkplate.partialUpdate(true);
    return "";
  }

  Serial.println("Battery data received successfully");
  return batteryData;
}

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


void setup() {
  inkplate.begin();
  inkplate.clearDisplay();
  inkplate.display();
  Serial.begin(9600);  // must be 9600 for arduino IDE to understand
  Serial.println("Starting Victron display...");

  ensureWiFiConnected();

  // Retry logic for getting token
  int maxRetries = 3;
  int retryDelay = 5000;  // 5 seconds between retries
  String token = "";

  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Serial.print("Attempt ");
    Serial.print(attempt);
    Serial.println(" to get Victron token");

    inkplate.print("\nGetting token (attempt ");
    inkplate.print(attempt);
    inkplate.print("/");
    inkplate.print(maxRetries);
    inkplate.print(")");
    inkplate.partialUpdate(true);

    token = getVictronToken();

    if (token != "") {
      Serial.println("Successfully got token");
      inkplate.print("\nGot token");
      inkplate.partialUpdate(true);
      break;
    }

    if (attempt < maxRetries) {
      Serial.print("Waiting ");
      Serial.print(retryDelay / 1000);
      Serial.println(" seconds before retry");
      inkplate.print("\nRetry in ");
      inkplate.print(retryDelay / 1000);
      inkplate.print(" secs");
      inkplate.partialUpdate(true);
      delay(retryDelay);
    }
  }

  if (token == "") {
    Serial.println("Failed to get Victron token after retries");
    inkplate.print("\nFailed to get token");
    inkplate.partialUpdate(true);
    // Continue anyway to show what we can
  }

  // Retry logic for getting battery data
  String batteryData = "";
  maxRetries = 3;
  retryDelay = 5000;

  if (token != "") {
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
        Serial.print("Waiting ");
        Serial.print(retryDelay / 1000);
        Serial.println(" seconds before retry");
        inkplate.print("\nRetry in ");
        inkplate.print(retryDelay / 1000);
        inkplate.print(" secs");
        inkplate.partialUpdate(true);
        delay(retryDelay);
      }
    }
  }

  if (batteryData != "") {
    // Parse the battery data JSON
    JsonDocument batteryDoc;
    DeserializationError batteryError = deserializeJson(batteryDoc, batteryData);

    if (!batteryError) {
      // Check if required data exists in the response
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

      // Continue with your existing data processing code...
      float percentages[MAX_POINTS];
      uint64_t timestamps[MAX_POINTS];
      float solarValues[MAX_POINTS];
      float gridInValues[MAX_POINTS];
      float gridOutValues[MAX_POINTS];

      // Initialize arrays
      for (int i = 0; i < MAX_POINTS; i++) {
        solarValues[i] = 0;
        gridInValues[i] = 0;
        gridOutValues[i] = 0;
      }

      // Determine if we need to downsample
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
          if (bsEntry.size() >= 2) {  // Ensure we have at least timestamp and value
            timestamps[pointIndex] = bsEntry[0].as<uint64_t>();
            percentages[pointIndex] = bsEntry[1].as<float>();
          }
        }
      }

      // Safely extract other metrics
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

      // Similar safe extraction for grid data...
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

      // Draw the graph if we have valid data
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

  Serial.println("Setup completed");
}

void loop() {
  // Empty loop
}