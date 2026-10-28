/*
 * CP02 - Edge Computing & Computer Systems (FIAP 2026)
 * Projeto: Monitoramento da Vinheria Agnello - Fase 2
 *
 * Mede LUMINOSIDADE (LDR), TEMPERATURA e UMIDADE (DHT22) do ambiente onde
 * os vinhos ficam armazenados e sinaliza a situação:
 *   - LED verde    -> tudo OK
 *   - LED amarelo  -> alguma grandeza em nível de alerta   (buzzer 3 s)
 *   - LED vermelho -> alguma grandeza em nível crítico     (buzzer 3 s)
 *   - Display LCD I2C mostra os valores e o status de cada grandeza
 *   - RTC (DS1307) fornece data e hora
 *   - EEPROM guarda as configurações (fuso UTC e unidade) e os logs de anomalias
 *
 * Todos os valores exibidos são a MÉDIA das leituras dos últimos 10 segundos.
 *
 * Comandos pelo Monitor Serial (9600 baud):
 *   AJUDA      -> lista os comandos
 *   UTC=-3     -> define o fuso horário (de -12 a 14)
 *   UNIDADE=C  -> temperatura em Celsius  (UNIDADE=F para Fahrenheit)
 *   LOGS       -> mostra os registros gravados na EEPROM
 *   LIMPAR     -> apaga os registros
 *
 * Bibliotecas: LiquidCrystal I2C, DHT sensor library, RTClib
 */

#include <Wire.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <RTClib.h>

// ======================= PINOS =======================
const int PIN_LDR          = A0;   // saída analógica (AO) do módulo LDR
const int PIN_DHT          = 5;    // pino de dados do DHT22
const int PIN_LED_VERMELHO = 4;
const int PIN_LED_AMARELO  = 3;
const int PIN_LED_VERDE    = 2;
const int PIN_BUZZER       = 13;
// LCD e RTC usam o barramento I2C: SDA = A4, SCL = A5

// ===================== COMPONENTES ===================
LiquidCrystal_I2C lcd(0x27, 16, 2);   // endereço I2C 0x27, 16 colunas x 2 linhas
DHT dht(PIN_DHT, DHT22);              // no Wokwi usamos o DHT22 (mesmo uso do DHT11)
RTC_DS1307 rtc;

// ====================== ESTADOS ======================
const byte ESTADO_OK      = 0;
const byte ESTADO_ALERTA  = 1;
const byte ESTADO_CRITICO = 2;

// ============ CALIBRAÇÃO DO LDR (com map) ============
// Valores brutos do ADC (0-1023) medidos no Wokwi pelo Monitor Serial.
// No módulo LDR do Wokwi, quanto MAIS luz, MENOR a tensão na saída AO,
// por isso o "escuro" é o valor alto e o "claro" o valor baixo.
const int LDR_ESCURO = 1015;   // leitura com o mínimo de luz  -> 0%
const int LDR_CLARO  = 10;     // leitura com o máximo de luz  -> 100%
// Com esses valores: até 30% ~ até 30 lux (penumbra), 60% ~ 175 lux.

// ===================== LIMITES =======================
// Luminosidade (%): vinho deve ficar na penumbra
const int LUZ_ALERTA  = 30;    // acima de 30% -> alerta ("meia luz")
const int LUZ_CRITICO = 60;    // acima de 60% -> crítico ("muito claro")

// Temperatura (°C): ideal ~13 °C, variações > 3 °C prejudicam o vinho
const float TEMP_IDEAL      = 13.0;
const float TEMP_TOL_OK     = 3.0;   // 10 a 16 °C -> OK
const float TEMP_TOL_ALERTA = 6.0;   // 7 a 19 °C  -> alerta; fora disso -> crítico

// Umidade (%): ideal ~70%, aceitável de 60% a 80%
const float UMID_OK_MIN      = 60.0;
const float UMID_OK_MAX      = 80.0;
const float UMID_ALERTA_MIN  = 50.0; // 50-60% ou 80-90% -> alerta
const float UMID_ALERTA_MAX  = 90.0; // abaixo de 50% ou acima de 90% -> crítico

// ================== MÉDIA DE 10 s ====================
// Uma amostra por segundo; a média usa as últimas 10 (= últimos 10 s)
const int NUM_AMOSTRAS = 10;
const unsigned long INTERVALO_AMOSTRA = 1000;

float amostrasTemp[NUM_AMOSTRAS];
float amostrasUmid[NUM_AMOSTRAS];
int   amostrasLuz[NUM_AMOSTRAS];
int   indiceAmostra = 0;
int   totalAmostras = 0;     // quantas posições já foram preenchidas (até 10)
float ultimaTemp = 0, ultimaUmid = 0;   // última leitura válida do DHT

