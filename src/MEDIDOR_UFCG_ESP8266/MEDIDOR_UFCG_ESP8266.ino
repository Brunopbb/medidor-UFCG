#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include "secrets.h"

#define FIRMWARE_VERSION "1.0.11" 

const char *SSID = SECRET_SSID;
const char *PASSWORD = SECRET_PASSWORD;
const char *BROKER = SECRET_BROKER;
const char *URL_VERSAO = SECRET_URL_VERSAO;
const char *URL_FIRMWARE = SECRET_URL_FIRMWARE;

const char *CLIENT_ID = "MEDIDOR_UFCG";           // medidor de CASA (o do LABMET usa MEDIDOR_UFCG_LABMET)
const char *TOPIC_PUBLISH = "/UFCG/pwrc/";
const char *TOPIC_LOG = "/UFCG/pwrc/log";
const char *TOPIC_SUBSCRIBE = "MEDIDOR_UFCG_CONTROLE_CASA";

String Leitura = "";
int tentativa;
unsigned long Agora;
unsigned long ultimaTentativaMQTT = 0;
unsigned long ultimoStatus = 0;
unsigned long ultimoOTA = 0;

void ConnectWifi(void);
void connectMQTT(void); 
void Callback(char* topic, byte* payload, unsigned int length);
void checkAndDownloadUpdate(void);

WiFiClient WifiClient;
PubSubClient client(BROKER, 1883, WifiClient);

void setup() {
  Serial.begin(19200);
  Serial.setTimeout(250);
  
  // Linha em branco para limpar lixo do boot no Monitor Serial
  Serial.println();
  Serial.println("=== BOOT: MEDIDOR CASA ===");
  
  client.setBufferSize(512);
  client.setKeepAlive(60);   // era 15s: curto demais para enlace com perda
  client.setSocketTimeout(20);
  client.setCallback(Callback);
  
  ConnectWifi();
  connectMQTT();

  String msgBoot = "Medidor Iniciado. Versao: " + String(FIRMWARE_VERSION);
  client.publish(TOPIC_LOG, msgBoot.c_str());
  Serial.println(msgBoot);
  delay(1000);
}

void Callback(char* topic, byte * payload, unsigned int length) {
  String mensagem = "";
  for (unsigned int i = 0; i < length; i++) {
    mensagem += (char)payload[i];
  }

  if (mensagem == "RESET_ESP") {
    Serial.println("Comando via MQTT: Reiniciando ESP8266...");
    ESP.reset();
  }
  else if (mensagem == "RESET") {
    Serial.print("QRESETM");
  }
  else if (mensagem == "ATUALIZAR") {
    Serial.println("Comando via MQTT: Iniciar OTA");
    client.publish(TOPIC_LOG, "Iniciando checagem de OTA...");
    delay(1000);
    checkAndDownloadUpdate();
  }
  else if (mensagem.startsWith("QCALIB")) {
    Serial.print(mensagem); 
    Serial.println("Aviso: Comando de Calibracao recebido e repassado!");
    client.publish(TOPIC_LOG, "Comando de Calibracao repassado ao ATMega...");
  }
  else {
    client.publish(TOPIC_LOG, "MEDIDOR_UFCG_OK");
  }
}

void loop() {
  if (Serial.available() > 0) {
    Leitura = Serial.readStringUntil('\n'); 
    Leitura.trim(); 
  }

  // --- GUARDIÃO DE CONEXÃO WI-FI ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("AVISO: Conexao Wi-Fi perdida! Tentando reconectar...");
    ConnectWifi();
    connectMQTT();
  }

  // --- GUARDIÃO MQTT: reconecta assim que cair, sem esperar chegar JSON ---
  if (!client.connected()) {
    if (millis() - ultimaTentativaMQTT > 5000) {
      ultimaTentativaMQTT = millis();
      Serial.println("AVISO: MQTT caiu. Tentando reconectar...");
      if (client.connect(CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
        client.subscribe(TOPIC_SUBSCRIBE);
        Serial.println("MQTT reconectado.");
      }
    }
  } else {
    client.loop();
  }

  // --- Diagnostico de rede a cada 60s ---
  if (client.connected() && (millis() - ultimoStatus > 60000)) {
    ultimoStatus = millis();
    // Buffer fixo em vez de concatenar String: no ESP8266 a concatenacao cria
    // temporarios que fragmentam o heap (~390 B/min medidos em 29/08).
    char st[110];
    snprintf(st, sizeof(st), "RSSI=%ddBm IP=%s heap=%u frag=%u%% up=%lus",
             WiFi.RSSI(), WiFi.localIP().toString().c_str(),
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapFragmentation(),
             millis() / 1000UL);
    client.publish(TOPIC_LOG, st);
  }

  if (Leitura != "") {
    
    // --- O GUARDIÃO DO JSON ---
    if (Leitura.startsWith("{")) {
      // É um JSON válido! Pode ir para o banco de dados.
      client.publish(TOPIC_PUBLISH, Leitura.c_str());
      Serial.println("JSON enviado com sucesso!");
    } else {
      // É um texto, log ou erro do Arduino. Vai para o tópico de log!
      client.publish(TOPIC_LOG, Leitura.c_str());
    }
    
    Leitura = "";
  }
}

