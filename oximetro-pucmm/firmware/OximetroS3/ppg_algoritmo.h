// ===========================================================
// ppg_algoritmo.h
// Algoritmo propio de BPM y SpO2 para MAX30101 / MAX30102
//
// C++ puro (sin dependencias de Arduino): se puede probar
// en PC con senales sinteticas.
//
// Etapas:
//   1. Deteccion de dedo con histeresis
//   2. Componente DC (media movil exponencial)
//   3. Pasa-banda 0.5 - 4 Hz (dos biquads Butterworth)
//   4. Deteccion de picos con umbral adaptativo y
//      periodo refractario
//   5. BPM = 60000 / mediana de intervalos entre latidos
//   6. SpO2 por "ratio of ratios" R = (ACr/DCr)/(ACir/DCir)
//   7. Indice de perfusion y calidad de senal (0-100)
//
// AVISO: SpO2 sin calibrar. Uso experimental/educativo,
// no apto para diagnostico medico.
// ===========================================================

#pragma once

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -----------------------------------------------------------
// PARAMETROS AJUSTABLES
// -----------------------------------------------------------

namespace ppg {

// Deteccion de dedo (valor IR crudo, ADC 18 bits)
const uint32_t DEDO_UMBRAL_ENTRADA = 10000;
const uint32_t DEDO_UMBRAL_SALIDA  = 7000;
const uint16_t DEDO_MUESTRAS_CONFIRMAR = 20;   // 200 ms a 100 Hz

// Saturacion del ADC (maximo 262143)
const uint32_t UMBRAL_SATURACION = 250000;

// Filtro pasa-banda
const float FC_PASA_ALTOS = 0.5f;   // Hz (30 BPM)
const float FC_PASA_BAJOS = 4.0f;   // Hz (240 BPM)

// Tiempo para que los filtros se asienten al poner el dedo
const uint32_t MS_ESTABILIZACION = 2000;

// Rango fisiologico aceptado
const float IBI_MIN_MS = 300.0f;    // 200 BPM
const float IBI_MAX_MS = 1500.0f;   // 40 BPM

// Un intervalo se rechaza si se desvia mas de este
// porcentaje de la mediana actual
const float TOLERANCIA_IBI = 0.30f;
const uint8_t RECHAZOS_PARA_REINICIAR = 3;

// Latidos necesarios para reportar un valor
const uint8_t LATIDOS_MINIMOS = 4;

// Si no hay latidos validos en este tiempo, el valor
// deja de considerarse valido
const uint32_t MS_SIN_LATIDO_MAX = 3000;

// Calibracion SpO2 = A - B * R
// Valores de referencia de literatura; CALIBRAR contra un
// oximetro comercial (ver comentario en el sketch).
const float SPO2_A = 110.0f;
const float SPO2_B = 25.0f;

const uint8_t TAM_HISTORIAL = 8;

// -----------------------------------------------------------
// ESTADOS
// -----------------------------------------------------------

enum Estado : uint8_t {
  SIN_DEDO      = 0,
  ESTABILIZANDO = 1,
  MIDIENDO      = 2,   // dedo presente, acumulando latidos
  VALIDO        = 3,   // BPM/SpO2 disponibles
  SENAL_MALA    = 4    // movimiento, saturacion o sin latidos
};

inline const char* nombreEstado(Estado e) {
  switch (e) {
    case SIN_DEDO:      return "SIN_DEDO";
    case ESTABILIZANDO: return "ESTABILIZANDO";
    case MIDIENDO:      return "MIDIENDO";
    case VALIDO:        return "VALIDO";
    case SENAL_MALA:    return "SENAL_MALA";
  }
  return "?";
}

// -----------------------------------------------------------
// RESULTADO
// -----------------------------------------------------------

struct Resultado {
  Estado estado = SIN_DEDO;

  bool  bpmValido = false;
  float bpm = NAN;

  bool  spo2Valido = false;
  float spo2 = NAN;

  float ratioR = NAN;          // para calibracion
  float perfusion = NAN;       // indice de perfusion IR (%)
  uint8_t calidad = 0;         // 0 - 100

  bool  saturado = false;

