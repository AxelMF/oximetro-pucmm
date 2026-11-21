// ===========================================================
// firebase_rtdb.h
// Cliente minimo para Firebase Realtime Database por REST.
//
// - Inicio de sesion con email/contrasena (Firebase Auth)
// - Refresco automatico del token (expira cada hora)
// - PUT (sobrescribir) y POST (agregar con ID unico)
//
// Solo usa librerias del core ESP32 (WiFiClientSecure y
// HTTPClient): no hace falta instalar nada adicional.
// ===========================================================

#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

class ClienteFirebase {
public:

  void configurar(const char* apiKey, const char* urlBase,
                  const char* email, const char* clave) {
    apiKey_ = apiKey;
    urlBase_ = urlBase;
    while (urlBase_.endsWith("/")) urlBase_.remove(urlBase_.length() - 1);
    email_ = email;
    clave_ = clave;

    // TLS sin verificar el certificado del servidor.
    // Evita depender de la hora (NTP) y de certificados raiz
    // que Google rota. El trafico sigue cifrado.
    // Para produccion: cliente_.setCACert(certificadoRaiz);
    cliente_.setInsecure();
  }

  // Inicia sesion o refresca el token si esta por vencer.
  bool asegurarToken() {
    if (idToken_.length() && (int32_t)(expiraMs_ - millis()) > 0) return true;
    if (refreshToken_.length() && refrescar()) return true;
    return iniciarSesion();
  }

  // Fuerza un nuevo token en el proximo asegurarToken()
  void invalidarToken() { expiraMs_ = millis(); }

  bool put(const String& ruta, const String& json)  { return escribir("PUT", ruta, json); }
  bool post(const String& ruta, const String& json) { return escribir("POST", ruta, json); }
  bool patch(const String& ruta, const String& json){ return escribir("PATCH", ruta, json); }

  const String& uid() const { return uid_; }
  bool autenticado() const { return idToken_.length() > 0; }
  int ultimoCodigo() const { return ultimoCodigo_; }
  const String& ultimoError() const { return ultimoError_; }

private:

  bool iniciarSesion() {
    String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + apiKey_;
    String cuerpo = "{\"email\":\"" + email_ + "\",\"password\":\"" + clave_ +
                    "\",\"returnSecureToken\":true}";
    String resp;
    int cod = peticion("POST", url, cuerpo, "application/json", &resp);

    if (cod != 200) {
      ultimoError_ = "Login fallo (" + String(cod) + "): " + extraer(resp, "message");
      idToken_ = "";
      return false;
    }

    idToken_      = extraer(resp, "idToken");
    refreshToken_ = extraer(resp, "refreshToken");
    uid_          = extraer(resp, "localId");
    programarVencimiento(extraer(resp, "expiresIn"));
    ultimoError_ = "";
    return idToken_.length() > 0;
  }

  bool refrescar() {
    String url = "https://securetoken.googleapis.com/v1/token?key=" + apiKey_;
    String cuerpo = "grant_type=refresh_token&refresh_token=" + refreshToken_;
    String resp;
    int cod = peticion("POST", url, cuerpo, "application/x-www-form-urlencoded", &resp);

    if (cod != 200) {
      refreshToken_ = "";
      return false;
    }

    idToken_      = extraer(resp, "id_token");
    refreshToken_ = extraer(resp, "refresh_token");
    String u = extraer(resp, "user_id");
    if (u.length()) uid_ = u;
    programarVencimiento(extraer(resp, "expires_in"));
    return idToken_.length() > 0;
  }

  void programarVencimiento(const String& segundos) {
    long s = segundos.toInt();
    if (s <= 0) s = 3600;
    // Renovar 5 minutos antes de que venza
    expiraMs_ = millis() + (uint32_t)(s - 300) * 1000UL;
  }

  bool escribir(const char* metodo, const String& ruta, const String& json) {
    if (!asegurarToken()) return false;

    String url = urlBase_ + ruta + ".json?auth=" + idToken_;
    String resp;
    int cod = peticion(metodo, url, json, "application/json", &resp);

    if (cod == 401) {
      // Token rechazado: forzar renovacion y reintentar una vez
      invalidarToken();
      if (!asegurarToken()) return false;
      url = urlBase_ + ruta + ".json?auth=" + idToken_;
      cod = peticion(metodo, url, json, "application/json", &resp);
    }

    if (cod != 200) {
      ultimoError_ = String(metodo) + " " + ruta + " -> " + String(cod) + " " + extraer(resp, "error");
      return false;
    }
    return true;
  }

  int peticion(const char* metodo, const String& url, const String& cuerpo,
               const char* tipo, String* respuesta) {
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(cliente_, url)) {
      ultimoCodigo_ = -1;
      return -1;
    }
    http.addHeader("Content-Type", tipo);
    int cod = http.sendRequest(metodo, cuerpo);
    if (respuesta) *respuesta = (cod > 0) ? http.getString() : String("");
    http.end();
    ultimoCodigo_ = cod;
    return cod;
  }

  // Extrae el valor de una clave de un JSON simple (texto o numero).
  // Suficiente para las respuestas de Firebase Auth; evita
  // depender de ArduinoJson.
  static String extraer(const String& s, const char* clave) {
    String k = String("\"") + clave + "\"";
    int i = s.indexOf(k);
    if (i < 0) return "";
    i = s.indexOf(':', i + k.length());
    if (i < 0) return "";
    i++;
    int n = s.length();
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    if (i >= n) return "";
    if (s[i] == '"') {
      int f = s.indexOf('"', i + 1);
      return (f < 0) ? String("") : s.substring(i + 1, f);
    }
    int f = i;
    while (f < n && s[f] != ',' && s[f] != '}' && s[f] != '\n' && s[f] != ' ') f++;
    return s.substring(i, f);
  }

  WiFiClientSecure cliente_;
  String apiKey_, urlBase_, email_, clave_;
  String idToken_, refreshToken_, uid_;
  uint32_t expiraMs_ = 0;
  int ultimoCodigo_ = 0;
  String ultimoError_;
};
