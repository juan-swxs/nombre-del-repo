/* ================================================================
   DRAFT CONTROL — app.js
   Comunicación HTTP con ESP32 via WiFi
   Endpoints esperados en el ESP32:
     POST /relay  { "ch": 1-3, "state": 0|1 }
     POST /stop   { "all": 1 }
     GET  /ping   → responde 200 OK con {"status":"ok"}
   ================================================================ */

'use strict';

/* ── Estado global ── */
const state = {
  activeRelay: 0,          // 0 = ninguno activo
  pourProgress: [0, 0, 0], // progreso visual de cada grifo (0–100)
  pourTimers: [null, null, null],
  connected: false,
};

/* ── Nombres de las cervezas ── */
const BEER_NAMES = ['', 'Rubia', 'Roja', 'Negra'];

/* ── Obtener IP actual ── */
function getIP() {
  const sel = document.getElementById('ip-select').value;
  if (sel === 'custom') {
    const custom = document.getElementById('custom-ip').value.trim();
    return custom || '192.168.1.100';
  }
  return sel;
}

/* ── Log ── */
function log(msg, type = '') {
  const el = document.getElementById('log-text');
  el.textContent = msg;
  el.className = 'log-text ' + type;
}

/* ── Estado de conexión ── */
function setConnState(connected) {
  state.connected = connected;
  const dot   = document.getElementById('conn-dot');
  const label = document.getElementById('conn-label');
  if (connected) {
    dot.className   = 'conn-dot connected';
    label.textContent = getIP();
  } else {
    dot.className   = 'conn-dot disconnected';
    label.textContent = 'Sin conexión';
  }
}

/* ── Petición HTTP genérica ── */
async function httpPost(path, body) {
  const ip  = getIP();
  const url = `http://${ip}${path}`;
  try {
    const res = await fetch(url, {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(body),
      signal:  AbortSignal.timeout(2500),
    });
    setConnState(res.ok);
    return res.ok;
  } catch (err) {
    setConnState(false);
    log(`Error → ${ip} (${err.message || 'timeout'})`, 'error');
    return false;
  }
}

async function httpGet(path) {
  const ip  = getIP();
  const url = `http://${ip}${path}`;
  try {
    const res = await fetch(url, {
      method: 'GET',
      signal: AbortSignal.timeout(2000),
    });
    setConnState(res.ok);
    return res;
  } catch (err) {
    setConnState(false);
    log(`Sin respuesta → ${ip}`, 'error');
    return null;
  }
}

/* ── Ping ── */
async function pingDevice() {
  const btn = document.getElementById('btn-ping');
  btn.disabled = true;
  log(`Haciendo ping a ${getIP()}…`, 'warn');

  const res = await httpGet('/ping');
  if (res && res.ok) {
    log(`${getIP()} respondió correctamente`, 'ok');
  } else {
    log(`${getIP()} no responde`, 'error');
  }
  btn.disabled = false;
}

/* ── Limpiar estado visual de un relé ── */
function clearCard(n) {
  const card = document.getElementById('card-' + n);
  const btn  = document.getElementById('btn-' + n);
  const lbl  = document.getElementById('label-' + n);
  const bar  = document.getElementById('pour-' + n);

  card.classList.remove('active');
  btn.classList.remove('pouring');
  lbl.textContent = 'Servir';
  bar.style.width = '0%';

  clearInterval(state.pourTimers[n - 1]);
  state.pourTimers[n - 1]  = null;
  state.pourProgress[n - 1] = 0;
}

/* ── Animación de progreso de servido ── */
function startPourAnimation(n) {
  state.pourProgress[n - 1] = 0;
  const bar = document.getElementById('pour-' + n);

  state.pourTimers[n - 1] = setInterval(() => {
    if (state.pourProgress[n - 1] < 100) {
      state.pourProgress[n - 1] += 0.4;
      bar.style.width = state.pourProgress[n - 1].toFixed(1) + '%';
    }
  }, 50);
}

/* ── Servir cerveza ── */
async function serveBeer(n) {
  /* Si ya está activo este grifo → cerrar */
  if (state.activeRelay === n) {
    clearCard(n);
    state.activeRelay = 0;
    const ok = await httpPost('/relay', { ch: n, state: 0 });
    log(ok
      ? `Grifo ${n} cerrado · ${BEER_NAMES[n]} detenida`
      : `Error al cerrar grifo ${n}`,
      ok ? '' : 'error'
    );
    return;
  }

  /* Cerrar el grifo anterior si existe */
  if (state.activeRelay !== 0) {
    const prev = state.activeRelay;
    clearCard(prev);
    await httpPost('/relay', { ch: prev, state: 0 });
  }

  /* Activar nuevo grifo */
  state.activeRelay = n;

  const card = document.getElementById('card-' + n);
  const btn  = document.getElementById('btn-' + n);
  const lbl  = document.getElementById('label-' + n);

  card.classList.add('active');
  btn.classList.add('pouring');
  lbl.textContent = 'Sirviendo';
  startPourAnimation(n);

  log(`Sirviendo ${BEER_NAMES[n]} · relé ${n} activo → ${getIP()}`, 'ok');

  const ok = await httpPost('/relay', { ch: n, state: 1 });
  if (!ok) {
    clearCard(n);
    state.activeRelay = 0;
    log(`No se pudo activar grifo ${n}`, 'error');
  }
}

/* ── Paro de emergencia ── */
async function emergencyStop() {
  const prev = state.activeRelay;

  /* Limpiar visualmente todos */
  [1, 2, 3].forEach(n => clearCard(n));
  state.activeRelay = 0;

  log('PARO DE EMERGENCIA · cerrando todos los relés…', 'error');

  const ok = await httpPost('/stop', { all: 1 });
  log(
    ok
      ? 'Todos los relés cerrados'
      : 'Error en paro — verificar ESP32',
    ok ? 'warn' : 'error'
  );
}

/* ── Selector de IP ── */
document.getElementById('ip-select').addEventListener('change', function () {
  const wrap = document.getElementById('custom-ip-wrap');
  if (this.value === 'custom') {
    wrap.classList.add('show');
    document.getElementById('custom-ip').focus();
  } else {
    wrap.classList.remove('show');
  }
  setConnState(false);
});

/* ── Auto-ping inicial ── */
window.addEventListener('DOMContentLoaded', () => {
  setTimeout(pingDevice, 800);
});