// Médias atuais (o que é exibido e avaliado)
float mediaTemp = 0;
float mediaUmid = 0;
int   mediaLuz  = 0;
int   ldrBruto  = 0;         // leitura bruta do ADC (para calibrar)

// ================ BUZZER (CP1) =======================
const unsigned long DURACAO_BUZZER   = 3000;   // toca por 3 s
const unsigned long INTERVALO_BUZZER = 10000;  // repete a cada 10 s
bool buzzerLigado = false;
unsigned long inicioBuzzer = 0;
unsigned long ultimoAlarme = 0;

// ==================== LCD ============================
const unsigned long TEMPO_TELA = 3000;   // troca de tela a cada 3 s
const int NUM_TELAS = 4;                 // data/hora, temperatura, umidade, luz

// ==================== EEPROM =========================
// Endereço 0: configurações | a partir do 16: logs (fila circular)
const byte ASSINATURA = 0xA7;            // indica que a EEPROM já foi configurada
const int  END_CONFIG = 0;
const int  END_LOGS   = 16;

struct Config {
  byte     assinatura;
  int8_t   utcOffset;      // fuso horário em horas (Brasília = -3)
  char     unidade;        // 'C' ou 'F'
  uint16_t proximoLog;     // posição onde o próximo log será gravado
  uint16_t totalLogs;      // quantos logs válidos existem
};

struct RegistroLog {       // 9 bytes por registro
  uint32_t timestamp;      // data/hora em UTC (segundos desde 1970)
  int16_t  temp10;         // temperatura x10 (ex.: 13.5 °C -> 135)
  uint8_t  umidade;        // %
  uint8_t  luz;            // %
  uint8_t  estado;         // 1 = alerta, 2 = crítico
};

const int MAX_LOGS = (E2END + 1 - END_LOGS) / sizeof(RegistroLog);
const unsigned long INTERVALO_LOG = 10000;   // grava no máx. 1 log a cada 10 s
unsigned long ultimoLog = 0;

Config config;

// ============ LOGO: taça enchendo de vinho ===========
// 5 quadros da animação: a taça vai do vazio ao cheio
byte quadroTaca[5][8] = {
  {B10001, B10001, B10001, B10001, B01010, B00100, B00100, B01110},
  {B10001, B10001, B10001, B11111, B01110, B00100, B00100, B01110},
  {B10001, B10001, B11111, B11111, B01110, B00100, B00100, B01110},
  {B10001, B11111, B11111, B11111, B01110, B00100, B00100, B01110},
  {B11111, B11111, B11111, B11111, B01110, B00100, B00100, B01110}
};

// ============ PROTÓTIPOS DAS FUNÇÕES ==================
void coletarAmostra();
void calcularMedias();
byte classificarLuz(int luz);
byte classificarTemp(float temp);
byte classificarUmid(float umid);
void atualizarLeds(byte estado);
void controlarBuzzer(bool deveTocar);
void imprimirLinha(int linha, const char* texto);
void formatarTemp(float tempC, char* saida);
void mostrarTela(int tela, byte geral, byte eLuz, byte eTemp, byte eUmid);
void animacaoLogo();
DateTime horaLocal();
void carregarConfig();
void salvarConfig();
void gravarLog(byte estado);
void mostrarLogs();
void lerComandoSerial();
void imprimirSerial();

