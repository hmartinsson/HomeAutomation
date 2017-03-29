// RFM69 MQTT gateway sketch
//
// This gateway relays messages between a MQTT-broker and several wireless nodes and will:
// - receive sensor data from several nodes periodically and on-demand
// - send/receive commands from the broker to control actuators and node parameters
//
//  Connection to the MQTT broker is over a fixed ethernet connection:
//
//    The MQTT topic is /home/rfm_gw/direction/nodeid/devid
//    where direction is: southbound (sb) towards the remote node and northbound (nb) towards MQTT broker
//
//  Connection to the nodes is over a closed radio network:
//
//    RFM Message format is: nodeID/deviceID/command/integer/float/string
//    where Command = 1 for a read request and 0 for a write request
//
//  Current defined gateway devices are:
//  0 uptime:     gateway uptime in minutes 
//  3 Version:    read version gateway software
//  10 reset:     reset node when set.
//  30 power:     check if power is on or off. notify if off.
//  
//  Reserved ranges for node devices, as implemented in the gateway are:
//  0  - 15       Node system devices
//  16 - 31       Binary output (LED, relay)
//  32 - 39       Integer output (pwm, dimmer)
//  40 - 47       Binary input (button, switch, PIR-sensor)
//  48 - 63       Real input (temperature, humidity)
//  64 - 71       Integer input (light intensity)
//
//  72  string:     transparant string transport
//
//  73 - 90   Special devices not implemented in gateway (yet)
//
//  Currently defined error messages are:
//  90  error:      Tx only: error message if no wireless connection
//  91  error:      Tx only: syntax error
//  92  error:      Tx only: invalid device type
//  99  wakeup:     Tx only: sends a message on node startup
//
//  ==> Note: 
//    - Interrupts are disabled during ethernet transactions in w5100.h (ethernet library)
//      (See http://harizanov.com/2012/04/rfm12b-and-arduino-ethernet-with-wiznet5100-chip/)
//    - Ethernet card and RFM68 board default use the same Slave Select pin (10) on the SPI bus;
//      To avoid conflict the RFM module is controlled by another SS pin (8).
//
//
// RFM69 Library by Felix Rusu - felix@lowpowerlab.com
// Get the RFM69 library at: https://github.com/LowPowerLab
// Get the MQQT client library at: https://github.com/knolleary/pubsubclient
//
// version 1.8 - by Computourist@gmail.com december 2014
// version 1.9 - fixed resubscription after network outage  Jan 2015
// version 2.0 - increased payload size; standard device types; trim float values; uptime & version function gateway; Jan 2015
// version 2.1 - implemented string device 72; devices 40-48 handled uniformly    Feb 2015
// version 2.2 - changed handling of binary inputs to accomodate Openhab: message for ON and OFF on statechange; 
//         - RSSI value changed to reception strength in the gateway giving a more accurate and uptodate value ; March 2015
// version 2.3 - System device 9 (number of retransmissions) implemented in gateway ; 
//         - Deleted debug option 's' to toggle push interval due to memory constraints;  Oct 2015
// version 2.4 - fixed function declaration to comply with new Arduino IDE;
//         - changed debug routines to comply with memory constraints;    Jan 2016
// version 2.5 - added possibility to reset the arduino by sending msg '1' to device 10.

#include <RFM69.h>
#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include <avr/wdt.h>

//
// CONFIGURATION PARAMETERS
#define NODEID 1        // unique node ID in the closed radio network; gateway is 1
#define RFM_SS 8        // Slave Select RFM69 is connected to pin 8
#define NETWORKID 100       // closed radio network ID
#define ENCRYPTKEY "1234567890123456"     // shared 16-char encryption key is equal on Gateway and nodes
#define DEBUGRADIO          // uncomment for radio debugging
#define DEBUG       // uncomment for MQTT debugging
#define VERSION "GW V2.5"

// Wireless settings. Match frequency to the hardware version of the radio (uncomment one):
#define FREQUENCY RF69_868MHZ
#define IS_RFM69HW                        // uncomment only for RFM69HW! Leave out if you have RFM69W!
#define ACK_TIME 50                       // max # of ms to wait for an ack

