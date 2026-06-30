/*
 * LIXEIRA INTELIGENTE IoT
 * ========================
 * Extensão do sistema de triagem automática com suporte a MQTT.
 * 
 * Dependências adicionais:
 *   - PubSubClient (by Nick O'Leary)  -> Gerenciador de Bibliotecas Arduino
 *   - WiFiNINA  (se usar Arduino Uno WiFi R2 / Nano 33 IoT)
 *     OU
 *   - Ethernet  (se usar shield Ethernet W5100/W5500)
 * 
 * Broker: broker.hivemq.com (público, porta 1883)
 * Tópicos publicados:
 *   lixeira/<ID>/status   -> JSON com todos os sensores (modo espera)
 *   lixeira/<ID>/triagem  -> JSON com resultado de cada triagem
 *   lixeira/<ID>/alerta   -> JSON com alertas de nível / travamento
 * 
 * Tópicos assinados:
 *   lixeira/<ID>/cmd      -> comandos remotos: "reset", "travar", "liberar"
 */

#include <Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Escolha uma das duas opções de conectividade ─────────────────────────────
#define USAR_WIFI      // Comente esta linha e descomente a de baixo para Ethernet
// #define USAR_ETHERNET

#ifdef USAR_WIFI
  #include <WiFiNINA.h>
  #include <PubSubClient.h>
  const char* WIFI_SSID     = "SEU_WIFI";
  const char* WIFI_SENHA    = "SUA_SENHA";
  WiFiClient redeCliente;
#endif

#ifdef USAR_ETHERNET
  #include <Ethernet.h>
  #include <PubSubClient.h>
  byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
  EthernetClient redeCliente;
#endif

// ── MQTT ──────────────────────────────────────────────────────────────────────
const char* MQTT_BROKER  = "broker.hivemq.com";
const int   MQTT_PORTA   = 1883;
const char* LIXEIRA_ID   = "lixeira_01"; // Mude se tiver múltiplas unidades

char topico_status[40];
char topico_triagem[40];
char topico_alerta[40];
char topico_cmd[40];

PubSubClient mqtt(redeCliente);

// ── DISPLAY OLED ──────────────────────────────────────────────────────────────
#define LARGURA_OLED 128
#define ALTURA_OLED  64
#define RESET_OLED   -1
Adafruit_SSD1306 display(LARGURA_OLED, ALTURA_OLED, &Wire, RESET_OLED);

// ── PINOS ────────────────────────────────────────────────────────────────────
const int trigger1 = 2;  const int eco1 = 3;  // Gatilho (entrada de lixo)
const int trigger2 = 4;  const int eco2 = 5;  // Nível compartimento DIR (Úmido)
const int trigger3 = 6;  const int eco3 = 7;  // Nível compartimento ESQ (Seco)
const int pinoUmidade    = A0;
const int pinoServo      = 9;

// ── LIMITES E CALIBRAÇÃO ──────────────────────────────────────────────────────
const float limiarUmidade           = 1;    // % mínimo para considerar "molhado"
const int   distanciaDeteccaoLixo  = 15;   // cm — gatilho de entrada
const int   ALTURA_COMPARTIMENTO   = 30;   // cm — altura interna do compartimento
const int   LIMITE_CHEIO_AVISO     = 75;   // % — exibe alerta amarelo
const int   LIMITE_CHEIO_TRAVA     = 95;   // % — trava a aleta, bloqueia entrada

Servo motorAleta;
int anguloBase = 90;
int anguloEsq  = 20;
int anguloDir  = 170;
int anguloAtual = 90;

// ── ESTADO GLOBAL ─────────────────────────────────────────────────────────────
bool  travado          = false;  // Trava remota ou por nível
int   totalTriagens    = 0;
int   totalUmidos      = 0;
int   totalSecos       = 0;
unsigned long ultimoPublish = 0;
const unsigned long INTERVALO_STATUS = 2000; // ms entre publicações de status

