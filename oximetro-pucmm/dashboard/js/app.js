// ===========================================================
// Oxímetro PUCMM — panel de visualización
// Lee /oximetros/{uid}/actual y /oximetros/{uid}/historial
// de Firebase Realtime Database.
//
// Con ?demo en la URL usa datos simulados (sin Firebase).
// ===========================================================

const SDK = "https://www.gstatic.com/firebasejs/10.12.2";

const ONLINE_MS = 40000;          // sin datos en 40 s = desconectado
const VIVO_VENTANA_MS = 10 * 60000;
const PUNTOS_MAX = 400;           // puntos dibujados por serie

const $ = (id) => document.getElementById(id);

const MENSAJES_ESTADO = {
  SIN_DEDO:      { texto: "Coloca el dedo sobre el sensor", tipo: "" },
  ESTABILIZANDO: { texto: "Estabilizando la señal…", tipo: "" },
  MIDIENDO:      { texto: "Midiendo latidos…", tipo: "" },
  VALIDO:        { texto: "Lectura estable", tipo: "ok" },
  SENAL_MALA:    { texto: "Señal inestable: mantén el dedo quieto", tipo: "alerta" },
};

const ERRORES_LOGIN = {
  "auth/invalid-credential":     "Correo o contraseña incorrectos.",
  "auth/invalid-email":          "Escribe un correo válido.",
  "auth/missing-password":       "Escribe la contraseña.",
  "auth/user-disabled":          "Esta cuenta está deshabilitada en Firebase.",
  "auth/too-many-requests":      "Demasiados intentos. Espera unos minutos y vuelve a intentarlo.",
  "auth/network-request-failed": "Sin conexión a internet.",
};

// -----------------------------------------------------------
// ESTADO
// -----------------------------------------------------------

const estado = {
  fuente: null,
  actual: null,
  vivo: [],              // [{t, bpm, spo2}]
  historial: [],         // registros del periodo elegido
  rango: "vivo",
  cancelarHistorial: null,
  cancelarActual: null,
};

let grafica = null;

// -----------------------------------------------------------
// FORMATO
// -----------------------------------------------------------

const fmtHora = new Intl.DateTimeFormat("es-DO", { hour: "2-digit", minute: "2-digit" });
const fmtHoraSeg = new Intl.DateTimeFormat("es-DO", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const fmtDia = new Intl.DateTimeFormat("es-DO", { weekday: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });

const num = (v) => typeof v === "number" && Number.isFinite(v);
const fmt = (v, dec = 0) => (num(v) ? v.toFixed(dec) : "--");

function hace(ms) {
  const s = Math.round(ms / 1000);
  if (s < 5) return "ahora";
  if (s < 60) return `hace ${s} s`;
  const m = Math.round(s / 60);
  if (m < 60) return `hace ${m} min`;
  const h = Math.round(m / 60);
  if (h < 48) return `hace ${h} h`;
  return `hace ${Math.round(h / 24)} días`;
}

function duracion(ms) {
  const min = Math.round(ms / 60000);
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60);
  return `${h} h ${min % 60} min`;
}

function etiquetaTiempo(t) {
  if (estado.rango === "vivo") return fmtHoraSeg.format(t);
  if (Number(estado.rango) > 86400000) return fmtDia.format(t);
  return fmtHora.format(t);
}

function calidadWifi(rssi) {
  if (!num(rssi)) return "--";
  if (rssi >= -60) return `Buena (${rssi} dBm)`;
  if (rssi >= -72) return `Regular (${rssi} dBm)`;
  return `Débil (${rssi} dBm)`;
}

// -----------------------------------------------------------
// VISTAS
// -----------------------------------------------------------

function mostrarLogin(mensaje) {
  $("vista-panel").hidden = true;
  $("vista-login").hidden = false;
  const e = $("login-error");
  e.hidden = !mensaje;
  e.textContent = mensaje || "";
}

function mostrarPanel() {
  $("vista-login").hidden = true;
  $("vista-panel").hidden = false;
  if (!grafica) crearGrafica();
}

function mostrarAviso(texto) {
  const a = $("aviso");
  a.hidden = !texto;
  a.textContent = texto || "";
}

// -----------------------------------------------------------
// LECTURA ACTUAL
// -----------------------------------------------------------

function alRecibirActual(a) {
  estado.actual = a;

  if (a && num(a.ts)) {
    const ult = estado.vivo[estado.vivo.length - 1];
    if (!ult || ult.t !== a.ts) {
      estado.vivo.push({ t: a.ts, bpm: num(a.bpm) ? a.bpm : null, spo2: num(a.spo2) ? a.spo2 : null });
      const limite = estado.fuente.ahora() - VIVO_VENTANA_MS;
      while (estado.vivo.length && estado.vivo[0].t < limite) estado.vivo.shift();
    }
  }

  pintarActual();
  if (estado.rango === "vivo") actualizarGrafica();
}

