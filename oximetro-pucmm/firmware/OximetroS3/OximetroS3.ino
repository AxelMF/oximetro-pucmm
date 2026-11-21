// ===========================================================
// OximetroS3.ino  -  Fase 2: algoritmo propio + Firebase
// ESP32-S3 + MAX30101 (SparkFun MAX3010x library)
//
// Archivos del sketch (misma carpeta "OximetroS3"):
//   OximetroS3.ino     este archivo (sensor, nucleo 1)
//   ppg_algoritmo.h    BPM / SpO2
//   red.h              WiFi + Firebase (tarea en nucleo 0)
//   firebase_rtdb.h    cliente REST de Realtime Database
//   config.h           credenciales y periodos de envio
//
// Comandos por el Monitor Serie (enviar una letra):
//   m  -> modo MONITOR  (texto legible, 1 linea por segundo)
//   p  -> modo PLOTTER  (ondas PPG para el Serial Plotter)
//   c  -> modo CALIBRACION (R crudo por latido, CSV)
//
// AVISO: uso experimental/educativo. SpO2 sin calibrar.
// ===========================================================

#include <Wire.h>
#include "MAX30105.h"
#include "ppg_algoritmo.h"
#include "red.h"

// -----------------------------------------------------------
// PINES I2C
// -----------------------------------------------------------
// ATENCION ESP32-S3: GPIO19/GPIO20 son USB D-/D+.
// Si conectas la placa por el puerto USB nativo ("USB" y no
// "UART/COM") usar GPIO20 como SDA puede cortar el Serial.
// Recomendado: SDA = 8, SCL = 9 (I2C por defecto del S3).
// Se mantienen 20/21 para no obligarte a recablear.
#define SDA_PIN 20
#define SCL_PIN 21

// -----------------------------------------------------------
// CONFIGURACION DEL SENSOR
// -----------------------------------------------------------
// 400 muestras/s con promedio de 4 -> 100 Hz efectivos.
const byte  LED_BRILLO      = 0x1F;  // ~6.4 mA
const byte  PROMEDIO        = 4;
const byte  MODO_LED        = 2;     // rojo + IR
const int   TASA_MUESTREO   = 400;
const int   ANCHO_PULSO     = 411;   // 18 bits
const int   RANGO_ADC       = 4096;
const float FS              = (float)TASA_MUESTREO / PROMEDIO;

// -----------------------------------------------------------
// SALIDA SERIE
// -----------------------------------------------------------
enum ModoSalida { MONITOR, PLOTTER, CALIBRACION };
ModoSalida modo = MONITOR;

const uint8_t  DIEZMADO_PLOTTER = 2;     // 100 Hz / 2 = 50 Hz al plotter
const uint32_t PERIODO_MONITOR  = 1000;  // ms

MAX30105 sensor;
ppg::AlgoritmoPPG algoritmo;

uint32_t ultimoReporte = 0;
uint32_t ultimoActual = 0;
uint32_t ultimoSegundo = 0;
uint8_t  cuentaPlotter = 0;

// -----------------------------------------------------------
// MEDICION
// -----------------------------------------------------------

Medicion obtenerMedicion() {
  const ppg::Resultado& r = algoritmo.resultado();
  Medicion m;
  m.uptimeMs = millis();
  m.estado = r.estado;
  m.bpm = r.bpmValido ? r.bpm : NAN;
  m.spo2 = r.spo2Valido ? r.spo2 : NAN;
  m.ratioR = r.ratioR;
  m.perfusion = r.perfusion;
  m.calidad = r.calidad;
  return m;
}

// -----------------------------------------------------------
// HISTORIAL: promedio de lecturas validas por intervalo
// -----------------------------------------------------------

struct Acumulador {
  uint32_t inicio = 0;
  uint16_t n = 0;
  uint16_t nSpo2 = 0;
  float sumaBpm = 0;
  float sumaSpo2 = 0;
  float minSpo2 = 999;
  float sumaPi = 0;
  uint32_t sumaCalidad = 0;

  void limpiar(uint32_t ahora) { *this = Acumulador(); inicio = ahora; }
} acum;

// Se llama una vez por segundo
void acumularHistorial() {
  uint32_t ahora = millis();
  if (acum.inicio == 0) acum.inicio = ahora;

  Medicion m = obtenerMedicion();

  if (m.estado == ppg::VALIDO && m.calidad >= CALIDAD_MIN_HISTORIAL && !isnan(m.bpm)) {
    acum.n++;
    acum.sumaBpm += m.bpm;
    acum.sumaCalidad += m.calidad;
    if (!isnan(m.perfusion)) acum.sumaPi += m.perfusion;
    if (!isnan(m.spo2)) {
      acum.nSpo2++;
      acum.sumaSpo2 += m.spo2;
      if (m.spo2 < acum.minSpo2) acum.minSpo2 = m.spo2;
    }
  }

  if (ahora - acum.inicio < PERIODO_HISTORIAL_MS) return;

  // Al menos la mitad del intervalo con lectura valida
  if (acum.n >= PERIODO_HISTORIAL_MS / 2000) {
    RegistroHistorial r;
    r.epochMs   = epochMsActual();
    r.bpm       = acum.sumaBpm / acum.n;
    r.spo2      = acum.nSpo2 ? acum.sumaSpo2 / acum.nSpo2 : NAN;
    r.spo2Min   = acum.nSpo2 ? acum.minSpo2 : NAN;
    r.perfusion = acum.sumaPi / acum.n;
    r.calidad   = (uint8_t)(acum.sumaCalidad / acum.n);
    r.muestras  = acum.n;
    publicarHistorial(r);
  }
  acum.limpiar(ahora);
}