// ── PROTÓTIPOS ────────────────────────────────────────────────────────────────
long lerDistancia(int trig, int eco);
int  calcularNivel(long distancia);
void displayAviso(String l1, String l2, String l3, String l4);
void conectarMQTT();
void callbackMQTT(char* topico, byte* payload, unsigned int tamanho);
void publicarStatus(long distEntrada, int umidade, int nivelDir, int nivelEsq);
void publicarTriagem(bool umido, int umidade, int nivelDir, int nivelEsq);
void publicarAlerta(String tipo, String mensagem);
String construirJsonStatus(long distEntrada, int umidade, int nivelDir, int nivelEsq);
String construirJsonTriagem(bool umido, int umidade, int nivelDir, int nivelEsq);

// =============================================================================
void setup() {
  Serial.begin(9600);
  Serial.setTimeout(50);

  // Tópicos MQTT
  sprintf(topico_status,  "lixeira/%s/status",  LIXEIRA_ID);
  sprintf(topico_triagem, "lixeira/%s/triagem", LIXEIRA_ID);
  sprintf(topico_alerta,  "lixeira/%s/alerta",  LIXEIRA_ID);
  sprintf(topico_cmd,     "lixeira/%s/cmd",     LIXEIRA_ID);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha OLED"));
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  // Pinos
  pinMode(eco1, INPUT);    pinMode(trigger1, OUTPUT);
  pinMode(eco2, INPUT);    pinMode(trigger2, OUTPUT);
  pinMode(eco3, INPUT);    pinMode(trigger3, OUTPUT);
  pinMode(pinoUmidade, INPUT);

  // Servo
  motorAleta.attach(pinoServo);
  motorAleta.write(anguloBase);

  // Rede
  #ifdef USAR_WIFI
    display.setCursor(0,0); display.setTextSize(1);
    display.println("Conectando WiFi..."); display.display();
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
      delay(500); Serial.print("."); tentativas++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    }
  #endif
  #ifdef USAR_ETHERNET
    Ethernet.begin(mac);
    delay(1000);
  #endif

  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(callbackMQTT);
  conectarMQTT();

  delay(2000);
}

