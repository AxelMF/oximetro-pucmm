// ===========================================================
// red.h
// Tarea de red en el nucleo 0: WiFi, hora NTP y Firebase.
//
// El sensor y el algoritmo corren en loop() (nucleo 1) y
// solo dejan datos en dos colas. Asi una conexion lenta
// nunca hace perder muestras del MAX30101.
//
//   colaActual    (1 elemento, se sobrescribe)  -> PUT  /actual
//   colaHistorial (buffer offline)              -> POST /historial
//
// Estructura en Realtime Database:
//   /oximetros/{uid}/actual
//   /oximetros/{uid}/historial/{idAutomatico}
// ===========================================================

#pragma once

#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include "config.h"
#include "firebase_rtdb.h"
#include "ppg_algoritmo.h"

// -----------------------------------------------------------
// DATOS QUE VIAJAN POR LAS COLAS
// -----------------------------------------------------------

struct Medicion {
  uint32_t uptimeMs;
  ppg::Estado estado;
  float bpm;          // NAN si no es valido
  float spo2;         // NAN si no es valido
  float ratioR;
  float perfusion;
  uint8_t calidad;
};

// Promedio de un intervalo (por defecto 10 s) de lecturas validas
struct RegistroHistorial {
  int64_t epochMs;    // 0 si aun no hay hora NTP
  float bpm;
  float spo2;
  float spo2Min;
  float perfusion;
  uint8_t calidad;
  uint16_t muestras;
};

enum EstadoRed : uint8_t {
  RED_SIN_WIFI = 0,
  RED_SIN_AUTH = 1,
  RED_OK       = 2,
  RED_ERROR    = 3
};

inline const char* nombreRed(EstadoRed e) {
  switch (e) {
    case RED_SIN_WIFI: return "SIN_WIFI";
    case RED_SIN_AUTH: return "SIN_AUTH";
    case RED_OK:       return "OK";
    case RED_ERROR:    return "ERROR";
  }
  return "?";
}

// -----------------------------------------------------------
// ESTADO COMPARTIDO
// -----------------------------------------------------------

QueueHandle_t colaActual = nullptr;
QueueHandle_t colaHistorial = nullptr;

volatile EstadoRed estadoRed = RED_SIN_WIFI;
volatile uint16_t historialPendiente = 0;
volatile uint32_t historialDescartado = 0;
volatile bool logRedHabilitado = true;   // se apaga en modo plotter

ClienteFirebase firebase;

// -----------------------------------------------------------
// HORA
// -----------------------------------------------------------

inline bool horaValida() {
  return time(nullptr) > 1700000000;   // posterior a nov-2023
}

