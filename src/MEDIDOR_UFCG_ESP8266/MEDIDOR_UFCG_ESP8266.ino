#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>

extern "C" {
  #include "user_interface.h"
}

// Forca calibracao completa de RF em todo boot. Por padrao o ESP8266 reaproveita
// a calibracao anterior, e um estado ruim sobrevive ao ESP.restart(). Custa cerca
// de 200 ms no boot e elimina uma das causas de travamento do radio.
extern "C" void preinit(void) {
  system_phy_set_powerup_option(3);
}
#include "secrets.h"

// O conversor A/D passa a medir a alimentacao do proprio ESP (VDD3P3), em mV.
// O firmware nao usa analogRead, entao nao ha o que perder.
ADC_MODE(ADC_VCC);

#define FIRMWARE_VERSION "1.0.26" 

const char *SSID = SECRET_SSID;
const char *PASSWORD = SECRET_PASSWORD;
const char *BROKER = SECRET_BROKER;
const char *URL_VERSAO = SECRET_URL_VERSAO;
const char *URL_FIRMWARE = SECRET_URL_FIRMWARE;

const char *CLIENT_ID = "MEDIDOR_UFCG";           // medidor de CASA (o do LABMET usa MEDIDOR_UFCG_LABMET)
const char *TOPIC_PUBLISH = "/UFCG/pwrc/";
const char *TOPIC_LOG = "/UFCG/pwrc/log/casa";   // separado do LABMET; o servidor grava /UFCG/pwrc/log/#
const char *TOPIC_SUBSCRIBE = "MEDIDOR_UFCG_CONTROLE_CASA";

// Potencia de transmissao do Wi-Fi (dBm). O padrao do ESP8266 e 20,5 dBm, e os
// picos de corrente de TX (~170 mA) saem da mesma fonte de 3,3 V que alimenta a
// placa inteira, com so 10 uF junto ao ESP. O roteador fica a poucos metros:
// RSSI de -34 dBm em 23/09. Com 12 dBm o enlace ainda sobra em mais de 40 dB e
// o pico de TX cai bastante. Tentativa por software contra os travamentos
// (1.0.25), ja que a placa nao sera modificada.
#define WIFI_TX_DBM 12.0f

// Linha lida da serial. Era um String: trocado por buffer fixo porque a
// realocacao a cada minuto fragmentava o heap (frag chegou a 9% em 30/08).
char linha[600];

int tentativa;
unsigned long Agora;
unsigned long ultimaTentativaMQTT = 0;
unsigned long intervaloMQTT = 5000;
unsigned long conexaoDesde = 0;   // quando a sessao MQTT atual foi estabelecida
unsigned long ultimaTentativaWifi = 0;
unsigned long ultimaVezComWifi = 0;
unsigned long ultimaConexaoOk = 0;
unsigned long ultimoStatus = 0;
unsigned long ultimoOTA = 0;
bool pedidoOTA = false;

// --- FILA DE LEITURAS EM FLASH ---
// A fila anterior vivia em RAM e guardava 6 leituras (6 minutos). Em 01/09 o
// broker ficou 2,6 h fora do ar e perdemos 151 dos 157 minutos. O build reserva
// 128 KB de sistema de arquivos que nunca foram usados: a 400 B por leitura,
// 80 KB guardam ~200 leituras, ou seja mais de 3 horas.
//
// LIMITACAO CONHECIDA: o JSON nao carrega horario (o ATMega nao tem RTC), e o
// Telegraf carimba a hora na chegada. As leituras recuperadas entram agrupadas
// no instante do envio. A ENERGIA fica correta, porque cada linha continua
// valendo 1 minuto no somatorio; o que distorce e o eixo do tempo no grafico.
#define SPOOL_ARQ  "/spool.jsonl"
#define SPOOL_POS  "/spool.pos"
#define SPOOL_MAX  80000UL