// =============================================================================
void loop() {
  // Mantém conexão MQTT ativa
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  // Controle manual via Serial (mantido do projeto original)
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      int novoAngulo = cmd.toInt();
      if (novoAngulo >= 0 && novoAngulo <= 180) {
        anguloAtual = novoAngulo;
        motorAleta.write(anguloAtual);
      }
    }
  }

  // ── Leitura dos sensores ───────────────────────────────────────────────────
  long distEntrada       = lerDistancia(trigger1, eco1);
  long distNivelDir      = lerDistancia(trigger2, eco2);
  long distNivelEsq      = lerDistancia(trigger3, eco3);
  int  valorBrutoUmidade = analogRead(pinoUmidade);
  int  umidade           = constrain(map(valorBrutoUmidade, 1010, 0, 0, 100), 0, 100);
  int  nivelDir          = calcularNivel(distNivelDir);
  int  nivelEsq          = calcularNivel(distNivelEsq);

  // ── Verificação de nível crítico ───────────────────────────────────────────
  bool compartimentoCheio = (nivelDir >= LIMITE_CHEIO_TRAVA || nivelEsq >= LIMITE_CHEIO_TRAVA);
  bool compartimentoAlerta = (nivelDir >= LIMITE_CHEIO_AVISO || nivelEsq >= LIMITE_CHEIO_AVISO);

  if (compartimentoCheio && !travado) {
    travado = true;
    String mensagem = "{\"tipo\":\"CHEIO_CRITICO\",\"msg\":\"Compartimento cheio! Aleta travada.\","
                      "\"nivelDir\":" + String(nivelDir) + ",\"nivelEsq\":" + String(nivelEsq) + "}";
    mqtt.publish(topico_alerta, mensagem.c_str(), true); // retained = true
    Serial.println("[ALERTA] Compartimento cheio - travado!");
  }

  if (!compartimentoCheio && travado) {
    // Esvaziado remotamente ou fisicamente — libera se não houve trava remota
    // (trava remota é limpa apenas via comando MQTT "liberar")
    // Aqui só libera se foi trava automática
  }

  // ── LÓGICA DE TRIAGEM ─────────────────────────────────────────────────────
  if (distEntrada > 0 && distEntrada <= distanciaDeteccaoLixo) {

    if (travado) {
      // Sistema travado — rejeita entrada e avisa
      displayAviso("CHEIO!", "Sistema", "travado.", "Esvazie!");
      mqtt.publish(topico_alerta,
        "{\"tipo\":\"ENTRADA_BLOQUEADA\",\"msg\":\"Tentativa de entrada bloqueada - sistema travado\"}");
      delay(3000);
      return;
    }

    displayAviso("OBJETO", "DETECTADO", "Lendo", "umidade...");
    delay(1500);

    valorBrutoUmidade = analogRead(pinoUmidade);
    umidade = constrain(map(valorBrutoUmidade, 1010, 0, 0, 100), 0, 100);
    String textoUmidade = "Umid: " + String(umidade) + "%";

    delay(1500);

    bool umido = (umidade >= limiarUmidade);
    totalTriagens++;

    if (umido) {
      totalUmidos++;
      displayAviso("UMIDO", "Descartando", "Lado Dir ->", textoUmidade);
      anguloAtual = anguloDir;
    } else {
      totalSecos++;
      displayAviso("SECO", "Descartando", "<- Lado Esq", textoUmidade);
      anguloAtual = anguloEsq;
    }
    motorAleta.write(anguloAtual);

    // Publica resultado da triagem
    publicarTriagem(umido, umidade, nivelDir, nivelEsq);

    delay(2500);
    anguloAtual = anguloBase;
    motorAleta.write(anguloAtual);

  } else {
    // ── MODO ESPERA ───────────────────────────────────────────────────────────
    // Monta display de depuração
    String statusTrava = travado ? " [TRAVADO]" : "";
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,  0); display.print("Servo: "); display.print(anguloAtual); display.print("graus"); display.print(statusTrava);
    display.setCursor(0, 13); display.print("Umidade: "); display.print(umidade); display.print("% ("); display.print(valorBrutoUmidade); display.print(")");
    display.setCursor(0, 26); display.print("Gatilho: "); display.print(distEntrada); display.print(" cm");
    display.setCursor(0, 39); display.print("Esq: "); display.print(nivelEsq); display.print("% | Dir: "); display.print(nivelDir); display.print("%");
    display.setCursor(0, 52); display.print("Total: "); display.print(totalTriagens); display.print(" | U:"); display.print(totalUmidos); display.print(" S:"); display.print(totalSecos);
    display.display();

    // Alerta visual no compartimento quase cheio
    if (compartimentoAlerta && !compartimentoCheio) {
      Serial.println("[AVISO] Nível alto - considere esvaziar.");
    }

    // Publica status periodicamente
    unsigned long agora = millis();
    if (agora - ultimoPublish >= INTERVALO_STATUS) {
      publicarStatus(distEntrada, umidade, nivelDir, nivelEsq);
      ultimoPublish = agora;

      Serial.print("Servo: "); Serial.print(anguloAtual); Serial.print("° | ");
      Serial.print("Umid: "); Serial.print(umidade); Serial.print("% | ");
      Serial.print("Gatilho: "); Serial.print(distEntrada); Serial.print("cm | ");
      Serial.print("Esq: "); Serial.print(nivelEsq); Serial.print("% | ");
      Serial.print("Dir: "); Serial.print(nivelDir); Serial.print("% | ");
      Serial.print("Triagens: "); Serial.println(totalTriagens);
    }

    delay(250);
  }
}

// =============================================================================
// FUNÇÕES AUXILIARES
// =============================================================================

long lerDistancia(int trig, int eco) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duracao = pulseIn(eco, HIGH, 30000);
  if (duracao == 0) return 999;
  return duracao / 58;
}

// Converte distância do sensor de nível em porcentagem de preenchimento
int calcularNivel(long distancia) {
  if (distancia >= 999) return 0; // Sensor sem retorno = vazio / erro
  // Quanto menor a distância, mais cheio está
  int nivel = map(distancia, ALTURA_COMPARTIMENTO, 2, 0, 100);
  return constrain(nivel, 0, 100);
}