inline int64_t epochMsActual() {
  if (!horaValida()) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// -----------------------------------------------------------
// JSON
// -----------------------------------------------------------

inline String num(float v, int dec) {
  if (isnan(v) || isinf(v)) return "null";
  return String(v, dec);
}

inline String marcaTiempo(int64_t epochMs) {
  // Si no hay hora local, Firebase pone la del servidor
  if (epochMs <= 0) return "{\".sv\":\"timestamp\"}";
  char b[24];
  snprintf(b, sizeof(b), "%lld", (long long)epochMs);
  return String(b);
}

inline String jsonActual(const Medicion& m) {
  String j = "{";
  j += "\"bpm\":"       + num(m.bpm, 1);
  j += ",\"spo2\":"     + num(m.spo2, 1);
  j += ",\"pi\":"       + num(m.perfusion, 2);
  j += ",\"r\":"        + num(m.ratioR, 4);
  j += ",\"calidad\":"  + String(m.calidad);
  j += ",\"estado\":\"" + String(ppg::nombreEstado(m.estado)) + "\"";
  j += ",\"estadoCod\":" + String((int)m.estado);
  j += ",\"uptime\":"   + String(m.uptimeMs);
  j += ",\"rssi\":"     + String(WiFi.RSSI());
  j += ",\"pendientes\":" + String(historialPendiente);
  j += ",\"firmware\":\"" FIRMWARE_VERSION "\"";
  j += ",\"ts\":{\".sv\":\"timestamp\"}";
  j += "}";
  return j;
}

inline String jsonHistorial(const RegistroHistorial& r) {
  String j = "{";
  j += "\"t\":"        + marcaTiempo(r.epochMs);
  j += ",\"bpm\":"     + num(r.bpm, 1);
  j += ",\"spo2\":"    + num(r.spo2, 1);
  j += ",\"spo2Min\":" + num(r.spo2Min, 1);
  j += ",\"pi\":"      + num(r.perfusion, 2);
  j += ",\"calidad\":" + String(r.calidad);
  j += ",\"n\":"       + String(r.muestras);
  j += "}";
  return j;
}

// -----------------------------------------------------------
// ENCOLAR DESDE loop() (nucleo 1)
// -----------------------------------------------------------

inline void publicarActual(const Medicion& m) {
  if (colaActual) xQueueOverwrite(colaActual, &m);
}

inline void publicarHistorial(const RegistroHistorial& r) {
  if (!colaHistorial) return;
  if (xQueueSend(colaHistorial, &r, 0) != pdTRUE) {
    // Buffer lleno (mucho tiempo sin red): se descarta el mas viejo
    RegistroHistorial viejo;
    xQueueReceive(colaHistorial, &viejo, 0);
    xQueueSend(colaHistorial, &r, 0);
    historialDescartado++;
  }
}

// -----------------------------------------------------------
// TAREA DE RED (nucleo 0)
// -----------------------------------------------------------

inline void logRed(const String& s) {
  if (logRedHabilitado) Serial.println("# [RED] " + s);
}

void tareaRed(void*) {

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_CLAVE);

  firebase.configurar(FIREBASE_API_KEY, FIREBASE_DB_URL,
                      FIREBASE_EMAIL, FIREBASE_CLAVE);

  bool ntpIniciado = false;
  uint32_t ultimoIntentoWifi = millis();
  uint32_t esperaError = 0;
  String rutaBase;

  for (;;) {

    // ---------- WiFi ----------
    if (WiFi.status() != WL_CONNECTED) {
      if (estadoRed != RED_SIN_WIFI) logRed("WiFi desconectado");
      estadoRed = RED_SIN_WIFI;
      if (millis() - ultimoIntentoWifi > 10000) {
        ultimoIntentoWifi = millis();
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_CLAVE);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!ntpIniciado) {
      logRed("WiFi conectado, IP " + WiFi.localIP().toString());
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntpIniciado = true;
    }

    // ---------- Autenticacion ----------
    if (!firebase.asegurarToken()) {
      estadoRed = RED_SIN_AUTH;
      logRed(firebase.ultimoError());
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }

    if (rutaBase.length() == 0) {
      rutaBase = "/oximetros/" + firebase.uid();
      logRed("Autenticado. Ruta: " + rutaBase);
    }

    bool trabajo = false;
    bool fallo = false;

    // ---------- Lectura actual ----------
    Medicion m;
    if (xQueueReceive(colaActual, &m, 0) == pdTRUE) {
      trabajo = true;
      if (!firebase.put(rutaBase + "/actual", jsonActual(m))) fallo = true;
    }

    // ---------- Historial (solo se quita de la cola si se envio) ----------
    RegistroHistorial r;
    if (!fallo && xQueuePeek(colaHistorial, &r, 0) == pdTRUE) {
      trabajo = true;
      if (firebase.post(rutaBase + "/historial", jsonHistorial(r))) {
        xQueueReceive(colaHistorial, &r, 0);
      } else {
        fallo = true;
      }
    }
    historialPendiente = uxQueueMessagesWaiting(colaHistorial);

    if (fallo) {
      estadoRed = RED_ERROR;
      logRed(firebase.ultimoError());
      // Espera creciente: 2, 4, 8 ... hasta 30 s
      esperaError = esperaError ? min<uint32_t>(esperaError * 2, 30000) : 2000;
      vTaskDelay(pdMS_TO_TICKS(esperaError));
      continue;
    }

    esperaError = 0;
    estadoRed = RED_OK;
    vTaskDelay(pdMS_TO_TICKS(trabajo ? 20 : 100));
  }
}

inline void iniciarRed() {
  colaActual = xQueueCreate(1, sizeof(Medicion));
  colaHistorial = xQueueCreate(HISTORIAL_BUFFER, sizeof(RegistroHistorial));

  // Nucleo 0 (el del WiFi). loop() corre en el nucleo 1.
  // TLS necesita bastante pila.
  xTaskCreatePinnedToCore(tareaRed, "red", 12288, nullptr, 1, nullptr, 0);
}
