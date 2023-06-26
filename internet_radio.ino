#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <uri/UriBraces.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <BluetoothA2DPSource.h>

#include "AudioTools.h"
#include "AudioLibs/AudioA2DP.h"
#include "MP3DecoderHelix.h"

using namespace libhelix;

// WiFi params
const char* SSID = "Krzysiu-WiFi";
const char* PASSWORD = "ugodowy518";

const char* BLUETOOTH_RECEIVER = "KRZYSIU";

class Station {
  private:
    char name[32];
    char server[35];
    char url_params[35];

  public:
    Station(void) {
      memset(name, 0, sizeof(name));
      memset(server, 0, sizeof(server));
      memset(url_params, 0, sizeof(url_params));
    }

    Station(const String& station_name, const String& station_server, const String& station_url_params) {
      strncpy(name, station_name.c_str(), sizeof(name) - 1);
      strncpy(server, station_server.c_str(), sizeof(server) - 1);
      strncpy(url_params, station_url_params.c_str(), sizeof(url_params) - 1);
      name[sizeof(name) - 1] = '\0'; 
      server[sizeof(server) - 1] = '\0'; 
      url_params[sizeof(url_params) - 1] = '\0';  
    }

    String getName() const {
      return String(name);
    }

    String getServer() const {
      return String(server);
    }

    String getUrlParams() const {
      return String(url_params);
    }

    String getUrl() const {
      return "https://" + getServer() + getUrlParams();
    }
};

const int EEPROM_SIZE = 1024;
const int MAX_STATIONS = 10;
int stations_nr = 0;

Station stations[MAX_STATIONS];

// audio buffer
volatile bool do_continue_streaming = true;
SemaphoreHandle_t streamingSemaphore;
const unsigned int bufferSize = 16384;
uint8_t buffer[bufferSize];  

// server params
const char* SERVER_NAME = "esp32";

WebServer server(80);

MP3DecoderHelix mp3;
BluetoothA2DPSource a2dp_source;

const int MAX_TASKS = 10;
TaskHandle_t streamingTasks[MAX_TASKS];

void setup(void) {
  Serial.begin(115200);

  mp3.setDataCallback(mp3DecoderCallback);
  
  setupWifi();
  setupBluetooth();
  setupServer(); 
}

void loop(void) {
  server.handleClient();
  delay(2);//allow the cpu to switch to other tasks
}

// setups
void setupWifi(void) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi ");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");

  Serial.println("WiFi name: " + String(SSID));
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setupBluetooth(void) {
  // a2dp_source.set_ssid_callback(validate_bluetooth_receiver);
  // a2dp_source.set_auto_reconnect(true);
  a2dp_source.start(BLUETOOTH_RECEIVER);
  a2dp_source.set_volume(255);
}

void setupServer(void) {
  if(MDNS.begin(SERVER_NAME)) {
    Serial.println("Server name: " + String(SERVER_NAME) + ".local");
  }

  // run once to clear eeprom
  // removeStationsFromEEPROM();
  readStationsFromEEPROM();

  streamingSemaphore = xSemaphoreCreateBinary();
  if (streamingSemaphore == NULL) {
    Serial.println("Semaphore creation failed!");
    while (1);
  }
  xSemaphoreGive(streamingSemaphore);

  server.on("/", handleRoot);

  server.on("/stations", HTTP_GET, handleGetStations);
  server.on("/stations", HTTP_POST, handleAddStation);


  server.on(UriBraces("/stations/{}"), HTTP_GET, handleGetStation);
  server.on(UriBraces("/stream/{}"), HTTP_GET, handleStreamStation);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

bool validate_bluetooth_receiver(const char* ssid, esp_bd_addr_t address, int rssi){
   if (!strcmp(ssid, BLUETOOTH_RECEIVER)) {
     Serial.println("Bluetooth connected to " + String(BLUETOOTH_RECEIVER));
   }

   return !strcmp(ssid, BLUETOOTH_RECEIVER) == 0;
}

void removeStationsFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  for (int addr = 0; addr < EEPROM_SIZE; addr++) {
    EEPROM.write(addr, 0);
  }
  EEPROM.commit();
  EEPROM.end();
}

void readStationsFromEEPROM() {
  int addr = 0;
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(addr, stations_nr);
  addr += sizeof(int);

  for (int stations_ctr = 0; stations_ctr < stations_nr; stations_ctr++) {
    Station station;
    EEPROM.get(addr, station);
    stations[stations_ctr] = station;
    addr += sizeof(Station);
  }

  EEPROM.end();
}

void saveStationToEEPROM(Station station) {
  EEPROM.begin(EEPROM_SIZE);

  int stations_nr_addr = 0;
  int station_addr = stations_nr_addr + sizeof(stations_nr) + sizeof(Station) * (stations_nr - 1);

  EEPROM.put(stations_nr_addr, stations_nr);
  EEPROM.put(station_addr, station);
  
  EEPROM.commit();
  EEPROM.end();
}

void updateStations(Station station) {
  stations[stations_nr] = station;
  stations_nr++;
  saveStationToEEPROM(station);
}

void handleRoot() {
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  Serial.println("root");
  server.send(200, "text/html", "Welcome to esp32 internet radio <button><a href=\"http://esp32.local/stations\">Start</a></button>");
}

