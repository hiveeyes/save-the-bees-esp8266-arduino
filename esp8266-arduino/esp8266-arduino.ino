// Libraries used: To be added manually on the Arduino IDE!
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_AM2315.h>

// Pins for the Multiplexer & Loadcells.
#define LoadPin A0
#define TarePin D6
#define ModePin D5
#define S0 D8
#define S1 D7

// WiFi and relayr MQTT credentials.
#define MQTT_CLIENTID "theBeePrototype01"
#define MQTT_SERVER "mqtt.relayr.io"
// Stored in EEPROM.
char ssid[60];
char password[60];
char relayrUser[60];
char relayrPassword[60];

WiFiServer server(80);
Adafruit_AM2315 am2315;

void getTempHumiReadings();
void getLoadReadings();
void setTare();
float calibrate();

// This creates the WiFi client and the pub-sub client instance.
WiFiClient espClient;
PubSubClient client(espClient);

// Config variables.
const int sleepTimeS = 30; // seconds
bool sendMode;
bool testMode = false;

// Function prototypes.
void setup_wifi();
void mqtt_connect();
void callback(char* topic, byte* payload, unsigned int length);
void handlePayload(char* payload);
void publish();

// Varibales for the AM2315 Sensor.
float temperature = 0;
float humidity = 0;
float resultantForce = 0;
int loadArray[4];
int tareArray[4];
int tareAddress = 0;   // Location we want the data to be put.

//------------------------------------------------------------------------------------//
//                                       SETUP                                        //
//------------------------------------------------------------------------------------//

void setup()
{
  // Initializing pins.
//  pinMode(D1, INPUT_PULLUP);
//  pinMode(D2, INPUT_PULLUP);
  pinMode(S0, OUTPUT);
  pinMode(S1, OUTPUT);
  pinMode(LoadPin, INPUT);
  pinMode(TarePin, INPUT);
  pinMode(ModePin, INPUT);

  Serial.begin(9600);

  // Load stored data from the EEPROM.
  EEPROM.begin(4096);
  EEPROM.get(tareAddress, tareArray);
  EEPROM.get(16, ssid);
  EEPROM.get(76, password);
  EEPROM.get(136, relayrUser);
  EEPROM.get(196, relayrPassword);
  EEPROM.end();

  delay(100);

  // Debug. //
  Serial.println("-----------------");
  Serial.println("TARE: " + String(tareArray[0]) + " " + String(tareArray[1]) + " " + String(tareArray[2]) + String(tareArray[3]));
  Serial.println(ssid);
  Serial.println(password);
  Serial.println(relayrUser);
  Serial.println(relayrPassword);
  Serial.println("-----------------");
  // ----- //

  sendMode = digitalRead(ModePin);
  Serial.println("Send mode: " + String(sendMode));

  // Check in which mode is the node and execute appropriate setup.
  if (!sendMode) {
    Serial.println("Setup mode.");
    setupServer();
  } else {
    Serial.println("Default mode.");
    setup_wifi();
    client.setServer(MQTT_SERVER, 1883);
    client.setCallback(callback);

    Wire.begin();     // create a wire object
    mqtt_connect();

//    delay(1000);
    if (! am2315.begin()) {
      Serial.println("Sensor not found, check wiring & pullups!");
      while (1);
    }
  }
}

//------------------------------------------------------------------------------------//
// This function connects to the WiFi network, and prints the current IP address      //
//------------------------------------------------------------------------------------//

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println("");
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

//------------------------------------------------------------------------------------//
// Callback function, necessary for the MQTT communication                            //
//------------------------------------------------------------------------------------//

void callback(char* topic, byte* payload, unsigned int length)
{
  //Store the received payload and convert it to string
  char p[length + 1];
  memcpy(p, payload, length);
  p[length] = NULL;
  //Print the topic and the received payload
  Serial.println("topic: " + String(topic));
  Serial.println("payload: " + String(p));
  //Call our method to parse and use the received payload
  handlePayload(p);
}


//------------------------------------------------------------------------------------//
// This processes the payload; commands and configurations should be implemented here //
//------------------------------------------------------------------------------------//

void handlePayload(char* payload)
{
  StaticJsonBuffer<200> jsonBuffer;
  //Convert payload to json
  JsonObject& json = jsonBuffer.parseObject(payload);

  if (!json.success())
  {
    Serial.println("json parsing failed");
    return;
  }

  //Get the value of the key "name", aka. listen to incoming commands and configurations
  const char* command = json["name"];
  Serial.println("parsed command: " + String(command));
}


//------------------------------------------------------------------------------------//
// This function establishes the connection with the MQTT server                      //
//------------------------------------------------------------------------------------//

