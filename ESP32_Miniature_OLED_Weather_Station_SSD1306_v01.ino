/*
This software, the ideas and concepts is Copyright (c) David Bird 2019 and beyond.
All rights to this software are reserved.
It is prohibited to redistribute or reproduce of any part or all of the software contents in any form other than the following:
 1. You may print or download to a local hard disk extracts for your personal and non-commercial use only.
 2. You may copy the content to individual third parties for their personal use, but only if you acknowledge the author David Bird as the source of the material.
 3. You may not, except with my express written permission, distribute or commercially exploit the content.
 4. You may not transmit it or store it in any other website or other form of electronic retrieval system for commercial purposes.
 5. You MUST include all of this copyright and permission notice ('as annotated') and this shall be included in all copies or substantial portions of the software and where the software use is visible to an end-user.
 
THE SOFTWARE IS PROVIDED "AS IS" FOR PRIVATE USE ONLY, IT IS NOT FOR COMMERCIAL USE IN WHOLE OR PART OR CONCEPT.
FOR PERSONAL USE IT IS SUPPLIED WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHOR OR COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "SSD1306.h"               // See https://github.com/squix78/esp8266-oled-ssd1306 or via Sketch/Include Library/Manage Libraries 'ESP32 and ESP8266 OLED driver'
SSD1306 display(0x3c, SDA, SCL);   // OLED display object definition (address, SDA, SCL)

Adafruit_BME280 bme;              // I2C

// Change to your WiFi credentials and select your time zone
const char* ssid     = "your_SSID";
const char* password = "your_PASSWORD";
const char* Timezone = "GMT0BST,M3.5.0/01,M10.5.0/02";  // UK, see below for others and link to database
String      Format   = "X";       // Time format M for dd-mm-yy and 23:59:59, "I" for mm-dd-yy and 12:59:59 PM, "X" for Metric units but WSpeed in MPH

//Calibration factors, extent of wind speed average, and Wind Sensor pin adjust as necessary
#define pressure_offset 3.5       // Air pressure calibration, adjust for your altitude
#define WS_Calibration  1.1       // Wind Speed calibration factor
#define WS_Samples      10        // Number of Wind Speed samples for an average
#define WindSensorPin   4         // Only use pins that can support an interrupt

static String         Date_str, Time_str;
volatile unsigned int local_Unix_time = 0, next_update_due = 0;
volatile unsigned int update_duration = 60 * 60; // Time duration in seconds, so synchronise every hour
static float          bme_temp, bme_humi, bme_pres, WindSpeed;
static unsigned int   Last_Event_Time;
float WSpeedReadings[WS_Samples]; // To hold readings from the Wind Speed Sensor
int   WS_Samples_Index = 0;       // The index of the current wind speed reading
float WS_Total         = 0;       // The running wind speed total
float WS_Average       = 0;       // The wind speed average

//#########################################################################################
void IRAM_ATTR MeasureWindSpeed_ISR() {
  portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL_ISR(&timerMux);
  Last_Event_Time = millis();    // Record current time for next event calculations
  portEXIT_CRITICAL_ISR(&timerMux);
}
//#########################################################################################
void IRAM_ATTR Timer_TImeout_ISR() {
  portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL_ISR(&timerMux);
  local_Unix_time++;
  portEXIT_CRITICAL_ISR(&timerMux);
}
//#########################################################################################
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA, SCL, 100000);                  // (sda,scl,bus speed) Start the Wire service for the OLED display using assigned pins for SCL and SDA at 100KHz
  bool status = bme.begin(0x76);                 // For Adafruit sensors use address 0x77, for most 3rd party types use address 0x76
  if (!status) Serial.println("Could not find a valid BME280 sensor, check wiring!"); // Check for a sensor
  display.init();                                // Initialise the display
  display.flipScreenVertically();                // In my case flip the screen around by 180°
  display.setContrast(128);                      // If you want turn the display contrast down, 255 is maxium and 0 in minimum, in practice about 128 is OK
  StartWiFi();
  Start_Time_Services();
  Setup_Interrupts_and_Initialise_Clock();       // Now setup a timer interrupt to occur every 1-second, to keep seconds accurate
  for (int index = 0; index < WS_Samples; index++) { // Now clear the Wind Speed average array
    WSpeedReadings[index] = 0;
  }
}
//#########################################################################################
void loop() {
  UpdateLocalTime();     // The variables 'Date_str' and 'Time_str' now have current date-time values
  BME280_Read_Sensor();  // The variables 'bme_temp', 'bme_humi', 'bme_pres' now have current values
  display.clear();
  display.drawString(0, 0, Date_str);                                                                                    // Display current date
  display.drawString((Format=="I"?68:85), 0, Time_str);                                                                  // Adjust position for addition of AM/PM indicator if required
  display.drawLine(0,12,128,12);                                                                                         // Draw a line to seperate date and time section
  display.setFont(ArialMT_Plain_16);                                                                                     // Set the Font size larger
  display.drawString(2, 20, String(bme_temp, 1)+"°"+(Format=="M"||Format=="X"?"C":"F"));                                 // Display temperature in °C (M) or °F (I)
  display.drawString(2, 42, String(bme_humi, 0)+"%");                                                                    // Display temperature and relative humidity in %
  display.drawString((Format=="I"?70:62),20, String(bme_pres, (Format=="I"?1:0))+(Format=="M"||Format=="X"?"hPa":"in")); // Display air pressure in hecto Pascals or inches
  display.drawString((Format=="I"?70:62),42, String(Calculate_WindSpeed(), 1) + (Format=="I"||Format=="X"?"mph":"kph")); // Display wind speed in mph (X) or kph (M)
  display.setFont(ArialMT_Plain_10);                                                                                     // Set the Font to normal
  display.display();                                                                                                     // Update display
  delay(500);                                                                                                            // Small arbitrary delay
}
//#########################################################################################
void UpdateLocalTime() {
  time_t now;
  if (local_Unix_time > next_update_due) { // only get a time synchronisation from the NTP server at the update-time delay set
    time(&now);
    Serial.println("Synchronising local time, time error was: " + String(now - local_Unix_time));
    // If this displays a negative result the interrupt clock is running fast or positive running slow
    local_Unix_time = now;
    next_update_due = local_Unix_time + update_duration;
  } else now = local_Unix_time;
  //See http://www.cplusplus.com/reference/ctime/strftime/
  char hour_output[30], day_output[30];
  if (Format == "M" || Format == "X") {
    strftime(day_output, 30, "%d-%m-%y", localtime(&now)); // Formats date as: 24-05-17
    strftime(hour_output, 30, "%T", localtime(&now));      // Formats time as: 14:05:49
  }
  else {
    strftime(day_output, 30, "%m-%d-%y", localtime(&now)); // Formats date as: 05-24-17
    strftime(hour_output, 30, "%r", localtime(&now));      // Formats time as: 2:05:49pm
  }
  Date_str = day_output;
  Time_str = hour_output;
}
//#########################################################################################
void StartWiFi() {
  /* Set the ESP to be a WiFi-client, otherwise by default, it acts as ss both a client and an access-point
      and can cause network-issues with other WiFi-devices on your WiFi-network. */
  WiFi.mode(WIFI_STA);
  Serial.print(F("\r\nConnecting to: ")); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED ) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.print("WiFi connected to address: "); Serial.println(WiFi.localIP());
}
//#########################################################################################
void Setup_Interrupts_and_Initialise_Clock() {
  hw_timer_t * timer = NULL;
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &Timer_TImeout_ISR, true);
  timerAlarmWrite(timer, 1000000, true);
  timerAlarmEnable(timer);
  pinMode(WindSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WindSensorPin), &MeasureWindSpeed_ISR, RISING);
  //Now get current Unix time and assign the value to local Unix time counter and start the clock.
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println(F("Failed to obtain time"));
  }
  time_t now;
  time(&now);
  local_Unix_time = now + 1; // The addition of 1 counters the NTP setup time delay
  next_update_due = local_Unix_time + update_duration;
}
//#########################################################################################
void Start_Time_Services() {
  // Now configure time services
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", Timezone, 1); // See below for other time zones
  delay(1000); // Wait for time services
}
//#########################################################################################
void BME280_Read_Sensor() {
  if (Format == "M" || Format == "X") bme_temp = bme.readTemperature(); 
    else bme_temp = bme.readTemperature() * 9.00F / 5.00F + 32;
  if (Format == "M" || Format == "X") bme_pres = bme.readPressure() / 100.0F + pressure_offset;
    else bme_pres = (bme.readPressure() / 100.0F + pressure_offset) / 33.863886666667; // For inches
  bme_humi = bme.readHumidity();
}