  // Senales filtradas para graficar
  float irAC = 0;
  float redAC = 0;
};

// -----------------------------------------------------------
// FILTRO BIQUAD (formulas RBJ, forma directa I)
// -----------------------------------------------------------

class Biquad {
public:
  void pasaBajos(float fc, float fs, float q = 0.70710678f) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float c = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0_ = (1.0f - c) * 0.5f / a0;
    b1_ = (1.0f - c) / a0;
    b2_ = b0_;
    a1_ = -2.0f * c / a0;
    a2_ = (1.0f - alpha) / a0;
    reiniciar();
  }

  void pasaAltos(float fc, float fs, float q = 0.70710678f) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float c = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0_ = (1.0f + c) * 0.5f / a0;
    b1_ = -(1.0f + c) / a0;
    b2_ = b0_;
    a1_ = -2.0f * c / a0;
    a2_ = (1.0f - alpha) / a0;
    reiniciar();
  }

  void reiniciar() { x1_ = x2_ = y1_ = y2_ = 0; }

  float procesar(float x) {
    float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_; x1_ = x;
    y2_ = y1_; y1_ = y;
    return y;
  }

private:
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// -----------------------------------------------------------
// HISTORIAL CIRCULAR CON MEDIANA
// -----------------------------------------------------------

class Historial {
public:
  void limpiar() { n_ = 0; idx_ = 0; }

  void agregar(float v) {
    datos_[idx_] = v;
    idx_ = (idx_ + 1) % TAM_HISTORIAL;
    if (n_ < TAM_HISTORIAL) n_++;
  }

  uint8_t cantidad() const { return n_; }

  float mediana() const {
    if (n_ == 0) return NAN;
    float t[TAM_HISTORIAL];
    for (uint8_t i = 0; i < n_; i++) t[i] = datos_[i];
    // Ordenamiento por insercion (n <= 8)
    for (uint8_t i = 1; i < n_; i++) {
      float k = t[i];
      int j = i - 1;
      while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
      t[j + 1] = k;
    }
    if (n_ % 2) return t[n_ / 2];
    return 0.5f * (t[n_ / 2 - 1] + t[n_ / 2]);
  }

  // Coeficiente de variacion (desviacion / media)
  float variacion() const {
    if (n_ < 2) return 0;
    float m = 0;
    for (uint8_t i = 0; i < n_; i++) m += datos_[i];
    m /= n_;
    float s = 0;
    for (uint8_t i = 0; i < n_; i++) s += (datos_[i] - m) * (datos_[i] - m);
    return sqrtf(s / (n_ - 1)) / m;
  }

private:
  float datos_[TAM_HISTORIAL];
  uint8_t n_ = 0;
  uint8_t idx_ = 0;
};

// -----------------------------------------------------------
// ALGORITMO PRINCIPAL
// -----------------------------------------------------------

class AlgoritmoPPG {
public:

  void iniciar(float fs) {
    fs_ = fs;
    msPorMuestra_ = 1000.0f / fs;
    // Envolvente: cae a la mitad en ~1.5 s
    decaimiento_ = powf(0.5f, 1.0f / (1.5f * fs));
    // DC: constante de tiempo ~1 s
    alfaDC_ = 1.0f / fs;
    dedo_ = false;
    contadorDedo_ = 0;
    reiniciarMedicion();
    res_ = Resultado();
  }

