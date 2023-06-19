#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <uri/UriBraces.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <BluetoothA2DPSource.h>

// WiFi params
const char* SSID = "***";
const char* PASSWORD = "***";

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
BluetoothA2DPSource a2dp_source;

const int MAX_TASKS = 10;
TaskHandle_t streamingTasks[MAX_TASKS];

void setup(void) {
  Serial.begin(115200);

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
  a2dp_source.start("***");
  a2dp_source.set_volume(100);
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
  server.send(200, "text/plain", "Welcome into esp32 Internet Radio!");
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
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON data");
    return;
  }
  
  // Extract the parameters from the JSON object
  String name = jsonBuffer["name"];
  String server_name = jsonBuffer["server"];
  String url_params = jsonBuffer["url-params"];
  Station station(name, server_name, url_params);

  updateStations(station);
  
  server.send(200, "text/plain", "Station added successfully");
}

void handleGetStations() {  
  xSemaphoreTake(streamingSemaphore, portMAX_DELAY); 
  do_continue_streaming = false;
  xSemaphoreGive(streamingSemaphore);

  String message = "List of available stations:\n";
  for (int stations_ctr = 0; stations_ctr < stations_nr; stations_ctr++) {
    Station station = stations[stations_ctr];
    message += String(stations_ctr + 1) + ": " + station.getName() + " [" + station.getServer() + "]\n";
  }
  server.send(200, "text/plain", message);
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
    server.send(200, "text/plain", "Name: " + station.getName() + "\nUrl: " +  station.getUrl());
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
      1,                               // Task priority
      &streamingTasks[station_nr - 1], // Task handle
      0                                // Core to run the task on (0 or 1)
    );
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

    server.send(200, "text/plain", "Streaming ...");

    bool do_close_stream = false;

    while (!do_close_stream && client.connected()) {
      while (!do_close_stream && client.available()) {
        unsigned int bytesRead = client.readBytes(buffer, bufferSize);
        if (bytesRead > 0) {
          // Serial.println("Array: " + String(bytesRead));
          // printByteArray(buffer, bytesRead);

          SoundData *data = new OneChannel8BitSoundData((int8_t*)buffer, bytesRead);
          if (data != nullptr) {
            Serial.println("Play: ");
            a2dp_source.write_data(data);
            vTaskDelay(pdMS_TO_TICKS(500));
            delete data;
          }
        }
        xSemaphoreTake(streamingSemaphore, portMAX_DELAY);
        do_close_stream = !do_continue_streaming;
        xSemaphoreGive(streamingSemaphore);
        vTaskDelay(pdMS_TO_TICKS(5));
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

void printByteArray(uint8_t* array, size_t length) {
  for (size_t i = 0; i < length; i++) {
    Serial.print(array[i], HEX);
    Serial.print(" "); 
  }
  Serial.println();
}


