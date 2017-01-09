/*
 *   Save The Bees v1.0.0
 *
 */

/***************************************************************************************
**  Libraries **
***************************************************************************************/

#include <Adafruit_AM2315.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <RTClib.h>
#include <WiFiUdp.h>
#include <Wire.h>

/***************************************************************************************
**  Global Constants **
***************************************************************************************/

//  Credentials from the relayr developer dashboard
#define MQTT_SERVER "mqtt.relayr.io"

//  MQTT Params
#define MIN_PUBLISH_PERIOD 200
#define MQTT_PORT 1883

//  Bulk Params
#define BULK_DIM 60    //  Number of packet inside the bulk to send
#define PACKET_SIZE 10 //  Number of bytes used for each packet

//  Map of the EEPROM header
#define SSID_BYTE 0
#define SSID_PWD_BYTE 60
#define DEVICEID_BYTE 120
#define MQTT_PWD_BYTE 180
#define MQTT_CLIENTID_BYTE 240
#define TARE_BYTE 300
#define PACKET_CYCLE_BYTE 316
#define RTC_CYCLE_BYTE 318

//  Map of the packet bytes
#define TIMESTAMP_BYTE 320 //  Number of the first timestamp byte
#define TEMP_BYTE 324      //  Number of the first temperature
#define HUM_BYTE 326       //  Number of the humidity byte of the 1st packet
#define WEIGHT_BYTE 328

//  Deep Sleep Time in seconds
#define SLEEP_TIME 60

//  RTC will sync to NTP at the first connection after reached this cycle
#define RTC_CYCLE 10080

//  Size of flash memory
#define FLASH_SIZE 4096

//  Pin to change WiFi Mode
#define MODEPIN D5

// Pins for the Multiplexer & Loadcells
#define LOADPIN A0
#define TAREPIN D6
#define S0 D8
#define S1 D7

// Pins for AM2315
#define AM_SDA D4
#define AM_SCL D3

#define WEB_SERVER_PORT 80
#define ACCESS_POINT_SSID "BeeSetup"
#define ACCESS_POINT_PWD "BeeHive"
#define ACCESS_POINT_CHANNEL 1
#define ACCESS_POINT_HIDDEN false

/***************************************************************************************
**  Global Variables Function **
***************************************************************************************/

WiFiServer server(WEB_SERVER_PORT);
WiFiClient espClient;
PubSubClient pubSubClient(espClient);
RTC_DS1307 rtc;
Adafruit_AM2315 am2315;

static WiFiUDP udp;
const int led = BUILTIN_LED;
bool ledState = LOW;
unsigned long lastPublishTime;
unsigned long lastBlinkTime;
int publishingPeriod = 3000;
uint16_t humidity, temperature, weight;
float humidity_tmp, temperature_tmp, weight_tmp;
unsigned long unixTime;
short currentCycle;
long currentRTCCycle;
bool defaultMode;
char SSID[60];
char SSID_password[60];
char device_ID[60];
char MQTT_password[60];
char MQTT_clientID[60];
int loadArray[4];
int tareArray[4];

/***************************************************************************************
**  Functions Declaration **
***************************************************************************************/

void setup_wifi();
void setupServer();
void publish_packet(uint8_t packet_number);
void publish(long tmp_unixTime, short tmp_temp, short tmp_hum);
void callback(char *topic, byte *payload, unsigned int length);
void handlePayload(char *payload);
void mqtt_connect();
void blink(int interval);
void load_sensors();
unsigned long ntpUnixTime(UDP &udp);
void EEPROMWriteLong(int address, long value);
long EEPROMReadLong(long address);
void EEPROMWriteInt(int address, int value);
long EEPROMReadInt(int address);

void getTempHumiReadings();
void getLoadReadings();
void setTare();
float calibrate();

/***************************************************************************************
**  Setup Function **
***************************************************************************************/

