#include <avr/wdt.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

#define M90_CHIP_SELECT 10

// --- NOVOS ENDEREÇOS NA EEPROM (Para as 3 Fases separadas) ---
#define ADDR_GAIN_UA 0
#define ADDR_GAIN_IA 2
#define ADDR_GAIN_UB 4
#define ADDR_GAIN_IB 6
#define ADDR_GAIN_UC 8
#define ADDR_GAIN_IC 10

// --- CONSTANTES ORIGINAIS ---
#define N 10
#define FACTOR 0.1
#define URES 0.0001633005       
#define IRES 0.00000390625      
#define POWRES 0.01633005       
#define TOTALPOWERS 0.06532021  
#define PFRES 0.001
#define H_RES 0.006103515625
#define UF_RES 0.032656
#define IF_RES 0.0032656
#define F_RES 0.01

#define CALIBRATE_VOLTAGE 0
#define CALIBRATE_CURRENT 0
#define START 1
#define END 0

enum CHANNELS {UA, UB, UC, IA = 4, IB, IC, TOTAL = -1};
enum CHECKSUM_NUMBER {CONFIG, ENERGY, FUNDAMENTAL_HARMONIC, MEASUREMENT};

float P1 = 0, P2 = 0, P3 = 0, Q1 = 0, Q2 = 0, Q3 = 0;
float FPA = 0, FPB = 0, FPC = 0;
float V_A = 0, V_B = 0, V_C = 0;       
float I_A = 0, I_B = 0, I_C = 0, I_N = 0;                         
float FREQ = 0;                        
float S1 = 0, S2 = 0, S3 = 0;          

char c;
int N_Leituras = 0, entrada = 0, contador = 0;
String COMANDO = ""; 
unsigned long Agora;

uint16_t read16(uint16_t);
void write16(uint16_t, uint16_t);
bool pmicSetup();
uint16_t csCalculator(byte, byte);
bool checksumStatus(int);
uint16_t measurementGainCalibration(int, float);
bool measurementCalibration(bool) ;
bool energyCalibration(bool);
float frequency();
uint32_t rms(int8_t);
float realRms(int8_t);
float neutralCurrentRms();
float activePower(int8_t);
float reactivePower(int8_t);
float aparentPower(int8_t);
float powerFactor(int8_t);
int32_t twosComplementToDecimal(uint32_t, const int);
int32_t powOfTwo(int8_t);

SoftwareSerial ESP8266(2, 3); // RX, TX do ESP8266

void setup() {
  pinMode(5, OUTPUT);
  digitalWrite(5, LOW); delay(1000);
  digitalWrite(5, HIGH); delay(1000);

  Serial.begin(19200);
  ESP8266.begin(19200);

  pinMode(M90_CHIP_SELECT, OUTPUT);
  digitalWrite(M90_CHIP_SELECT, HIGH);

  while (!pmicSetup()) {
    delay(1000);
  }

  measurementCalibration(START);

  // --- CARREGANDO OS 6 GANHOS DA EEPROM ---
  uint16_t gain_UA, gain_IA, gain_UB, gain_IB, gain_UC, gain_IC;
  EEPROM.get(ADDR_GAIN_UA, gain_UA); EEPROM.get(ADDR_GAIN_IA, gain_IA);
  EEPROM.get(ADDR_GAIN_UB, gain_UB); EEPROM.get(ADDR_GAIN_IB, gain_IB);
  EEPROM.get(ADDR_GAIN_UC, gain_UC); EEPROM.get(ADDR_GAIN_IC, gain_IC);

  // Validação e carregamento de padrão de fábrica se a memória estiver vazia
  if (gain_UA == 0xFFFF || gain_UA == 0) gain_UA = 47751;
  if (gain_IA == 0xFFFF || gain_IA == 0) gain_IA = 12890;
  if (gain_UB == 0xFFFF || gain_UB == 0) gain_UB = 47751;
  if (gain_IB == 0xFFFF || gain_IB == 0) gain_IB = 12890;
  if (gain_UC == 0xFFFF || gain_UC == 0) gain_UC = 47751;
  if (gain_IC == 0xFFFF || gain_IC == 0) gain_IC = 12890;

  // Injetando ganhos individualmente no chip metrológico
  write16(0x61, gain_UA); write16(0x65, gain_UB); write16(0x69, gain_UC);
  write16(0x62, gain_IA); write16(0x66, gain_IB); write16(0x6A, gain_IC);
  write16(0x6D, 0);       

  measurementCalibration(END);

  energyCalibration(START);
  write16(0x48, 246); 
  write16(0x4A, 254); 
  write16(0x4C, 284); 
  energyCalibration(END);

  delay(1000);
  Agora = millis();
}

