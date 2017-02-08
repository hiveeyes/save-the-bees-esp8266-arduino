/*
 *   Save The Bees 1.1.2
 *
 *   This Arduino sketch is developed for the Wemos D1 mini board but should
 *   work on every ESP8266 based board with few modifications.
 *
 *   It has two operating modes:
 *   - Setup Mode: the board works as an access point and the user can enter the
 *                 WiFi and relayr credentials to be used in default mode;
 *   - Default Mode: the board wakes up from deep sleep after SLEEP_TIME
 *                   seconds, it gets the readings from the sensors and the
 *                   timestamp
 *                   from the RTC module and save them as a packet into the
 *                   EEPROM.
 *                   When the board saved BULK_DIM packets, it connects to the
 *                   relayr cloud and send all of them. Then the cycle of readings
 *                   starts again.
 *
 *   Last Edit: 13 Jan 2016 20.52 CET
 *
 *   Copyright Riccardo Marconcini (riccardo DOT marconcini AT relayr DOT de)
 *
 *   TODO: function to retrieve automatically flash size if possible
 */



/***************************************************************************************
**  Libraries                                                                         **
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
**  Global Constants                                                                  **
***************************************************************************************/

// Software version
#define stb "1.1.2"

//  MQTT Params
#define MQTT_SERVER "mqtt.relayr.io"
#define MIN_PUBLISH_PERIOD 200
#define MQTT_PORT 1883

//  Bulk Params
#define BULK_DIM 60    //  Number of packets inside the bulk to send
#define PACKET_SIZE 10 //  Number of bytes used for each packet

//  Map of the EEPROM header: these are the starting bytes for each parameter
#define SSID_BYTE 0
#define SSID_PWD_BYTE 60
#define DEVICEID_BYTE 120
#define MQTT_PWD_BYTE 180
#define MQTT_CLIENTID_BYTE 240
#define TARE_BYTE 300
#define PACKET_CYCLE_BYTE 320
#define RTC_CYCLE_BYTE 322

//  Map of the EEPROM packet bytes
#define TIMESTAMP_BYTE 324 //  Number of the first  timestamp byte in 1st packet
#define TEMP_BYTE 328   //  Number of the first temperature byte in 1st packet
#define HUM_BYTE 330    //  Number of the first humidity byte in 1st packet
#define WEIGHT_BYTE 332 //  Number of the first weight byte in 1st packet

//  Deep Sleep Time in seconds
#define SLEEP_TIME 60

//  RTC will sync to NTP at the first connection after reached this cycle
//  iteration
#define RTC_CYCLE 10080

//  Size of flash memory
#define FLASH_SIZE 4096

//  Pin to change Operating Mode
#define MODEPIN A0

//  Wifi Credentials of Setup Mode
#define WEB_SERVER_PORT 80
#define ACCESS_POINT_SSID "BeeSetup"
#define ACCESS_POINT_PWD "BeeHive91"
#define ACCESS_POINT_CHANNEL 1
#define ACCESS_POINT_HIDDEN false

//  Pins for the ADC & Loadcells
#define SELPIN D8       //Selection Pin
#define DATAOUT D7      //MOSI
#define DATAIN  D6      //MISO
#define SPICLOCK  D5    //Clock

//  Calibration fract
#define CALIBRATION_FRACT 68



/***************************************************************************************
**  Global Variables Function                                                         **
***************************************************************************************/

WiFiServer server(WEB_SERVER_PORT);
WiFiClient espClient;
PubSubClient pubSubClient(espClient);
RTC_DS3231 rtc; //  or RTC_DS1307
Adafruit_AM2315 am2315;
WiFiUDP udp;
uint16_t lastPublishTime;
uint16_t publishingPeriod = 3000;
uint16_t loadArray[5];
uint16_t tareArray[5];
uint16_t zeroArray[] {411, 403, 414, 465, 400};
uint16_t humidity, temperature, weight;
uint16_t currentCycle;
u_long currentRTCCycle;
u_long unixTime;
float humidity_tmp, temperature_tmp, weight_tmp;
bool setupMode;
bool tareMode;
char SSID[60];
char SSID_password[60];
char device_ID[60];
char MQTT_password[60];
char MQTT_clientID[60];



