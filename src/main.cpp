// ============================================================================
// STAZIONE SMART - Nodo Singolo
// Sensori: SHT30, MQ-135, PIR HC-SR501
// Interazione: TTP223 (touch button)
// Display: TFT 2.0" ST7789V
// Rete: WiFi + InfluxDB + Web Dashboard
// ============================================================================

#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "esp_task_wdt.h"

#include "config.h"

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
// I2C (SHT30)
#define I2C_SDA         21
#define I2C_SCL         22

// MQ-135 (Analogico)
#define MQ135_PIN       34    // ADC, input-only

// PIR HC-SR501
#define PIR_PIN         27

// TTP223 Touch Button
#define TOUCH_PIN       26

// TFT ST7789V (SPI)
#define TFT_CS          5
#define TFT_DC          2
#define TFT_RST         4
#define TFT_MOSI        23
#define TFT_SCLK        18
#define TFT_BLK         15

// ============================================================================
// OGGETTI
// ============================================================================
Adafruit_SHT31 sht30 = Adafruit_SHT31();
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);

// ============================================================================
// DATI SENSORI
// ============================================================================
struct SensorData {
    float temperature;
    float humidity;
    float dewPoint;
    float heatIndex;
    const char* comfort;
    int   airQuality;
    bool  motion;
    bool  alarm;
    int   alarmCount;
    unsigned long lastMotionTime;
} data;

// ============================================================================
// CALCOLI DERIVATI (da esp32-sht3x-ssd1306)
// ============================================================================
float calcDewPoint(float temp, float hum) {
    const float b = 17.62;
    const float c = 243.12;
    float gamma = log(hum / 100.0) + (b * temp) / (c + temp);
    return (c * gamma) / (b - gamma);
}

float calcHeatIndex(float tempC, float hum) {
    float T = tempC * 1.8 + 32.0;
    if (T < 80.0) return tempC;
    float HI = -42.379 + 2.04901523*T + 10.14333127*hum
               - 0.22475541*T*hum - 0.00683783*T*T
               - 0.05481717*hum*hum + 0.00122874*T*T*hum
               + 0.00085282*T*hum*hum - 0.00000199*T*T*hum*hum;
    return (HI - 32.0) / 1.8;
}

const char* getComfortZone(float temp, float hum) {
    bool tempOk = (temp >= 20.0 && temp <= 26.0);
    bool humOk = (hum >= 30.0 && hum <= 60.0);
    if (tempOk && humOk) return "OK";
    if (temp < 20.0 && humOk) return "Cold";
    if (temp > 26.0 && humOk) return "Hot";
    if (tempOk && hum < 30.0) return "Dry";
    if (tempOk && hum > 60.0) return "Humid";
    if (temp < 20.0 && hum < 30.0) return "Cold+Dry";
    if (temp < 20.0 && hum > 60.0) return "Cold+Hum";
    if (temp > 26.0 && hum < 30.0) return "Hot+Dry";
    return "Hot+Hum";
}

// ============================================================================
// DISPLAY
// ============================================================================
int currentScreen = 0;
#define NUM_SCREENS 4
bool lastTouchState = false;
unsigned long lastDebounce = 0;
#define DEBOUNCE_MS 300

// ============================================================================
// SOGLIE
// ============================================================================
#define AIR_GOOD        400
#define AIR_MODERATE    800
#define AIR_BAD         1200

// ============================================================================
// TIMING
// ============================================================================
unsigned long lastSensorRead   = 0;
unsigned long lastInfluxSend   = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTelegramAlarm = 0;
unsigned long lastWifiCheck    = 0;

#define SENSOR_INTERVAL   2000    // 2s
#define INFLUX_INTERVAL   60000   // 60s
#define DISPLAY_INTERVAL  1000    // 1s
#define WIFI_CHECK_INTERVAL 30000 // 30s
#define MAX_INFLUX_ERRORS 10      // reboot dopo 10 errori consecutivi
#define WDT_TIMEOUT       30      // watchdog 30s

// ============================================================================
// STATO RETE
// ============================================================================
bool wifiConnected = false;
bool webServerStarted = false;
bool influxOk = false;
int influxErrors = 0;