bool     fsOk      = false;
uint32_t spoolPos  = 0;    // offset da proxima leitura ainda nao enviada
uint32_t spoolFim  = 0;    // tamanho do arquivo
uint16_t spoolQtd  = 0;    // leituras pendentes (so para o diagnostico)
uint16_t spoolFalhas = 0;  // leituras que a fila recusou (perdidas)

// Contabilidade fechada de cada leitura lida da serial. A identidade
//     lidas == publicadas + enfileiradas + perdas
// tem de valer sempre. Se nao valer, a leitura sumiu num caminho que o codigo
// nao cobre; se lidas nem incrementa, o problema esta ANTES, na propria serial.
uint16_t contLidas = 0;
uint16_t contPub   = 0;
uint16_t contFila  = 0;

void spoolGravaPos(void) {
  if (!fsOk) return;
  File f = LittleFS.open(SPOOL_POS, "w");
  if (f) { f.print(spoolPos); f.close(); }
}

void spoolLimpa(void) {
  if (fsOk) { LittleFS.remove(SPOOL_ARQ); LittleFS.remove(SPOOL_POS); }
  spoolPos = 0; spoolFim = 0; spoolQtd = 0;
}

// Reconstroi o estado apos um reboot: sem isso um reset no meio de uma queda
// longa reenviaria tudo do inicio e duplicaria energia no banco.
void spoolCarrega(void) {
  if (!fsOk) return;
  File f = LittleFS.open(SPOOL_ARQ, "r");
  if (!f) { spoolLimpa(); return; }
  spoolFim = f.size();

  File p = LittleFS.open(SPOOL_POS, "r");
  if (p) { spoolPos = (uint32_t)p.parseInt(); p.close(); }
  if (spoolPos > spoolFim) spoolPos = spoolFim;

  spoolQtd = 0;
  f.seek(spoolPos);
  while (f.available()) { if (f.read() == '\n') spoolQtd++; }
  f.close();
  if (spoolPos >= spoolFim) spoolLimpa();
}

bool spoolAnexa(const char *s) {
  if (!fsOk || spoolFim >= SPOOL_MAX) { spoolFalhas++; return false; }
  File f = LittleFS.open(SPOOL_ARQ, "a");
  if (!f) { spoolFalhas++; return false; }
  size_t n = f.println(s);
  f.close();
  if (n == 0) return false;
  spoolFim += n;
  spoolQtd++;
  return true;
}

bool spoolPendente(void) { return fsOk && (spoolPos < spoolFim); }

void ConnectWifi(void);
void connectMQTT(void); 
void Callback(char* topic, byte* payload, unsigned int length);
void checkAndDownloadUpdate(void);
void otaExecuta(void);
void drenaSpool(void);

WiFiClient WifiClient;
PubSubClient client(BROKER, 1883, WifiClient);

// --- DIAGNOSTICO PERSISTENTE (1.0.23) ---
// Entre 20 e 23/09 o medidor travou sete vezes, com dois ESPs diferentes, e so
// voltou com corte de energia. Nada do que acontece durante a queda chega ao
// servidor, e o corte apaga a RAM. Este registro vai para a flash e e publicado
// quando o medidor volta, para distinguir: ESP parado, ESP vivo sem conseguir
// associar ao Wi-Fi (e com qual codigo), ou reiniciando em ciclo.
#define DIAG_ARQ  "/diag.log"
#define DIAG_OLD  "/diag.old"
#define DIAG_MAX  8000UL

uint16_t vccMin      = 65535;   // menor alimentacao (mV) desde o ultimo status
uint16_t vccMinBoot  = 65535;   // menor alimentacao (mV) desde o boot
unsigned long ultimaAmostraVcc = 0;
volatile uint16_t wifiDesc = 0; // desconexoes de Wi-Fi desde o ultimo registro
volatile uint8_t  wifiMotivo = 0;   // ultimo codigo 802.11 de desconexao
uint16_t wifiDescTotal = 0;
bool wifiEstavaOk = false;
bool mqttEstavaOk = false;
unsigned long ultimoDiagWifi = 0;
unsigned long semWifiDesde = 0;
bool diagPendente = true;       // ha registro na flash ainda nao publicado
uint32_t diagPos = 0;
WiFiEventHandler hDesc;

