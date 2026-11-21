// ===========================================================
// config.example.h  -  Copia este archivo como config.h y pon tus credenciales y parametros de red
// NO subas este archivo a GitHub con tus datos reales.
// ===========================================================

#pragma once

#define FIRMWARE_VERSION "2.0.0"

// ---------- WiFi (solo 2.4 GHz) ----------
#define WIFI_SSID   "TU_RED_WIFI"
#define WIFI_CLAVE  "TU_CLAVE_WIFI"

// ---------- Firebase ----------
// Consola Firebase > Configuracion del proyecto > General > "Clave de API web"
#define FIREBASE_API_KEY  "TU_CLAVE_API_WEB"

// Proyecto: oximetro-pucmm
// Realtime Database > Datos > URL de arriba (sin "/" final)
// Si creaste la base en EE.UU. (us-central1) es esta. Si elegiste
// otra region sera: https://oximetro-pucmm-default-rtdb.<region>.firebasedatabase.app
#define FIREBASE_DB_URL   "https://oximetro-pucmm-default-rtdb.firebaseio.com"

// Authentication > Usuarios > Agregar usuario (uno por dispositivo)
// Usa una contrasena sin comillas ni barras invertidas.
#define FIREBASE_EMAIL    "oximetro1@oximetro-pucmm.com"
#define FIREBASE_CLAVE    "contrasena-del-dispositivo"

// ---------- Envio ----------
const uint32_t PERIODO_ACTUAL_MS       = 2000;   // con dedo
const uint32_t PERIODO_ACTUAL_REPOSO_MS = 15000; // sin dedo (latido de vida)
const uint32_t PERIODO_HISTORIAL_MS    = 10000;  // un registro promedio cada 10 s
const uint8_t  CALIDAD_MIN_HISTORIAL   = 50;     // solo se guardan lecturas confiables
const uint16_t HISTORIAL_BUFFER        = 180;    // 30 min sin red a 1 registro / 10 s
