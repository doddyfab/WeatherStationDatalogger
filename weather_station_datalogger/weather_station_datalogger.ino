/**
 * Weather Station Datalogger by SLA99
 * Autonomous datalogger for temperature witch can : 
 *  - save day data in internal storage (min, moy, max)
 *  - send data to a thingspeak account (option)
 *  - provide stored datas through FTP server in LAN
 * 
 * It works with : 
 *  - Nodemcu Amica module v2                       -> https://www.amazon.fr/gp/product/B06Y1LZLLY
 *  - Temp sensor : SHT35-1M                        -> https://fr.aliexpress.com/item/1005004088598291.html
 *  - Clock : DS3231 (manage dayling saving time)   -> https://www.amazon.fr/gp/product/B01H5NAFUY
 *  - Solar panel, TP4056 charger, 18650 battery x3 -> https://fr.aliexpress.com/item/1005002265333958.html
 * 
 *     Downloads, docs, tutorials: https://sla99.fr
 *     Changelog : 
 *     2022/09/27  v1.0    init version
 */



#include <SimpleFTPServer.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h> //I2C library
#include "RTClib.h"
#include <WEMOS_SHT3X.h>
#include "FS.h"
#include <LittleFS.h>

//Variables
const char* ssid          = "xxxxx";
const char* ssid_password = "xxxxx";
#define button_offline    14          //for OFFLINE mode D5
#define button_ftp        13          //for FTP mode D7
int buttonStateFTP        = 0;        //state of button FTP
int buttonStateOffline    = 0;        //state of button Offline
boolean prog              = false;    //to check if prog mode
boolean debug             = true;     //TRUE = activate all println
int interval              = 300;      //intervalle deep sleep en secondes
float temp;                           //to store temp value
char DST[1];                          //to store dayling saving time
const char* DST_FILE      = "/DST";   //File to store DST variable
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
String THINGSPEAK_APIKEY  ="xxxxx";

//Init
FtpServer ftpSrv;       //FTP server
RTC_DS3231 rtc;         //DS3231 clock
SHT3X sht30(0x44);      //SHT35


/* 
 *  Function to write somedatas on a file in internal storage
 */ 
void writeFile(const char * path, const char * message) {
  Serial.printf("Writing to file: %s\n", path);

  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  
  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  
  if (file.println(message)) {
    Serial.println("Message writed");
  } else {
    Serial.println("Writed failed");
  }
  file.close();
  LittleFS.end();
}

/* 
 *  Function to split a string in multiple variables
 */ 
 
String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length()-1;

  for(int i=0; i<=maxIndex && found<=index; i++){
    if(data.charAt(i)==separator || i==maxIndex){
        found++;
        strIndex[0] = strIndex[1]+1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }

  return found>index ? data.substring(strIndex[0], strIndex[1]) : "";
}

/* 
 *  Function to display current time in monitor mode
 */ 
void printCurrentTime(){
    DateTime now = rtc.now();
    
    Serial.print(now.year(), DEC);
    Serial.print('/');
    Serial.print(now.month(), DEC);
    Serial.print('/');
    Serial.print(now.day(), DEC);
    Serial.print(" (");
    Serial.print(daysOfTheWeek[now.dayOfTheWeek()]);
    Serial.print(") ");
    Serial.print(now.hour(), DEC);
    Serial.print(':');
    Serial.print(now.minute(), DEC);
    Serial.print(':');
    Serial.print(now.second(), DEC);
    Serial.println();
    
    Serial.print(" since midnight 1/1/1970 = ");
    Serial.print(now.unixtime());
    Serial.print("s = ");
    Serial.print(now.unixtime() / 86400L);
    Serial.println("d");
}


/* 
 *  Function to launch URL in https (certificate are ignored)
 */ 
 