void diagRegistra(const char *evento) {
  if (!fsOk) return;
  File f = LittleFS.open(DIAG_ARQ, "a");
  if (!f) return;
  if (f.size() > DIAG_MAX) {            // gira: guarda o bloco anterior inteiro
    f.close();
    LittleFS.remove(DIAG_OLD);
    LittleFS.rename(DIAG_ARQ, DIAG_OLD);
    diagPos = 0;
    f = LittleFS.open(DIAG_ARQ, "a");
    if (!f) return;
  }
  time_t t = time(nullptr);
  f.printf("t=%lu up=%lu vcc=%u vmin=%u %s\n",
           (t > 1600000000L) ? (unsigned long)t : 0UL, millis() / 1000UL,
           (unsigned)ESP.getVcc(), (unsigned)vccMinBoot, evento);
  f.close();
  diagPendente = true;
}

// Publica o registro em lotes de 4 linhas por passagem do loop, como a fila de
// leituras, para nao travar a serial. Primeiro o bloco antigo, depois o atual.
void diagPublica(void) {
  if (!diagPendente || !fsOk || !client.connected()) return;
  if (conexaoDesde == 0 || (millis() - conexaoDesde) < 10000) return;
  const char *arq = LittleFS.exists(DIAG_OLD) ? DIAG_OLD : DIAG_ARQ;
  File f = LittleFS.open(arq, "r");
  if (!f) { diagPendente = LittleFS.exists(DIAG_ARQ); diagPos = 0; return; }
  f.seek(diagPos);
  char buf[200];
  memcpy(buf, "DIAG ", 5);
  uint8_t n = 0;
  while (n < 4 && f.available()) {
    size_t k = f.readBytesUntil('\n', buf + 5, sizeof(buf) - 6);
    buf[5 + k] = '\0';
    if (k == 0) continue;
    if (!client.publish(TOPIC_LOG, buf)) { f.close(); return; }
    diagPos = f.position();
    n++;
  }
  bool fim = !f.available();
  f.close();
  if (fim) {
    LittleFS.remove(arq);
    diagPos = 0;
    diagPendente = LittleFS.exists(DIAG_ARQ) || LittleFS.exists(DIAG_OLD);
  }
}