void setup() {

  //  Initialize the PINS
  pinMode(led, OUTPUT);
  // pinMode(AM_SDA, INPUT_PULLUP);
  // pinMode(AM_SCL, INPUT_PULLUP);
  pinMode(S0, OUTPUT);
  pinMode(S1, OUTPUT);
  pinMode(LOADPIN, INPUT);
  pinMode(TAREPIN, INPUT);
  pinMode(MODEPIN, INPUT);

  //  Initialize the EEPROM
  EEPROM.begin(FLASH_SIZE);

  //  Initialize the Serial output
  Serial.begin(9600);
  Serial.println("\n\n");
  Serial.println("*** Save The Bees v.1.0.0 ***");

  // Check in which mode is the node and execute appropriate setup.
  defaultMode = digitalRead(MODEPIN);

  if (!defaultMode) {
    Serial.println("Setup mode active");
    setupServer();

  } else {

    Serial.println("Default mode active");

    //  Check if RTC is connected via I2C
    while (true) {
      if (!rtc.begin()) {
        Serial.println("RTC module not found... Retrying");
      } else
        break;
    }

    while (true) {
      if (!am2315.begin()) {
        Serial.println("AM2315 module not found... Retrying");
      } else
        break;
    }

    Serial.println("Modules initialized");

    //  Force the WiFi off to avoid the troubles of wifi after woke up from deep
    //  sleep with a I2C device connected
    WiFi.mode(WIFI_OFF);

    //  Read the cycle counters from EEPROM
    currentCycle = EEPROMReadInt(PACKET_CYCLE_BYTE);
    currentRTCCycle = EEPROMReadLong(RTC_CYCLE_BYTE);

    //  Read the vriables from EEPROM
    EEPROM.get(TARE_BYTE, tareArray);

    //  In case of flashing a scipt with reduced BULK DIM than the previous
    //  flashed version, set counter to zero
    if (currentCycle > BULK_DIM) {
      currentCycle = 0;
    }

    if (currentCycle < BULK_DIM) {

      Serial.print("Packet number: ");
      Serial.print(currentCycle + 1);
      Serial.print(" of ");
      Serial.println(BULK_DIM);

      Serial.print("RTC cycle: ");
      Serial.print(currentRTCCycle + 1);
      Serial.print(" of ");
      Serial.println(RTC_CYCLE);

      if (currentRTCCycle >= RTC_CYCLE)
        Serial.println(
            "The RTC time will be adjusted during next internet connection");

      // Get data from sensors
      load_sensors();
      getLoadReadings();

      //  Get the timestamp from RTC module
      unixTime = rtc.now().unixtime();
      Serial.print("RTC time: ");
      Serial.println(unixTime);

      //  Save the readings and the timestamp into the EEPROM
      EEPROMWriteLong(TIMESTAMP_BYTE + (currentCycle * PACKET_SIZE), unixTime);
      EEPROMWriteInt(TEMP_BYTE + (currentCycle * PACKET_SIZE), temperature);
      EEPROMWriteInt(HUM_BYTE + (currentCycle * PACKET_SIZE), humidity);
      EEPROMWriteInt(WEIGHT_BYTE + (currentCycle * PACKET_SIZE), weight);

      Serial.println("Data saved into the EEPROM");

      //  Increment the counters
      currentCycle++;
      currentRTCCycle++;

      // If the packet just saved is not the last of the bulk, I save the cycle
      // number
      if (currentCycle != BULK_DIM) {
        EEPROMWriteInt(PACKET_CYCLE_BYTE, currentCycle);
        EEPROMWriteLong(RTC_CYCLE_BYTE, currentRTCCycle);
        EEPROM.end();
      }

      //  If the packet just saved is the last one, send all the data in bulk
      else {

        //  Read Params from EEPROM
        EEPROM.get(SSID_BYTE, SSID);
        EEPROM.get(SSID_PWD_BYTE, SSID_password);

        //  Initializing WiFi
        setup_wifi();

        //  Check if the RTC should be adjusted
        if (currentRTCCycle >= RTC_CYCLE) {
          delay(500);

          long NTPtime = 0;

          while (true) {
            delay(2000);
            NTPtime = ntpUnixTime(udp);
            Serial.print("NTP Time: ");
            Serial.println(NTPtime);
            if (NTPtime != 0)
              break;
          }

          //  Adjust RTC Module
          rtc.adjust(DateTime(NTPtime));
          Serial.println("RTC time just adusted with NTP time");

          //  Clear RTC counter
          EEPROMWriteLong(RTC_CYCLE_BYTE, 0);
        }

        //  Establish the connection to MQTT server
        EEPROM.get(DEVICEID_BYTE, device_ID);
        EEPROM.get(MQTT_PWD_BYTE, MQTT_password);
        EEPROM.get(MQTT_CLIENTID_BYTE, MQTT_clientID);

        pubSubClient.setServer(MQTT_SERVER, MQTT_PORT);
        pubSubClient.setCallback(callback);
        publishingPeriod = publishingPeriod > MIN_PUBLISH_PERIOD
                               ? publishingPeriod
                               : MIN_PUBLISH_PERIOD;
        mqtt_connect();

        // Cycle that publishes all the packets stored into the EEPROM
        for (int i = 0; i < BULK_DIM; i++) {
          publish_packet(i);
          delay(500);
        }

        //  After sent all packets, the packet counter is set to 0
        Serial.println("Clearing EEPROM");
        EEPROMWriteInt(PACKET_CYCLE_BYTE, 0);
        EEPROM.end();
      }

      Serial.println("Going to sleep... Zzz");

      //  Yield function to give time to finish executing all background
      //  processes
      yield();

      //  Setting the deep sleep time
      ESP.deepSleep(SLEEP_TIME * 1000000);
    }
  }
}
/***************************************************************************************
**  Loop Function **
***************************************************************************************/

