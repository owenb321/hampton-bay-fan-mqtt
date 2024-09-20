#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// Configure wifi settings
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"

// Configure MQTT broker settings
#define MQTT_HOST "192.168.1.0"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#define MQTT_CLIENT_NAME "HAMPTONBAY"
#define BASE_TOPIC "home/hamptonbay"

#define STATUS_TOPIC BASE_TOPIC "/status"
#define SUBSCRIBE_TOPIC_ON_SET BASE_TOPIC "/+/on/set"
#define SUBSCRIBE_TOPIC_ON_STATE BASE_TOPIC "/+/on/state"
#define SUBSCRIBE_TOPIC_SPEED_SET BASE_TOPIC "/+/speed/set"
#define SUBSCRIBE_TOPIC_SPEED_STATE BASE_TOPIC "/+/speed/state"
#define SUBSCRIBE_TOPIC_LIGHT_SET BASE_TOPIC "/+/light/set"
#define SUBSCRIBE_TOPIC_LIGHT_STATE BASE_TOPIC "/+/light/state"

// Set receive and transmit pin numbers (GDO0 and GDO2)
#ifdef ESP32 // for esp32! Receiver on GPIO pin 4. Transmit on GPIO pin 2.
  #define RX_PIN 4 
  #define TX_PIN 2
#elif ESP8266  // for esp8266! Receiver on pin 4 = D2. Transmit on pin 5 = D1.
  #define RX_PIN 4
  #define TX_PIN 5
#else // for Arduino! Receiver on interrupt 0 => that is pin #2. Transmit on pin 6.
  #define RX_PIN 0
  #define TX_PIN 6
#endif 

// Set CC1101 frequency
// 303.631 determined from FAN-9T remote tramsmissions
#define FREQUENCY     303.631

// RC-switch settings
#define RF_PROTOCOL 6
#define RF_REPEATS  8

// Define fan states
#define FAN_OFF 0
#define FAN_HI  1
#define FAN_MED 2
#define FAN_LOW 3

const char *fanStateTable[4] = {
  "off", "high", "medium", "low"
};

RCSwitch mySwitch = RCSwitch();
WiFiClient espClient;
PubSubClient client(espClient);


int long value;      // int to save value
int bits;           // int to save bit number
int prot;          // int to save Protocol number

// Keep track of states for all dip settings
struct fan
{
  bool lightState;
  bool fanState;
  uint8_t fanSpeed;
};
fan fans[16];


// The ID returned from the RF code appears to be inversed and reversed
//   e.g. a dip setting of on off off off (1000) yields 1110
// Convert between IDs from MQTT from dip switch settings and what is used in the RF codes
const byte dipToRfIds[16] = {
    [ 0] = 15, [ 1] = 7, [ 2] = 11, [ 3] = 3,
    [ 4] = 13, [ 5] = 5, [ 6] =  9, [ 7] = 1,
    [ 8] = 14, [ 9] = 6, [10] = 10, [11] = 2,
    [12] = 12, [13] = 4, [14] =  8, [15] = 0,
};
const char *idStrings[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};
char idchars[] = "01";