void setup() {
  // O buffer de RX padrao tem 256 bytes e o JSON do ATMega tem ~380. Qualquer
  // bloqueio do loop() (o client.connect() e bloqueante) destruia a leitura por
  // transbordo ANTES de ela ser lida - por isso a fila nunca era acionada:
  // fila=0 e perdas=0 com dados sumindo. Precisa vir antes do begin().
  Serial.setRxBufferSize(1024);
  Serial.begin(19200);
  Serial.setTimeout(250);
  
  // Linha em branco para limpar lixo do boot no Monitor Serial
  Serial.println();
  Serial.println("=== BOOT: MEDIDOR CASA ===");
  
  client.setBufferSize(640);
  // O PubSubClient so descobre um socket morto pelo keepalive: 60 s sem receber
  // nada dispara PINGREQ, outros 60 s sem PINGRESP encerram. Com keepAlive=60
  // isso davam ate 2 MINUTOS acreditando estar conectado, publicando no vazio -
  // medido em 02/09: 3 leituras perdidas com fila=0 e perdas=0.
  // Com 20 s a deteccao cai para ~40 s. Uma travada de 13 s (ja observada nesta
  // malha mesh) nao gera falso positivo, porque sao precisas duas janelas
  // consecutivas sem resposta. E um falso positivo agora e inofensivo: gera
  // reconexao e enfileiramento, nao perda.
  client.setKeepAlive(20);
  client.setSocketTimeout(5);   // era 20: cada connect() falho travava o loop 20 s
  client.setCallback(Callback);
  
  // Hora por NTP. As leituras recuperadas da fila chegam muito depois de terem
  // sido medidas, e o Telegraf carimba a hora na chegada. Com o campo ts a hora
  // real da medicao viaja junto com o dado.
  configTime(0, 0, "a.st1.ntp.br", "pool.ntp.org");

  // Monta a fila em flash. Se a area nunca foi formatada (caso do primeiro
  // boot apos o OTA), formata uma vez.
  fsOk = LittleFS.begin();
  if (!fsOk) {
    Serial.println("LittleFS: formatando pela primeira vez...");
    LittleFS.format();
    fsOk = LittleFS.begin();
  }
  if (fsOk) spoolCarrega();

  hDesc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &e) {
    wifiDesc++; wifiDescTotal++; wifiMotivo = (uint8_t)e.reason;   // so contadores: sem E/S aqui
  });
  {
    rst_info *ri = ESP.getResetInfoPtr();
    char ev[150];
    snprintf(ev, sizeof(ev), "BOOT motivo=%u(%s) exc=%u epc1=0x%08lx versao=%s",
             (unsigned)ri->reason, ESP.getResetReason().c_str(), (unsigned)ri->exccause,
             (unsigned long)ri->epc1, FIRMWARE_VERSION);
    diagRegistra(ev);
  }
  Serial.print("LittleFS: "); Serial.println(fsOk ? "ok" : "FALHOU");

  ConnectWifi();
  connectMQTT();

  // Relata o estado do sistema de arquivos pelo MQTT: sem isso o diagnostico
  // fica cego, porque o Serial do ESP vai para o ATMega e nao para o broker.
  {
    FSInfo info;
    bool temInfo = fsOk && LittleFS.info(info);
    char fsmsg[120];
    snprintf(fsmsg, sizeof(fsmsg), "FS ok=%d total=%u usado=%u blocos=%u",
             fsOk ? 1 : 0,
             (unsigned)(temInfo ? info.totalBytes : 0),
             (unsigned)(temInfo ? info.usedBytes  : 0),
             (unsigned)(temInfo ? info.blockSize  : 0));
    client.publish(TOPIC_LOG, fsmsg);
    Serial.println(fsmsg);
  }

  String msgBoot = "Medidor Iniciado. Versao: " + String(FIRMWARE_VERSION) +
                   " reset=" + ESP.getResetReason() + " vcc=" + String(ESP.getVcc()) + "mV";
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
    // Apenas marca. Executar HTTP aqui dentro significa abrir uma conexao nova
    // de dentro do laco de leitura do PubSubClient - reentrancia que o
    // ESP8266 nao tolera bem. O trabalho acontece no loop().
    Serial.println("Comando via MQTT: OTA agendado");
    client.publish(TOPIC_LOG, "OTA agendado para o proximo ciclo...");
    pedidoOTA = true;
  }
  // Repassa qualquer comando no formato Q...M. Antes so "QCALIB" passava, o que
  // barrava comandos novos de diagnostico antes de chegarem ao ATMega.
  else if (mensagem.startsWith("Q") && mensagem.endsWith("M")) {
    Serial.print(mensagem); 
    Serial.println("Aviso: Comando de Calibracao recebido e repassado!");
    client.publish(TOPIC_LOG, "Comando de Calibracao repassado ao ATMega...");
  }
  else {
    client.publish(TOPIC_LOG, "MEDIDOR_UFCG_OK");
  }
}