void loop() {
  wdt_enable(WDTO_1S);   // 250ms era apertado: o JSON leva ~160ms a 19200 baud

  // --- LEITURA DE COMANDOS DO ESP8266 ---
  while (ESP8266.available() > 0) {
    c = ESP8266.read();
    // 'Q' inicia um comando novo: limpa o buffer para nao concatenar lixo anterior
    if (c == 'Q') { COMANDO = ""; contador = 1; }
    else if (c == 'M' && contador == 1) { COMANDO += c; contador = 2; break; }
    else if (contador == 1) {
      if (COMANDO.length() < 40) COMANDO += c;
      else { COMANDO = ""; contador = 0; }   // sem terminador: descarta
    }
  }

  if (contador == 2) {
    if (COMANDO == "QRESETM") {
      delay(500); // Força Watchdog
    }
    
    // ====================================================================
    // 1. MODO MONOFÁSICO (Calibra Fase A e Clona para B e C automaticamente)
    // ====================================================================
    else if (COMANDO.startsWith("QCALIB_V_ALL:")) {
      float ref = COMANDO.substring(13, COMANDO.length() - 1).toFloat();
      wdt_disable(); 
      uint16_t novoGanho = measurementGainCalibration(UA, ref);
      EEPROM.put(ADDR_GAIN_UA, novoGanho); EEPROM.put(ADDR_GAIN_UB, novoGanho); EEPROM.put(ADDR_GAIN_UC, novoGanho);
      write16(0x61, novoGanho); write16(0x65, novoGanho); write16(0x69, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao Tensao ALL Salva!\"}");
      wdt_enable(WDTO_1S); 
    }
    else if (COMANDO.startsWith("QCALIB_I_ALL:")) {
      float ref = COMANDO.substring(13, COMANDO.length() - 1).toFloat();
      wdt_disable(); 
      uint16_t novoGanho = measurementGainCalibration(IA, ref);
      EEPROM.put(ADDR_GAIN_IA, novoGanho); EEPROM.put(ADDR_GAIN_IB, novoGanho); EEPROM.put(ADDR_GAIN_IC, novoGanho);
      write16(0x62, novoGanho); write16(0x66, novoGanho); write16(0x6A, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao Corrente ALL Salva!\"}");
      wdt_enable(WDTO_1S); 
    }
    
    // ====================================================================
    // 2. MODO TRIFÁSICO INDIVIDUAL (Calibra Fase por Fase)
    // ====================================================================
    
    // --- TENSÃO (VA, VB, VC) ---
    else if (COMANDO.startsWith("QCALIB_VA:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(UA, ref);
      EEPROM.put(ADDR_GAIN_UA, novoGanho); write16(0x61, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao VA Salva!\"}"); wdt_enable(WDTO_1S); 
    }
    else if (COMANDO.startsWith("QCALIB_VB:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(UB, ref);
      EEPROM.put(ADDR_GAIN_UB, novoGanho); write16(0x65, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao VB Salva!\"}"); wdt_enable(WDTO_1S); 
    }
    else if (COMANDO.startsWith("QCALIB_VC:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(UC, ref);
      EEPROM.put(ADDR_GAIN_UC, novoGanho); write16(0x69, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao VC Salva!\"}"); wdt_enable(WDTO_1S); 
    }

    // --- CORRENTE (IA, IB, IC) ---
    else if (COMANDO.startsWith("QCALIB_IA:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(IA, ref);
      EEPROM.put(ADDR_GAIN_IA, novoGanho); write16(0x62, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao IA Salva!\"}"); wdt_enable(WDTO_1S); 
    }
    else if (COMANDO.startsWith("QCALIB_IB:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(IB, ref);
      EEPROM.put(ADDR_GAIN_IB, novoGanho); write16(0x66, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao IB Salva!\"}"); wdt_enable(WDTO_1S); 
    }
    else if (COMANDO.startsWith("QCALIB_IC:")) {
      float ref = COMANDO.substring(10, COMANDO.length() - 1).toFloat();
      wdt_disable(); uint16_t novoGanho = measurementGainCalibration(IC, ref);
      EEPROM.put(ADDR_GAIN_IC, novoGanho); write16(0x6A, novoGanho);
      ESP8266.println("{\"INFO\":\"Calibracao IC Salva!\"}"); wdt_enable(WDTO_1S); 
    }
    
    contador = 0; COMANDO = "";
  }

  entrada = 0;

  // --- ACÚMULO DE DADOS DENTRO DE 1 MINUTO (FIXO) ---
  if ((millis() - Agora) < 60000) {
    P1 += activePower(UA); P2 += activePower(UB); P3 += activePower(UC);
    Q1 += reactivePower(UA); Q2 += reactivePower(UB); Q3 += reactivePower(UC);
    FPA += powerFactor(UA); FPB += powerFactor(UB); FPC += powerFactor(UC);
    
    V_A += realRms(UA); V_B += realRms(UB); V_C += realRms(UC);
    I_A += realRms(IA); I_B += realRms(IB); I_C += realRms(IC);
    I_N += neutralCurrentRms();
    FREQ += frequency();
    S1 += aparentPower(UA); S2 += aparentPower(UB); S3 += aparentPower(UC);
    
    N_Leituras++; entrada = 1;
  }

  // --- FECHAMENTO DO MINUTO E ENVIO DO JSON ---
  if (entrada == 0) {
    Agora = millis();

    if (N_Leituras < 1) N_Leituras = 1;   // evita divisao por zero (geraria NaN no JSON)

    P1 /= N_Leituras; P2 /= N_Leituras; P3 /= N_Leituras;
    Q1 /= N_Leituras; Q2 /= N_Leituras; Q3 /= N_Leituras;
    FPA /= N_Leituras; FPB /= N_Leituras; FPC /= N_Leituras;
    V_A /= N_Leituras; V_B /= N_Leituras; V_C /= N_Leituras;
    I_A /= N_Leituras; I_B /= N_Leituras; I_C /= N_Leituras;
    I_N /= N_Leituras; FREQ /= N_Leituras;
    S1 /= N_Leituras; S2 /= N_Leituras; S3 /= N_Leituras;

    // FASE A
    if (V_A < 5.0) { V_A = 0; I_A = 0; P1 = 0; Q1 = 0; S1 = 0; FPA = 0; } 
    else if (I_A < 0.05) { I_A = 0; P1 = 0; Q1 = 0; S1 = 0; FPA = 0; }

    // FASE B
    if (V_B < 5.0) { V_B = 0; I_B = 0; P2 = 0; Q2 = 0; S2 = 0; FPB = 0; } 
    else if (I_B < 0.05) { I_B = 0; P2 = 0; Q2 = 0; S2 = 0; FPB = 0; }

    // FASE C
    if (V_C < 5.0) { V_C = 0; I_C = 0; P3 = 0; Q3 = 0; S3 = 0; FPC = 0; } 
    else if (I_C < 0.05) { I_C = 0; P3 = 0; Q3 = 0; S3 = 0; FPC = 0; }

    if (V_A == 0 && V_B == 0 && V_C == 0) FREQ = 0;

    StaticJsonDocument<512> doc;
    doc["ID"] = "MEDIDOR_UFCG";  // CASA. O do laboratorio usa MEDIDOR_UFCG_LABMET.
    doc["P1"] = P1; doc["P2"] = P2; doc["P3"] = P3;
    doc["Q1"] = Q1; doc["Q2"] = Q2; doc["Q3"] = Q3;
    doc["FPA"] = FPA; doc["FPB"] = FPB; doc["FPC"] = FPC;
    doc["VA"] = V_A; doc["VB"] = V_B; doc["VC"] = V_C;
    doc["IA"] = I_A; doc["IB"] = I_B; doc["IC"] = I_C;
    doc["IN"] = I_N; doc["FREQ"] = FREQ;
    doc["S1"] = S1; doc["S2"] = S2; doc["S3"] = S3;

    serializeJson(doc, ESP8266);
    ESP8266.println();

    P1 = P2 = P3 = Q1 = Q2 = Q3 = FPA = FPB = FPC = 0;
    V_A = V_B = V_C = I_A = I_B = I_C = I_N = FREQ = S1 = S2 = S3 = 0;
    N_Leituras = 0;
  }
  wdt_reset(); 
}

// ***************** FUNÇÕES SECUNDÁRIAS DO ATM90E36A (INALTERADAS) *****************

uint16_t read16(uint16_t address) {
  uint16_t readData;
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
  digitalWrite(M90_CHIP_SELECT, LOW);
  SPI.transfer16(0x8000 | address);
  readData = SPI.transfer16(0x0000);
  digitalWrite(M90_CHIP_SELECT, HIGH);
  SPI.endTransaction();
  return readData;
}

void write16(uint16_t address, uint16_t data) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
  digitalWrite(M90_CHIP_SELECT, LOW);
  SPI.transfer16(address);
  SPI.transfer16(data);
  digitalWrite(M90_CHIP_SELECT, HIGH);
  SPI.endTransaction();
}

bool pmicSetup() {
  SPI.begin();
  pinMode(M90_CHIP_SELECT, OUTPUT);
  digitalWrite(M90_CHIP_SELECT, HIGH);
  write16(0x00, 0x789A); delay(100);
  write16(0x30, 0x5678); write16(0x33, 0x1087);
  write16(0x3B, csCalculator(0x31, 0x3A));
  write16(0x30, 0x8765); delay(1000);
  return !checksumStatus(CONFIG);
}

uint16_t measurementGainCalibration(int channel, float ref) {
  uint8_t i; 
  uint32_t calc_gain; 
  float meas = 0;
  
  if (channel <= UC) {
    write16(0x61 + channel * 4, 52800);
  } else {
    write16(0x62 + (channel - 4) * 4, 30000);
  }
  delay(200); 

  for (i = 0; i < N; i++) {
    meas += realRms(channel);
    wdt_reset();
    delay(500);
    wdt_reset();
  }
  meas = meas / N;

  if (meas < 0.1) return (channel <= UC) ? 52800 : 30000;

  if (channel <= UC) {
    calc_gain = (uint32_t)(52800 * (ref / meas));
  } else {
    calc_gain = (uint32_t)(30000 * (ref / meas));
  }

  if (calc_gain > 65000) calc_gain = 65000;

  return (uint16_t)calc_gain;
}

bool measurementCalibration(bool opt) {
  switch (opt) {
    case 1: write16(0x60, 0x5678); return 1;
    case 0: write16(0x6F, csCalculator(0x61, 0x6E)); write16(0x60, 0x8765); return !checksumStatus(MEASUREMENT);
  }
  return 0;
}

bool energyCalibration(bool opt) {
  switch (opt) {
    case 1: write16(0x40, 0x5678); return 1;
    case 0: write16(0x4D, csCalculator(0x41, 0x4C)); write16(0x40, 0x8765); return !checksumStatus(ENERGY);
  }
  return 0;
}

uint16_t csCalculator(byte startRegister, byte endRegister) {
  int i; byte lowCsByte = 0, highCsByte = 0; uint16_t aux;
  for (i = startRegister; i <= endRegister; i++) {
    aux = read16(i);
    lowCsByte += (byte)(aux >> 8) + (byte)(aux & (0x00FF));
    highCsByte = highCsByte ^ (byte)(aux >> 8) ^ (byte)(aux & (0x00FF));
  }
  return ((uint16_t)lowCsByte) + (((uint16_t)highCsByte) << 8);
}

bool checksumStatus(int number) {
  switch (number) {
    case CONFIG: return bitRead(read16(0x01), 14);
    case ENERGY: return bitRead(read16(0x01), 12);
    case MEASUREMENT: return bitRead(read16(0x01), 8);
  }
  return 0;
}

float frequency() { return read16(0xF8) * F_RES; }
uint32_t rms(int8_t channel) { return (((((uint32_t)read16(0xD9 + channel)) << 8)) + ((read16(0xE9 + channel) >> 8))); }
float neutralCurrentRms() { return (((uint32_t)read16(0xDC)) << 8) * IRES / FACTOR; }

float realRms(int8_t channel) {
  switch (channel) {
    case UA: case UB: case UC: return rms(channel) * URES;
    case IA: case IB: case IC: return rms(channel) * IRES / FACTOR;
  }
  return 0;
}

float activePower(int8_t channel) {
  uint32_t readData = (((uint32_t)read16(0xB0 + channel + 1)) << 8) + (read16(0xC0 + channel + 1) >> 8);
  float ap = twosComplementToDecimal(readData, 24);
  return (channel == TOTAL) ? (ap * TOTALPOWERS / FACTOR) : (ap * POWRES / FACTOR);
}

float reactivePower(int8_t channel) {
  uint32_t readData = (((uint32_t)read16(0xB4 + channel + 1)) << 8) + (read16(0xC4 + channel + 1) >> 8);
  float rp = twosComplementToDecimal(readData, 24);
  return (channel == TOTAL) ? (rp * TOTALPOWERS / FACTOR) : (rp * POWRES / FACTOR);
}

float aparentPower(int8_t channel) {
  float ap = (((uint32_t)read16(0xB8 + channel + 1)) << 8) + (read16(0xC8 + channel + 1) >> 8);
  return (channel == TOTAL) ? (ap * TOTALPOWERS / FACTOR) : (ap * POWRES / FACTOR);
}

float powerFactor(int8_t channel) { return twosComplementToDecimal(read16(0xBC + channel + 1), 16) * PFRES; }

int32_t twosComplementToDecimal(uint32_t complement, const int Nbits) {
  int32_t output = -1;
  output *= (int32_t)bitRead(complement, Nbits - 1);
  output *= powOfTwo(Nbits - 1);
  for (int8_t i = 0; i <= Nbits - 2; i++) {
    output += ((int32_t)bitRead(complement, i)) * powOfTwo(i);
  }
  return output;
}

int32_t powOfTwo(int8_t expoent) { return ((int32_t)1) << expoent; }