void handleNotFound() {
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}
String lstripHttp(const String& url) {
    String result = url;
    if (result.startsWith("http://")) {
        result = result.substring(7);
    } else if (result.startsWith("https://")) {
        result = result.substring(8);
    }
    return result;
}
void handleAddStation() {  
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  if (stations_nr == MAX_STATIONS) {
    server.send(400, "text/plain", "No available slots for new stations");
  }

  // Parse the JSON data from the request body
  StaticJsonDocument<200> jsonBuffer;
  DeserializationError error = deserializeJson(jsonBuffer, server.arg("plain"));
  
  String name = server.arg("name");
  String serverName = lstripHttp(server.arg("url"));
  String urlParams = "";
  
  Station station(name, serverName, urlParams);
  updateStations(station);
  
  server.send(200, "text/html", "Station added successfully <a href=\"http://esp32.local/stations\">Back</a>");
}

void handleGetStations() {  
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  String message = "<h1>List of available stations:</h1>\n";
  for (int stations_ctr = 0; stations_ctr < stations_nr; stations_ctr++) {
    Station station = stations[stations_ctr];
    message += "<a href=\"http://esp32.local/stations/"+String(stations_ctr + 1)+"\">" +String(stations_ctr + 1) + ": " + station.getName() + " [" + station.getServer() + "]</a>\n<button><a href=\"http://esp32.local/stream/"+String(stations_ctr + 1) + "\">Stream</a></button><br>";
  }
  message += R"(
  <form action="stations" method="post">
    <label for="name">Name:</label>
    <input type="text" id="name" name="name" required>
    <br><br>
    <label for="url">URL:</label>
    <input type="url" id="url" name="url" required>
    <br><br>
    <input type="submit" value="Submit">
  </form>
  )";
  server.send(200, "text/html", message);
}

void handleGetStation() {  
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  int station_nr = server.pathArg(0).toInt();
  if(station_nr <= 0 || station_nr > stations_nr ) {
    handleNotFound();
  } else {
    Station station = stations[station_nr - 1];
    server.send(200, "text/html", "<a href=\"http://esp32.local/stations\">Back</a><p>Name: " + station.getName() + "</p>\n<p>Url: " +  station.getUrl() + "</p>");
  }
}

void handleStreamStation() {
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = true;
  xSemaphoreGive(streamingSemaphore); 

  int station_nr = server.pathArg(0).toInt();
  if(station_nr <= 0 || station_nr > stations_nr ) {
    handleNotFound();
    return;
  }
  
  if (streamingTasks[station_nr - 1] == NULL) {
    char taskName[20];
    snprintf(taskName, sizeof(taskName), "Streaming Audio %d", station_nr);

    int *station_idx = new int(station_nr - 1);

    xTaskCreatePinnedToCore(
      streamingTaskFunction,           // Task function
      taskName,                        // Task name
      10000,                           // Stack size (bytes)
      station_idx,                     // Task parameter (station object pointer)
      tskIDLE_PRIORITY,                // Task priority
      &streamingTasks[station_nr - 1], // Task handle
      0                                // Core to run the task on (0 or 1)
    );
    
    Station station = stations[station_nr - 1];
    server.send(200, "text/html", "<h1>Streaming</h1><h2><a href=\"http://esp32.local/stations\">Back</a></h2>\n<p>Name: " + station.getName() + "</p>\n<p>Url: " +  station.getUrl() + "</p>");
  }
}

void streamingTaskFunction(void* parameter) {
  int* station_idx_ptr = static_cast<int*>(parameter);
  Station *station = stations + (*station_idx_ptr);

  Serial.println("Connecting to: " + station->getServer());

  WiFiClient client;
  if(client.connect(station->getServer().c_str(), 80)) {

    // send request to streaming endpoint
    client.print(
      String("GET ") + station->getUrl() + " HTTP/1.1\r\n" +
      "Host: " + station->getServer() + "\r\n" +
      "Connection: keep-alive\r\n\r\n"
    );

    bool do_close_stream = false;
    
    mp3.begin();

    while (!do_close_stream && client.connected()) {
      while (!do_close_stream && client.available()) {
        unsigned int bytesRead = client.readBytes(buffer, bufferSize);
        if (bytesRead > 0) {
          mp3.write((int16_t*)buffer, bytesRead);
        }
        xSemaphoreTake(streamingSemaphore, portMAX_DELAY);
        do_close_stream = !do_continue_streaming;
        xSemaphoreGive(streamingSemaphore);
      }
    } 
  } else {
    server.send(500, "text/plain", "Failed to connect to station");
  }

  streamingTasks[*station_idx_ptr] = NULL;
  delete station_idx_ptr;
  client.stop();
  vTaskDelete(NULL);
}


OneChannelSoundData data_array[100];
int current_index = 0;

void mp3DecoderCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref) {
  while(a2dp_source.has_sound_data()){
    vTaskDelay(1);
  }
  data_array[current_index] = OneChannelSoundData(pcm_buffer, len);
  a2dp_source.write_data(data_array+current_index);    
  current_index++;
  current_index = current_index % 100;
}