function pintarActual() {
  const a = estado.actual;
  const ahora = estado.fuente.ahora();
  const enLinea = a && num(a.ts) && ahora - a.ts < ONLINE_MS;

  // Conexión del dispositivo
  const punto = $("punto-conexion");
  punto.className = "punto " + (a ? (enLinea ? "punto--activo" : "punto--inactivo") : "");
  $("txt-conexion").textContent = !a
    ? "El dispositivo aún no ha enviado datos"
    : enLinea
      ? `Dispositivo en línea, actualizado ${hace(ahora - a.ts)}`
      : `Dispositivo sin conexión, último dato ${hace(ahora - a.ts)}`;

  const bpm = enLinea && num(a.bpm) ? a.bpm : null;
  const spo2 = enLinea && num(a.spo2) ? a.spo2 : null;

  $("v-bpm").textContent = fmt(bpm, 0);
  $("v-spo2").textContent = fmt(spo2, 0);
  document.querySelector(".lectura--pulso").classList.toggle("lectura--sin-dato", bpm === null);
  document.querySelector(".lectura--oxigeno").classList.toggle("lectura--sin-dato", spo2 === null);

  // Latido al ritmo medido
  const latido = $("latido");
  latido.classList.toggle("latido--activo", bpm !== null);
  if (bpm !== null) latido.style.setProperty("--periodo", `${(60 / bpm).toFixed(3)}s`);

  // Estado de la medición
  const e = $("estado");
  if (!a) {
    e.textContent = "Esperando datos del dispositivo";
    e.className = "estado";
  } else if (!enLinea) {
    e.textContent = "Sin datos recientes. Revisa que el oxímetro esté encendido y con WiFi.";
    e.className = "estado estado--alerta";
  } else {
    const m = MENSAJES_ESTADO[a.estado] || { texto: a.estado, tipo: "" };
    e.textContent = m.texto;
    e.className = "estado" + (m.tipo ? ` estado--${m.tipo}` : "");
  }

  $("v-pi").textContent = a && num(a.pi) ? `${a.pi.toFixed(2)} %` : "--";
  $("v-calidad").textContent = a && num(a.calidad) ? a.calidad : "--";
  $("m-calidad").value = a && num(a.calidad) ? a.calidad : 0;
  $("v-rssi").textContent = a ? calidadWifi(a.rssi) : "--";
  $("v-firmware").textContent = a && a.firmware ? `v${a.firmware}` : "--";
}

// -----------------------------------------------------------
// HISTORIAL
// -----------------------------------------------------------

function cambiarRango(valor) {
  estado.rango = valor;
  if (estado.cancelarHistorial) estado.cancelarHistorial();
  estado.cancelarHistorial = null;

  // El resumen usa la última hora cuando la gráfica está en vivo
  const ventana = valor === "vivo" ? 3600000 : Number(valor);
  const desde = estado.fuente.ahora() - ventana;

  estado.historial = [];
  pintarResumen();
  actualizarGrafica();

  estado.cancelarHistorial = estado.fuente.suscribirHistorial(desde, (registros) => {
    estado.historial = registros;
    pintarResumen();
    actualizarGrafica();
  });
}

function pintarResumen() {
  const h = estado.historial;
  const conBpm = h.filter((r) => num(r.bpm));
  const conSpo2 = h.filter((r) => num(r.spo2));
  const media = (arr, k) => arr.reduce((s, r) => s + r[k], 0) / arr.length;

  $("r-bpm").textContent = conBpm.length ? `${media(conBpm, "bpm").toFixed(0)} lpm` : "--";
  $("r-rango").textContent = conBpm.length
    ? `${Math.min(...conBpm.map((r) => r.bpm)).toFixed(0)} – ${Math.max(...conBpm.map((r) => r.bpm)).toFixed(0)} lpm`
    : "--";
  $("r-spo2").textContent = conSpo2.length ? `${media(conSpo2, "spo2").toFixed(1)} %` : "--";

  const minimas = h.map((r) => (num(r.spo2Min) ? r.spo2Min : r.spo2)).filter(num);
  $("r-spo2min").textContent = minimas.length ? `${Math.min(...minimas).toFixed(1)} %` : "--";

  // Cada registro resume ~10 s del firmware
  $("r-tiempo").textContent = h.length ? duracion(h.length * 10000) : "--";

  // Tabla: últimos 12 registros
  const cuerpo = $("tabla-cuerpo");
  cuerpo.replaceChildren();
  const ultimos = h.slice(-12).reverse();

  if (!ultimos.length) {
    const tr = document.createElement("tr");
    tr.className = "tabla__vacia";
    const td = document.createElement("td");
    td.colSpan = 5;
    td.textContent = "No hay registros en este periodo. El historial se llena con lecturas estables de al menos 5 segundos.";
    tr.append(td);
    cuerpo.append(tr);
  } else {
    for (const r of ultimos) {
      const tr = document.createElement("tr");
      for (const v of [fmtHoraSeg.format(r.t), fmt(r.bpm, 0), fmt(r.spo2, 1), fmt(r.spo2Min, 1), fmt(r.calidad, 0)]) {
        const td = document.createElement("td");
        td.textContent = v;
        tr.append(td);
      }
      cuerpo.append(tr);
    }
  }

  $("btn-csv").disabled = !h.length;
}