/***************************************************************************************
**  Functions Declaration                                                             **
***************************************************************************************/

void setup();
void loop();
void setup_wifi();
void setupServer();
void runServer();
void parseClientRequest(String req);
void publish_packet(uint16_t packet_number);
void publish(u_long tmp_unixTime, uint16_t tmp_temp, uint16_t tmp_hum,
             uint16_t tmp_weight);



/***************************************************************************************
**  Setup Function                                                                    **
***************************************************************************************/

void setup() {

        //  Initialize the PINS
        pinMode(SELPIN, OUTPUT);
        pinMode(DATAOUT, OUTPUT);
        pinMode(DATAIN, INPUT);
        pinMode(SPICLOCK, OUTPUT);
        pinMode(MODEPIN, INPUT);

        digitalWrite(SELPIN,HIGH);
        digitalWrite(DATAOUT,LOW);
        digitalWrite(SPICLOCK,LOW);

        //  Initialize the EEPROM
        EEPROM.begin(FLASH_SIZE);

        //  Initialize the serial output
        Serial.begin(9600);
        Serial.println("\n\n");
        Serial.println("*** Save The Bees " + String(stb) + " ***");

        EEPROM.get(TARE_BYTE, tareArray);
        Serial.println("TARE: " + String(tareArray[0]) + " " +
                       String(tareArray[1]) + " " + String(tareArray[2]) + " " +
                       String(tareArray[3]) + " " + String(tareArray[4]));

        // Check in which mode is the board
        if (analogRead(MODEPIN) > 600)
                setupMode = true;
        else
                setupMode = false;

        if (setupMode) {
                Serial.println("Setup mode active");
                setupServer();
                EEPROM.end();

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

                //  Force the WiFi off to avoid troubles with wifi after woke up from deep
                //  sleep with a I2C device connected
                WiFi.mode(WIFI_OFF);

                //  Read the cycle counters from EEPROM
                currentCycle = EEPROMReadInt(PACKET_CYCLE_BYTE);
                currentRTCCycle = EEPROMReadLong(RTC_CYCLE_BYTE);

                //  Read the tare from EEPROM
                EEPROM.get(TARE_BYTE, tareArray);

                //  In case of flashing a sketch with reduced BULK DIM than the previous
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

                        //  Get readings from sensors
                        loadSensors();
                        loadLoadcells();

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

                        // If the packet just saved is not the last of the bulk, save the cycles
                        // number
                        if (currentCycle != BULK_DIM) {
                                EEPROMWriteInt(PACKET_CYCLE_BYTE, currentCycle);
                                EEPROMWriteLong(RTC_CYCLE_BYTE, currentRTCCycle);
                                EEPROM.end();
                        }

                        //  If the packet just saved is the last one, send all the data in bulk
                        else {

                                //  Read WiFi credentials from EEPROM
                                EEPROM.get(SSID_BYTE, SSID);
                                EEPROM.get(SSID_PWD_BYTE, SSID_password);

                                //  Initializing WiFi
                                setup_wifi();

                                //  Check if the RTC should be adjusted
                                if (currentRTCCycle >= RTC_CYCLE) {
                                        delay(500);

                                        u_long NTPtime = 0;

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

                                //  Cycle that publishes all the packets stored into the EEPROM
                                for (uint16_t i = 0; i < BULK_DIM; i++) {
                                        publish_packet(i);
                                        delay(500);
                                }

                                //  After sent all packets, the packet counter is set to 0
                                Serial.println("Clearing EEPROM");
                                EEPROMWriteInt(PACKET_CYCLE_BYTE, 0);
                                EEPROM.end();
                        }

                        Serial.println("Going to sleep... Beezzz");

                        //  Yield function to give time to finish executing all background
                        //  processes
                        yield();

                        //  Setting the deep sleep time
                        ESP.deepSleep(SLEEP_TIME * 1000000);
                }
        }
}