void mqtt_connect()
{
  Serial.println("");
  Serial.println("Connecting to MQTT server...");

  if (client.connect(MQTT_CLIENTID, relayrUser, relayrPassword))
  {
    Serial.println("Connection successful!");
//    Here you can also subscribe to topics.
//    char cmdTopic[44];
//    char configTopic[47];
//    sprintf(cmdTopic, "/v1/%s/cmd", relayrUser);
//    sprintf(configTopic, "/v1/%s/config", relayrUser);
//    client.subscribe(cmdTopic);
//    client.subscribe(configTopic);
  }

  else
  {
    Serial.println("Connection failed! Check your credentials or the WiFi network");
    //This reports the error code
    Serial.println("rc = ");
    Serial.print(client.state());
    //And it tries again in 5 seconds
    Serial.println("Retrying in 5 seconds...");
    delay(5000);
  }
}


//------------------------------------------------------------------------------------//
// This is the MAIN LOOP, it's repeated until the end of time! :)                     //
//------------------------------------------------------------------------------------//

void loop() {

  if (!sendMode) {
    runServer();
  }
  else {
    
    //If we're connected, we can send data...
    if (client.connected())
    {
      client.loop();
      //Publishing...
      getTempHumiReadings();
      getLoadReadings();
      publish();
      Serial.println("Going to sleep for " + String(sleepTimeS) + " seconds.");
      ESP.deepSleep(sleepTimeS * 1000000);
    }
    
    //If the connection is lost, then reconnect...
    else
    {
      Serial.println("Retrying...");
      mqtt_connect();
    }

    // This function prevents the device from crashing
    // by allowing the ESP8266 background functions to be executed
    // (WiFi, TCP/IP stack, etc.).
    yield();
  }
}

//------------------------------------------------------------------------------------//
// Publish function: What we want to send to the relayr Cloud                         //
//------------------------------------------------------------------------------------//

void publish()
{
  //MQTT_MAX_PACKET_SIZE is defined in "PubSubClient.h", it's 128 bytes by default
  //A modified version with 512 bytes it's available here:
  //   https://github.com/uberdriven/pubsubclient
  StaticJsonBuffer<MQTT_MAX_PACKET_SIZE> pubJsonBuffer;
  //Create our JsonArray
  JsonArray& root = pubJsonBuffer.createArray();

  //-------------------------------------------------
  //First object: analog input 0
  JsonObject& leaf1 = root.createNestedObject();
  //This is how we name what we are sending
  leaf1["meaning"] = "temperature";
  //This contains the readings of the port
  leaf1["value"] = temperature;
  //-------------------------------------------------

  //-------------------------------------------------
  JsonObject& leaf2 = root.createNestedObject();
  //This is how we name what we are sending
  leaf2["meaning"] = "humidity";
  //This contains the value of the counter (increased with every iteration)
  leaf2["value"] = humidity;
  //-------------------------------------------------

  //-------------------------------------------------
  JsonObject& leaf3 = root.createNestedObject();
  //This is how we name what we are sending
  leaf3["meaning"] = "weight";
  //This contains the value of the counter (increased with every iteration)
  leaf3["value"] = resultantForce;
  //-------------------------------------------------


  char message_buff[MQTT_MAX_PACKET_SIZE];
  root.printTo(message_buff, sizeof(message_buff));
  char dataTopic[45];
  sprintf(dataTopic, "/v1/%s/data", relayrUser);
  client.publish(dataTopic, message_buff);
  Serial.println("Publishing " + String(message_buff));
}


//------------------------------------------------------------------------------------//
// Sensor specific functions                                                          //
//------------------------------------------------------------------------------------//

void getTempHumiReadings() {
  delay(500);
  temperature = am2315.readTemperature();
  delay(500);
  humidity = am2315.readHumidity();
  delay(500);
}

void getLoadReadings() {
  int sum = 0;

  if (digitalRead(TarePin)) setTare();


  // Read all the load cells by switching through the multiplexer
  for (int i = 0; i < 4; i++) {
    //Serial.println(String(bitRead(i, 0), HEX) + "  " + String(bitRead(i, 1), HEX));

    digitalWrite(S0, bitRead(i, 0));
    digitalWrite(S1, bitRead(i, 1));

    loadArray[i] = analogRead(LoadPin);

    //Serial.println(String(loadArray[i]) + " " + String(tareArray[i]) + " " + String(loadArray[i] - tareArray[i]));

    sum += (loadArray[i] - tareArray[i]);
  }
  //Serial.println(sum);
  resultantForce = calibrate(sum / 4);
}

void setTare() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(S0, bitRead(i, 0));
    digitalWrite(S1, bitRead(i, 1));
    tareArray[i] = analogRead(LoadPin);
  }
  EEPROM.begin(4096);
  EEPROM.put(tareAddress, tareArray);
  EEPROM.end();
  Serial.println("Setting TARE to: " + String(tareArray[0]) + " " + String(tareArray[1]) + " " + String(tareArray[2]) + String(tareArray[3]));
}

// calibrate volatge to kg
float calibrate(float input) {
  Serial.println(input);
  float output = 0.071962 * input + 0.048715;
  if (input == 0) output = 0;
  //float output = input * 0.0857142857;
  return output;
}

//------------------------------------------------------------------------------------//
// Enduser wifi and relayr setup functions.                                           //
//------------------------------------------------------------------------------------//