function descargarCSV() {
  const filas = [["fecha_hora", "epoch_ms", "bpm", "spo2", "spo2_min", "perfusion", "calidad", "muestras"]];
  for (const r of estado.historial) {
    filas.push([
      new Date(r.t).toISOString(), r.t,
      num(r.bpm) ? r.bpm : "", num(r.spo2) ? r.spo2 : "", num(r.spo2Min) ? r.spo2Min : "",
      num(r.pi) ? r.pi : "", num(r.calidad) ? r.calidad : "", num(r.n) ? r.n : "",
    ]);
  }
  const blob = new Blob([filas.map((f) => f.join(",")).join("\n")], { type: "text/csv" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `oximetro_${new Date().toISOString().slice(0, 16).replace(/[:T]/g, "-")}.csv`;
  a.click();
  URL.revokeObjectURL(a.href);
}

// -----------------------------------------------------------
// GRÁFICA
// -----------------------------------------------------------

function color(nombre) {
  return getComputedStyle(document.documentElement).getPropertyValue(nombre).trim();
}

function crearGrafica() {
  if (!window.Chart) return;
  Chart.defaults.font.family = color("--familia");
  Chart.defaults.color = color("--tinta-suave");

  grafica = new Chart($("grafica"), {
    type: "line",
    data: {
      datasets: [
        { label: "Pulso", yAxisID: "yBpm", data: [], borderColor: color("--pulso"), borderWidth: 2.25, pointRadius: 0, tension: 0.25 },
        { label: "SpO₂", yAxisID: "ySpo2", data: [], borderColor: color("--oxigeno"), borderWidth: 2.25, pointRadius: 0, tension: 0.25 },
        { label: "SpO₂ mínima", yAxisID: "ySpo2", data: [], borderColor: color("--oxigeno-claro"), borderWidth: 1.5, borderDash: [4, 3], pointRadius: 0, tension: 0.25 },
      ],
    },
    options: {
      parsing: false,
      normalized: true,
      animation: false,
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: "nearest", axis: "x", intersect: false },
      scales: {
        x: {
          type: "linear",
          grid: { display: false },
          border: { color: color("--rejilla-mayor") },
          ticks: { maxTicksLimit: 7, callback: (v) => etiquetaTiempo(v), maxRotation: 0 },
        },
        yBpm: {
          position: "left",
          suggestedMin: 50, suggestedMax: 110,
          grid: { display: false },
          border: { display: false },
          ticks: { color: color("--pulso") },
          title: { display: true, text: "lpm", color: color("--pulso") },
        },
        ySpo2: {
          position: "right",
          min: 80, max: 100,
          grid: { display: false },
          border: { display: false },
          ticks: { color: color("--oxigeno"), stepSize: 5 },
          title: { display: true, text: "SpO₂ %", color: color("--oxigeno") },
        },
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: color("--tinta"),
          titleColor: color("--papel"),
          bodyColor: color("--papel"),
          callbacks: {
            title: (items) => (items.length ? fmtDia.format(items[0].parsed.x) : ""),
            label: (item) => {
              const u = item.datasetIndex === 0 ? " lpm" : " %";
              return ` ${item.dataset.label}: ${item.parsed.y.toFixed(item.datasetIndex === 0 ? 0 : 1)}${u}`;
            },
          },
        },
      },
    },
  });

  actualizarGrafica();
}

// Inserta cortes (null) donde faltan datos para no unir huecos
function serie(registros, campo, huecoMs) {
  const out = [];
  let prev = null;
  for (const r of registros) {
    if (prev && r.t - prev > huecoMs) out.push({ x: prev + 1, y: null });
    out.push({ x: r.t, y: num(r[campo]) ? r[campo] : null });
    prev = r.t;
  }
  return out;
}