  // Procesa una muestra. Devuelve true si se detecto un latido.
  bool procesar(uint32_t red, uint32_t ir) {
    n_++;
    uint32_t t = (uint32_t)(n_ * msPorMuestra_);

    // ---------- 1. Deteccion de dedo con histeresis ----------
    bool lecturaDedo = dedo_ ? (ir > DEDO_UMBRAL_SALIDA)
                             : (ir > DEDO_UMBRAL_ENTRADA);

    if (lecturaDedo != dedo_) {
      if (++contadorDedo_ >= DEDO_MUESTRAS_CONFIRMAR) {
        dedo_ = lecturaDedo;
        contadorDedo_ = 0;
        reiniciarMedicion();
        if (dedo_) {
          tInicio_ = t;
          redRef_ = (float)red;
          irRef_ = (float)ir;
          dcRed_ = (float)red;
          dcIr_ = (float)ir;
        }
      }
    } else {
      contadorDedo_ = 0;
    }

    if (!dedo_) {
      res_ = Resultado();
      res_.estado = SIN_DEDO;
      return false;
    }

    // ---------- 2. Saturacion ----------
    bool sat = (ir >= UMBRAL_SATURACION || red >= UMBRAL_SATURACION);
    if (sat) tUltimaSaturacion_ = t;
    res_.saturado = (t - tUltimaSaturacion_) < 1000 && tUltimaSaturacion_ != 0;

    // ---------- 3. DC y pasa-banda ----------
    dcRed_ += alfaDC_ * ((float)red - dcRed_);
    dcIr_  += alfaDC_ * ((float)ir  - dcIr_);

    // Se resta el valor inicial para evitar el escalon
    // enorme en los filtros al poner el dedo.
    // Signo negativo: mas sangre = mas absorcion = menos luz,
    // asi el pico sistolico queda positivo.
    float redF = -lpRed_.procesar(hpRed_.procesar((float)red - redRef_));
    float irF  = -lpIr_.procesar(hpIr_.procesar((float)ir - irRef_));

    res_.redAC = redF;
    res_.irAC = irF;

    if (t - tInicio_ < MS_ESTABILIZACION) {
      res_.estado = ESTABILIZANDO;
      ir2_ = ir1_; ir1_ = irF;
      return false;
    }

    // Rastrear maximo y minimo del ciclo actual
    if (irF > cicloMaxIr_) cicloMaxIr_ = irF;
    if (irF < cicloMinIr_) cicloMinIr_ = irF;
    if (redF > cicloMaxRed_) cicloMaxRed_ = redF;
    if (redF < cicloMinRed_) cicloMinRed_ = redF;

    // ---------- 4. Deteccion de picos ----------
    envolvente_ *= decaimiento_;
    if (irF > envolvente_) envolvente_ = irF;

    bool latido = false;

    // Maximo local en la muestra anterior
    bool maximoLocal = (ir1_ > ir2_) && (ir1_ >= irF);
    float umbral = 0.5f * envolvente_;

    float refractario = IBI_MIN_MS;
    float medIbi = ibis_.mediana();
    if (ibis_.cantidad() >= 3 && 0.6f * medIbi > refractario) {
      refractario = 0.6f * medIbi;
    }

    uint32_t tPico = t - (uint32_t)msPorMuestra_;

    if (maximoLocal && ir1_ > umbral && ir1_ > 0 &&
        (tUltimoPico_ == 0 || (tPico - tUltimoPico_) >= refractario)) {

      latido = true;
      registrarLatido(tPico, sat);
    }

    ir2_ = ir1_;
    ir1_ = irF;

    // ---------- 5. Estado y validez ----------
    actualizarEstado(t);

    return latido;
  }

  const Resultado& resultado() const { return res_; }
  bool dedoPresente() const { return dedo_; }

private:

  void reiniciarMedicion() {
    hpRed_.pasaAltos(FC_PASA_ALTOS, fs_);
    hpIr_.pasaAltos(FC_PASA_ALTOS, fs_);
    lpRed_.pasaBajos(FC_PASA_BAJOS, fs_);
    lpIr_.pasaBajos(FC_PASA_BAJOS, fs_);

    ibis_.limpiar();
    ratios_.limpiar();
    perfusiones_.limpiar();

    envolvente_ = 0;
    ir1_ = ir2_ = 0;
    tUltimoPico_ = 0;
    tUltimoValido_ = 0;
    tUltimaSaturacion_ = 0;
    rechazosSeguidos_ = 0;
    reiniciarCiclo();
  }

  void reiniciarCiclo() {
    cicloMaxIr_ = cicloMaxRed_ = -1e9f;
    cicloMinIr_ = cicloMinRed_ = 1e9f;
  }

  void registrarLatido(uint32_t tPico, bool sat) {

    // Amplitud pico a pico del ciclo que termina
    float acIr = cicloMaxIr_ - cicloMinIr_;
    float acRed = cicloMaxRed_ - cicloMinRed_;
    bool cicloCompleto = (tUltimoPico_ != 0);
    reiniciarCiclo();

    if (!cicloCompleto) {
      tUltimoPico_ = tPico;
      return;
    }

    float ibi = (float)(tPico - tUltimoPico_);
    tUltimoPico_ = tPico;

    if (ibi < IBI_MIN_MS || ibi > IBI_MAX_MS || sat) return;

    // Rechazo de intervalos atipicos
    if (ibis_.cantidad() >= 3) {
      float med = ibis_.mediana();
      if (fabsf(ibi - med) > TOLERANCIA_IBI * med) {
        rechazosSeguidos_++;
        if (rechazosSeguidos_ >= RECHAZOS_PARA_REINICIAR) {
          // El ritmo cambio de verdad: empezar de nuevo
          ibis_.limpiar();
          ratios_.limpiar();
          perfusiones_.limpiar();
          rechazosSeguidos_ = 0;
        } else {
          return;
        }
      } else {
        rechazosSeguidos_ = 0;
      }
    }

    ibis_.agregar(ibi);
    tUltimoValido_ = tPico;

    // Ratio of ratios
    if (acIr > 0 && acRed > 0 && dcIr_ > 0 && dcRed_ > 0) {
      float piIr = acIr / dcIr_;
      float r = (acRed / dcRed_) / piIr;
      if (r > 0.2f && r < 3.0f) ratios_.agregar(r);
      perfusiones_.agregar(piIr * 100.0f);
    }
  }

