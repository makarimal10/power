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

const char* db_host = "https://powerlogdata-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char* db_auth = "jgdhEngVhS57m6qEQ9Kcam8lnu8vVi4ASF7xQdxF";

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
#define BATAS_KONSUMSI_BULANAN 14.9   //KWh
#define BATAS_KONSUMSI_HARIAN 0.5     //KWh
#define DAYA_STANDBY 1.2



struct powermeter_data{
  float tegangan = 0;
  float arus = 0;
  float daya = 0;
  float energi = 0;
  float konsumsi_bulanan = 0;
  float konsumsi_harian = 0;
  float last_konsumsi_bulanan = 0; 
  float last_konsumsi_harian = 0;
  char state[10];
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
  uint8_t last_reset_monthly = 0;
  uint8_t last_reset_harian = 0;
} time_reset;

enum powermmeter_status {
  standby,
  digunakan,
  tidak_digunakan,
  maksimal
} state;

bool showWaktu = true;            // true = tampilkan waktu, false = tampilkan data
bool lastDisplayState = true;

float last_valid_voltage = 0;
float last_valid_current = 0;
float last_valid_power   = 0;
float last_valid_energy  = 0;
float corresion_factor_energy = 0;
float cirresion_factor_current = 0.1;
bool get_corresion_factor_energy = true;

void reconnect();
void callback(char *topic, byte* message, unsigned int length);
void dbSendData(String path, String data, String timestamp);
const char* status_to_str(powermmeter_status state);

bool file_write(String path, float value);
float file_readFloat(String path);
int file_readInt(String path);


void lcd_show(uint8_t cursor_x, uint8_t cursor_y, String text);

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600);
  delay(500);

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
  lcd.clear();

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

  data.last_konsumsi_bulanan = file_readFloat("/last_konsumsi_bulanan.f");
  data.last_konsumsi_harian = file_readFloat("/last_konsumsi_harian.f");
  time_reset.last_reset_monthly = file_readInt("/last_reset_monthly.i");
  time_reset.last_reset_harian = file_readInt("/last_reset_harian.i");

  vTaskDelay(1000/portTICK_PERIOD_MS);
}