// ============================================================================
// HTML DASHBOARD
// ============================================================================
const char HTML_DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stazione ESP32</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#1a1a2e,#16213e);min-height:100vh;color:#fff;padding:20px}
.header{text-align:center;margin-bottom:25px;padding:15px;background:rgba(255,255,255,0.1);border-radius:12px}
.header h1{font-size:1.8em;background:linear-gradient(90deg,#00d2ff,#3a7bd5);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.status{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}
.ok{background:#0f8}
.ko{background:#f44;animation:blink .5s infinite}
@keyframes blink{50%{opacity:.2}}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:18px;max-width:1200px;margin:0 auto}
.card{background:rgba(255,255,255,0.06);border-radius:12px;padding:20px;border:1px solid rgba(255,255,255,0.1)}
.card:hover{transform:translateY(-3px);box-shadow:0 8px 30px rgba(0,0,0,0.3)}
.card h2{font-size:1em;margin-bottom:12px;padding-bottom:10px;border-bottom:1px solid rgba(255,255,255,0.1)}
.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid rgba(255,255,255,0.04)}
.label{color:rgba(255,255,255,0.6);font-size:.9em}
.val{font-size:1.3em;font-weight:700}
.unit{font-size:.7em;color:rgba(255,255,255,0.4)}
.bar{width:100%;height:6px;background:rgba(255,255,255,0.1);border-radius:3px;margin-top:8px;overflow:hidden}
.fill{height:100%;border-radius:3px;transition:width .5s}
.green{background:linear-gradient(90deg,#0f8,#0da)}
.yellow{background:linear-gradient(90deg,#fc0,#f90)}
.red{background:linear-gradient(90deg,#f60,#f00)}
.alarm-banner{background:linear-gradient(90deg,#f00,#fb0);padding:12px;border-radius:8px;text-align:center;font-weight:600;margin-bottom:15px;animation:blink 1s infinite;display:none}
.alarm-banner.on{display:block}
.pill{display:inline-block;padding:4px 12px;border-radius:20px;font-size:.85em;font-weight:600}
.pill-ok{background:rgba(0,255,136,0.2);color:#0f8}
.pill-alarm{background:rgba(255,0,0,0.2);color:#f44}
.footer{text-align:center;margin-top:25px;color:rgba(255,255,255,0.3);font-size:.8em}
</style>
</head>
<body>
<div class="header">
<h1>Stazione Smart ESP32</h1>
<p><span class="status ok" id="si"></span><span id="st">Online</span></p>
<p style="font-size:.8em;color:rgba(255,255,255,0.4);margin-top:4px">Aggiornamento: <span id="ts">--</span></p>
</div>
<div class="alarm-banner" id="ab">ALLARME - Movimento rilevato!</div>
<div class="grid">
<div class="card">
<h2>Temperatura / Umidita</h2>
<div class="row"><span class="label">Temperatura</span><span class="val"><span id="t">--</span><span class="unit"> C</span></span></div>
<div class="row"><span class="label">Umidita</span><span class="val"><span id="h">--</span><span class="unit"> %</span></span></div>
<div class="bar"><div class="fill green" id="hb" style="width:50%"></div></div>
</div>
<div class="card">
<h2>Qualita Aria</h2>
<div class="row"><span class="label">ADC</span><span class="val" id="aq">--</span></div>
<div class="row"><span class="label">Stato</span><span class="val" id="as">--</span></div>
<div class="bar"><div class="fill" id="ab2" style="width:25%"></div></div>
</div>
<div class="card">
<h2>Sicurezza</h2>
<div class="row"><span class="label">PIR Movimento</span><span id="ps">--</span></div>
<div class="row"><span class="label">Allarmi totali</span><span class="val" id="ac">0</span></div>
</div>
<div class="card">
<h2>Sistema</h2>
<div class="row"><span class="label">IP</span><span id="ip">--</span></div>
<div class="row"><span class="label">InfluxDB</span><span id="ix">--</span></div>
<div class="row"><span class="label">Uptime</span><span id="up">--</span></div>
</div>
</div>
<div class="footer">Stazione Smart ESP32 - WiFi + InfluxDB</div>
<script>
function u(){
fetch('/api/data').then(r=>r.json()).then(d=>{
document.getElementById('ts').textContent=new Date().toLocaleTimeString('it-IT');
document.getElementById('t').textContent=d.temp.toFixed(1);
document.getElementById('h').textContent=d.hum.toFixed(0);
document.getElementById('hb').style.width=d.hum+'%';
document.getElementById('aq').textContent=d.air;
var s=d.air<400?'Ottima':d.air<800?'Buona':d.air<1200?'Moderata':'Scarsa';
var c=d.air<800?'green':d.air<1200?'yellow':'red';
document.getElementById('as').textContent=s;
var b=document.getElementById('ab2');
b.style.width=Math.min(d.air/40.95,100)+'%';
b.className='fill '+c;
document.getElementById('ps').innerHTML=d.motion?'<span class="pill pill-alarm">RILEVATO</span>':'<span class="pill pill-ok">Nessuno</span>';
document.getElementById('ac').textContent=d.alarms;
document.getElementById('ip').textContent=d.ip;
document.getElementById('ix').textContent=d.influx_ok?'OK':'Errore';
var sec=Math.floor(d.uptime/1000);var m=Math.floor(sec/60);var hr=Math.floor(m/60);
document.getElementById('up').textContent=hr+'h '+m%60+'m '+sec%60+'s';
var ab=document.getElementById('ab');
var si=document.getElementById('si');
if(d.alarm){ab.classList.add('on');si.className='status ko';document.getElementById('st').textContent='ALLARME';}
else{ab.classList.remove('on');si.className='status ok';document.getElementById('st').textContent='Online';}
}).catch(()=>{document.getElementById('st').textContent='Disconnesso';});
}
u();setInterval(u,2000);
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// ROTTA STATICA: imposta gateway per raggiungere subnet InfluxDB
// ============================================================================
void addStaticRoute() {
    struct netif *nif = netif_default;
    if (nif != NULL) {
        ip4_addr_t gw;
        IP4_ADDR(&gw, ROUTE_GW_A, ROUTE_GW_B, ROUTE_GW_C, ROUTE_GW_D);
        netif_set_gw(nif, &gw);
        Serial.printf("Gateway impostato: %s (rotta verso subnet InfluxDB)\n",
                       ip4addr_ntoa(netif_ip4_gw(nif)));
    } else {
        Serial.println("Errore: netif non disponibile");
    }
}

// ============================================================================
// INIZIALIZZAZIONE WIFI (DHCP) + ROUTING
// ============================================================================
void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);           // non scrivere credenziali su flash
    WiFi.setSleep(false);             // disabilita power saving WiFi
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connessione a %s (DHCP)", WIFI_SSID);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\nConnesso! IP: %s (DHCP)\n", WiFi.localIP().toString().c_str());
        Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());

        // Aggiungi rotta statica verso subnet InfluxDB
        addStaticRoute();
    } else {
        Serial.println("\nConnessione WiFi fallita!");
    }
}

// forward declaration
void initWebServer();

// ============================================================================
// WIFI RECONNECT CHECK (polling nel loop)
// ============================================================================
int wifiReconnectAttempts = 0;

void checkWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnected) {
            wifiConnected = true;
            wifiReconnectAttempts = 0;
            addStaticRoute();
            Serial.printf("WiFi: riconnesso! IP: %s\n", WiFi.localIP().toString().c_str());

            if (!webServerStarted) {
                initWebServer();
                webServerStarted = true;
            }
        }
        return;
    }

    // Disconnesso
    if (wifiConnected) {
        wifiConnected = false;
        influxOk = false;
        Serial.println("WiFi: disconnesso");
    }

    wifiReconnectAttempts++;
    Serial.printf("WiFi: tentativo riconnessione %d...\n", wifiReconnectAttempts);

    // Dopo 5 tentativi falliti, reset completo dello stack WiFi
    if (wifiReconnectAttempts > 5) {
        Serial.println("WiFi: reset completo stack WiFi");
        WiFi.disconnect(true);  // true = cancella config
        delay(1000);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        wifiReconnectAttempts = 0;
    } else {
        WiFi.disconnect();
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ============================================================================
// INIZIALIZZAZIONE SENSORI
// ============================================================================
void initSensors() {
    Wire.begin(I2C_SDA, I2C_SCL);

    // SHT30 (indirizzo default 0x44)
    if (!sht30.begin(0x44)) {
        Serial.println("SHT30 non trovato su 0x44, provo 0x45...");
        if (!sht30.begin(0x45)) {
            Serial.println("SHT30 ERRORE!");
        } else {
            Serial.println("SHT30 OK (0x45)");
        }
    } else {
        Serial.println("SHT30 OK (0x44)");
    }

    // MQ-135
    pinMode(MQ135_PIN, INPUT);
    Serial.println("MQ-135 OK (preriscaldamento 24-48h)");

    // PIR
    pinMode(PIR_PIN, INPUT);
    Serial.println("PIR OK (stabilizzazione 30-60s)");

    // TTP223
    pinMode(TOUCH_PIN, INPUT);
    Serial.println("TTP223 OK");
}

// ============================================================================
// INIZIALIZZAZIONE DISPLAY
// ============================================================================
void initDisplay() {
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);

    tft.init(240, 320);
    tft.setRotation(1);  // Landscape 320x240
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setCursor(30, 100);
    tft.println("STAZIONE SMART");
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(30, 130);
    tft.println("Inizializzazione...");
    Serial.println("Display TFT OK");
}

// ============================================================================
// WEB SERVER
// ============================================================================
void handleRoot() {
    server.send_P(200, "text/html", HTML_DASHBOARD);
}

void handleApiData() {
    String json = "{";
    json += "\"temp\":" + String(data.temperature, 1) + ",";
    json += "\"hum\":" + String(data.humidity, 1) + ",";
    json += "\"dew\":" + String(data.dewPoint, 1) + ",";
    json += "\"hi\":" + String(data.heatIndex, 1) + ",";
    json += "\"comfort\":\"" + String(data.comfort) + "\",";
    json += "\"air\":" + String(data.airQuality) + ",";
    json += "\"motion\":" + String(data.motion ? "true" : "false") + ",";
    json += "\"alarm\":" + String(data.alarm ? "true" : "false") + ",";
    json += "\"alarms\":" + String(data.alarmCount) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"influx_ok\":" + String(influxOk ? "true" : "false") + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"uptime\":" + String(millis());
    json += "}";
    server.send(200, "application/json", json);
}

void initWebServer() {
    server.on("/", handleRoot);
    server.on("/api/data", handleApiData);
    server.onNotFound([]() {
        server.send(404, "text/plain", "404");
    });
    server.begin();
    Serial.println("Web Server avviato porta 80");
}

// ============================================================================
// LETTURA SENSORI
// ============================================================================
void readSensors() {
    // SHT30
    data.temperature = sht30.readTemperature();
    data.humidity = sht30.readHumidity();

    if (isnan(data.temperature)) data.temperature = 0;
    if (isnan(data.humidity)) data.humidity = 0;

    // Valori derivati
    if (data.temperature > 0 && data.humidity > 0) {
        data.dewPoint = calcDewPoint(data.temperature, data.humidity);
        data.heatIndex = calcHeatIndex(data.temperature, data.humidity);
        data.comfort = getComfortZone(data.temperature, data.humidity);
    } else {
        data.dewPoint = 0;
        data.heatIndex = 0;
        data.comfort = "---";
    }

    // MQ-135
    data.airQuality = analogRead(MQ135_PIN);

    // PIR
    bool prevMotion = data.motion;
    data.motion = digitalRead(PIR_PIN) == HIGH;

    if (data.motion && !prevMotion) {
        data.lastMotionTime = millis();
        data.alarmCount++;
    }

    data.alarm = data.motion;

    // Debug
    Serial.printf("T:%.1fC H:%.1f%% D:%.1f HI:%.1f [%s] Air:%d PIR:%s\n",
                  data.temperature, data.humidity,
                  data.dewPoint, data.heatIndex, data.comfort,
                  data.airQuality,
                  data.motion ? "SI" : "no");
}

// ============================================================================
// TOUCH BUTTON (cambio schermata)
// ============================================================================
void handleTouch() {
    bool touchState = digitalRead(TOUCH_PIN) == HIGH;
    unsigned long now = millis();

    if (touchState && !lastTouchState && (now - lastDebounce > DEBOUNCE_MS)) {
        lastDebounce = now;
        currentScreen = (currentScreen + 1) % NUM_SCREENS;
        Serial.printf("Schermata: %d\n", currentScreen);
    }
    lastTouchState = touchState;
}

// ============================================================================
// INFLUXDB
// ============================================================================
void sendToInfluxDB() {
    if (!wifiConnected) return;

    HTTPClient http;
    WiFiClient client;

    String url = "http://";
    url += INFLUXDB_HOST;
    url += ":";
    url += INFLUXDB_PORT;
    url += "/write?db=";
    url += INFLUXDB_DB;

    // Line protocol - sensori + valori derivati
    char body[512];
    snprintf(body, sizeof(body),
        "sensor_data,host=esp32-stazione,comfort=%s "
        "temp=%.1f,humidity=%.1f,dewpoint=%.1f,heatindex=%.1f,"
        "air_quality=%di,motion=%di,alarm_count=%di",
        data.comfort,
        data.temperature, data.humidity, data.dewPoint, data.heatIndex,
        data.airQuality, data.motion ? 1 : 0, data.alarmCount);

    http.begin(client, url);
    http.addHeader("Content-Type", "text/plain");
    http.setTimeout(3000);

    int code = http.POST(body);
    http.end();

    if (code == 204 || code == 200) {
        influxOk = true;
        influxErrors = 0;
        Serial.println("InfluxDB OK");
    } else {
        influxOk = false;
        influxErrors++;
        Serial.printf("InfluxDB errore: %d (consecutivi: %d)\n", code, influxErrors);
        if (influxErrors >= MAX_INFLUX_ERRORS) {
            Serial.println("Troppi errori InfluxDB, reboot...");
            delay(1000);
            ESP.restart();
        }
    }
}

// ============================================================================
// SYSTEM STATS -> INFLUXDB
// ============================================================================
void sendSystemStats() {
    if (!wifiConnected) return;

    HTTPClient http;
    WiFiClient client;

    String url = "http://";
    url += INFLUXDB_HOST;
    url += ":";
    url += INFLUXDB_PORT;
    url += "/write?db=";
    url += INFLUXDB_DB;

    char payload[300];
    snprintf(payload, sizeof(payload),
        "esp32_stats,host=esp32-stazione "
        "free_heap=%lui,min_free_heap=%lui,heap_size=%lui,"
        "rssi=%di,uptime=%lui,"
        "cpu_freq=%ui,flash_size=%lui,sketch_size=%lui,free_sketch=%lui",
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        ESP.getHeapSize(),
        WiFi.RSSI(),
        millis() / 1000,
        ESP.getCpuFreqMHz(),
        ESP.getFlashChipSize(),
        ESP.getSketchSize(),
        ESP.getFreeSketchSpace());

    http.begin(client, url);
    http.addHeader("Content-Type", "text/plain");
    http.setTimeout(3000);

    int code = http.POST(payload);
    http.end();

    if (code == 204 || code == 200) {
        Serial.println("SystemStats OK");
    } else {
        Serial.printf("SystemStats errore: %d\n", code);
    }
}

// ============================================================================
// TELEGRAM (opzionale)
// ============================================================================
void sendTelegramAlert() {
    if (!TELEGRAM_ENABLED || !wifiConnected) return;
    if (millis() - lastTelegramAlarm < TELEGRAM_COOLDOWN) return;

    WiFiClientSecure secClient;
    secClient.setInsecure();
    HTTPClient http;

    String msg = "ALLARME+Movimento+rilevato+sulla+stazione+ESP32";
    String url = "https://api.telegram.org/bot";
    url += TELEGRAM_BOT_TOKEN;
    url += "/sendMessage?chat_id=";
    url += TELEGRAM_CHAT_ID;
    url += "&text=";
    url += msg;

    http.begin(secClient, url);
    int code = http.GET();
    http.end();

    if (code == 200) {
        lastTelegramAlarm = millis();
        Serial.println("Telegram: notifica inviata");
    }
}

// ============================================================================
// DISPLAY - SCHERMATE (layout statico + aggiornamento solo valori)
// ============================================================================
int lastScreen = -1;  // forza primo disegno completo

// Disegna layout statico schermata 0
void drawLayout0() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(40, 5);
    tft.print("TEMPERATURA");
    tft.drawFastHLine(0, 28, 320, ST77XX_WHITE);
    tft.drawFastHLine(0, 120, 320, ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(40, 135);
    tft.print("UMIDITA");
    tft.drawRect(20, 215, 280, 15, ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(270, 3);
    tft.print("1/4");
}

void updateValues0() {
    // Temperatura
    tft.fillRect(30, 50, 260, 60, ST77XX_BLACK);
    tft.setTextSize(5);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(30, 50);
    tft.printf("%.1f", data.temperature);
    tft.setTextSize(3);
    tft.print(" C");

    // Umidita
    tft.fillRect(60, 155, 200, 35, ST77XX_BLACK);
    tft.setTextSize(4);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(60, 155);
    tft.printf("%.0f %%", data.humidity);

    // Dew point + Heat index + Comfort
    tft.fillRect(10, 195, 310, 15, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(10, 195);
    tft.printf("Dew:%.1f HI:%.1f", data.dewPoint, data.heatIndex);

    // Comfort
    tft.fillRect(10, 218, 310, 18, ST77XX_BLACK);
    tft.setTextSize(2);
    uint16_t comfortColor = (strcmp(data.comfort, "OK") == 0) ? ST77XX_GREEN : ST77XX_ORANGE;
    tft.setTextColor(comfortColor);
    tft.setCursor(10, 218);
    tft.printf("[%s]", data.comfort);
}

// Disegna layout statico schermata 1
void drawLayout1() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(40, 5);
    tft.print("QUALITA ARIA");
    tft.drawFastHLine(0, 28, 320, ST77XX_WHITE);
    tft.drawRect(20, 175, 280, 20, ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(20, 200);
    tft.print("0");
    tft.setCursor(140, 200);
    tft.print("2048");
    tft.setCursor(275, 200);
    tft.print("4095");
    tft.setTextColor(0x7BEF);
    tft.setCursor(270, 3);
    tft.print("2/4");
}

void updateValues1() {
    uint16_t color;
    const char* status;
    if (data.airQuality < AIR_GOOD) {
        color = ST77XX_GREEN; status = "BUONA";
    } else if (data.airQuality < AIR_MODERATE) {
        color = ST77XX_YELLOW; status = "MODERATA";
    } else if (data.airQuality < AIR_BAD) {
        color = ST77XX_ORANGE; status = "SCARSA";
    } else {
        color = ST77XX_RED; status = "PESSIMA";
    }

    // Valore ADC
    tft.fillRect(70, 55, 200, 50, ST77XX_BLACK);
    tft.setTextSize(5);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(70, 55);
    tft.printf("%d", data.airQuality);

    // Stato
    tft.fillRect(40, 120, 250, 30, ST77XX_BLACK);
    tft.setTextSize(3);
    tft.setCursor(40, 120);
    tft.setTextColor(color);
    tft.print(status);

    // Barra
    tft.fillRect(21, 176, 278, 18, ST77XX_BLACK);
    int barW = map(data.airQuality, 0, 4095, 0, 278);
    tft.fillRect(21, 176, barW, 18, color);
}

// Disegna layout statico schermata 2
void drawLayout2() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_RED);
    tft.setCursor(50, 5);
    tft.print("SICUREZZA");
    tft.drawFastHLine(0, 28, 320, ST77XX_WHITE);
    tft.setCursor(20, 50);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("PIR: ");
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(270, 3);
    tft.print("3/4");
}

void updateValues2() {
    // Stato PIR
    tft.fillRect(80, 45, 240, 30, ST77XX_BLACK);
    tft.setCursor(80, 50);
    if (data.motion) {
        tft.setTextSize(3);
        tft.setTextColor(ST77XX_RED);
        tft.print("MOVIMENTO!");
    } else {
        tft.setTextSize(3);
        tft.setTextColor(ST77XX_GREEN);
        tft.print("OK");
    }

    // Contatore allarmi
    tft.fillRect(20, 110, 300, 20, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(20, 110);
    tft.printf("Allarmi: %d", data.alarmCount);

    // Ultimo movimento
    tft.fillRect(20, 145, 300, 20, ST77XX_BLACK);
    if (data.lastMotionTime > 0) {
        unsigned long ago = (millis() - data.lastMotionTime) / 1000;
        tft.setCursor(20, 145);
        tft.setTextColor(ST77XX_YELLOW);
        tft.printf("Ultimo: %lus fa", ago);
    }

    // Stato generale
    tft.fillRect(20, 185, 300, 35, ST77XX_BLACK);
    tft.setCursor(20, 190);
    tft.setTextSize(3);
    if (data.alarm) {
        tft.setTextColor(ST77XX_RED);
        tft.print("! ALLARME !");
    } else {
        tft.setTextColor(ST77XX_GREEN);
        tft.print("TUTTO OK");
    }
}

// Disegna layout statico schermata 3
void drawLayout3() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_BLUE);
    tft.setCursor(80, 5);
    tft.print("RETE");
    tft.drawFastHLine(0, 28, 320, ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(10, 55);
    tft.printf("SSID: %s", WIFI_SSID);
    tft.setCursor(10, 70);
    tft.printf("IP:   %s", WiFi.localIP().toString().c_str());
    tft.setCursor(10, 85);
    tft.printf("GW:   %s", WiFi.gatewayIP().toString().c_str());
    tft.setCursor(10, 100);
    tft.print("Route: InfluxDB subnet via ");
    tft.print(WiFi.gatewayIP());
    tft.drawFastHLine(0, 115, 320, ST77XX_WHITE);
    tft.setCursor(10, 125);
    tft.printf("InfluxDB: %s:%d", INFLUXDB_HOST, INFLUXDB_PORT);
    tft.setCursor(10, 140);
    tft.printf("Database: %s", INFLUXDB_DB);
    tft.drawFastHLine(0, 170, 320, ST77XX_WHITE);
    tft.setCursor(10, 180);
    tft.printf("Dashboard: http://%s", WiFi.localIP().toString().c_str());
    tft.setTextColor(0x7BEF);
    tft.setCursor(270, 3);
    tft.print("4/4");
}

void updateValues3() {
    // WiFi status
    tft.fillRect(10, 40, 200, 10, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(10, 40);
    tft.printf("WiFi: %s", wifiConnected ? "Connesso" : "ERRORE");

    // InfluxDB stato
    tft.fillRect(10, 155, 300, 10, ST77XX_BLACK);
    tft.setCursor(10, 155);
    tft.printf("Stato: %s (err: %d)", influxOk ? "OK" : "ERRORE", influxErrors);

    // Uptime
    unsigned long sec = millis() / 1000;
    unsigned long m = sec / 60;
    unsigned long h = m / 60;
    tft.fillRect(10, 200, 300, 10, ST77XX_BLACK);
    tft.setCursor(10, 200);
    tft.printf("Uptime: %luh %lum %lus", h, m % 60, sec % 60);

    tft.fillRect(10, 220, 200, 10, ST77XX_BLACK);
    tft.setCursor(10, 220);
    tft.printf("RSSI: %d dBm", WiFi.RSSI());
}

void updateDisplay() {
    // Cambio schermata: ridisegna layout statico
    if (currentScreen != lastScreen) {
        switch (currentScreen) {
            case 0: drawLayout0(); break;
            case 1: drawLayout1(); break;
            case 2: drawLayout2(); break;
            case 3: drawLayout3(); break;
        }
        lastScreen = currentScreen;
    }

    // Aggiorna solo i valori dinamici
    switch (currentScreen) {
        case 0: updateValues0(); break;
        case 1: updateValues1(); break;
        case 2: updateValues2(); break;
        case 3: updateValues3(); break;
    }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== STAZIONE SMART ESP32 ===\n");

    initDisplay();
    initSensors();
    initWiFi();

    if (wifiConnected) {
        initWebServer();
        webServerStarted = true;
    }

    // Stabilizzazione PIR
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(20, 100);
    tft.println("Stabilizzazione PIR");
    tft.setCursor(20, 130);
    tft.println("Attendere 30s...");

    for (int i = 30; i > 0; i--) {
        tft.fillRect(20, 170, 200, 30, ST77XX_BLACK);
        tft.setCursor(20, 170);
        tft.setTextColor(ST77XX_WHITE);
        tft.printf("%d secondi", i);
        delay(1000);
    }

    // Watchdog: se il loop si blocca per >30s, reboot automatico
    esp_task_wdt_init(WDT_TIMEOUT, true);
    esp_task_wdt_add(NULL);

    Serial.println("Inizializzazione completata!\n");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
    esp_task_wdt_reset();  // feed watchdog
    unsigned long now = millis();

    // WiFi check periodico
    if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
        lastWifiCheck = now;
        checkWiFi();
    }

    // Web server
    if (wifiConnected) {
        server.handleClient();
    }

    // Touch button
    handleTouch();

    // Lettura sensori
    if (now - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = now;
        readSensors();

        // Telegram su allarme
        if (data.alarm) {
            sendTelegramAlert();
        }
    }

    // InfluxDB
    if (now - lastInfluxSend >= INFLUX_INTERVAL) {
        lastInfluxSend = now;
        sendToInfluxDB();
        sendSystemStats();
    }

    // Display
    if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
        lastDisplayUpdate = now;
        updateDisplay();
    }
}