void loop() {
  // 1. A SERIAL E LIDA SEMPRE E PRIMEIRO. Se o loop bloquear aqui, o buffer de
  //    256 bytes do hardware transborda e a leitura do minuto some para sempre.
  if (Serial.available() > 0) {
    size_t n = Serial.readBytesUntil('\n', linha, sizeof(linha) - 1);
    linha[n] = '\0';
    while (n > 0 && (linha[n - 1] == '\r' || linha[n - 1] == ' ')) linha[--n] = '\0';
  }

  // 2. Wi-Fi: reconexao NAO bloqueante. A versao anterior chamava ConnectWifi(),
  //    que ficava ate 45 s em delay(1000) sem ler a serial. Era essa a origem
  //    das lacunas de 2 a 4 minutos.
  // RECUPERACAO ESCALONADA DE WI-FI
  // A maquina de estados do radio do ESP8266 as vezes trava: ele fica tentando
  // associar indefinidamente, com o LED piscando, e NEM ESP.restart() resolve.
  // So o corte de alimentacao limpa, porque zera o estado de RF. Desligar e
  // religar o radio por software reproduz esse efeito sem intervencao humana.
  // Observado em 15 e 16/09: o usuario teve de cortar a energia da placa oito
  // vezes para o medidor voltar.
  // Amostra a alimentacao 1x/s. Mais rapido que isso o ADC atrapalha o radio.
  if (millis() - ultimaAmostraVcc >= 1000) {
    ultimaAmostraVcc = millis();
    uint16_t v = ESP.getVcc();
    if (v < vccMin) vccMin = v;
    if (v < vccMinBoot) vccMinBoot = v;
  }

  // Transicoes de Wi-Fi vao para a flash. Durante uma queda, um resumo a cada
  // 1 min nos primeiros 10 min e depois a cada 5 min (poupa a flash).
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiEstavaOk && semWifiDesde) {
      char ev[90];
      snprintf(ev, sizeof(ev), "WIFI_VOLTOU apos=%lus rssi=%d", (millis() - semWifiDesde) / 1000UL, WiFi.RSSI());
      diagRegistra(ev);
    }
    wifiEstavaOk = true; semWifiDesde = 0;
  } else {
    if (wifiEstavaOk || semWifiDesde == 0) {
      semWifiDesde = millis(); ultimoDiagWifi = millis();
      char ev[70];
      snprintf(ev, sizeof(ev), "WIFI_CAIU motivo=%u status=%d", (unsigned)wifiMotivo, (int)WiFi.status());
      diagRegistra(ev);
      wifiDesc = 0;
    }
    wifiEstavaOk = false;
    unsigned long fora = millis() - semWifiDesde;
    if (millis() - ultimoDiagWifi >= (fora < 600000UL ? 60000UL : 300000UL)) {
      ultimoDiagWifi = millis();
      char ev[90];
      snprintf(ev, sizeof(ev), "WIFI_FORA ha=%lus tentativas_falhas=%u motivo=%u status=%d",
               fora / 1000UL, (unsigned)wifiDesc, (unsigned)wifiMotivo, (int)WiFi.status());
      diagRegistra(ev);
      wifiDesc = 0;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimaTentativaWifi > 15000) {
      ultimaTentativaWifi = millis();
      unsigned long semWifi = millis() - ultimaVezComWifi;

      if (semWifi > 300000UL) {
        // 5 min travado: ultimo recurso antes de depender de intervencao
        Serial.println("Wi-Fi travado ha 5 min. Reiniciando o modulo...");
        diagRegistra("REINICIO por 5 min sem Wi-Fi");
        delay(50);
        ESP.restart();
      } else if (semWifi > 60000UL) {
        // 1 min travado: religa o radio, equivalente por software ao corte
        Serial.println("Wi-Fi travado. Religando o radio...");
        if (semWifi < 76000UL) diagRegistra("RADIO religado apos 1 min sem Wi-Fi");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(250);
        WiFi.mode(WIFI_STA);
        WiFi.setOutputPower(WIFI_TX_DBM);
        WiFi.setSleepMode(WIFI_MODEM_SLEEP);
        WiFi.begin(SSID, PASSWORD);
      } else {
        WiFi.begin(SSID, PASSWORD);
      }
    }
  } else {
    ultimaVezComWifi = millis();
  }

  // 3. MQTT: so tenta com Wi-Fi de pe, e sem bloquear.
  if (!client.connected()) {
    // Recuo progressivo: 5 s, 10 s, 20 s ate o teto de 30 s. Cada tentativa
    // bloqueia o loop, entao insistir rapido custa leituras em vez de salva-las.
    if (WiFi.status() == WL_CONNECTED && (millis() - ultimaTentativaMQTT > intervaloMQTT)) {
      ultimaTentativaMQTT = millis();
      if (client.connect(CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
        client.subscribe(TOPIC_SUBSCRIBE);
        ultimaConexaoOk = millis();
        conexaoDesde = millis();
        intervaloMQTT = 5000;
      } else {
        intervaloMQTT = (intervaloMQTT < 30000) ? intervaloMQTT * 2 : 30000;
      }
    }
  } else {
    client.loop();
    ultimaConexaoOk = millis();
    drenaSpool();
    diagPublica();
  }
  if (client.connected()) {
    mqttEstavaOk = true;
  } else if (mqttEstavaOk) {
    mqttEstavaOk = false;
    char ev[80];
    snprintf(ev, sizeof(ev), "MQTT_CAIU estado=%d wifi=%d rssi=%d",
             client.state(), (int)WiFi.status(), WiFi.RSSI());
    diagRegistra(ev);
  }

  // 4. Salvaguarda: 15 min sem broker e sinal de travamento real.
  if (ultimaConexaoOk != 0 && (millis() - ultimaConexaoOk > 900000UL)) {
    Serial.println("15 min sem MQTT. Reiniciando...");
    diagRegistra("REINICIO por 15 min sem MQTT");
    delay(100);
    ESP.restart();
  }

  // 5. OTA roda AQUI, nunca dentro do callback do MQTT.
  if (pedidoOTA) {
    pedidoOTA = false;
    checkAndDownloadUpdate();
  }

  // 6. Diagnostico de rede a cada 60s
  if (client.connected() && (millis() - ultimoStatus > 60000)) {
    ultimoStatus = millis();
    char st[240];
    snprintf(st, sizeof(st), "RSSI=%ddBm tx=%.0fdBm vcc=%umV vmin=%umV vmin_boot=%umV wdesc=%u heap=%u frag=%u%% up=%lus fila=%u fs=%d perdas=%u lidas=%u pub=%u enf=%u",
             WiFi.RSSI(), (double)WIFI_TX_DBM, (unsigned)ESP.getVcc(), (unsigned)vccMin, (unsigned)vccMinBoot, (unsigned)wifiDescTotal,
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapFragmentation(),
             millis() / 1000UL, (unsigned)spoolQtd,
             fsOk ? 1 : 0, (unsigned)spoolFalhas,
             (unsigned)contLidas, (unsigned)contPub, (unsigned)contFila);
    client.publish(TOPIC_LOG, st);
    vccMin = 65535;

    // Sinal de vida para o cao de guarda do ATMega (versao A2): so e enviado
    // com o broker conectado. Se ele parar por 20 min, o ATMega aplica um reset
    // por hardware no pino RST do ESP. Um ATMega antigo descarta o comando.
    // O texto nao pode conter 'Q' nem 'M' no meio: e o delimitador do parser.
    Serial.print("QVIVOM");
  }

  // 7. Destino da leitura
  if (linha[0] != '\0') {
    if (linha[0] == '{') {
      contLidas++;

      // Acrescenta a identidade temporal ANTES de publicar ou enfileirar:
      //   up = segundos desde o boot. Se for menor que a lacuna, houve reboot,
      //        logo o medidor ficou sem alimentacao e o consumo real foi zero.
      //   ts = epoch da medicao. Vai junto com o dado, entao uma leitura
      //        recuperada horas depois ainda sabe quando foi medida.
      // O ts so entra se o NTP ja sincronizou, senao o campo e omitido e o
      // servidor continua usando a hora de chegada.
      char saida[640];
      time_t agora = time(nullptr);
      if (agora > 1600000000L) {
        snprintf(saida, sizeof(saida), "{\"up\":%lu,\"ts\":%lu,%s",
                 millis() / 1000UL, (unsigned long)agora, linha + 1);
      } else {
        snprintf(saida, sizeof(saida), "{\"up\":%lu,%s",
                 millis() / 1000UL, linha + 1);
      }

      // Com fila pendente, a nova leitura vai para o fim dela: preserva a ordem.
      if (client.connected() && !spoolPendente()) {
        if (client.publish(TOPIC_PUBLISH, saida)) contPub++;
        else if (spoolAnexa(saida)) contFila++;
      } else {
        if (spoolAnexa(saida)) contFila++;
      }
    } else if (client.connected()) {
      client.publish(TOPIC_LOG, linha);
    }
    linha[0] = '\0';
  }
}