void loop() {
  if(!mqtt.connected()){
    reconnect();
  }
  mqtt.loop();
  
  DateTime now = rtc.now();

  if(get_corresion_factor_energy){
    float energy = pzem.energy();
    if(!isnan(energy) && energy >= 0.0  && energy < 20.0){
      corresion_factor_energy = energy;
      ESP_LOGI(TAG, "corresion_factor_energy: %f", corresion_factor_energy);
      get_corresion_factor_energy = false;
    }
  }  
  
  float v = pzem.voltage();
  float i = pzem.current() - cirresion_factor_current;
  float p = pzem.power();
  float e = pzem.energy() - corresion_factor_energy;

  bool validV = !isnan(v) && v >= 0.0 && v <= 260.0;
  bool validI = !isnan(i) && i >= 0.0  && i <= 100.0;
  bool validP = !isnan(p) && p >= 0.0  && p <= 5000.0;
  bool validE = !isnan(e) && e >= 0.0  && e < 1000.0;

  if (last_valid_voltage > 0 && fabs(v - last_valid_voltage) > 250) validV = false;
  if (last_valid_current > 0 && fabs(i - last_valid_current) > 5)  validI = false;
  if (last_valid_power   > 0 && fabs(p - last_valid_power)   > 1000) validP = false;

  data.tegangan = validV ? (last_valid_voltage = v) : last_valid_voltage;
  data.arus     = validI ? (last_valid_current = i) : last_valid_current;
  data.daya     = validP ? (last_valid_power   = p) : last_valid_power;

  if (validE) {
      last_valid_energy = e;
      data.energi = e;
      data.konsumsi_bulanan = data.last_konsumsi_bulanan + e;
      data.konsumsi_harian = data.last_konsumsi_harian + e;
  } else {
      data.energi = last_valid_energy;
      data.konsumsi_bulanan = data.last_konsumsi_bulanan + last_valid_energy;
      data.konsumsi_harian = data.last_konsumsi_harian + last_valid_energy;
  }
  // ESP_LOGI(TAG, "e: %f, corresion_factor_energy: %f, energi: %f, konsumsi_bulanan: %f", e, corresion_factor_energy, data.energi, data.konsumsi_bulanan);
  // ESP_LOGI(TAG, "arus: %3f, tegangan: %2f, daya: %2f, koreksi energi: %f, energi: %f, konsumsi bulanan: %2f",
  //    data.arus, data.tegangan, data.daya, corresion_factor_energy, data.energi, data.konsumsi_bulanan);
  
  /*  INTRUKSI RELAY  */
  switch (state){
  case standby:
    if (interval.timer == true){
      mqtt.publish("powermeter/notif", "{\"state\":true}");
      interval.standby = millis();
      interval.timer = false;
      digitalWrite(PIN_RELAY, HIGH);
    }
    if (data.daya > DAYA_STANDBY){
      state = digunakan;
    }
    if (millis() - interval.standby >= 60000UL){
      state = tidak_digunakan;
    }
    break;

  case digunakan:
    if (data.daya <= DAYA_STANDBY){
      state = standby;
      interval.timer = true;
    }

    if (data.konsumsi_bulanan >= BATAS_KONSUMSI_BULANAN){
      state = maksimal;
    }

    if (data.konsumsi_harian >= BATAS_KONSUMSI_HARIAN){
      state = maksimal;
    }
    break;

  case tidak_digunakan:
    digitalWrite(PIN_RELAY, LOW);
    break;

  case maksimal:
    digitalWrite(PIN_RELAY, LOW);
    break;
  }

  // ESP_LOGI(TAG, "status: %s, timer: %d, interval stanndby: %d", status_to_str(state), interval.timer, interval.standby);

  /*  RESET BULANAN */
  if (now.month() != time_reset.last_reset_monthly){
    ESP_LOGI(TAG, "Berhasil reset data bulanan");

    data.last_konsumsi_bulanan = 0;
    time_reset.last_reset_monthly = now.month();
    file_write("/last_konsumsi_bulanan.f", data.last_konsumsi_bulanan);
    file_write("/last_reset_monthly.i", now.month());
  }

  /*  RESET HARIAN  */
  if (now.day() != time_reset.last_reset_harian){
    data.konsumsi_harian = 0;
    time_reset.last_reset_harian = now.day();

    file_write("/last_reset_harian.i", now.day());
    file_write("/last_konsumsi_harian.f", data.konsumsi_harian);
  }

  /* MQTT & DATABASE  */
  if (millis() - interval.last_upload > interval.upload_data * 1000UL){
    
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"arus\":%f,\"tegangan\":%f,\"daya\":%f,\"energi\":%f,\"konsumsi_harian\":%f,\"konsumsi_bulanan\":%f,\"timestamp\":\"%s\",\"device_state\":\"%s\"}", 
      data.arus, data.tegangan, data.daya, data.energi, data.konsumsi_harian, data.konsumsi_bulanan, now.timestamp().c_str(), status_to_str(state));
    
    mqtt.publish("powermeter/data", payload);

    char time_header[20];
    sprintf(time_header, "%04d:%02d:%02d:%02d:%02d:%02d", now.year(),now.month(),now.day(),now.hour(),now.minute(),now.second());
    String today = String(now.day()) + '-' + String(now.month()) + '-' + String(now.year());
    dbSendData("/powermeter/", payload, time_header);

    file_write("/last_konsumsi_bulanan.f", data.konsumsi_bulanan);
    file_write("/last_konsumsi_harian.f", data.konsumsi_harian);

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
    sprintf(row_2, "P:%.2f E:%.3f", data.daya, data.energi);
    lcd_show(0, 0, row_1);
    lcd_show(0, 1, row_2);
  }


 /*   FORMAT SETTING JAM = TAHUN-BULAN-TANGGAL JAM:MENIT:DETIK 
       example: 2026-03-15 14:16:00
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
    if (mqtt.connect(clientId.c_str(), mqtt_username, mqtt_password,"powermeter/status", 1, true, "offline")) {
      lcd.clear();
      ESP_LOGI(TAG,"MQTT Connected");
      lcd_show(0,0,"MQTT: Connected");
      mqtt.subscribe("powermeter/button");
      mqtt.publish("powermeter/status", "online",true);
      delay(1000);
      lcd.clear();
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
    Serial.println(status_to_str(state));
  }
  else if (pesan == "OFF"){
    // if (state != maksimal){
    //   state = digunakan;
    //   Serial.println(pesan);
    // }
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

const char* status_to_str(powermmeter_status state){
  switch(state){
    case standby:
      return "standby";
    case digunakan:
      return "digunakan";
    case tidak_digunakan:
      return "tidak digunakan";
    case maksimal:
      return "maksimal";
    default:
      return "unknown status";
  }
}