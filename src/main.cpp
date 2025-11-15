static const char* TAG = "MAIN";

#include "Arduino.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include "FS.h"
#include "LittleFS.h"
#include "HTTPClient.h"
#include "RTClib.h"
#include "LiquidCrystal_I2C.h"
#include "PZEM004Tv30.h"
#include "esp_log.h"

#define PIN_RELAY 27

const char* db_host = "https://power-meter-738d5-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char* db_auth = "fqfVIk9G866nb8qfuTVnKeEA1KX1WX9hXDrODUSN";

const char* ssid = "M23";
const char* password = "11223344";

const char* mqtt_server = "8df95bf6d13345d9bf2ded4c227e696e.s1.eu.hivemq.cloud";
const char* mqtt_username = "server_user";
const char* mqtt_password = "Password1";
uint16_t mqtt_port = 8883;

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
PZEM004Tv30 pzem(Serial2, 16, 17);
RTC_DS3231 rtc;

/*   HYPERPARAMETER   */
#define BATAS_KONSUMSI_HARIAN 3860     //Wh
#define BATAS_KONSUMSI_BULANAN 14900   //Wh
#define ARUS_STANDBY 0.001



struct powermeter_data{
  float tegangan = 0;
  float arus = 0;
  float daya = 0;
  float konsumsi_harian = 0;
  float konsumsi_bulanan = 0;
  float last_konsumsi_harian = 0;
  float last_konsumsi_bulanan = 0; 
} data;

struct interval {
  uint32_t upload_data = 15;
  uint32_t last_upload = 0;
  uint32_t show_data = 10;
  uint32_t last_show = 0;
  uint32_t standby = 0;
  bool timer = false;
} interval;


struct time_reset{
  uint32_t tanggal_reset = 25;
  uint32_t bulan_reset = 0;
  bool is_reset = false;
} time_reset;

enum powermmeter_status {
  standby,
  digunakan,
  tidak_digunakan,
  maksimal
} state;

bool showWaktu = true;            // true = tampilkan waktu, false = tampilkan data
bool lastDisplayState = true;

void reconnect();
void callback(char *topic, byte* message, unsigned int length);
void dbSendData(String path, String data, String timestamp);


bool file_write(String path, float value);
float file_readFloat(String path);
int file_readInt(String path);


void lcd_show(uint8_t cursor_x, uint8_t cursor_y, String text);

void setup() {
  Serial.begin(115200);
  ESP_LOGI(TAG, "Setup started");

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  Wire.begin();
  rtc.begin();
  lcd.begin();
  lcd.backlight();

  if (LittleFS.begin()){
    lcd_show(0, 0, "FS Started");
    delay(1000);
    ESP_LOGI(TAG, "LittleFS mounted successfully");
  } else {
    lcd_show(0, 0, "FS Failed, Restart");
    delay(1000);

    LittleFS.format();
    esp_restart();
    ESP_LOGE(TAG, "Failed to mount LittleFS");
  }

  WiFi.begin(ssid, password);
  secureClient.setInsecure();
  while (WiFi.status() != WL_CONNECTED) {
    lcd_show(0, 0, "WiFi:");
    lcd_show(5, 0, ssid);
    lcd_show(0, 1, "Connecting");
    ESP_LOGI(TAG, "WiFi Connecting to %s", ssid);
    delay(500);
  }
  lcd.clear();
  lcd_show(0, 0, "WiFi:");
  lcd_show(5, 0, ssid);
  lcd_show(0, 1, "Connected");
  delay(3000);
  lcd.clear();


  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(callback);
  
  state = standby;
  interval.timer = true;

  data.last_konsumsi_harian = file_readFloat("/last_konsumsi_harian.f");
  data.last_konsumsi_bulanan = file_readFloat("/last_konsumsi_bulanan.f");
  time_reset.tanggal_reset = file_readInt("/tanggal_reset.i");
  time_reset.bulan_reset = file_readInt("/bulan_reset.i");
  time_reset.is_reset = file_readInt("/is_reset.i") == 1 ? true : false;
}