// =====================================================
void setup() {
  Serial.begin(9600);
  Serial.setTimeout(50);

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARELO, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  lcd.init();
  lcd.backlight();
  dht.begin();

  if (!rtc.begin()) {
    Serial.println(F("RTC nao encontrado!"));
  }
  if (!rtc.isrunning()) {
    // RTC parado: acerta com a data/hora da compilação
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  carregarConfig();
  animacaoLogo();

  Serial.println(F("Vinheria Agnello - monitoramento iniciado. Digite AJUDA."));
}

void loop() {
  static unsigned long ultimaAmostra = 0;
  static unsigned long ultimaTela = 0;
  static int telaAtual = 0;

  // 1) A cada 1 s: coleta uma amostra e recalcula as médias dos últimos 10 s
  if (millis() - ultimaAmostra >= INTERVALO_AMOSTRA || totalAmostras == 0) {
    ultimaAmostra = millis();
    coletarAmostra();
    calcularMedias();
    imprimirSerial();
  }

  // 2) Classifica cada grandeza e define o estado geral (o pior dos três)
  byte estadoLuz  = classificarLuz(mediaLuz);
  byte estadoTemp = classificarTemp(mediaTemp);
  byte estadoUmid = classificarUmid(mediaUmid);
  byte estadoGeral = max(estadoLuz, max(estadoTemp, estadoUmid));

  // 3) Sinalização com LEDs e buzzer
  atualizarLeds(estadoGeral);
  controlarBuzzer(estadoGeral != ESTADO_OK);

  // 4) Grava log na EEPROM se algo estiver fora do ideal
  if (estadoGeral != ESTADO_OK && millis() - ultimoLog >= INTERVALO_LOG) {
    ultimoLog = millis();
    gravarLog(estadoGeral);
  }

  // 5) Alterna as telas do LCD a cada 3 s
  if (millis() - ultimaTela >= TEMPO_TELA) {
    ultimaTela = millis();
    telaAtual = (telaAtual + 1) % NUM_TELAS;
  }
  static unsigned long ultimoDesenho = 0;
  if (millis() - ultimoDesenho >= 500) {
    ultimoDesenho = millis();
    mostrarTela(telaAtual, estadoGeral, estadoLuz, estadoTemp, estadoUmid);
  }

  // 6) Comandos de configuração pelo Monitor Serial
  lerComandoSerial();
}

// ===================== LEITURAS ======================

// Lê o LDR (média rápida de 10 leituras, como no CP1) e o DHT22,
// e guarda o resultado no buffer circular de 10 amostras
void coletarAmostra() {
  long soma = 0;
  analogRead(PIN_LDR);                 // leitura descartada (estabiliza o ADC)
  for (int i = 0; i < 10; i++) {
    soma += analogRead(PIN_LDR);
    delay(2);
  }
  ldrBruto = soma / 10;

  // Calibração com map(): converte a faixa medida para 0-100%
  int luz = map(ldrBruto, LDR_ESCURO, LDR_CLARO, 0, 100);
  luz = constrain(luz, 0, 100);

  float t = dht.readTemperature();     // °C
  float u = dht.readHumidity();        // %
  // Se a leitura falhar (NaN), reaproveita a última leitura válida
  if (!isnan(t)) ultimaTemp = t;
  if (!isnan(u)) ultimaUmid = u;

  amostrasLuz[indiceAmostra]  = luz;
  amostrasTemp[indiceAmostra] = ultimaTemp;
  amostrasUmid[indiceAmostra] = ultimaUmid;

  indiceAmostra = (indiceAmostra + 1) % NUM_AMOSTRAS;
  if (totalAmostras < NUM_AMOSTRAS) totalAmostras++;
}

// Média das amostras guardadas (últimos 10 s)
void calcularMedias() {
  float somaT = 0, somaU = 0;
  long somaL = 0;
  for (int i = 0; i < totalAmostras; i++) {
    somaT += amostrasTemp[i];
    somaU += amostrasUmid[i];
    somaL += amostrasLuz[i];
  }
  mediaTemp = somaT / totalAmostras;
  mediaUmid = somaU / totalAmostras;
  mediaLuz  = somaL / totalAmostras;
}

// =================== CLASSIFICAÇÃO ===================

byte classificarLuz(int luz) {
  if (luz > LUZ_CRITICO) return ESTADO_CRITICO;
  if (luz > LUZ_ALERTA)  return ESTADO_ALERTA;
  return ESTADO_OK;
}

byte classificarTemp(float temp) {
  float diferenca = abs(temp - TEMP_IDEAL);
  if (diferenca > TEMP_TOL_ALERTA) return ESTADO_CRITICO;
  if (diferenca > TEMP_TOL_OK)     return ESTADO_ALERTA;
  return ESTADO_OK;
}

byte classificarUmid(float umid) {
  if (umid < UMID_ALERTA_MIN || umid > UMID_ALERTA_MAX) return ESTADO_CRITICO;
  if (umid < UMID_OK_MIN     || umid > UMID_OK_MAX)     return ESTADO_ALERTA;
  return ESTADO_OK;
}

// =================== SINALIZAÇÃO =====================

void atualizarLeds(byte estado) {
  digitalWrite(PIN_LED_VERDE,    estado == ESTADO_OK      ? HIGH : LOW);
  digitalWrite(PIN_LED_AMARELO,  estado == ESTADO_ALERTA  ? HIGH : LOW);
  digitalWrite(PIN_LED_VERMELHO, estado == ESTADO_CRITICO ? HIGH : LOW);
}

// Toca 3 s e repete a cada 10 s enquanto a condição continuar.
// Usa millis() em vez de delay() para não travar as leituras.
void controlarBuzzer(bool deveTocar) {
  unsigned long agora = millis();

  if (!deveTocar) {
    noTone(PIN_BUZZER);
    buzzerLigado = false;
    ultimoAlarme = 0;
    return;
  }
  if (!buzzerLigado && (ultimoAlarme == 0 || agora - ultimoAlarme >= INTERVALO_BUZZER)) {
    tone(PIN_BUZZER, 1000);
    buzzerLigado = true;
    inicioBuzzer = agora;
    ultimoAlarme = agora;
  }
  if (buzzerLigado && agora - inicioBuzzer >= DURACAO_BUZZER) {
    noTone(PIN_BUZZER);
    buzzerLigado = false;
  }
}

// ======================= LCD =========================

// Escreve uma linha inteira (completa com espaços para apagar sobras)
void imprimirLinha(int linha, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", texto);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

// Converte a temperatura para a unidade escolhida e gera o texto "13.5C"
void formatarTemp(float tempC, char* saida) {
  float valor = (config.unidade == 'F') ? tempC * 9.0 / 5.0 + 32.0 : tempC;
  char num[8];
  dtostrf(valor, 1, 1, num);
  sprintf(saida, "%s%c", num, config.unidade);
}

void mostrarTela(int tela, byte geral, byte eLuz, byte eTemp, byte eUmid) {
  char l0[17], l1[17], aux[10];

  switch (tela) {
    case 0: {  // Data, hora local e status geral
      DateTime agora = horaLocal();
      snprintf(l0, sizeof(l0), "%02d/%02d/%02d  %02d:%02d",
               agora.day(), agora.month(), agora.year() % 100,
               agora.hour(), agora.minute());
      const char* st = (geral == ESTADO_OK) ? "OK" : (geral == ESTADO_ALERTA) ? "ALERTA" : "CRITICO";
      snprintf(l1, sizeof(l1), "Status: %s", st);
      break;
    }
    case 1:    // Temperatura
      formatarTemp(mediaTemp, aux);
      if (eTemp == ESTADO_OK)        strcpy(l0, "Temperatura OK");
      else if (mediaTemp < TEMP_IDEAL) strcpy(l0, "Temp. BAIXA");
      else                           strcpy(l0, "Temp. ALTA");
      snprintf(l1, sizeof(l1), "Temp. = %s", aux);
      break;
    case 2:    // Umidade
      if (eUmid == ESTADO_OK)        strcpy(l0, "Umidade OK");
      else if (mediaUmid < UMID_OK_MIN) strcpy(l0, "Umidade BAIXA");
      else                           strcpy(l0, "Umidade ALTA");
      snprintf(l1, sizeof(l1), "Umidade = %d%%", (int)(mediaUmid + 0.5));
      break;
    default:   // Luminosidade
      if (eLuz == ESTADO_OK) {
        strcpy(l0, "Ambiente escuro");
        snprintf(l1, sizeof(l1), "Luz OK (%d%%)", mediaLuz);
      } else if (eLuz == ESTADO_ALERTA) {
        strcpy(l0, "Ambiente a meia");
        snprintf(l1, sizeof(l1), "luz (%d%%)", mediaLuz);
      } else {
        strcpy(l0, "Ambiente muito");
        snprintf(l1, sizeof(l1), "CLARO (%d%%)", mediaLuz);
      }
      break;
  }
  imprimirLinha(0, l0);
  imprimirLinha(1, l1);
}

// Animação de abertura: taças enchendo, nome digitado letra a letra e piscando
void animacaoLogo() {
  lcd.clear();

  // 1) As duas taças enchem de vinho
  for (int q = 0; q < 5; q++) {
    lcd.createChar(0, quadroTaca[q]);   // redesenha o caractere 0 com o novo quadro
    lcd.setCursor(0, 1);  lcd.write(byte(0));
    lcd.setCursor(15, 1); lcd.write(byte(0));
    delay(300);
  }

  // 2) Nome da empresa aparece letra por letra
  const char nome[] = "VINHERIA AGNELLO";
  for (int i = 0; i < 16; i++) {
    lcd.setCursor(i, 0);
    lcd.print(nome[i]);
    delay(80);
  }

  // 3) Mensagem de boas-vindas entre as taças
  lcd.setCursor(3, 1);
  lcd.print("Bem-vindo!");

  // 4) Nome pisca 3 vezes
  for (int i = 0; i < 3; i++) {
    delay(400);
    lcd.setCursor(0, 0); lcd.print("                ");
    delay(250);
    lcd.setCursor(0, 0); lcd.print(nome);
  }
  delay(1200);
  lcd.clear();
}

// ===================== RTC ===========================

// O RTC guarda a hora em UTC; somamos o fuso configurado
DateTime horaLocal() {
  return rtc.now() + TimeSpan((int32_t)config.utcOffset * 3600L);
}

// ==================== EEPROM =========================

void carregarConfig() {
  EEPROM.get(END_CONFIG, config);
  if (config.assinatura != ASSINATURA) {
    // Primeira execução: grava configurações padrão
    config.assinatura = ASSINATURA;
    config.utcOffset  = -3;      // horário de Brasília
    config.unidade    = 'C';
    config.proximoLog = 0;
    config.totalLogs  = 0;
    EEPROM.put(END_CONFIG, config);
  }
}

void salvarConfig() {
  EEPROM.put(END_CONFIG, config);
}

// Grava um registro na fila circular (quando enche, sobrescreve o mais antigo)
void gravarLog(byte estado) {
  RegistroLog reg;
  reg.timestamp = rtc.now().unixtime();
  reg.temp10    = (int16_t)(mediaTemp * 10);
  reg.umidade   = (uint8_t)(mediaUmid + 0.5);
  reg.luz       = (uint8_t)mediaLuz;
  reg.estado    = estado;

  EEPROM.put(END_LOGS + config.proximoLog * sizeof(RegistroLog), reg);
  config.proximoLog = (config.proximoLog + 1) % MAX_LOGS;
  if (config.totalLogs < MAX_LOGS) config.totalLogs++;
  salvarConfig();
}

void mostrarLogs() {
  Serial.print(F("=== LOGS NA EEPROM: "));
  Serial.print(config.totalLogs);
  Serial.println(F(" registro(s) ==="));

  // O mais antigo fica na posição "proximoLog" quando a fila já deu a volta
  int inicio = (config.totalLogs < MAX_LOGS) ? 0 : config.proximoLog;
  for (int i = 0; i < (int)config.totalLogs; i++) {
    RegistroLog reg;
    int pos = (inicio + i) % MAX_LOGS;
    EEPROM.get(END_LOGS + pos * sizeof(RegistroLog), reg);

    DateTime dt = DateTime(reg.timestamp) + TimeSpan((int32_t)config.utcOffset * 3600L);
    char linha[80];
    char temp[8];
    dtostrf(reg.temp10 / 10.0, 1, 1, temp);
    snprintf(linha, sizeof(linha), "%02d/%02d/%04d %02d:%02d:%02d | T=%sC | U=%d%% | L=%d%% | %s",
             dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute(), dt.second(),
             temp, reg.umidade, reg.luz, reg.estado == ESTADO_CRITICO ? "CRITICO" : "ALERTA");
    Serial.println(linha);
  }
}

// ================= MONITOR SERIAL ====================

void lerComandoSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "AJUDA") {
    Serial.println(F("Comandos: UTC=-3 | UNIDADE=C ou F | LOGS | LIMPAR"));
  } else if (cmd.startsWith("UTC=")) {
    int fuso = cmd.substring(4).toInt();
    if (fuso >= -12 && fuso <= 14) {
      config.utcOffset = fuso;
      salvarConfig();
      Serial.print(F("Fuso salvo na EEPROM: UTC"));
      Serial.println(fuso);
    } else {
      Serial.println(F("Fuso invalido (use de -12 a 14)"));
    }
  } else if (cmd == "UNIDADE=C" || cmd == "UNIDADE=F") {
    config.unidade = cmd.charAt(8);
    salvarConfig();
    Serial.print(F("Unidade salva na EEPROM: "));
    Serial.println(config.unidade);
  } else if (cmd == "LOGS") {
    mostrarLogs();
  } else if (cmd == "LIMPAR") {
    config.proximoLog = 0;
    config.totalLogs = 0;
    salvarConfig();
    Serial.println(F("Logs apagados."));
  } else {
    Serial.println(F("Comando desconhecido. Digite AJUDA."));
  }
}

// Linha de acompanhamento no Monitor Serial (1 por segundo)
void imprimirSerial() {
  DateTime agora = horaLocal();
  char hora[9];
  snprintf(hora, sizeof(hora), "%02d:%02d:%02d", agora.hour(), agora.minute(), agora.second());
  Serial.print(hora);
  Serial.print(F(" | LDR bruto: "));  Serial.print(ldrBruto);
  Serial.print(F(" | Luz: "));        Serial.print(mediaLuz);
  Serial.print(F("% | Temp: "));      Serial.print(mediaTemp, 1);
  Serial.print(F("C | Umid: "));      Serial.print(mediaUmid, 1);
  Serial.println(F("%  (medias de 10 s)"));
}