void setupServer() {

  // Setup the access point.
  WiFi.mode(WIFI_AP);
  WiFi.softAP("BeeSetup");

  // Start the server.
  server.begin();
  Serial.println("Server started");

  // Print the IP address.
  Serial.print("Access the server at http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");
}

void runServer() {
  WiFiClient wifiClient = server.available();
  if (!wifiClient) {
    return;
  }

  // Wait until the client sends some data
  while (!wifiClient.available()) {
    delay(1);
  }

  // Read and parse the request.
  String request = wifiClient.readString();
  parseClientRequest(request);
  wifiClient.flush();

  // Return the response
  wifiClient.println("HTTP/1.1 200 OK");
  wifiClient.println("Content-Type: text/html");
  wifiClient.println("");
  wifiClient.println("<!DOCTYPE html><html><head><title>bees</title><style>body{background:#323a45;font-family:sans-serif;color:#343434}.hex:before{content:' ';width:0;height:0;border-bottom:120px solid #e49436;border-left:208px solid transparent;border-right:208px solid transparent;position:absolute;top:-120px}.hex{margin-top:124px;width:416px;height:240px;background-color:#e49436;position:relative;float:left;margin-left:3px}.hex:after{content:'';width:0;position:absolute;bottom:-120px;border-top:120px solid #e49436;border-left:208px solid transparent;border-right:208px solid transparent;z-index:1}.hex-row{clear:left}.hex-row.even{margin-left:209px}.hexIn{width:370px;text-align:center;position:absolute;top:50%;left:50%;transform:translate(-50%,-50%)}input[type=text]{width:300px;height:40px;background-color:#fff;color:#111;font-size:1em}input[type=text]{font-family:monospace;width:300px;padding:2px 2px;box-sizing:border-box;border:2px solid #ccc;border-radius:4px;background-color:#f8f8f8;z-index:10}#submit{line-height:32px;font-size:1em;color:#343434;font-family:sans-serif;font-weight:bold;border:2px solid #ccc;border-radius:4px;background-color:#f8f8f8}#submit:hover{color:#f8f8f8;background-color:#ccc}</style></head>");
  wifiClient.println("<body><div style='float: left; width: 100%;'><div class='hex-row'><form id='formID' action=''><div class='hex'><div class='hexIn'><h1>Wifi Credentials</h1> <b>Wifi SSID:</b> <br> <input type='text' name='SSID' placeholder='myWifi' value='"+String(ssid)+"'> <br> <b>Wifi Password:</b> <br> <input type='text' name='PASSWORD' placeholder='myPassword' value='"+String(password)+"'></div></div><div class='hex'><div class='hexIn'><h1>relayr Credentials</h1> <span>Please find the credentials in the <a href='https://developer.relayr.io/dashboard/devices/'>developer dashboard</a> on the device site behind the samll gear.</span> <br> <b>relayr DeviceID:</b> <input type='text' name='RELAYRUSER' placeholder='12345678-1234-1234-1234-0123456789ab' value='"+String(relayrUser)+"'> <br><b>relayr Device Password:</b> <br> <input type='text' name='RELAYRPASSWORD' placeholder='ABc1D23efg-h' value='"+String(relayrPassword)+"'></div></div></div><div class='hex-row even'><div class='hex'><div class='hexIn'> <pre>"+String("SomeMessage")+"</pre> <br> <br> <input type='submit' id='submit' value='Save to Device'></div></div></form></div></div></body></html>");
  delay(1);
}

void parseClientRequest(String req){

  // Get the SSID, PASSWORD, RELAYRUSER and RELAYRPASSWORD.
  int startIndex = req.indexOf("GET /?SSID=");
  // First check if request contains 'SSID='.
  if(startIndex != -1){
    // If so parse/search for all the other points of interest.
    int endIndex = req.indexOf("&PASSWORD=");
    req.substring(startIndex+11,endIndex).toCharArray(ssid, 60);
    // Get the PASSWORD.
    startIndex = req.indexOf("&PASSWORD=");
    endIndex = req.indexOf("&RELAYRUSER=");
    req.substring(startIndex+10,endIndex).toCharArray(password, 60);
    // Get the RELAYRUSER.
    startIndex = req.indexOf("&RELAYRUSER=");
    endIndex = req.indexOf("&RELAYRPASSWORD=");
    req.substring(startIndex+12,endIndex).toCharArray(relayrUser, 60);
    // Get the RELAYRPASSWORD.
    startIndex = req.indexOf("&RELAYRPASSWORD=");
    endIndex = req.indexOf(" HTTP/");
    req.substring(startIndex+16, endIndex).toCharArray(relayrPassword, 60);

    // Debug. //
    Serial.println("-----------------");
    Serial.println(ssid);
    Serial.println(password);
    Serial.println(relayrUser);
    Serial.println(relayrPassword);
    Serial.println("-----------------");
    // ----- //

    EEPROM.begin(4096);
    EEPROM.put(16, ssid);
    EEPROM.put(76, password);
    EEPROM.put(136, relayrUser);
    EEPROM.put(196, relayrPassword);
    EEPROM.end();
  }
}