void loop() {
  if(!mqtt.connected()){
    reconnect();
  }
  mqtt.loop();

  DateTime now = rtc.now();

  data.tegangan = isnan(pzem.voltage()) ? 0 : pzem.voltage();
  data.arus = isnan(pzem.current()) ? 0 : pzem.current();
  data.daya = isnan(pzem.power()) ? 0 : pzem.power();
  data.konsumsi_harian = data.last_konsumsi_harian + pzem.energy();
  data.konsumsi_bulanan = data.last_konsumsi_bulanan + pzem.energy();
  
  /*  INTRUKSI RELAY  */
  switch (state){
  case standby:
    if (interval.timer == true){
      interval.standby = millis();
      interval.timer = false;
      digitalWrite(PIN_RELAY, HIGH);
    }
    if (data.arus > ARUS_STANDBY){
      state = digunakan;
      interval.standby = 0;
    }
    if (millis() - interval.standby >= 60000UL){
      state = tidak_digunakan;
      interval.standby = 0;
      digitalWrite(PIN_RELAY, LOW);
    }
    break;

  case digunakan:
    if (data.arus <= ARUS_STANDBY){
      if (interval.timer == false){
        interval.standby = millis();
        interval.timer = true;
      }
      if (millis() - interval.standby >= 60000UL){
        state = tidak_digunakan;
        digitalWrite(PIN_RELAY, LOW);
      }
    }
    if (data.konsumsi_harian >= BATAS_KONSUMSI_HARIAN){
      state = maksimal;
      digitalWrite(PIN_RELAY, LOW);
    }
    break;

  case tidak_digunakan:
    digitalWrite(PIN_RELAY, LOW);
    break;

  case maksimal:
    digitalWrite(PIN_RELAY, LOW);
    break;
  }


  /*  RESET DATA HANDLE */
  if (now.day() == time_reset.tanggal_reset && time_reset.is_reset == false){
    if (now.hour() == 0 && now.minute() == 0 && now.second() < 10){
      time_reset.is_reset = true;
      file_write("/is_reset.i", time_reset.is_reset ? 1 : 0);

      data.last_konsumsi_bulanan = 0;
      data.last_konsumsi_harian = 0;
      file_write("/last_konsumsi_harian.f", data.last_konsumsi_harian);
      file_write("/last_konsumsi_bulanan.f", data.last_konsumsi_bulanan);
      time_reset.bulan_reset = now.month();
      file_write("/tanggal_reset.i", time_reset.tanggal_reset);
      file_write("/bulan_reset.i", time_reset.bulan_reset);
      ESP_LOGI(TAG, "Reset konsumsi harian & bulanan");
    }
    else{
      ESP_LOGI(TAG, "Sudah melakukan Reset...");
    }
  }
  else if (now.day() != time_reset.tanggal_reset && time_reset.is_reset == true){
    time_reset.is_reset = false;
    file_write("/is_reset.i", time_reset.is_reset ? 1 : 0);
  }


  /* MQTT & DATABASE  */
  if (millis() - interval.last_upload > interval.upload_data * 1000UL){
    
    char payload[128];
    sprintf(payload, "{\"arus\":%.3f,\"tegangan\":%.2f,\"daya\":%.2f,\"konsumsi_harian\":%.3f,\"konsumsi_bulanan\":%.3f}", data.arus, data.tegangan, data.daya, data.konsumsi_harian, data.konsumsi_bulanan);
    mqtt.publish("powermeter/data", payload);

    char time_header[20];
    sprintf(time_header, "%04d:%02d:%02d:%02d:%02d:%02d", now.year(),now.month(),now.day(),now.hour(),now.minute(),now.second());
    String today = String(now.day()) + '-' + String(now.month()) + '-' + String(now.year());
    dbSendData("/powermeter/", payload, time_header);

    file_write("/last_konsumsi_harian.f", data.konsumsi_harian);
    file_write("/last_konsumsi_bulanan.f", data.konsumsi_bulanan);

    interval.last_upload = millis();
  }

  /* LCD HANDLING */
    if ((now.second() % interval.show_data == 0) && (now.second() != interval.last_show)) {
    showWaktu = !showWaktu;
    interval.last_show = now.second();
  } 

  if (showWaktu != lastDisplayState) {
    lcd.clear();
    lastDisplayState = showWaktu;
  }

  if (showWaktu) {
    char waktu[9];
    sprintf(waktu, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    char tanggal[11];
    sprintf(tanggal, "%02d/%02d/%04d", now.day(), now.month(), now.year());
      
    lcd_show(4, 0, waktu);
    lcd_show(3, 1, tanggal);
  } 
  else {
    char row_1[17];
    sprintf(row_1, "V:%.1f I:%.3f", data.tegangan, data.arus);
    char row_2[17];
    sprintf(row_2, "P:%.2f K:%.3f", data.daya, data.konsumsi_harian);
    lcd_show(0, 0, row_1);
    lcd_show(0, 1, row_2);
  }


 /*   FORMAT SETTING JAM = TAHUN-BULAN-TANGGAL JAM:MENIT:DETIK 
       example: 2025-03-20 10:06:17
       kirim via serial monitor                                   */
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    if (input.length() >= 19) {
      int year = input.substring(0, 4).toInt();
      int month = input.substring(5, 7).toInt();
      int day = input.substring(8, 10).toInt();
      int hour = input.substring(11, 13).toInt();
      int minute = input.substring(14, 16).toInt();
      int second = input.substring(17, 19).toInt();

      rtc.adjust(DateTime(year, month, day, hour, minute, second));
    }
  }
}