float Calculate_WindSpeed() {
  if ((millis() - Last_Event_Time) > 2) { // Ignore short time intervals to debounce switch contacts
    WindSpeed = (1.00F / (((millis() - Last_Event_Time) / 1000.00F) * 2)) * WS_Calibration; // Calculate wind speed
  }
  // Calculate average wind speed
  WS_Total                         = WS_Total - WSpeedReadings[WS_Samples_Index]; // Subtract the last reading:
  WSpeedReadings[WS_Samples_Index] = WindSpeed;                                   // Add the reading to the total:
  WS_Total                         = WS_Total + WSpeedReadings[WS_Samples_Index]; // Advance to the next position in the array:
  WS_Samples_Index                 = WS_Samples_Index + 1;                        // If we're at the end of the array...
  if (WS_Samples_Index >= WS_Samples) {                                           // ...wrap around to the beginning:
    WS_Samples_Index = 0;
  }
  WindSpeed = WS_Total / WS_Samples;                                              // calculate the average wind speed:
  if (Format == "M") WindSpeed = WindSpeed * 1.60934;                             // Convert to kph if in Metric mode
  return WindSpeed;
}

/*
 * Example time zones see: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
//const char* Timezone = "GMT0BST,M3.5.0/01,M10.5.0/02";     // UK
//const char* Timezone = "MET-2METDST,M3.5.0/01,M10.5.0/02"; // Most of Europe
//const char* Timezone = "CET-1CEST,M3.5.0,M10.5.0/3";       // Central Europe
//const char* Timezone = "EST-2METDST,M3.5.0/01,M10.5.0/02"; // Most of Europe
//const char* Timezone = "EST5EDT,M3.2.0,M11.1.0";           // EST USA
//const char* Timezone = "CST6CDT,M3.2.0,M11.1.0";           // CST USA
//const char* Timezone = "MST7MDT,M4.1.0,M10.5.0";           // MST USA
//const char* Timezone = "NZST-12NZDT,M9.5.0,M4.1.0/3";      // Auckland
//const char* Timezone = "EET-2EEST,M3.5.5/0,M10.5.5/0";     // Asia
//const char* Timezone = "ACST-9:30ACDT,M10.1.0,M4.1.0/3":   // Australia
*/