// Reduce a ~PUNTOS_MAX promediando por intervalos
function reducir(registros, desde, hasta) {
  if (registros.length <= PUNTOS_MAX) return registros;
  const paso = (hasta - desde) / PUNTOS_MAX;
  const cubos = new Map();
  for (const r of registros) {
    const k = Math.floor((r.t - desde) / paso);
    let c = cubos.get(k);
    if (!c) { c = { t: 0, n: 0, bpm: 0, nb: 0, spo2: 0, ns: 0, spo2Min: Infinity }; cubos.set(k, c); }
    c.t += r.t; c.n++;
    if (num(r.bpm)) { c.bpm += r.bpm; c.nb++; }
    if (num(r.spo2)) { c.spo2 += r.spo2; c.ns++; }
    const m = num(r.spo2Min) ? r.spo2Min : r.spo2;
    if (num(m) && m < c.spo2Min) c.spo2Min = m;
  }
  return [...cubos.values()].map((c) => ({
    t: c.t / c.n,
    bpm: c.nb ? c.bpm / c.nb : null,
    spo2: c.ns ? c.spo2 / c.ns : null,
    spo2Min: Number.isFinite(c.spo2Min) ? c.spo2Min : null,
  }));
}

function actualizarGrafica() {
  if (!grafica) return;
  const ahora = estado.fuente.ahora();
  const vivo = estado.rango === "vivo";
  const ventana = vivo ? VIVO_VENTANA_MS : Number(estado.rango);
  const desde = ahora - ventana;

  let datos;
  if (vivo) {
    // Contexto: historial de los últimos minutos antes del primer dato en vivo
    const primero = estado.vivo.length ? estado.vivo[0].t : Infinity;
    const previo = estado.historial.filter((r) => r.t >= desde && r.t < primero);
    datos = previo.concat(estado.vivo);
  } else {
    datos = reducir(estado.historial, desde, ahora);
  }
  const hueco = vivo ? 25000 : Math.max(60000, (ventana / PUNTOS_MAX) * 3);

  const [dPulso, dSpo2, dMin] = grafica.data.datasets;
  dPulso.data = serie(datos, "bpm", hueco);
  dSpo2.data = serie(datos, "spo2", hueco);
  dMin.data = vivo ? [] : serie(datos, "spo2Min", hueco);

  grafica.options.scales.x.min = desde;
  grafica.options.scales.x.max = ahora;
  grafica.update("none");

  const hayDatos = datos.some((r) => num(r.bpm) || num(r.spo2));
  const vacio = $("grafica-vacia");
  vacio.hidden = hayDatos;
  vacio.textContent = vivo
    ? "Coloca el dedo en el sensor: la tendencia en vivo aparece con la primera lectura estable."
    : "No hay registros en este periodo.";
}

// -----------------------------------------------------------
// FUENTES DE DATOS
// -----------------------------------------------------------

async function fuenteFirebase() {
  const [{ initializeApp }, authMod, dbMod, { firebaseConfig }] = await Promise.all([
    import(`${SDK}/firebase-app.js`),
    import(`${SDK}/firebase-auth.js`),
    import(`${SDK}/firebase-database.js`),
    import("./firebase-config.js"),
  ]);
  const { getAuth, onAuthStateChanged, signInWithEmailAndPassword, signOut } = authMod;
  const { getDatabase, ref, onValue, query, orderByChild, startAt } = dbMod;

  const app = initializeApp(firebaseConfig);
  const auth = getAuth(app);
  const db = getDatabase(app);

  let desfase = 0;
  onValue(ref(db, ".info/serverTimeOffset"), (s) => { desfase = s.val() || 0; });

  let uid = null;

  const errorLectura = (err) => {
    mostrarAviso(err && err.code === "PERMISSION_DENIED"
      ? "Esta cuenta no tiene permiso para leer datos. Entra con la cuenta del dispositivo y revisa las reglas de la base de datos."
      : `No se pudieron leer los datos: ${err.message}`);
  };

  return {
    demo: false,
    ahora: () => Date.now() + desfase,

    alCambiarSesion(cb) {
      onAuthStateChanged(auth, (u) => { uid = u ? u.uid : null; cb(u); });
    },

    entrar: (email, clave) => signInWithEmailAndPassword(auth, email, clave),
    salir: () => signOut(auth),

    suscribirActual(cb) {
      return onValue(ref(db, `oximetros/${uid}/actual`), (s) => { mostrarAviso(""); cb(s.val()); }, errorLectura);
    },

    suscribirHistorial(desde, cb) {
      const q = query(ref(db, `oximetros/${uid}/historial`), orderByChild("t"), startAt(desde));
      return onValue(q, (s) => {
        const lista = [];
        s.forEach((hijo) => { const v = hijo.val(); if (v && num(v.t)) lista.push(v); });
        lista.sort((a, b) => a.t - b.t);
        cb(lista);
      }, errorLectura);
    },
  };
}