void lcd_show(uint8_t cursor_x, uint8_t cursor_y, String text){
  lcd.setCursor(cursor_x, cursor_y);
  lcd.print(text);
}

String makeClientId(){
  String id = "ESP32-";
  id += WiFi.macAddress();      // atau use ESP.getEfuseMac() / chip ID
  id.replace(":", "");
  return id;
}

void reconnect() {
  lcd.clear();
  while (!mqtt.connected()) {
    String clientId = makeClientId();
    Serial.print("Mencoba koneksi MQTT dengan clientId: ");
    Serial.println(clientId);

    lcd_show(0,0,"MQTT: Connecting");
    // connect dengan username & password dan clientId unik
    if (mqtt.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
      lcd.clear();
      ESP_LOGI(TAG,"MQTT Connected");
      lcd_show(0,0,"MQTT: Connected");
      mqtt.subscribe("powermeter/button");
      break;
    } 
    else {
      int8_t st = mqtt.state();
      ESP_LOGE(TAG,"Gagal konek MQTT, state=%d", st);
      
      /*
        Status codes (PubSubClient):
        -4 : MQTT_CONNECTION_TIMEOUT
        -3 : MQTT_CONNECTION_LOST
        -2 : MQTT_CONNECT_FAILED
        -1 : MQTT_DISCONNECTED
         0 : MQTT_CONNECTED
         1.. : CONNACK return codes dari broker (lihat manual broker)
      */
      lcd.clear();
      lcd_show(0,0,"MQTT Conn Failed");
      delay(3000); // tunggu sebelum retry
    }
    lcd.clear();
  }
}

void callback(char *topic, byte* message, unsigned int length){
  String pesan = "";
  for(int i=0; i<length; i++){
    pesan += (char)message[i];
  }
  Serial.printf("toipc: %s, message: %s\n",topic, pesan.c_str());
  if (pesan == "ON"){
    state = standby;
    interval.timer = true;
    Serial.println(pesan);
  }
  else if (pesan == "OFF"){
    if (state != maksimal){
      state = digunakan;
      Serial.println(pesan);
    }
  }
}


void dbSendData(String path, String data, String timestamp){
  Serial.printf("Data: %s\n",data.c_str());
  if(WiFi.status() == WL_CONNECTED){
    HTTPClient http;

    String url = String(db_host) + path +"/" + timestamp + ".json";
    if (strlen(db_auth) > 0){
      url += "?auth=" + String(db_auth);
    }
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int http_response = http.PUT(data);

    if (http_response > 0){
      String response = http.getString();
      Serial.print("DB Response: ");
      Serial.println(response);
    }
    else{
      Serial.println("Error Sending to DB: ");
      Serial.println(http_response);
    }
    http.end();
  }
  else{
    Serial.println("Wifi Not Connected");
  }
}

bool file_write(String path, float value){
  File file = LittleFS.open(path, "w");
  if (!file){
    ESP_LOGE(TAG, "Failed to open file for writing: %s", path.c_str());
    return false;
  }
  file.print(value);
  file.close();
  return true;
}

float file_readFloat(String path){
  File file = LittleFS.open(path, "r");
  if (!file){
    ESP_LOGE(TAG, "Failed to open file for reading: %s", path.c_str());
    return 0.0;
  }
  String content = file.readStringUntil('\n');
  file.close();
  return content.toFloat();
}

int file_readInt(String path){
  File file = LittleFS.open(path, "r");
  if (!file){
    ESP_LOGE(TAG, "Failed to open file for reading: %s", path.c_str());
    return 0;
  }
  String content = file.readStringUntil('\n');
  file.close();
  return content.toInt();
}