void loop() {
  if (!defaultMode) {
    runServer();
  }
}

/***************************************************************************************
**  Setup WiFi Function **
***************************************************************************************/

void setup_wifi() {
  delay(500);

  Serial.println("");
  Serial.print("Connecting to ");
  Serial.println(SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, SSID_password);

  int count = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    count++;

    if (count > 15) {
      WiFi.mode(WIFI_OFF);
      delay(500);
      WiFi.mode(WIFI_STA);
      WiFi.begin(SSID, SSID_password);
      count = 0;
      delay(1000);
    }
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

/***************************************************************************************
**  Publish Packet Function **
***************************************************************************************/

void publish_packet(int packet_number) {

  //  Check the connection before publish
  if (pubSubClient.connected()) {

    Serial.println("");
    Serial.println("Start publishing...");
    delay(3000);

    //  Publish within the defined publishing period
    if (millis() - lastPublishTime > publishingPeriod) {

      lastPublishTime = millis();
      delay(100);

      long tmp_unixTime =
          EEPROMReadLong(TIMESTAMP_BYTE + (packet_number * PACKET_SIZE));
      short tmp_temp = EEPROMReadInt(TEMP_BYTE + (packet_number * PACKET_SIZE));
      short tmp_hum = EEPROMReadInt(HUM_BYTE + (packet_number * PACKET_SIZE));
      short tmp_weight =
          EEPROMReadInt(WEIGHT_BYTE + (packet_number * PACKET_SIZE));

      publish(tmp_unixTime, tmp_temp, tmp_hum, tmp_weight);

      Serial.print("Just published packet number ");
      Serial.println(packet_number + 1);
      Serial.println("");
    }

    //  Blink LED
    blink(publishingPeriod / 2);
  }

  else {
    Serial.println("Connection not available: retrying...");
    mqtt_connect();
  }
}

/***************************************************************************************
**  Publish Function **
***************************************************************************************/

void publish(long tmp_unixTime, short tmp_temp, short tmp_hum,
             short tmp_weight) {

  //  Create the JSON containing the data
  StaticJsonBuffer<MQTT_MAX_PACKET_SIZE> pubJsonBuffer;
  JsonArray &root = pubJsonBuffer.createArray();

  //  Timestamp of the readings needs to be specified in milliseconds but ESP
  //  cannot handle 64bits types, so it is transformed in string
  String ut = (String(tmp_unixTime) + String("000"));

  //  First object: temperature
  JsonObject &leaf1 = root.createNestedObject();
  leaf1["ts"] = ut;
  leaf1["meaning"] = "temperature_raw";
  leaf1["value"] = tmp_temp;

  //  Second object: humidity
  JsonObject &leaf2 = root.createNestedObject();
  leaf2["ts"] = ut;
  leaf2["meaning"] = "humidity_raw";
  leaf2["value"] = tmp_hum;

  //  Third object: weight
  JsonObject &leaf3 = root.createNestedObject();
  leaf3["ts"] = ut;
  leaf3["meaning"] = "weight_raw";
  leaf3["value"] = tmp_weight;

  //  Creating MQTT packet
  char message_buff[MQTT_MAX_PACKET_SIZE];
  root.printTo(message_buff, sizeof(message_buff));

  // Test remove string
  bool ts_to_remove = true;

  const char *ts = "\"ts\"";
  char *p = message_buff;

  while ((p = strstr(p, ts)) != NULL) {
    if (p) {
      Serial.print("found [");
      Serial.print(ts);
      Serial.print("] at position ");
      Serial.println((int)(p - message_buff));

      int idxToDel1 = ((int)(p - message_buff)) + 5;
      memmove(&message_buff[idxToDel1], &message_buff[idxToDel1 + 1],
              strlen(message_buff) - idxToDel1);

      int idxToDel2 = ((int)(p - message_buff)) + 18;
      memmove(&message_buff[idxToDel2], &message_buff[idxToDel2 + 1],
              strlen(message_buff) - idxToDel2);
    }
    p++;
  }

  // Publish the MQTT packet
  char dataTopic[45];
  sprintf(dataTopic, "/v1/%s/data", device_ID);

  pubSubClient.publish(dataTopic, message_buff);
  Serial.println("Publishing " + String(message_buff));
}

//------------------------------------------------------------------------------------//
// Enduser wifi and relayr setup functions. //
//------------------------------------------------------------------------------------//

void setupServer() {

  // Setup the access point
  WiFi.mode(WIFI_AP);

  Serial.print("Setting WEMOS as Soft Access Point... ");
  /*boolean result = WiFi.softAP("BeeSetup");
     if (result == true)
     Serial.println("Done");
     else
     Serial.println("Failed!");*/
  WiFi.softAP("BeeSetup");

  // Start the server
  server.begin();
  Serial.println("WebServer started");

  // Print the IP address.
  Serial.print("Access the Web Server at http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");
}

void runServer() {

  // Check if a client has connected
  WiFiClient wifiClient = server.available();
  if (!wifiClient) {
    return;
  }

  // Wait until the client sends some data
  while (!wifiClient.available()) {
    delay(1000);
  }

  // Read and parse the request.
  String request = wifiClient.readString();
  parseClientRequest(request);
  wifiClient.flush();

  // Return the response
  wifiClient.println("HTTP/1.1 200 OK");
  wifiClient.println("Content-Type: text/html");
  wifiClient.println("");
  wifiClient.println(
      "<!DOCTYPE html><html><head> <title>Save The Bees</title> <style>body { "
      "background: #003056; font-family: Helvetica, Arial, sans-serif; color: "
      "#2B2B2B}.hexagon:before { content: ''; width: 0; height: 0; "
      "border-bottom: 120px solid #e49436; border-left: 208px solid "
      "transparent; border-right: 208px solid transparent; position: absolute; "
      "top: -120px}.hexagon { width: 416px; height: 280px; margin-top: 124px; "
      "margin-left: 3px; background-color: #e49436; position: relative; float: "
      "left}.hexagon:after { content: ''; width: 0; position: absolute; "
      "bottom: -120px; border-top: 120px solid #e49436; border-left: 208px "
      "solid transparent; border-right: 208px solid transparent}.hexagon-row { "
      "clear: left}.hexagon-row.even { margin-left: 209px}.hexagonIn { width: "
      "370px; text-align: center; position: absolute; top: 50%; left: 50%; "
      "transform: translate(-50%, -50%)}.input { color: #3c3c3c; font-family: "
      "Helvetica, Arial, sans-serif; font-weight: 500; font-size: 18px; "
      "border-radius: 4px; line-height: 22px; background-color: #E1E1E1; "
      "padding: 5px 5px 5px 5px; margin-bottom: 5px; width: 100%; box-sizing: "
      "border-box; border: 3px solid #E1E1E1}.input:focus { background: "
      "#E1E1E1; box-shadow: 0; border: 3px solid #003056; color: #003056; "
      "outline: none; padding: 5px 5px 5px 5px;}#button { font-family: "
      "Helvetica, sans-serif; float: left; width: 100%; cursor: pointer; "
      "background-color: #E1E1E1; color: #003056; border: #003056 solid 3px; "
      "font-size: 24px; padding-top: 22px; padding-bottom: 22px; transition: "
      "all 0.3s; margin-top: 0px; border-radius: 4px}#button:hover { "
      "background-color: #003056; color: #E1E1E1; border: #E1E1E1 solid "
      "3px}</style></head><body> <form id='credentialsForm' action=''> <div "
      "class='hexagon-row'> <div class='hexagon'> <div class='hexagonIn'> "
      "<h1>Wifi Credentials</h1> <b>Wifi SSID:</b> <br> <input type='text' "
      "class='input' name='SSID' placeholder='SSID name' value='" +
      String(SSID) + "'> <br><br> <b>Wifi Password:</b> <br> <input "
                     "type='password' class='input' name='SSID_password' "
                     "placeholder='SSID password' value='" +
      String(SSID_password) +
      "'> </div> </div> <div class='hexagon'> <div class='hexagonIn'> "
      "<h1>relayr Credentials</h1> <b>DeviceID:</b> <br> <input type='text' "
      "class='input' name='device_ID' "
      "placeholder='12345678-1234-1234-1234-0123456789ab' value='" +
      String(device_ID) + "'> <br><br> <b>MQTT Password:</b> <br> <input "
                          "type='password' class='input' name='MQTT_password' "
                          "placeholder='ABc1D23efg-h' value='" +
      String(MQTT_password) + "'> <br><br> <b>MQTT Client ID:</b> <br> <input "
                              "type='password' class='input' "
                              "name='MQTT_clientID' "
                              "placeholder='TH6gI6HKjhjkhvfcFDNWw' value='" +
      String(MQTT_clientID) +
      "'> </div> </div> </div> <div class='hexagon-row even'> <div "
      "class='hexagon'> <div class='hexagonIn'> <input type='submit' "
      "id='button' value='Save'> </div> </div> </div> </form></body></html>");
  delay(1);
}

void parseClientRequest(String req) {

  // Get the SSID, PASSWORD, RELAYRUSER and RELAYRPASSWORD.
  int startIndex = req.indexOf("GET /?SSID=");

  // First check if request contains 'SSID='.
  if (startIndex != -1) {

    // If so parse/search for all the other points of interest.
    int endIndex = req.indexOf("&SSID_password=");
    req.substring(startIndex + 11, endIndex).toCharArray(SSID, 60);

    // Get the PASSWORD.
    startIndex = req.indexOf("&SSID_password=");
    endIndex = req.indexOf("&device_ID=");
    req.substring(startIndex + 15, endIndex).toCharArray(SSID_password, 60);

    // Get the RELAYRUSER.
    startIndex = req.indexOf("&device_ID=");
    endIndex = req.indexOf("&MQTT_password=");
    req.substring(startIndex + 11, endIndex).toCharArray(device_ID, 60);

    // Get the RELAYRPASSWORD.
    startIndex = req.indexOf("&MQTT_password=");
    endIndex = req.indexOf("&MQTT_clientID=");
    req.substring(startIndex + 15, endIndex).toCharArray(MQTT_password, 60);

    // Get the RELAYRPASSWORD.
    startIndex = req.indexOf("&MQTT_clientID=");
    endIndex = req.indexOf(" HTTP/");
    req.substring(startIndex + 15, endIndex).toCharArray(MQTT_clientID, 60);

    // Debug. //
    Serial.println("-----------------");
    Serial.println(SSID);
    Serial.println(SSID_password);
    Serial.println(device_ID);
    Serial.println(MQTT_password);
    Serial.println(MQTT_clientID);
    Serial.println("-----------------");
    // ----- //

    EEPROM.put(SSID_BYTE, SSID);
    EEPROM.put(SSID_PWD_BYTE, SSID_password);
    EEPROM.put(DEVICEID_BYTE, device_ID);
    EEPROM.put(MQTT_PWD_BYTE, MQTT_password);
    EEPROM.put(MQTT_CLIENTID_BYTE, MQTT_clientID);
    EEPROM.end();
  }
}

/***************************************************************************************
**  Write/Read EEPROM Functions **
***************************************************************************************/

//  Write a Long into the EEPROM: write in the address byte and the next 3 ones
void EEPROMWriteLong(int address, long value) {

  byte four = (value & 0xFF);
  byte three = ((value >> 8) & 0xFF);
  byte two = ((value >> 16) & 0xFF);
  byte one = ((value >> 24) & 0xFF);

  EEPROM.write(address, four);
  EEPROM.write(address + 1, three);
  EEPROM.write(address + 2, two);
  EEPROM.write(address + 3, one);
  EEPROM.commit();
}

//  Read a Long from the EEPROM: read the address byte and the next 3 ones
//  returning the value
long EEPROMReadLong(long address) {
  long four = EEPROM.read(address);
  long three = EEPROM.read(address + 1);
  long two = EEPROM.read(address + 2);
  long one = EEPROM.read(address + 3);
  return ((four << 0) & 0xFF) + ((three << 8) & 0xFFFF) +
         ((two << 16) & 0xFFFFFF) + ((one << 24) & 0xFFFFFFFF);
}

// Write a Integer into the EEPROM: write in the address byte and the next one
void EEPROMWriteInt(int address, int value) {
  byte two = (value & 0xFF);
  byte one = ((value >> 8) & 0xFF);
  EEPROM.write(address, two);
  EEPROM.write(address + 1, one);
  EEPROM.commit();
}

//  Read a Integer from the EEPROM: read the address byte and the next one
//  returning the value
long EEPROMReadInt(int address) {
  int two = EEPROM.read(address);
  int one = EEPROM.read(address + 1);
  return ((two << 0) & 0xFF) + ((one << 8) & 0xFFFF);
}

/***************************************************************************************
**  NTP Function (from http://playground.arduino.cc/Code/NTPclient) **
***************************************************************************************/

unsigned long ntpUnixTime(UDP &udp) {

  static int udpInited = udp.begin(123);

  const char timeServer[] = "3.de.pool.ntp.org";
  const long ntpFirstFourBytes = 0xEC0600E3;

  // Fail if WiFiUdp.begin() could not init a socket
  if (!udpInited)
    return 0;

  udp.flush();

  if (!(udp.beginPacket(timeServer, 123) &&
        udp.write((byte *)&ntpFirstFourBytes, 48) == 48 && udp.endPacket()))
    return 0;

  // Wait for response; check every pollIntv ms up to maxPoll times
  const int pollIntv = 150; // poll every this many ms
  const byte maxPoll = 15;  // poll up to this many times
  int pktLen;               // received packet length
  for (byte i = 0; i < maxPoll; i++) {
    if ((pktLen = udp.parsePacket()) == 48)
      break;
    delay(pollIntv);
  }
  if (pktLen != 48)
    return 0; // no correct packet received

  // Read and discard the first useless bytes
  // Set useless to 32 for speed; set to 40 for accuracy.
  const byte useless = 40;
  for (byte i = 0; i < useless; ++i)
    udp.read();

  // Read the integer part of sending time
  unsigned long time = udp.read(); // NTP time
  for (byte i = 1; i < 4; i++)
    time = time << 8 | udp.read();

  // Round to the nearest second if we want accuracy
  // The fractionary part is the next byte divided by 256: if it is
  // greater than 500ms we round to the next second; we also account
  // for an assumed network delay of 50ms, and (0.5-0.05)*256=115;
  // additionally, we account for how much we delayed reading the packet
  // since its arrival, which we assume on average to be pollIntv/2.
  time += (udp.read() > 115 - pollIntv / 8);

  // Discard the rest of the packet
  udp.flush();

  return time - 2208988800ul; // convert NTP time to Unix time
}

/***************************************************************************************
**  Load Sensors Function **
***************************************************************************************/

void load_sensors() {

  while (true) {
    delay(500);

    //  Reading the sensor
    humidity_tmp = am2315.readHumidity();

    //  Multiply the value for 100 in order to use just 2 bytes to stored
    //  into the EEPROM
    humidity = humidity_tmp * 100.00;
    Serial.print("debug humidity: ");
    Serial.println(humidity_tmp);
    Serial.print("humidity: ");
    Serial.println(humidity);

    if (isnan(humidity_tmp) || (humidity == -1))
      Serial.println("Failed to read from the sensor! Retrying...");
    else
      break;
  }

  while (true) {
    delay(500);

    //  Reading the sensor
    temperature_tmp = am2315.readTemperature();

    //  Multiply the value for 100 in order to use just 2 bytes to stored
    //  into the EEPROM
    temperature = temperature_tmp * 100.00;
    Serial.print("debug temperature: ");
    Serial.println(temperature_tmp);
    Serial.print("temperature: ");
    Serial.println(temperature);

    if (isnan(temperature_tmp) || (temperature == -1))
      Serial.println("Failed to read from the sensor! Retrying...");
    else
      break;
  }
}

void getLoadReadings() {
  float sum = 0;

  if (digitalRead(TAREPIN))
    setTare();

  // Read all the load cells by switching through the multiplexer
  for (int i = 0; i < 4; i++) {
    // Serial.println(String(bitRead(i, 0), HEX) + "  " + String(bitRead(i, 1),
    // HEX));

    digitalWrite(S0, bitRead(i, 0));
    digitalWrite(S1, bitRead(i, 1));

    loadArray[i] = analogRead(LOADPIN);

    sum += calibrate((loadArray[i] - tareArray[i]));
  }

  weight_tmp = sum;
  weight = weight_tmp * 100;
  // weight = weight_tmp * 100;
  Serial.println("debug weight: " + String(weight_tmp) + "kg");
  Serial.println("debug weight: " + String(weight));
}

void setTare() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(S0, bitRead(i, 0));
    digitalWrite(S1, bitRead(i, 1));
    tareArray[i] = analogRead(LOADPIN);
  }

  EEPROM.put(TARE_BYTE, tareArray);
  Serial.println("Setting TARE to: " + String(tareArray[0]) + " " +
                 String(tareArray[1]) + " " + String(tareArray[2]) + " " +
                 String(tareArray[3]));
}