// Esvazia a fila assim que o broker volta. Poucas por vez, para nao afogar o
// PubSubClient nem travar o loop enquanto o ATMega continua enviando.
void drenaSpool(void) {
  if (!spoolPendente() || !client.connected()) return;

  // Espera a sessao assentar antes de drenar. Uma conexao recem-feita pode
  // morrer em seguida, e o publish() devolve sucesso mesmo num socket morto:
  // o ponteiro avancaria e a leitura sairia da fila sem ter sido entregue.
  // Medido em 02/09: das 7 enfileiradas, 7 saíram da fila e so 3 chegaram.
  // Perder alguns segundos aqui e irrelevante; perder a leitura nao e.
  if (conexaoDesde == 0 || (millis() - conexaoDesde) < 10000) return;

  File f = LittleFS.open(SPOOL_ARQ, "r");
  if (!f) return;
  f.seek(spoolPos);

  char buf[600];
  uint8_t enviados = 0;
  while (enviados < 3 && f.available()) {
    size_t n = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
    buf[n] = '\0';
    while (n > 0 && buf[n - 1] == '\r') buf[--n] = '\0';
    if (n == 0) break;
    if (!client.publish(TOPIC_PUBLISH, buf)) break;
    // So avanca depois do publish confirmado: uma queda no meio da drenagem
    // retoma exatamente de onde parou, sem duplicar nem pular leitura.
    spoolPos = f.position();
    if (spoolQtd) spoolQtd--;
    enviados++;
    client.loop();
  }
  f.close();

  if (spoolPos >= spoolFim) spoolLimpa();
  else if (enviados) spoolGravaPos();
}