// Ethernet settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xBA, 0xBE };  // MAC address for ethernet
byte mqtt_server[] = { 192, 168, 0, 6};   // MQTT broker address (Mosquitto)
byte ip[] = { 192, 168, 0 , 177 };      // Gateway address (if DHCP fails)

// PIN settings
#define POWER 5         // pin for detecting power loss
#define MQCON 7         // MQTT Connection indicator
#define R_LED 9         // Radio activity indicator
#define SERIAL_BAUD 115200

// Variables declaration
int   dest;         // destination node for radio packet
int   DID;          // Device ID
int   error;        // Syntax error code
int   powerReading;         // Analog reading from power pin
long  lastMinute = -1;      // timestamp last minute
long  upTime = 0;           // uptime in minutes
int   radioBlinkTime = 10;  // Time for radio LED on
int   powerOutCheckTime = 100; // Time interval to check power out.
bool  Rstat = false;        // radio indicator flag
bool  mqttCon = false;      // MQTT broker connection flag
bool  respNeeded = false;   // MQTT message flag in case of radio connection failure
bool  mqttToSend = false;   // message request issued by MQTT request
bool  retain_mess = false;   // MQTT broker retains messages
bool  promiscuousMode = false;    // only receive closed network nodes
bool  verbose = true;           // generate error messages
bool  IntMess, RealMess, StatMess, StrMess; // types of messages
bool  powerOut = true;              // Power out on or not
long  powerOutMillis;       // timestamp when power out was checked
long  onMillis;                       // timestamp when radio LED was turned on
char  *subTopic = "home/rfm_gw/sb/#"; // MQTT subscription topic ; direction is southbound
char  *clientName = "RFM_gateway";    // MQTT system name of gateway
char  buff_topic[30];       // MQTT publish topic string
char  buff_mess[32];        // MQTT publish message string


typedef struct {        // Radio packet structure max 66 bytes
int   nodeID;           // node identifier
int   devID;            // device identifier 0 is node; 31 is temperature, 32 is humidity
int   cmd;              // read or write
long  intVal;           // integer payload
float fltVal;           // floating payload
char  payLoad[32];      // char array payload
} Message;

Message mes;

RFM69 radio;
EthernetClient ethClient;

// Function declarations
void mqtt_subs(char* topic, byte* payload, unsigned int length);
PubSubClient mqttClient(mqtt_server, 1883, mqtt_subs, ethClient );

//
//==============  SETUP
//

void setup() {
#ifdef DEBUG
  Serial.begin(SERIAL_BAUD);
#endif
#ifdef DEBUGRADIO
  Serial.begin(SERIAL_BAUD);
#endif
  radio.setCS(RFM_SS);          // change default Slave Select pin for RFM
  radio.initialize(FREQUENCY,NODEID,NETWORKID);   // initialise radio module
#ifdef IS_RFM69HW
  radio.setHighPower();           // only for RFM69HW!
#endif
  radio.encrypt(ENCRYPTKEY);        // encrypt with shared key
  radio.promiscuous(promiscuousMode);     // listen only to nodes in closed network

  pinMode(R_LED, OUTPUT);         // set pin of radio indicator
  pinMode(MQCON, OUTPUT);         // set pin for MQTT connection indicator
  digitalWrite(R_LED, LOW);       // switch off radio indicator
  digitalWrite(MQCON, LOW);         // switch off MQTT connection indicator
  
#ifdef DEBUG
  Serial.print("GW Version ");
  Serial.println(VERSION);
  Serial.print("\n ");
  Serial.print(FREQUENCY==RF69_433MHZ ? 433 : FREQUENCY==RF69_868MHZ ? 868 : 915);
  Serial.println(" Mhz...");
#endif

  Ethernet.begin(mac, ip);
  // print your local IP address:
#ifdef DEBUG
  Serial.println(Ethernet.localIP());
  Serial.println("connecting");
#endif

  delay(1000);
  mqttCon = 0;          // reset connection flag
  while(mqttCon != 1){        // retry MQTT connection every 2 seconds
#ifdef DEBUG
    Serial.println("no link");
#endif
    mqttCon = mqttClient.connect(clientName); // retry connection to broker
    delay(2000);          // every 2 seconds
  }

  if(mqttCon){          // Connected !
#ifdef DEBUG
    Serial.println("MQTT-link OK");
#endif
    digitalWrite(MQCON, HIGH);      // switch on MQTT connection indicator
    mqttClient.subscribe(subTopic);     // subscribe to all southbound messages
  }
#ifdef DEBUG
  else Serial.println("MQTT-link NOK");
#endif
} // end setup