/***************************************************************************************
**  Loop Function                                                                     **
***************************************************************************************/

//  Functions that is executed in a loop after the setup function (in setup mode)
void loop() {
        if (setupMode) {
                runServer();
        }
}



/***************************************************************************************
**  WiFi Functions                                                                    **
***************************************************************************************/

//  Function to setup the WiFi commection as STAtion (client)
void setup_wifi() {
        delay(500);

        Serial.println("");
        Serial.print("Connecting to ");
        Serial.println(SSID);

        WiFi.mode(WIFI_STA);
        WiFi.begin(SSID, SSID_password, ACCESS_POINT_CHANNEL);

        uint8_t count = 0;

        while (WiFi.status() != WL_CONNECTED) {
                delay(1000);
                Serial.print(".");
                count++;

                if (count > 15) {
                        WiFi.mode(WIFI_OFF);
                        delay(2000);
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



//  Function to setup the WiFi commection as AP (access point)
void setupServer() {

        //  Setup the access point
        WiFi.mode(WIFI_AP);

        Serial.print("Setting WEMOS as Soft Access Point... ");
        boolean result = WiFi.softAP(ACCESS_POINT_SSID, ACCESS_POINT_PWD,
                                     ACCESS_POINT_CHANNEL, ACCESS_POINT_HIDDEN);
        if (result == true)
                Serial.println("Done");
        else
                Serial.println("Failed!");

        //  Start the server
        server.begin();
        Serial.println("WebServer started");

        //  Print the IP address.
        Serial.print("Access the Web Server at http://");
        Serial.print(WiFi.softAPIP());
        Serial.println("/");
}



//  Function that sends and HTTP answer to a client (browser) that connects to
//  web server
void runServer() {

        //  Check if a client (browser) has connected
        WiFiClient wifiClient = server.available();
        if (!wifiClient) {
                return;
        }

        //  Wait until the client (browser) sends a request
        while (!wifiClient.available()) {
                delay(1000);
        }

        //  Read and parse the request
        String request = wifiClient.readString();
        parseClientRequest(request);
        wifiClient.flush();

        if (tareMode) {
                //  Initialize the EEPROM
                EEPROM.begin(FLASH_SIZE);
                EEPROM.get(TARE_BYTE, tareArray);
                EEPROM.end();

                // Return the response
                wifiClient.println("HTTP/1.1 200 OK");
                wifiClient.println("Content-Type: text/html");
                wifiClient.println("");
                wifiClient.println("<!DOCTYPE html><html><head> <title>Tare</title> <style>body { background: #003056; font-family: Helvetica, Arial, sans-serif; color: #2B2B2B}.hexagon:before { content: ''; width: 0; height: 0; border-bottom: 120px solid #e49436; border-left: 208px solid transparent; border-right: 208px solid transparent; position: absolute; top: -120px}.hexagon { width: 416px; height: 240px; margin-top: 124px; margin-left: 3px; background-color: #e49436; position: relative; float: left}.hexagon:after { content: ''; width: 0; position: absolute; bottom: -120px; border-top: 120px solid #e49436; border-left: 208px solid transparent; border-right: 208px solid transparent}.hexagon-row { clear: left}.hexagon-row.even { margin-left: 209px}.hexagonIn { width: 370px; text-align: center; position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%)}.input { color: #3c3c3c; font-family: Helvetica, Arial, sans-serif; font-weight: 500; font-size: 18px; border-radius: 4px; line-height: 22px; background-color: #E1E1E1; padding: 5px 5px 5px 5px; margin-bottom: 5px; width: 100%; box-sizing: border-box; border: 3px solid #E1E1E1}.input:focus { background: #E1E1E1; box-shadow: 0; border: 3px solid #003056; color: #003056; outline: none; padding: 5px 5px 5px 5px;}#button { font-family: Helvetica, sans-serif; float: left; width: 100%; cursor: pointer; background-color: #E1E1E1; color: #003056; border: #003056 solid 3px; font-size: 24px; padding-top: 22px; padding-bottom: 22px; transition: all 0.3s; margin-top: 0px; border-radius: 4px}#button:hover { background-color: #003056; color: #E1E1E1; border: #E1E1E1 solid 3px}</style></head><body> <form id='tareForm' action='/tareset'> <div class='hexagon-row'> <div class='hexagon'> <div class='hexagonIn'> <h1> <b>Tare cell 1:</b><br><b>"+ String((float(tareArray[0]-zeroArray[0]))/CALIBRATION_FRACT) +" kg</b> <br><br> <b>Tare cell 2:</b><br><b>"+ String((float(tareArray[1]-zeroArray[1]))/CALIBRATION_FRACT) +" kg</b> </h1> </div> </div> <div class='hexagon'> <div class='hexagonIn'> <h1> <b>Tare cell 3:</b><br><b>"+ String((float(tareArray[2]-zeroArray[2]))/CALIBRATION_FRACT) +" kg</b> <br><br> <b>Tare cell 4:</b><br><b>"+ String((float(tareArray[3]-zeroArray[3]))/CALIBRATION_FRACT) +" kg</b> </h1> </div> </div> </div> <div class='hexagon-row even'> <div class='hexagon'> <div class='hexagonIn'> <h1><b>Tare cell 5:</b><br><b> " + "boh " +"kg</b></h1> <input type='submit' id='button' value='Set New Tare'> </div> </div> </div> </form></body></html>");
                delay(1);
        }

        else {
                //  Initialize the EEPROM
                EEPROM.begin(FLASH_SIZE);
                EEPROM.get(SSID_BYTE, SSID);
                EEPROM.get(SSID_PWD_BYTE, SSID_password);
                EEPROM.get(DEVICEID_BYTE, device_ID);
                EEPROM.get(MQTT_PWD_BYTE, MQTT_password);
                EEPROM.get(MQTT_CLIENTID_BYTE, MQTT_clientID);
                EEPROM.end();

                // Return the response
                wifiClient.println("HTTP/1.1 200 OK");
                wifiClient.println("Content-Type: text/html");
                wifiClient.println("");
                wifiClient.println("<!DOCTYPE html><html><head> <title>Save The Bees</title> <style>body { background: #003056; font-family: Helvetica, Arial, sans-serif; color: #2B2B2B}.hexagon:before { content: ''; width: 0; height: 0; border-bottom: 120px solid #e49436; border-left: 208px solid transparent; border-right: 208px solid transparent; position: absolute; top: -120px}.hexagon { width: 416px; height: 280px; margin-top: 124px; margin-left: 3px; background-color: #e49436; position: relative; float: left}.hexagon:after { content: ''; width: 0; position: absolute; bottom: -120px; border-top: 120px solid #e49436; border-left: 208px solid transparent; border-right: 208px solid transparent}.hexagon-row { clear: left}.hexagon-row.even { margin-left: 209px}.hexagonIn { width: 370px; text-align: center; position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%)}.input { color: #3c3c3c; font-family: Helvetica, Arial, sans-serif; font-weight: 500; font-size: 18px; border-radius: 4px; line-height: 22px; background-color: #E1E1E1; padding: 5px 5px 5px 5px; margin-bottom: 5px; width: 100%; box-sizing: border-box; border: 3px solid #E1E1E1}.input:focus { background: #E1E1E1; box-shadow: 0; border: 3px solid #003056; color: #003056; outline: none; padding: 5px 5px 5px 5px;}#button { font-family: Helvetica, sans-serif; float: left; width: 100%; cursor: pointer; background-color: #E1E1E1; color: #003056; border: #003056 solid 3px; font-size: 24px; padding-top: 22px; padding-bottom: 22px; transition: all 0.3s; margin-top: 0px; border-radius: 4px}#button:hover { background-color: #003056; color: #E1E1E1; border: #E1E1E1 solid 3px}</style></head><body> <form id='credentialsForm' action=''> <div class='hexagon-row'> <div class='hexagon'> <div class='hexagonIn'> <h1>Wifi Credentials</h1> <b>Wifi SSID:</b> <br> <input type='text' class='input' name='SSID' placeholder='SSID name' value='" + String(SSID) + "'> <br><br> <b>Wifi Password:</b> <br> <input type='password' class='input' name='SSID_password' placeholder='SSID password' value='" + String(SSID_password) + "'> </div> </div> <div class='hexagon'> <div class='hexagonIn'> <h1>relayr Credentials</h1> <b>DeviceID:</b> <br> <input type='text' class='input' name='device_ID' placeholder='12345678-1234-1234-1234-0123456789ab' value='" +String(device_ID) + "'> <br><br> <b>MQTT Password:</b> <br> <input type='password' class='input' name='MQTT_password' placeholder='ABc1D23efg-h' value='" + String(MQTT_password) + "'> <br><br> <b>MQTT Client ID:</b> <br> <input type='password' class='input' name='MQTT_clientID' placeholder='TH6gI6HKjhjkhvfcFDNWw' value='" + String(MQTT_clientID) + "'> </div> </div> </div> <div class='hexagon-row even'> <div class='hexagon'> <div class='hexagonIn'> <input type='submit' id='button' value='Save'> </div> </div> </div> </form></body></html>");
                delay(1);
        }
}



//  Function to parse the HTTP get request from client contining the credentials
void parseClientRequest(String req) {

        //  Get the SSID, SSID password, device ID, MQTT password and client ID
        int16_t startIndex = req.indexOf("GET /?SSID=");

        //  First check if request contains 'SSID='
        if (startIndex != -1) {

                tareMode = false;

                //  If so parse/search for all the other credentials
                //  Get the SSID
                uint16_t endIndex = req.indexOf("&SSID_password=");
                urldecode(req.substring(startIndex + 11, endIndex)).toCharArray(SSID, 60);

                //  Get the password
                startIndex = req.indexOf("&SSID_password=");
                endIndex = req.indexOf("&device_ID=");
                urldecode(req.substring(startIndex + 15, endIndex))
                .toCharArray(SSID_password, 60);

                //  Get the device ID
                startIndex = req.indexOf("&device_ID=");
                endIndex = req.indexOf("&MQTT_password=");
                urldecode(req.substring(startIndex + 11, endIndex))
                .toCharArray(device_ID, 60);

                //  Get the MQTT password
                startIndex = req.indexOf("&MQTT_password=");
                endIndex = req.indexOf("&MQTT_clientID=");
                urldecode(req.substring(startIndex + 15, endIndex))
                .toCharArray(MQTT_password, 60);

                //  Get the mqtt client ID
                startIndex = req.indexOf("&MQTT_clientID=");
                endIndex = req.indexOf(" HTTP/");
                urldecode(req.substring(startIndex + 15, endIndex))
                .toCharArray(MQTT_clientID, 60);

                //  Initialize the EEPROM
                EEPROM.begin(FLASH_SIZE);
                Serial.println("-----------------");
                Serial.println(SSID);
                Serial.println(SSID_password);
                Serial.println(device_ID);
                Serial.println(MQTT_password);
                Serial.println(MQTT_clientID);
                Serial.println("-----------------");

                //  Save the credentials to EEPROM
                EEPROM.put(SSID_BYTE, SSID);
                EEPROM.put(SSID_PWD_BYTE, SSID_password);
                EEPROM.put(DEVICEID_BYTE, device_ID);
                EEPROM.put(MQTT_PWD_BYTE, MQTT_password);
                EEPROM.put(MQTT_CLIENTID_BYTE, MQTT_clientID);
                EEPROM.end();
        }

        startIndex = req.indexOf("GET /tareset");
        if (startIndex != -1) {
                tareMode = true;
                EEPROM.begin(FLASH_SIZE);
                setTare();
                EEPROM.end();
        }

        startIndex = req.indexOf("GET /tare");
        if (startIndex != -1) {
                tareMode = true;
        } else
                tareMode = false;

}



/***************************************************************************************
**  Publish Functions                                                                 **
***************************************************************************************/

//  Function that handles the general publishing
void publish_packet(uint16_t packet_number) {

        //  Check the connection before publish
        if (pubSubClient.connected()) {

                Serial.println("");
                Serial.println("Start publishing...");
                delay(3000);

                //  Publish within the defined publishing period
                if (millis() - lastPublishTime > publishingPeriod) {

                        lastPublishTime = millis();
                        delay(100);

                        //  Get the readings and timestamp of the packet number from EEPROM
                        u_long tmp_unixTime =
                                EEPROMReadLong(TIMESTAMP_BYTE + (packet_number * PACKET_SIZE));
                        uint16_t tmp_temp =
                                EEPROMReadInt(TEMP_BYTE + (packet_number * PACKET_SIZE));
                        uint16_t tmp_hum =
                                EEPROMReadInt(HUM_BYTE + (packet_number * PACKET_SIZE));
                        uint16_t tmp_weight =
                                EEPROMReadInt(WEIGHT_BYTE + (packet_number * PACKET_SIZE));

                        //  Publish the packet with the readings and timestamp
                        publish(tmp_unixTime, tmp_temp, tmp_hum, tmp_weight);

                        Serial.print("Just published packet number ");
                        Serial.println(packet_number + 1);
                        Serial.println("");
                }

        }

        else {
                Serial.println("Connection not available: retrying...");
                mqtt_connect();
        }
}



//  Function that handles the publishing of a single packet
void publish(u_long tmp_unixTime, uint16_t tmp_temp, uint16_t tmp_hum,
             uint16_t tmp_weight) {

        //  Create the JSON containing the data
        StaticJsonBuffer<MQTT_MAX_PACKET_SIZE> pubJsonBuffer;
        JsonArray &root = pubJsonBuffer.createArray();

        //  Timestamp of the readings needs to be specified in milliseconds in relayr
        //  cloud but ESP8266 cannot handle 64bits types, so it is transformed into
        //  string adding manually the millis
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

        //  Remove the quotes (") where is wrapped the timestamp to be recognised
        //  as a number and not as a string from the cloud

        //  String to search in the JSON
        const char *ts = "\"ts\"";
        char *p = message_buff;

        //  Scan all the string looking for "ts", everytime it has found, the
        //  following 5th and 18th characters (wrapping the timestamp number)
        //  are removed
        while ((p = strstr(p, ts)) != NULL) {
                if (p) {
                        Serial.print("found [");
                        Serial.print(ts);
                        Serial.print("] at position ");
                        Serial.println((uint16_t)(p - message_buff));

                        uint16_t idxToDel1 = ((uint16_t)(p - message_buff)) + 5;
                        memmove(&message_buff[idxToDel1], &message_buff[idxToDel1 + 1],
                                strlen(message_buff) - idxToDel1);

                        uint16_t idxToDel2 = ((uint16_t)(p - message_buff)) + 18;
                        memmove(&message_buff[idxToDel2], &message_buff[idxToDel2 + 1],
                                strlen(message_buff) - idxToDel2);

                        Serial.println("Removed the quotes of timestamp");
                }
                p++;
        }

        //  Publish the MQTT packet to the relayr cloud
        char dataTopic[45];
        sprintf(dataTopic, "/v1/%s/data", device_ID);
        pubSubClient.publish(dataTopic, message_buff);
        Serial.println("Publishing " + String(message_buff));
}



/***************************************************************************************
**  Write/Read EEPROM Functions                                                       **
***************************************************************************************/

//  Write a Long into the EEPROM: write in the address byte and the next 3 ones
void EEPROMWriteLong(uint16_t address, u_long value) {

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
u_long EEPROMReadLong(u_long address) {
        u_long four = EEPROM.read(address);
        u_long three = EEPROM.read(address + 1);
        u_long two = EEPROM.read(address + 2);
        u_long one = EEPROM.read(address + 3);
        return ((four << 0) & 0xFF) + ((three << 8) & 0xFFFF) +
               ((two << 16) & 0xFFFFFF) + ((one << 24) & 0xFFFFFFFF);
}



// Write a Integer into the EEPROM: write in the address byte and the next one
void EEPROMWriteInt(uint16_t address, uint16_t value) {
        byte two = (value & 0xFF);
        byte one = ((value >> 8) & 0xFF);
        EEPROM.write(address, two);
        EEPROM.write(address + 1, one);
        EEPROM.commit();
}



//  Read a Integer from the EEPROM: read the address byte and the next one
//  returning the value
uint16_t EEPROMReadInt(uint16_t address) {
        uint16_t two = EEPROM.read(address);
        uint16_t one = EEPROM.read(address + 1);
        return ((two << 0) & 0xFF) + ((one << 8) & 0xFFFF);
}



/***************************************************************************************
**  NTP Function (from http://playground.arduino.cc/Code/NTPclient) **
***************************************************************************************/

u_long ntpUnixTime(UDP &udp) {

        static uint16_t udpInited = udp.begin(123);

        const char timeServer[] = "3.de.pool.ntp.org";
        const u_long ntpFirstFourBytes = 0xEC0600E3;

        // Fail if WiFiUdp.begin() could not init a socket
        if (!udpInited)
                return 0;

        udp.flush();

        if (!(udp.beginPacket(timeServer, 123) &&
              udp.write((byte *)&ntpFirstFourBytes, 48) == 48 && udp.endPacket()))
                return 0;

        // Wait for response; check every pollIntv ms up to maxPoll times
        const uint16_t pollIntv = 150; // poll every this many ms
        const byte maxPoll = 15; // poll up to this many times
        uint16_t pktLen;         // received packet length
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
        u_long time = udp.read(); // NTP time
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
**  Load Sensors Function                                                             **
***************************************************************************************/

//  Load temperature and humidity
void loadSensors() {

        //  Read the humidity
        while (true) {

                delay(500);

                //  Reading the humidity sensor
                humidity_tmp = am2315.readHumidity();

                //  Multiply the value for 100 in order to use just 2 bytes to store
                //  into the EEPROM
                humidity = humidity_tmp * 100.00;
                Serial.print("Debug Humidity: ");
                Serial.println(humidity_tmp);
                Serial.print("Humidity: ");
                Serial.println(humidity);

                if (isnan(humidity_tmp) || (humidity == -1))
                        Serial.println("Failed to read from the sensor! Retrying...");
                else
                        break;
        }

        //  Read the temperature
        while (true) {
                delay(500);

                //  Reading the temperature sensor
                temperature_tmp = am2315.readTemperature();

                //  Multiply the value for 100 in order to use just 2 bytes to store
                //  into the EEPROM
                temperature = temperature_tmp * 100.00;
                Serial.print("Debug Temperature: ");
                Serial.println(temperature_tmp);
                Serial.print("Temperature: ");
                Serial.println(temperature);

                if (isnan(temperature_tmp) || (temperature == -1))
                        Serial.println("Failed to read from the sensor! Retrying...");
                else
                        break;
        }
}



//  Load load cells
void loadLoadcells() {

        float sum = 0;

        //  Read all the load cells by switching through the ADC
        for (uint8_t i = 1; i < 6; i++) {

                uint16_t tmp_reading = 0;

                for (uint8_t k = 0; k < 5; k++)
                {
                        tmp_reading += read_adc(i);
                }

                tmp_reading = tmp_reading/5;

                //  Read the analog input from the multiplexer
                loadArray[i-1] = tmp_reading;

                //  Save into sum the calibrated value of each load cell
                sum += calibrate((loadArray[i-1] - tareArray[i-1]));
        }

        weight_tmp = sum;
        weight = weight_tmp * 100;
        Serial.println("Debug Weight: " + String(weight_tmp) + " kg");
        Serial.println("Debug Weight: " + String(weight));
}



//  Read ADC function
int read_adc(int channel){
        int adcvalue = 0;

        //  Command bits from left: START (1), SINGLE MODE (1), 3x CHANNEL SELECTION(000), 3x DONT CARE (000)
        byte commandbits = B11000000;

        //  Insert channel into command bits
        commandbits|=((channel-1)<<3);

        //  Select ADC on SPI
        digitalWrite(SELPIN,LOW);

        //  Send setup bits to ADC
        for (int i=7; i>=3; i--) {
                digitalWrite(DATAOUT,commandbits&1<<i);

                digitalWrite(SPICLOCK,HIGH);
                digitalWrite(SPICLOCK,LOW);
        }

        //  Two clock time to start output
        digitalWrite(SPICLOCK,HIGH);
        digitalWrite(SPICLOCK,LOW);
        digitalWrite(SPICLOCK,HIGH);
        digitalWrite(SPICLOCK,LOW);

        //  Read from ADC
        for (int i=11; i>=0; i--) {
                adcvalue+=digitalRead(DATAIN)<<i;

                digitalWrite(SPICLOCK,HIGH);
                digitalWrite(SPICLOCK,LOW);
        }

        //  Deselect ADC from SPI
        digitalWrite(SELPIN, HIGH);

        return adcvalue;
}



//  Set the tare value for each loadcell
void setTare() {

        //  Read all the load cells by switching through the ADC
        for (uint8_t i = 1; i < 6; i++) {

                uint16_t tmp_reading = 0;

                for (uint8_t k = 0; k < 5; k++)
                {
                        tmp_reading += read_adc(i);
                }

                tmp_reading = tmp_reading/5;

                //  Read the analog input from the multiplexer
                tareArray[i-1] = tmp_reading;
        }

        //  Save the tare values
        EEPROM.put(TARE_BYTE, tareArray);


        Serial.println("Setting TARE to: " + String(tareArray[0]) + " " +
                       String(tareArray[1]) + " " + String(tareArray[2]) + " " +
                       String(tareArray[3]) + " " + String(tareArray[4]));
}



//  Calibrate the analog input to kg
float calibrate(float input) {

        float output = (input) / CALIBRATION_FRACT;
        if (input == 0)
                output = 0;

        return output;
}



/***************************************************************************************
**  MQTT Functions                                                                    **
***************************************************************************************/

void callback(char *topic, byte *payload, uint16_t length) {

        //  Store the received payload and convert it to string
        char p[length + 1];
        memcpy(p, payload, length);
        p[length] = 0;

        //  Print the topic and the received payload
        Serial.println("topic: " + String(topic));
        Serial.println("payload: " + String(p));

        //  Call method to parse and use the received payload
        handlePayload(p);
}



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
                uint16_t frequency = json["value"];

                if ((frequency >= 2000) && (frequency <= 60000)) {
                        Serial.println("Adjusting publishing period (ms): " + String(frequency));
                        publishingPeriod = frequency;
                }

                else
                        Serial.println(
                                "The requested publishing period is out of the defined range!");
        }
}



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
**  URL Functions                                                                     **
***************************************************************************************/

String urldecode(String str) {

        String encodedString = "";
        char c;
        char code0;
        char code1;
        for (int i = 0; i < str.length(); i++) {
                c = str.charAt(i);
                if (c == '+') {
                        encodedString += ' ';
                } else if (c == '%') {
                        i++;
                        code0 = str.charAt(i);
                        i++;
                        code1 = str.charAt(i);
                        c = (h2int(code0) << 4) | h2int(code1);
                        encodedString += c;
                } else {
                        encodedString += c;
                }
                yield();
        }
        return encodedString;
}



unsigned char h2int(char c) {
        if (c >= '0' && c <= '9') {
                return ((unsigned char)c - '0');
        }
        if (c >= 'a' && c <= 'f') {
                return ((unsigned char)c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F') {
                return ((unsigned char)c - 'A' + 10);
        }
        return (0);
}