void setup_wifi() {

  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void transmitState(int fanId) {
  ELECHOUSE_cc1101.SetTx();           // set Transmit on
  mySwitch.disableReceive();         // Receiver off
  mySwitch.enableTransmit(TX_PIN);   // Transmit on
  mySwitch.setRepeatTransmit(RF_REPEATS); // transmission repetitions.
  mySwitch.setProtocol(RF_PROTOCOL);        // send Received Protocol

  // Determine fan component of RF code
  int fanRf = fans[fanId].fanState ? fans[fanId].fanSpeed : 0;
  // Build out RF code
  //   Code follows the 21 bit pattern
  //   000xxxx000000yzz00000
  //   Where x is the inversed/reversed dip setting, 
  //     y is light state, z is fan speed
  int rfCode = dipToRfIds[fanId] << 14 | fans[fanId].lightState << 7 | fanRf << 5;
  
  mySwitch.send(rfCode, 21);      // send 21 bit code
  ELECHOUSE_cc1101.SetRx();      // set Receive on
  mySwitch.disableTransmit();   // set Transmit off
  mySwitch.enableReceive(RX_PIN);   // Receiver on

  postStateUpdate(fanId);
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  char payloadChar[length + 1];
  sprintf(payloadChar, "%s", payload);
  payloadChar[length] = '\0';

  // Get ID after the base topic + a slash
  char id[5];
  memcpy(id, &topic[sizeof(BASE_TOPIC)], 4);
  id[4] = '\0';
  if(strspn(id, idchars)) {
    uint8_t idint = strtol(id, (char**) NULL, 2);
    char *attr;
    char *action;
    // Split by slash after ID in topic to get attribute and action
    attr = strtok(topic+sizeof(BASE_TOPIC) + 5, "/");
    action = strtok(NULL, "/");
    // Convert payload to lowercase
    for(int i=0; payloadChar[i]; i++) {
      payloadChar[i] = tolower(payloadChar[i]);
    }
    
    if(strcmp(attr, "on") == 0) {
      if(strcmp(payloadChar, "on") == 0) {
        fans[idint].fanState = true;
      }
      else if(strcmp(payloadChar, "off") == 0) {
        fans[idint].fanState = false;
      }
    }
    else if(strcmp(attr, "speed") == 0) {
      if(strcmp(payloadChar, "low") == 0) {
        fans[idint].fanSpeed = FAN_LOW;
      }
      else if(strcmp(payloadChar, "medium") == 0) {
        fans[idint].fanSpeed = FAN_MED;
      }
      else if(strcmp(payloadChar, "high") == 0) {
        fans[idint].fanSpeed = FAN_HI;
      }

      if(strcmp(payloadChar, "off") == 0) {
        // 'off' state is not recorded, just turn the fan off
        fans[idint].fanState = false;
      }
      else {
        // Turn on fan when speed is updated
        fans[idint].fanState = true;
      }
    }
    else if(strcmp(attr, "light") == 0) {
      if(strcmp(payloadChar, "on") == 0) {
        fans[idint].lightState = true;
      }
      else if(strcmp(payloadChar, "off") == 0) {
        fans[idint].lightState = false;
      }
    }
    if(strcmp(action, "set") == 0) {
      transmitState(idint);
    }
  }
  else {
    // Invalid ID
    return;
  }
}

void postStateUpdate(int id) {
  char outTopic[100];
  sprintf(outTopic, "%s/%s/on/state", BASE_TOPIC, idStrings[id]);
  client.publish(outTopic, fans[id].fanState ? "ON":"OFF", true);
  sprintf(outTopic, "%s/%s/speed/state", BASE_TOPIC, idStrings[id]);
  client.publish(outTopic, fanStateTable[fans[id].fanSpeed], true);
  sprintf(outTopic, "%s/%s/light/state", BASE_TOPIC, idStrings[id]);
  client.publish(outTopic, fans[id].lightState ? "ON":"OFF", true);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(MQTT_CLIENT_NAME, MQTT_USER, MQTT_PASS, STATUS_TOPIC, 0, true, "offline")) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish(STATUS_TOPIC, "online", true);
      // ... and resubscribe
      client.subscribe(SUBSCRIBE_TOPIC_ON_SET);
      client.subscribe(SUBSCRIBE_TOPIC_ON_STATE);
      client.subscribe(SUBSCRIBE_TOPIC_SPEED_SET);
      client.subscribe(SUBSCRIBE_TOPIC_SPEED_STATE);
      client.subscribe(SUBSCRIBE_TOPIC_LIGHT_SET);
      client.subscribe(SUBSCRIBE_TOPIC_LIGHT_STATE);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(9600);

  // initialize fan struct
  for(int i=0; i<16; i++) {
    fans[i].lightState = false;
    fans[i].fanState = false;
    fans[i].fanSpeed = FAN_LOW;
  }

  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(FREQUENCY);
  ELECHOUSE_cc1101.SetRx();
  mySwitch.enableReceive(RX_PIN);

  setup_wifi();
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(callback);
}

void loop() {

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Handle received transmissions
  if (mySwitch.available()) {
    value =  mySwitch.getReceivedValue();        // save received Value
    prot = mySwitch.getReceivedProtocol();     // save received Protocol
    bits = mySwitch.getReceivedBitlength();     // save received Bitlength

    Serial.print(prot);
    Serial.print(" - ");
    Serial.print(value);
    Serial.print(" - ");
    Serial.println(bits);

    if( prot == 6 && bits == 21 ) {
      int id = value >> 14;
      // Got a correct id in the correct protocol
      if(id < 16) {
        // Convert to dip id
        int dipId = dipToRfIds[id];
        Serial.print("Received command from ID - ");
        Serial.println(dipId);
        // Blank out id in message to get light state
        int states = value & 0b11111111;
        bool light = states >> 7;
        // Blank out light state to get fan state
        int fan = (states & 0b01111111) >> 5;
        if(fan == 0) {
          fans[dipId].fanState = false;
        }
        else {
          fans[dipId].fanState = true;
          fans[dipId].fanSpeed = fan;
        }
        fans[dipId].lightState = light;
        postStateUpdate(dipId);
      }
    }
    mySwitch.resetAvailable();
  }
}