// -----------------------------------------------------------
// IMPRESION
// -----------------------------------------------------------

void imprimirValor(const char* etiqueta, float v, int dec, const char* unidad) {
  Serial.print(etiqueta);
  if (isnan(v)) Serial.print("--");
  else Serial.print(v, dec);
  Serial.print(unidad);
}

void reporteMonitor() {
  Medicion m = obtenerMedicion();

  Serial.printf("[%-13s] ", ppg::nombreEstado(m.estado));
  imprimirValor("BPM: ", m.bpm, 0, "");
  imprimirValor(" | SpO2: ", m.spo2, 1, " %");
  imprimirValor(" | PI: ", m.perfusion, 2, " %");
  imprimirValor(" | R: ", m.ratioR, 3, "");
  Serial.printf(" | Q: %u", m.calidad);
  Serial.printf(" | Red: %s", nombreRed(estadoRed));
  if (historialPendiente) Serial.printf(" (%u pend.)", historialPendiente);
  if (algoritmo.resultado().saturado) Serial.print(" | SATURADO: baja LED_BRILLO");
  Serial.println();
}

void reportePlotter(bool latido) {
  const ppg::Resultado& r = algoritmo.resultado();
  Serial.printf("IR:%.1f,RED:%.1f,Latido:%.1f\n",
                r.irAC, r.redAC, latido ? r.irAC : 0.0f);
}

void leerComandos() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'm' || c == 'M') { modo = MONITOR;  Serial.println("# Modo MONITOR"); }
    if (c == 'p' || c == 'P') { modo = PLOTTER;  Serial.println("# Modo PLOTTER"); }
    if (c == 'c' || c == 'C') {
      modo = CALIBRACION;
      Serial.println("# Modo CALIBRACION: ms,R,PI,BPM");
    }
    // Los mensajes de red ensucian el plotter y el CSV
    logRedHabilitado = (modo == MONITOR);
  }
}

// -----------------------------------------------------------
// INICIALIZACION
// -----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {   // 400 kHz
    Serial.println("ERROR: MAX30101 no detectado. Revisa SDA/SCL y alimentacion.");
    while (true) delay(1000);
  }

  sensor.setup(LED_BRILLO, PROMEDIO, MODO_LED,
               TASA_MUESTREO, ANCHO_PULSO, RANGO_ADC);
  sensor.clearFIFO();

  algoritmo.iniciar(FS);

  Serial.printf("Oximetro v%s - MAX30101 a %.0f Hz\n", FIRMWARE_VERSION, FS);
  Serial.println("Comandos: m = monitor, p = plotter, c = calibracion");

  iniciarRed();

  Serial.println("Coloca el dedo sobre el sensor");
}

// -----------------------------------------------------------
// BUCLE PRINCIPAL (nucleo 1)
// -----------------------------------------------------------
// El FIFO del sensor guarda 32 muestras (320 ms a 100 Hz).
// Aqui no hay nada que bloquee: la red vive en otra tarea.

void loop() {

  leerComandos();
  sensor.check();

  while (sensor.available()) {
    uint32_t red = sensor.getFIFORed();
    uint32_t ir  = sensor.getFIFOIR();
    sensor.nextSample();

    bool latido = algoritmo.procesar(red, ir);

    if (modo == PLOTTER) {
      if (++cuentaPlotter >= DIEZMADO_PLOTTER || latido) {
        cuentaPlotter = 0;
        reportePlotter(latido);
      }
    }

    if (modo == CALIBRACION && latido) {
      const ppg::Resultado& r = algoritmo.resultado();
      if (!isnan(r.ratioR)) {
        Serial.printf("%lu,%.4f,%.3f,%.1f\n",
                      (unsigned long)millis(), r.ratioR, r.perfusion, r.bpm);
      }
    }
  }

  uint32_t ahora = millis();

  // Historial (1 Hz)
  if (ahora - ultimoSegundo >= 1000) {
    ultimoSegundo = ahora;
    acumularHistorial();
  }

  // Lectura actual a Firebase
  uint32_t periodo = algoritmo.dedoPresente() ? PERIODO_ACTUAL_MS : PERIODO_ACTUAL_REPOSO_MS;
  if (ahora - ultimoActual >= periodo) {
    ultimoActual = ahora;
    publicarActual(obtenerMedicion());
  }

  if (modo == MONITOR && ahora - ultimoReporte >= PERIODO_MONITOR) {
    ultimoReporte = ahora;
    reporteMonitor();
  }
}

// -----------------------------------------------------------
// COMO CALIBRAR EL SpO2
// -----------------------------------------------------------
// 1. Pon un oximetro comercial en un dedo y el MAX30101 en
//    otro de la misma mano. Modo 'c'.
// 2. Anota pares (R, SpO2_referencia) en reposo. Para tener
//    mas rango, reten la respiracion brevemente y sin forzar
//    (la SpO2 baja unos pocos puntos); detente si te mareas.
// 3. Ajusta una recta SpO2 = A - B*R (regresion lineal) y
//    actualiza SPO2_A y SPO2_B en ppg_algoritmo.h.
// ===========================================================