//==============  MAIN
void loop() {
  
  // CONTROL RADIO LED AND CALCULATE UPTIME 
  if (Rstat) {            // turn off radio LED after 10 msec
      if (millis() - onMillis > radioBlinkTime) {
        Rstat = false;
        digitalWrite(R_LED, LOW);
      }
  }

  // Check power and publish directly at power out.
  if (millis() - powerOutMillis > powerOutCheckTime) {
    powerOutMillis = millis();
    powerReading = analogRead(POWER);
    if (powerReading<620) {
      // Power out
#ifdef DEBUG
      if (!powerOut) Serial.println("Power is lost!");
#endif
      powerOut=true;
      sprintf(buff_mess,  "ON");  
      sprintf(buff_topic, "home/rfm_gw/nb/node01/dev30"); // construct MQTT topic and message
      mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...
    }
    else {
      if (powerOut==true) {
        // Power is back
#ifdef DEBUG
        Serial.println("Power is back!");
#endif
        sprintf(buff_mess,  "OFF");  
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev30"); // construct MQTT topic and message
        mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...        
      }
      powerOut = false;
    }
  }
  
  if (lastMinute != (millis()/60000)) {     // another minute passed ?
    lastMinute = millis()/60000;
    upTime++;
  }
  
  // RECEIVE AND SEND MESSAGES
  if (mqttToSend) { sendMsg(dest);}    // send MQTT instruction packets over the radio network
  
  if (radio.receiveDone()) { processPacket();}  // check for received radio packets and construct MQTT message
  
  if (!mqttClient.loop()) {     // check connection MQTT server and process MQTT subscription input
    mqttCon = 0;
    digitalWrite(MQCON, LOW);
    while(mqttCon != 1){      // try to reconnect every 2 seconds
      mqttCon = mqttClient.connect(clientName);
      delay(2000);
    }
    if(mqttCon){        // Yes, we have a link so,
      digitalWrite(MQCON, mqttCon); // turn on MQTT link indicator and
      mqttClient.subscribe(subTopic); // re-subscribe to mqtt topic
    }
  }
} // end loop



//==============  SENDMSG
//  sends messages over the radio network
void sendMsg(int target) {
  
  int i = 5;            // number of transmission retries
  
  while (respNeeded && i>0) {       // first try to send packets
    if (radio.sendWithRetry(target, (const void*)(&mes), sizeof(mes),5)) {
      Rstat = true;           // radio indicator on
      onMillis = millis();          // store timestamp      
      digitalWrite(R_LED, HIGH);   // blink radio LED
      respNeeded = false;
#ifdef DEBUGRADIO
      Serial.print("Msg to node " );
      Serial.println(target);
#endif
    } else {
      delay(500);        // half a second delay between retries
    }
    i--;
  }
  
  if (respNeeded && verbose) {          // if not succeeded in sending packets after 5 retries
    sprintf(buff_topic, "home/rfm_gw/nb/node%02d/dev90", target); // construct MQTT topic and message
    sprintf(buff_mess, "radio lost node %d", target);   // for radio loss (device 90)
    mqttClient.publish(buff_topic,buff_mess,retain_mess);     // publish ...
    respNeeded = false;           // reset response needed flag
#ifdef DEBUGRADIO
    Serial.print("No node ");
    Serial.println(target);
#endif
  }
   
  if (mqttToSend) mqttToSend = false;       // reset send trigger
} // end sendMsg