void ConnectWifi(void) {
  tentativa = 0;
  Serial.print("Conectando a rede Wi-Fi: ");
  Serial.println(SSID);

  // NAO usar WIFI_NONE_SLEEP nesta placa: o radio sempre ligado sobe o consumo
  // de ~15mA medios para ~70mA constantes. Medido em 29/08: sem ganho de cobertura.
  WiFi.mode(WIFI_STA);
  WiFi.setOutputPower(WIFI_TX_DBM);
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  WiFi.setAutoReconnect(true);

  // O SDK guarda canal e BSSID do ultimo AP em flash e tenta por ali primeiro.
  // Se o roteador muda de canal (modo automatico, ou mudanca manual), esse cache
  // fica obsoleto e a associacao falha em loop. Limpar forca varredura completa.
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);

  WiFi.begin(SSID , PASSWORD);
  
  while ((WiFi.status() != WL_CONNECTED) && (tentativa < 30)) {
    tentativa++;
    Serial.print("Tentativa de conexao Wi-Fi ");
    Serial.print(tentativa);
    Serial.println("/30...");
    delay(1000);
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    // Nao reinicia: apos uma queda de energia o roteador demora a subir, e
    // reiniciar em laco so perdia as leituras que a fila agora guarda.
    Serial.println("AVISO: Wi-Fi nao subiu no boot. O loop() segue tentando.");
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
  
  while ((!client.connected()) && ((millis() - Agora) < 20000)) {
    Serial.println("Tentativa de conexao MQTT...");
    if (client.connect(CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
      Serial.println("SUCESSO: Conectado ao broker MQTT!");
      client.subscribe(TOPIC_SUBSCRIBE);
      // Sem isto a sessao aberta no boot nunca era "assentada": drenaSpool() e
      // diagPublica() esperavam uma reconexao pelo loop(). Na pratica, as
      // leituras presas na fila depois de um corte de energia so desciam na
      // proxima queda de rede. Corrigido no 1.0.24.
      conexaoDesde = millis();
      Serial.print("Inscrito no topico: ");
      Serial.println(TOPIC_SUBSCRIBE);
    } else {
      Serial.print("Falha na conexao MQTT. Codigo de erro: ");
      Serial.print(client.state());
      Serial.println(" - Tentando novamente em 1s...");
      delay(1000);
    }
  }
  
  // Sem reset por timeout: quem cuida disso agora e a salvaguarda de 15 min
  // do loop(), que so dispara se a conexao nunca mais voltar.
  ultimaConexaoOk = millis();
}

void checkAndDownloadUpdate(void) {
  // Tentativas repetidas que falham nao liberam toda a memoria alocada.
  // Observado em 29/08: 10 tentativas seguidas derrubaram o heap em ~8 KB.
  if (ultimoOTA != 0 && (millis() - ultimoOTA) < 180000) {
    client.publish(TOPIC_LOG, "OTA ignorado: aguarde 3 min entre tentativas.");
    return;
  }
  ultimoOTA = millis();

  // O radio dormindo entre beacons faz o ponto de acesso enfileirar pacotes.
  // O MQTT tolera (mensagens minusculas, keepalive de 60 s); uma transferencia
  // de 300 KB nao. Radio sempre ligado apenas durante o OTA.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  delay(200);

  otaExecuta();

  // Restaurado em qualquer caminho de saida: sem isso o consumo medio sobe
  // de ~15 mA para ~70 mA constantes.
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  delay(50);
}

void otaExecuta(void) {
  WiFiClient updateClient;
  HTTPClient http;
  String serverVersion = "";
  int httpCode = 0;

  updateClient.setTimeout(20000);

  // A checagem de versao falhava de forma intermitente (HTTP=-11 em 29/08,
  // sucesso minutos depois sem nada mudar). Tres tentativas resolvem o caso
  // em que a primeira pega o radio saindo do sono.
  for (uint8_t t = 0; t < 3 && httpCode != HTTP_CODE_OK; t++) {
    if (t > 0) delay(1500);
    http.begin(updateClient, URL_VERSAO);
    http.setTimeout(15000);
    httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      serverVersion = http.getString();
      serverVersion.trim();
    }
    // Libera o WiFiClient ANTES do update: o ESPhttpUpdate precisa dele livre.
    http.end();
  }

  if (httpCode != HTTP_CODE_OK) {
    String erro = "Falha ao checar versao. HTTP=" + String(httpCode);
    Serial.println(erro);
    client.publish(TOPIC_LOG, erro.c_str());
    return;
  }

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
  // O ESPhttpUpdate tem timeout PROPRIO de 8 s (_httpClientTimeout) e sobrescreve
  // o do WiFiClient na linha 167 do .cpp. Foi o que matou o download do 1.0.14:
  // falhou em 12 s apesar dos 20 s que eu havia pedido no cliente. Numa malha
  // mesh uma travada de 8 s no meio de 344 KB e comum.
  ESPhttpUpdate.setClientTimeout(30000);
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
