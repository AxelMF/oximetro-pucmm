# Oxímetro PUCMM

Oxímetro de pulso experimental con **ESP32-S3 + MAX30101**. Mide pulso (BPM) y saturación de oxígeno (SpO₂) con un algoritmo propio, envía los datos a **Firebase Realtime Database** y los muestra en un panel web.

> Proyecto académico de Ingeniería Mecatrónica. La SpO₂ no está calibrada contra un equipo de referencia: **no usar para diagnóstico médico.**

## Estructura

```
firmware/OximetroS3/      Sketch de Arduino para el ESP32-S3
  OximetroS3.ino          Lectura del sensor (núcleo 1)
  ppg_algoritmo.h         Filtros, detección de latidos, BPM y SpO₂
  red.h                   WiFi + Firebase en una tarea aparte (núcleo 0)
  firebase_rtdb.h         Cliente REST de Realtime Database
  config.example.h        Plantilla de credenciales
firebase/
  database.rules.json     Reglas de seguridad de la base de datos
dashboard/                Panel web (HTML + CSS + JavaScript)
```

## Datos en Firebase

```
/oximetros/{uid}/actual                 Última lectura, se sobrescribe cada 2 s
/oximetros/{uid}/historial/{id}         Promedio cada 10 s de lecturas estables
```

`actual`: `bpm`, `spo2`, `pi` (perfusión %), `r` (ratio de ratios), `calidad` (0–100), `estado`, `rssi`, `firmware`, `ts`.
`historial`: `t` (epoch ms), `bpm`, `spo2`, `spo2Min`, `pi`, `calidad`, `n`.

## Puesta en marcha

### 1. Firebase

1. **Realtime Database** creada en `us-central1` → `https://oximetro-pucmm-default-rtdb.firebaseio.com`.
2. **Reglas**: pegar `firebase/database.rules.json` en la pestaña Reglas y publicar.
3. **Authentication** → activar *Correo electrónico/contraseña* y crear el usuario del dispositivo (por ejemplo `oximetro1@oximetro-pucmm.com`).

### 2. Firmware

1. Copiar `firmware/OximetroS3/config.example.h` como `config.h` y completar WiFi (2.4 GHz), clave de API web y contraseña del dispositivo. `config.h` está en `.gitignore`.
2. Arduino IDE: placa **ESP32S3 Dev Module**, librería **SparkFun MAX3010x**.
3. Monitor Serie a 115200: `m` monitor, `p` plotter, `c` calibración.

Conexiones: SDA → GPIO20, SCL → GPIO21, VIN → 3V3, GND → GND. Si usas el puerto USB nativo del S3, mueve el I²C a GPIO8/9 (GPIO20 es USB D+) y cambia `SDA_PIN`/`SCL_PIN`.

### 3. Panel web

- **Local**: `cd dashboard && python -m http.server 8000` y abrir `http://localhost:8000`. (Abrir `index.html` con doble clic no funciona por los módulos de JavaScript.)
- **Cloudflare Pages** (ver abajo).
- **Demostración sin dispositivo**: agregar `?demo` a la URL.

Se entra con el mismo correo y contraseña del dispositivo: las reglas solo permiten leer la rama del usuario autenticado.

## Publicación en Cloudflare Pages

1. Cloudflare → **Workers & Pages → Create → Pages → Connect to Git** y elegir `AxelMF/oximetro-pucmm`.
2. Configuración de compilación:
   - **Framework preset:** None
   - **Build command:** *(vacío)*
   - **Build output directory:** `dashboard`
3. **Save and Deploy.** Cada push a `main` vuelve a publicar automáticamente.
4. En Firebase → Authentication → Settings → **Authorized domains**, agregar el dominio `*.pages.dev` asignado (o el dominio propio).

`dashboard/_headers` define cabeceras de seguridad que Cloudflare aplica a todas las páginas.

## Calibración de SpO₂

Modo `c` del monitor serie con un oxímetro comercial en otro dedo de la misma mano. Con pares (R, SpO₂ de referencia), ajustar por regresión lineal `SpO₂ = A − B·R` y actualizar `SPO2_A` y `SPO2_B` en `ppg_algoritmo.h`.