// calibrate volatge to kg
float calibrate(float input) {
  // Serial.println(input);
  float output = (input) / 42;
  if (input == 0)
    output = 0;
  // float output = input * 0.0857142857;

  return output;
}

/***************************************************************************************
**  Callback Function **
***************************************************************************************/

void callback(char *topic, byte *payload, unsigned int length) {

  //  Store the received payload and convert it to string
  char p[length + 1];
  memcpy(p, payload, length);
  p[length] = NULL;

  //  Print the topic and the received payload
  Serial.println("topic: " + String(topic));
  Serial.println("payload: " + String(p));

  //  Call method to parse and use the received payload
  handlePayload(p);
}

/***************************************************************************************
**  Handle Function **
***************************************************************************************/

void handlePayload(char *payload) {
  StaticJsonBuffer<200> jsonBuffer;
  // Convert payload to json
  JsonObject &json = jsonBuffer.parseObject(payload);

  if (!json.success()) {
    Serial.println("json parsing failed");
    return;
  }

  // Get the value of the key "name", aka. listen to incoming commands and
  // configurations
  const char *command = json["name"];
  Serial.println("parsed command: " + String(command));

  // CONFIGURATION "frequency": We can change the publishing period
  if (String(command).equals("frequency")) {
    int frequency = json["value"];

    if ((frequency >= 2000) && (frequency <= 60000)) {
      Serial.println("Adjusting publishing period (ms): " + String(frequency));
      publishingPeriod = frequency;
    }

    else
      Serial.println(
          "The requested publishing period is out of the defined range!");
  }
}