void ConnectWifi(void) {
  tentativa = 0;
  Serial.print("Conectando a rede Wi-Fi: ");
  Serial.println(SSID);

  // NAO usar WIFI_NONE_SLEEP nesta placa: o radio sempre ligado sobe o consumo
  // de ~15mA medios para ~70mA constantes. Medido em 29/08: sem ganho de cobertura.
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  WiFi.setAutoReconnect(true);

  // O SDK guarda canal e BSSID do ultimo AP em flash e tenta por ali primeiro.
  // Se o roteador muda de canal (modo automatico, ou mudanca manual), esse cache
  // fica obsoleto e a associacao falha em loop. Limpar forca varredura completa.
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);

  WiFi.begin(SSID , PASSWORD);
  
  while ((WiFi.status() != WL_CONNECTED) && (tentativa < 45)) {
    tentativa++;
    Serial.print("Tentativa de conexao Wi-Fi ");
    Serial.print(tentativa);
    Serial.println("/45...");
    delay(1000);
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERRO CRITICO: Falha na conexao Wi-Fi. Reiniciando modulo...");
    delay(500);
    ESP.reset();
  } else {
    Serial.println("SUCESSO: Wi-Fi conectado!");
    Serial.print("Endereco IP local: ");
    Serial.println(WiFi.localIP());
  }
}

void connectMQTT(void) {
  Serial.print("Conectando ao broker MQTT: ");
  Serial.println(BROKER);
  
  Agora = millis();
  
  while ((!client.connected()) && ((millis() - Agora) < 55000)) {
    Serial.println("Tentativa de conexao MQTT...");
    if (client.connect(CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
      Serial.println("SUCESSO: Conectado ao broker MQTT!");
      client.subscribe(TOPIC_SUBSCRIBE);
      Serial.print("Inscrito no topico: ");
      Serial.println(TOPIC_SUBSCRIBE);
    } else {
      Serial.print("Falha na conexao MQTT. Codigo de erro: ");
      Serial.print(client.state());
      Serial.println(" - Tentando novamente em 1s...");
      delay(1000);
    }
  }
  
  if ((millis() - Agora) > 55000) {
    Serial.println("ERRO CRITICO: Tempo limite de conexao MQTT esgotado. Reiniciando...");
    ESP.reset();
  }
}

void checkAndDownloadUpdate(void) {
  // Tentativas repetidas de OTA que falham nao liberam toda a memoria alocada.
  // Observado em 29/08: 10 tentativas seguidas derrubaram o heap em ~8 KB.
  if (ultimoOTA != 0 && (millis() - ultimoOTA) < 180000) {
    client.publish(TOPIC_LOG, "OTA ignorado: aguarde 3 min entre tentativas.");
    return;
  }
  ultimoOTA = millis();

  WiFiClient updateClient;
  HTTPClient http;

  // Timeout padrao e 5s: curto demais para 300 KB em enlace com perda de pacotes.
  updateClient.setTimeout(20000);

  Serial.println("Buscando atualizacoes OTA...");
  http.begin(updateClient, URL_VERSAO);
  http.setTimeout(15000);
  int httpCode = http.GET();

  String serverVersion = "";
  if (httpCode == HTTP_CODE_OK) {
    serverVersion = http.getString();
    serverVersion.trim();
  }
  // Libera o WiFiClient ANTES do update: o ESPhttpUpdate precisa dele livre.
  http.end();

  if (httpCode != HTTP_CODE_OK) {
    String erro = "Falha ao checar versao. HTTP=" + String(httpCode);
    Serial.println(erro);
    client.publish(TOPIC_LOG, erro.c_str());
    return;
  }

  Serial.print("Versao no servidor: ");
  Serial.println(serverVersion);

  if (serverVersion == FIRMWARE_VERSION) {
    Serial.println("O firmware ja esta na versao mais recente.");
    client.publish(TOPIC_LOG, "Firmware ja atualizado.");
    return;
  }

  String msg = "Baixando " + serverVersion + " (atual " + String(FIRMWARE_VERSION) + ")...";
  Serial.println(msg);
  client.publish(TOPIC_LOG, msg.c_str());
  delay(200);
  client.disconnect();

  ESPhttpUpdate.rebootOnUpdate(true);
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = ESPhttpUpdate.update(updateClient, URL_FIRMWARE);

  if (ret == HTTP_UPDATE_OK) {
    Serial.println("Atualizacao OTA concluida!");   // reinicia sozinho
  } else {
    String falha = "Falha OTA (" + String((int)ret) + "): " + ESPhttpUpdate.getLastErrorString();
    Serial.println(falha);
    if (client.connect(CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
      client.subscribe(TOPIC_SUBSCRIBE);
      client.publish(TOPIC_LOG, falha.c_str());
    }
  }
}