function fuenteDemo() {
  // Simulación: reposo ~68 lpm con variación lenta y SpO2 ~97 %
  const inicio = Date.now();
  const bpmEn = (t) => 70 + 6 * Math.sin(t / 900000) + 3 * Math.sin(t / 97000) + (Math.random() - 0.5) * 2;
  const spo2En = (t) => 97 + 0.8 * Math.sin(t / 1300000) + (Math.random() - 0.5) * 0.6;

  const historial = [];
  for (let t = inicio - 7 * 86400000; t <= inicio; t += 10000) {
    // Simula uso intermitente: sesiones de ~25 min cada 3 h
    if (((inicio - t) / 60000) % 180 > 25) continue;
    const s = spo2En(t);
    historial.push({ t, bpm: bpmEn(t), spo2: s, spo2Min: s - Math.random() * 0.8, pi: 1 + Math.random(), calidad: 80 + Math.round(Math.random() * 20), n: 10 });
  }

  return {
    demo: true,
    ahora: () => Date.now(),
    alCambiarSesion(cb) { cb({ uid: "demo" }); },
    entrar: async () => {},
    salir: async () => { location.href = location.pathname; },

    suscribirActual(cb) {
      const emitir = () => {
        const t = Date.now();
        cb({ ts: t, bpm: bpmEn(t), spo2: spo2En(t), pi: 1.3, r: 0.52, calidad: 92, estado: "VALIDO", estadoCod: 3, rssi: -58, firmware: "2.0.0 demo" });
      };
      emitir();
      const id = setInterval(emitir, 2000);
      return () => clearInterval(id);
    },

    suscribirHistorial(desde, cb) {
      cb(historial.filter((r) => r.t >= desde));
      return () => {};
    },
  };
}

// -----------------------------------------------------------
// ARRANQUE
// -----------------------------------------------------------

function iniciarPanel() {
  mostrarPanel();
  estado.vivo = [];
  estado.actual = null;
  if (estado.cancelarActual) estado.cancelarActual();
  estado.cancelarActual = estado.fuente.suscribirActual(alRecibirActual);
  cambiarRango(document.querySelector('input[name="rango"]:checked').value);
}

function detenerPanel() {
  if (estado.cancelarActual) estado.cancelarActual();
  if (estado.cancelarHistorial) estado.cancelarHistorial();
  estado.cancelarActual = estado.cancelarHistorial = null;
}

async function arrancar() {
  const demo = new URLSearchParams(location.search).has("demo");

  try {
    estado.fuente = demo ? fuenteDemo() : await fuenteFirebase();
  } catch (err) {
    mostrarLogin("No se pudo cargar Firebase. Revisa tu conexión a internet y recarga la página.");
    console.error(err);
    return;
  }

  if (demo) {
    $("btn-salir").textContent = "Salir de la demostración";
  }

  estado.fuente.alCambiarSesion((usuario) => {
    if (usuario) iniciarPanel();
    else { detenerPanel(); mostrarLogin(); }
  });

  // Acceso
  $("form-login").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const boton = $("btn-entrar");
    boton.disabled = true;
    boton.textContent = "Entrando…";
    try {
      await estado.fuente.entrar($("login-email").value.trim(), $("login-clave").value);
      $("login-clave").value = "";
    } catch (err) {
      mostrarLogin(ERRORES_LOGIN[err.code] || `No se pudo entrar (${err.code || err.message}).`);
    } finally {
      boton.disabled = false;
      boton.textContent = "Entrar";
    }
  });

  $("btn-salir").addEventListener("click", () => estado.fuente.salir());
  $("btn-csv").addEventListener("click", descargarCSV);

  document.querySelectorAll('input[name="rango"]').forEach((r) =>
    r.addEventListener("change", (ev) => cambiarRango(ev.target.value)));

  // Refresca "hace X s", el estado de conexión y la ventana en vivo
  setInterval(() => {
    if (!estado.fuente || $("vista-panel").hidden) return;
    pintarActual();
    if (estado.rango === "vivo") actualizarGrafica();
  }, 1000);
}

// Chart.js se carga con defer; esperar a que el DOM esté listo
if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", arrancar);
} else {
  arrancar();
}