  void actualizarEstado(uint32_t t) {

    bool reciente = tUltimoValido_ != 0 &&
                    (t - tUltimoValido_) <= MS_SIN_LATIDO_MAX;

    // BPM
    if (ibis_.cantidad() >= LATIDOS_MINIMOS && reciente) {
      res_.bpm = 60000.0f / ibis_.mediana();
      res_.bpmValido = true;
    } else {
      res_.bpm = NAN;
      res_.bpmValido = false;
    }

    // SpO2
    if (ratios_.cantidad() >= LATIDOS_MINIMOS && reciente) {
      float r = ratios_.mediana();
      float s = SPO2_A - SPO2_B * r;
      res_.ratioR = r;
      if (s > 100.0f) s = 100.0f;
      res_.spo2 = s;
      res_.spo2Valido = (s >= 70.0f);
    } else {
      res_.ratioR = NAN;
      res_.spo2 = NAN;
      res_.spo2Valido = false;
    }

    res_.perfusion = perfusiones_.cantidad() ? perfusiones_.mediana() : NAN;
    res_.calidad = calcularCalidad(reciente);

    // Estado
    if (res_.saturado) {
      res_.estado = SENAL_MALA;
    } else if (res_.bpmValido) {
      res_.estado = VALIDO;
    } else if (tUltimoValido_ != 0 && !reciente) {
      res_.estado = SENAL_MALA;
    } else if (rechazosSeguidos_ > 0 && ibis_.cantidad() < LATIDOS_MINIMOS) {
      res_.estado = SENAL_MALA;
    } else {
      res_.estado = MIDIENDO;
    }
  }

  uint8_t calcularCalidad(bool reciente) {
    if (res_.saturado || !reciente) return 0;

    float q = 100.0f;

    // Indice de perfusion: tipico 0.5 - 10 %
    float pi = res_.perfusion;
    if (isnan(pi) || pi < 0.1f) q -= 60;
    else if (pi < 0.3f) q -= 30;
    else if (pi > 15.0f) q -= 30;   // probable movimiento

    // Regularidad de los intervalos
    float cv = ibis_.variacion();
    if (cv > 0.20f) q -= 40;
    else if (cv > 0.10f) q -= 20;

    // Pocos latidos acumulados
    if (ibis_.cantidad() < LATIDOS_MINIMOS) q -= 20;

    // Rechazos recientes
    q -= 10.0f * rechazosSeguidos_;

    if (q < 0) q = 0;
    return (uint8_t)q;
  }

  // Configuracion
  float fs_ = 100.0f;
  float msPorMuestra_ = 10.0f;
  float decaimiento_ = 0.995f;
  float alfaDC_ = 0.01f;

  // Dedo
  bool dedo_ = false;
  uint16_t contadorDedo_ = 0;

  // Tiempo (derivado del conteo de muestras)
  uint64_t n_ = 0;
  uint32_t tInicio_ = 0;
  uint32_t tUltimoPico_ = 0;
  uint32_t tUltimoValido_ = 0;
  uint32_t tUltimaSaturacion_ = 0;

  // Filtros
  Biquad hpRed_, hpIr_, lpRed_, lpIr_;
  float redRef_ = 0, irRef_ = 0;
  float dcRed_ = 0, dcIr_ = 0;

  // Picos
  float envolvente_ = 0;
  float ir1_ = 0, ir2_ = 0;
  float cicloMaxIr_, cicloMinIr_, cicloMaxRed_, cicloMinRed_;
  uint8_t rechazosSeguidos_ = 0;

  Historial ibis_;
  Historial ratios_;
  Historial perfusiones_;

  Resultado res_;
};

} // namespace ppg