void openURLhttps(String baseURL, String URL){
   std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);

    //client->setFingerprint(fingerprint);
    client->setInsecure();
    HTTPClient https;
    String fullURL = baseURL+URL;
    Serial.println(fullURL);
    Serial.print("[HTTPS] begin...\n");
    if (https.begin(*client, fullURL)) {  // HTTPS
      Serial.print("[HTTPS] GET...\n");
      // start connection and send HTTP header
      int httpCode = https.GET();
      // httpCode will be negative on error
      if (httpCode > 0) {
        Serial.printf("[HTTPS] GET... code: %d\n", httpCode);
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String payload = https.getString();
          Serial.println(payload);
        }
      } 
      else {
        Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
      }
      https.end();
    } 
    else {
      Serial.printf("[HTTPS] Unable to connect\n");
    }  
}


void setup() {
  Serial.begin(9600);
  
  //check of button status
  pinMode(button_ftp, INPUT);
  pinMode(button_offline, INPUT);
  delay(10);

  buttonStateOffline = digitalRead(button_offline);
  delay(10);
  buttonStateFTP = digitalRead(button_ftp);
  delay(10);
 
  Serial.print("Bouton FTP : ");
  Serial.println(buttonStateFTP);
  Serial.print("Bouton offline : ");
  Serial.println(buttonStateOffline);


  //First check of DST file
  //If not exist, created with default value 0 (change be changed)
  //else, read on local file DST value and DST variable initialized

  if(!LittleFS.begin()){
      Serial.println("An Error has occurred while mounting LittleFS");
      return;
    }
  File file = LittleFS.open(DST_FILE, "r");
  if (!file) {
      Serial.println("File not existing");
      //si le fichier n'existe pas, on initialise le DST (1 = heure été, 0 = heure hiver)
      writeFile(DST_FILE, "0");  
    }
    else{
      //On lit le fichier
      char buffer[64];
      while (file.available()) {
       int l = file.readBytesUntil('\n', buffer, sizeof(buffer));
       buffer[l] = 0;
       Serial.print("Fichier DST : ");
       Serial.println(buffer);
      }
      DST[0] = buffer[0];
      
      Serial.print("DST compare: ");
      Serial.println(strcmp(DST,"1"));
      file.close();
    }
    
  //start clock
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }
   
  /*
   * FTP Mode
   * Will boot in FTP mode to get access to local file
   * Login FTP :    esp8266
   * Password FTP : esp8266
   */
  if(buttonStateFTP == 1 and buttonStateOffline == 0){ 
    prog = true;
    WiFi.begin(ssid, ssid_password);
    Serial.println("FTP mode");
    Serial.println("");
    // Wait for connection
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    if(!LittleFS.begin()){
      Serial.println("An Error has occurred while mounting LittleFS");
      return;
    }
    else{
        Serial.println("SPIFFS opened!");
        ftpSrv.begin("esp8266","esp8266"); 
    }
  }

  /*
   * CLOCK set up mode
   * Will apply system time to the clock
   * Need to run the code on ESP8266 to apply new time
   */
  else if(buttonStateFTP == 1 and buttonStateOffline == 1){ 
      
      Serial.println("both mode");
      Serial.println("lets set the time!");
      // following line sets the RTC to the date & time this sketch was compiled
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      printCurrentTime();
  }
   /*
   * NORMAL mode
   * Will get temp value
   * Will store value internally
   * Will put value on thingspeak channel
   * Will go in deep sleep for a time
   */
  else{ 
    Serial.println("normal mode");
    
    //GET SHT value
    if(sht30.get()==0){
      temp = sht30.cTemp;
      Serial.print("Temperature");
      Serial.println(temp);
    }

    //starting clock ...
    DateTime now = rtc.now();
    Serial.println("démarrage RTC");

    //DST management
    if (now.dayOfTheWeek() == 0 && now.month() == 3 && now.day() >= 25 && now.hour() == 2 && strcmp(DST,"0") == 0){         
      rtc.adjust(DateTime(now.year(), now.month(), now.day(), now.hour()+1, now.minute(), now.second()));    
      Serial.println("##### CHANGEMENT HEURE -> HEURE ETE #####");          
      writeFile(DST_FILE, "1");  
    }     
    else if(now.dayOfTheWeek() == 0 && now.month() == 10 && now.day() >= 25 && now.hour() == 2 && strcmp(DST,"1") == 0 ){       
      rtc.adjust(DateTime(now.year(), now.month(), now.day(), now.hour()-1, now.minute(), now.second()));
      Serial.println("##### CHANGEMENT HEURE -> HEURE HIVER #####");          
      writeFile(DST_FILE, "0");     
    }
    printCurrentTime();
    
    //Filename generation with YYYYMMDD
    char filename[30];
    snprintf(filename,sizeof(filename),"/%04d%02d%02d", now.year(), now.month(), now.day());
    Serial.print("Nom du fichier : ");
    Serial.println(filename);

    //Starting filesystem
    if(!LittleFS.begin()){
      Serial.println("An Error has occurred while mounting LittleFS");
      return;
    }

    //Reading file YYYYMMDD
    File file = LittleFS.open(filename, "r");

    //if file not existing, probably we are around midnight
    //initializing with Tmin = Tmoy (average) = Tmax = temp
    if (!file) {
      Serial.println("File not existing");
      //si le fichier n'existe pas, il est minuit et on écrit la premiere ligne
      char message[50];
      snprintf(message,sizeof(message),"%lf;%lf;%lf;%d", temp,temp,temp,1);
      writeFile(filename, message);  
    }
    //If file exist
    //check if temp<Tmin -> update
    //check if temp>Tmax -> update
    //manage average (Tmoy) with counting number of occurence of temp, adding curent temp and updating average and occurence+1
    else{
      //On lit le fichier
      char myData[50];
      int i = 0;
      while (file.available()) {
        myData[i] = file.read();
        i++;
      }
      Serial.println(myData);
      file.close();
       
      String tempMin = getValue(myData, ';', 0);
      String tempMoy = getValue(myData, ';', 1);
      String tempMax = getValue(myData, ';', 2);
      String tempNumber = getValue(myData, ';', 3);

      Serial.println("-----------------------------------");
      Serial.print("Temp min : ");
      Serial.println(tempMin.toFloat());
      Serial.print("Temp moy : ");
      Serial.println(tempMoy.toFloat());
      Serial.print("Temp max : ");
      Serial.println(tempMax.toFloat());
      Serial.print("Temp number : ");
      Serial.println(tempNumber.toInt());

      float tMin = tempMin.toFloat();
      float tMoy = tempMoy.toFloat();
      float tMax = tempMax.toFloat();
      int tNumber = tempNumber.toInt();
      
      if(temp < tMin) tMin = temp;
      if(temp > tMax) tMax= temp;
      tMoy = ((tMoy*tNumber)+temp)/(tNumber+1);
      tNumber++;
      Serial.println("-----------------------------------");
      Serial.print("Temp min2 : ");
      Serial.println(tMin);
      Serial.print("Temp moy2 : ");
      Serial.println(tMoy);
      Serial.print("Temp max2 : ");
      Serial.println(tMax);
      Serial.print("Temp number2 : ");
      Serial.println(tNumber);
      Serial.println("-----------------------------------");

      char message[50];
      snprintf(message,sizeof(message),"%lf;%lf;%lf;%i", tMin,tMoy,tMax,tNumber);
      writeFile(filename, message);  
    }

    //ONLINE Mode
    //If offline mode unactivated, conenctinf to wifi and send data to thingspeak
    if(buttonStateOffline == 0){ 
      WiFi.begin(ssid, ssid_password);  
      Serial.println(WiFi.macAddress());
      while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if(debug == true){
          Serial.print(".");
        }
      }
      
      Serial.println("");
      Serial.println("WiFi connected");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());   
      Serial.println(WiFi.macAddress());
      Serial.println("Mode offline désactivé");
      
      if(THINGSPEAK_APIKEY.length()>0){
        String thingspeak_baseURL = "https://api.thingspeak.com";
        String thingspeak_URL = "/update?api_key="+String(THINGSPEAK_APIKEY)+"&field1="+String(temp);
        openURLhttps(thingspeak_baseURL, thingspeak_URL);  
      }
    }

    //Going to deepsleep
    Serial.println("Going into deep sleep for 300 seconds");
    ESP.deepSleep(300e6); 
    
  }

}
 
void loop() {
  //Lauching FTP server in FTP Mode
  if(prog == true){
     ftpSrv.handleFTP();        
  }
}