//==============  PROCESSPACKET
// receives data from the wireless network, parses the contents and constructs MQTT topic and value
void processPacket() {
  Rstat = true;             // set radio indicator flag 
  onMillis = millis();            // store timestamp
  digitalWrite(R_LED, HIGH);  // blink radio LED
  
  if (radio.DATALEN != sizeof(mes))       // wrong message size means trouble
#ifdef DEBUGRADIO
    Serial.println("inv msg strct")
#endif
    ;
  else{                             // message size OK...
    mes = *(Message*)radio.DATA;    // copy radio packet
                                    // and construct MQTT northbound topic
      
    sprintf(buff_topic, "home/rfm_gw/nb/node%02d/dev%02d", radio.SENDERID, mes.devID);  
  
#ifdef DEBUGRADIO
    Serial.print(radio.SENDERID); Serial.print(",");
    Serial.print(mes.devID);  Serial.print(",");
    Serial.print(mes.intVal); Serial.print(",");
    Serial.print(mes.fltVal); Serial.print(" RSSI: ");
    Serial.println(radio.RSSI);
#endif  
  }
  DID = mes.devID;            // construct MQTT message, according to device ID
  
  IntMess = (DID==0 || DID==1 || DID==7 || DID==9 || (DID>=64 && DID<72));  // Integer in payload message
  RealMess = (DID==4 || (DID>=48 && DID <64));          // Float in payload message
  StatMess = (DID==5 || DID==6 || DID==8 || DID==10 || (DID>=16 && DID <32) || (DID>=40 && DID <48));    // Status in payload message
  StrMess = (DID==3 || DID==72);      // String in payload
  
  if (IntMess) {              // send integer value load
    sprintf(buff_mess, "%d",mes.intVal);
  }
  
  if (RealMess) {             // send decimal value
    dtostrf(mes.fltVal, 10,2, buff_mess);
    while (buff_mess[0] == 32) {        // remove any leading spaces
      for (int i =0; i<strlen(buff_mess); i++) {
        buff_mess[i] = buff_mess[i+1];
      }
    }
  }
  
  if (StatMess) {             // put status in payload
    if (mes.intVal == 1 )sprintf(buff_mess, "ON");
    if (mes.intVal == 0 )sprintf(buff_mess, "OFF");
  }
  
  if (StrMess) {
    int i; for (i=0; i<32; i++){ 
      buff_mess[i] = (mes.payLoad[i]); 
    }
  }
  
  switch (mes.devID){
    case (2):             // RSSI value
      { sprintf(buff_mess, "%d", radio.RSSI);
      }
      break;
    case (92):              // invalid device message
      { sprintf(buff_mess, "NODE %d invalid device %d", mes.nodeID, mes.intVal);
      }
      break;
    case (99):              // wakeup message
    { sprintf(buff_mess, "NODE %d WAKEUP", mes.nodeID);
    }
    break;
  } // end switch
  
#ifdef DEBUG
  Serial.print("MQTT msg: ");
  Serial.print(buff_topic);
  Serial.print(": ");
  Serial.println(buff_mess);
#endif
  
  mqttClient.publish(buff_topic,buff_mess,retain_mess);     // publish MQTT message in northbound topic
  
  if (radio.ACKRequested()) {
    radio.sendACK();      // reply to any radio ACK requests
#ifdef DEBUG
  Serial.println("Ack sent.");
#endif    
  }
} // end processPacket

