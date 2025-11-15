static const char* TAG = "MQTTHandler";
#include "MQTT.h"
#include "WiFi.h"
#include "esp_log.h"

class MQTTHandler{
    public:
        MQTTHandler(const char* ssid, const char* password, const char* mqtt_server, uint16_t mqtt_port, const char* mqtt_username, const char* mqtt_password):
        _ssid(ssid), _password(password), _mqtt_server(mqtt_server), _mqtt_port(mqtt_port), _mqtt_username(mqtt_username), _mqtt_password(mqtt_password){};
        void connect();
        void publish(const char* topic, const char* payload);
        void setCallback(void (*callback)(char*, uint8_t*, unsigned int));
        bool connected();
        void loop();    
    
    private:
        const char* _ssid;
        const char* _password;
        const char* _mqtt_server;
        uint16_t _mqtt_port;
        const char* _mqtt_username;
        const char* _mqtt_password;
        WiFiClient wifi;
        MQTTClient mqtt;

        void connect(){
            ESP_LOGI(TAG, "Connecting to WiFi SSID: %s, Password: %s", _ssid, _password);
            while (WiFi.status() != WL_CONNECTED) {
                WiFi.begin(_ssid, _password);
                delay(500);
            }
            ESP_LOGI(TAG, "WiFi Connected!");
            ESP_LOGI(TAG, "Connecting to MQTT Broker: %s:%d, user: %s, password: %s", _mqtt_server, _mqtt_port,_mqtt_username, _mqtt_password);
            
            while(!mqtt.connected()){
                if (mqtt.connect("ESP32Client", _mqtt_username, _mqtt_password)){
                    ESP_LOGI(TAG, "MQTT Connected!");
                }
            }
        }

};