void displayAviso(String l1, String l2, String l3, String l4) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);  display.println(l1);
  display.setTextSize(1);
  display.setCursor(0, 25); display.println(l2);
  display.setCursor(0, 40); display.println(l3);
  display.setCursor(0, 55); display.println(l4);
  display.display();
}

void conectarMQTT() {
  int tentativas = 0;
  while (!mqtt.connected() && tentativas < 5) {
    Serial.print("Conectando ao broker MQTT...");
    String clientId = String(LIXEIRA_ID) + "_" + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.println(" OK!");
      mqtt.subscribe(topico_cmd);
      // Publica mensagem de presença (retained)
      String msg = "{\"id\":\"" + String(LIXEIRA_ID) + "\",\"status\":\"online\"}";
      mqtt.publish((String("lixeira/") + LIXEIRA_ID + "/presenca").c_str(), msg.c_str(), true);
    } else {
      Serial.print(" Falha, rc="); Serial.println(mqtt.state());
      delay(3000);
      tentativas++;
    }
  }
}

// Recebe comandos remotos via MQTT
void callbackMQTT(char* topico, byte* payload, unsigned int tamanho) {
  String cmd = "";
  for (unsigned int i = 0; i < tamanho; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.print("[MQTT CMD] "); Serial.println(cmd);

  if (cmd == "travar") {
    travado = true;
    mqtt.publish(topico_alerta, "{\"tipo\":\"CMD_TRAVADO\",\"msg\":\"Sistema travado remotamente\"}");
    displayAviso("CMD", "REMOTO", "Sistema", "TRAVADO");
    delay(2000);

  } else if (cmd == "liberar") {
    travado = false;
    mqtt.publish(topico_alerta, "{\"tipo\":\"CMD_LIBERADO\",\"msg\":\"Sistema liberado remotamente\"}");
    displayAviso("CMD", "REMOTO", "Sistema", "LIBERADO");
    delay(2000);

  } else if (cmd == "reset") {
    totalTriagens = 0;
    totalUmidos   = 0;
    totalSecos    = 0;
    mqtt.publish(topico_alerta, "{\"tipo\":\"RESET\",\"msg\":\"Contadores zerados via comando\"}");
    Serial.println("[CMD] Contadores zerados.");
  }
}

void publicarStatus(long distEntrada, int umidade, int nivelDir, int nivelEsq) {
  String json = "{";
  json += "\"id\":\"" + String(LIXEIRA_ID) + "\",";
  json += "\"travado\":" + String(travado ? "true" : "false") + ",";
  json += "\"umidade\":" + String(umidade) + ",";
  json += "\"gatilho_cm\":" + String(distEntrada) + ",";
  json += "\"nivel_dir_pct\":" + String(nivelDir) + ",";
  json += "\"nivel_esq_pct\":" + String(nivelEsq) + ",";
  json += "\"servo_graus\":" + String(anguloAtual) + ",";
  json += "\"total_triagens\":" + String(totalTriagens) + ",";
  json += "\"total_umidos\":" + String(totalUmidos) + ",";
  json += "\"total_secos\":" + String(totalSecos);
  json += "}";
  mqtt.publish(topico_status, json.c_str());
}

void publicarTriagem(bool umido, int umidade, int nivelDir, int nivelEsq) {
  String json = "{";
  json += "\"id\":\"" + String(LIXEIRA_ID) + "\",";
  json += "\"tipo\":\"" + String(umido ? "UMIDO" : "SECO") + "\",";
  json += "\"umidade_pct\":" + String(umidade) + ",";
  json += "\"nivel_dir_pct\":" + String(nivelDir) + ",";
  json += "\"nivel_esq_pct\":" + String(nivelEsq) + ",";
  json += "\"total_triagens\":" + String(totalTriagens) + ",";
  json += "\"total_umidos\":" + String(totalUmidos) + ",";
  json += "\"total_secos\":" + String(totalSecos);
  json += "}";
  mqtt.publish(topico_triagem, json.c_str());
}