//==============  MQTT_SUBS
//    receive messages from subscribed topics
//    parse MQTT topic / message and construct radio message
//
//    The values in the MQTT topic/message are converted to corresponding values on the Radio network
void mqtt_subs(char* topic, byte* payload, unsigned int length) { 
  int i;
  mes.nodeID = NODEID;        // gateway is node 1
  mes.fltVal = 0;
  mes.intVal = 0;
  mqttToSend = false;       // not a valid request yet...
  error = 4;            // assume invalid device until proven otherwise

#ifdef DEBUG
  Serial.print("MQTT-Topic: ");
  Serial.println(topic);
//  Serial.println(String((char*)payload));
  Serial.println(*payload);
#endif

  if (strlen(topic) == 27) {        // correct topic length ?
    dest = (topic[19]-'0')*10 + topic[20]-'0';  // extract target node ID from MQTT topic
    DID = (topic[25]-'0')*10 + topic[26]-'0'; // extract device ID from MQTT topic
    payload[length] = '\0';       // terminate string with '0'
    String strPayload = String((char*)payload); // convert to string
    mes.devID = DID;
    mes.cmd = 0;          // default is 'SET' value
    if (strPayload == "READ") mes.cmd = 1;    // in this case 'READ' value
    if (length == 0) {error = 2;}                   // no payload sent
    else {
      StatMess = ( DID==5 || DID==6 || DID==8 || DID==10 || (DID>=16 && DID<32));
      RealMess = (( DID==0 || DID==2 || DID==3 || DID==4 || (DID>=40 && DID<72))&& mes.cmd==1);
      IntMess = (DID==1 || DID==7 || DID==9 || (DID >=32 && DID <40));
      StrMess = (DID==72);
      
      if (dest == 1 && DID == 0) {          // gateway uptime wanted
        sprintf(buff_mess,  "%d", upTime);  
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev00"); // construct MQTT topic and message
        mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...
        error =0;
      }
      if (dest == 1 && DID == 3) {          // gateway version wanted
        for (i=0; i<sizeof(VERSION); i++){ 
          buff_mess[i] = (VERSION[i]);
        }
        mes.payLoad[i] = '\0';
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev03"); // construct MQTT topic and message
        mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...
        error =0;
      }
      if (dest == 1 && DID == 10) {          // perform device reset
        Serial.println("Restart");
        // restart in 60 milliseconds
        softwareReset( WDTO_60MS);
        error =0;
      }
      if (dest == 1 && DID == 30) {          // power on/off wanted
        if (powerOut) sprintf(buff_mess,  "ON");  
        if (!powerOut) sprintf(buff_mess,  "OFF");  
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev30"); // construct MQTT topic and message
        mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...
        error =0;
      }
      if (dest>1 && StatMess) {         // node status device
        mqttToSend = true; 
        if (strPayload == "ON") mes.intVal = 1;     // payload value is state 
        else if (strPayload == "OFF") mes.intVal = 0;
        else if (strPayload != "READ") { mqttToSend = false; error = 3;}// invalid payload; do not process
      }
      if (dest>1 && (DID >=40 && DID <48)) {
        if (strPayload == "READ") mqttToSend = true; 
        else {mqttToSend = false; error = 3;}   // invalid payload; do not process
      }
      if (dest>1 && RealMess) {         // node read device
        mqttToSend = true; 
      }
      if ( dest>1 && IntMess ) {          // node integer device
        if (mes.cmd == 0) mes.intVal = strPayload.toInt();  // timer/polling/Integer is in MQTT message
        mqttToSend = true;
      }
      if ( dest>1 && StrMess ) {          // node string device
        if (mes.cmd == 0) {
          int i;
          for (i=0; i<32; i++){ 
            (mes.payLoad[i])=payload[i]; 
          }
        }
        mqttToSend = true;
      }
      
      if (mqttToSend && (error == 4)) error = 0;    // valid device has been selected, hence error = 0
      respNeeded = mqttToSend;      // valid request needs radio response
#ifdef DEBUGRADIO
      Serial.println(strPayload);
      Serial.print("Val:");
      Serial.println(mes.intVal);
#endif
    } // end of else. payload sent
  } // end of f (strlen(topic) == 27). correct topic
  else {
    error = 1;
#ifdef DEBUG
    Serial.println("MQTT-subscr NOK");
#endif
  }
  if ((error != 0) && verbose) {          // in case of syntax error  
    sprintf(buff_mess, "syntax error %d for node %d", error,dest);  
    sprintf(buff_topic, "home/rfm_gw/nb/node01/dev91"); // construct MQTT topic and message
    mqttClient.publish(buff_topic,buff_mess,retain_mess);   // publish ...
#ifdef DEBUG
    Serial.print("Syntax err: ");
    Serial.println(error);
#endif
  }
} // end mqttSubs

//==============  SOFTWARERESET
//    resets MCU to the same state as when powered up.
void softwareReset( uint8_t prescaller) {
  // start watchdog with the provided prescaller
  wdt_enable( prescaller);
  // wait for the prescaller time to expire
  // without sending the reset signal by using
  // the wdt_reset() method
  while(1) {}
}