/***************************************************************************************
**  MQTT Connect Function **
***************************************************************************************/

void mqtt_connect() {
  Serial.println("");
  Serial.println("Connecting to MQTT server...");

  if (pubSubClient.connect(MQTT_clientID, device_ID, MQTT_password)) {
    Serial.println("Connection successful! Subscribing to topic...");

    char cmd[45];
    sprintf(cmd, "/v1/%s/cmd", device_ID);

    //  Subscribing to the topic "cmd", so we can listen to commands
    pubSubClient.subscribe(cmd);

    char config[45];
    sprintf(config, "/v1/%s/config", device_ID);
    //  And to "config" as well, for the configurations
    pubSubClient.subscribe(config);
  }

  else {
    Serial.println(
        "Connection failed! Check your credentials or the WiFi network");

    //  This reports the error code
    Serial.println("rc = ");
    Serial.print(pubSubClient.state());

    //  Try again in 5 seconds
    Serial.println("Retrying in 5 seconds...");
    delay(5000);
  }
}

/***************************************************************************************
**  Blink Led Function **
***************************************************************************************/

void blink(int interval) {
  if (millis() - lastBlinkTime > interval) {

    // Save the last time the LED blinked
    lastBlinkTime = millis();

    if (ledState == LOW)
      ledState = HIGH;
    else
      ledState = LOW;

    // Set the LED with the ledState of the variable:
    digitalWrite(led, ledState);
  }
}

/***************************************************************************************
**  Find text **
***************************************************************************************/
int find_text(String s, String searched_string) {
  int foundpos = -1;
  for (int i = 0; i <= searched_string.length() - s.length(); i++) {
    if (searched_string.substring(i, s.length() + i) == s) {
      foundpos = i;
      break;
    }
  }
  return foundpos;
}
