'use strict';

const $ = (id) => document.getElementById(id);
const API_BASE = new URLSearchParams(window.location.search).get('api') || '';
const isStaticPreview = ['github.io', 'localhost', '127.0.0.1'].some((host) => window.location.hostname.endsWith(host));
let token = localStorage.getItem('sf_token') || '';
let last = [];
let previewMode = false;

const demoStatus = {
  version: 'SmartFarm Pro Preview',
  wifi: { connected: false, rssi: 0, reconnects: 0 },
  mqtt: false,
  heap: 0,
  uptime: 0,
  temperature: 28.5,
  humidity: 64.0,
  relays: [false, false, false, false]
};

function setText(id, value) {
  const node = $(id);
  if (node) node.textContent = value;
}

function toast(message) {
  const node = $('toast');
  if (!node) return;
  node.textContent = message;
  node.classList.remove('hidden');
  window.setTimeout(() => node.classList.add('hidden'), 2500);
}

function showPreviewNote(show) {
  previewMode = show;
  const node = $('offlineNote');
  if (node) node.classList.toggle('hidden', !show);
}

function apiUrl(path) {
  // API paths stay relative so GitHub Pages sub-paths do not accidentally resolve to the domain root.
  return API_BASE ? new URL(path.replace(/^\//, ''), API_BASE.endsWith('/') ? API_BASE : `${API_BASE}/`).toString() : `.${path}`;
}

async function api(path, options = {}) {
  const request = { ...options, headers: { ...(options.headers || {}), 'X-Auth-Token': token } };
  const response = await fetch(apiUrl(path), request);
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

function renderStatus(status) {
  $('wifi').className = `status ${status.wifi.connected ? 'ok' : ''}`;
  $('mqtt').className = `status ${status.mqtt ? 'ok' : ''}`;
  setText('temp', `${Number(status.temperature || 0).toFixed(1)} °C`);
  setText('hum', `${Number(status.humidity || 0).toFixed(1)} %`);
  setText('rssi', `${status.wifi.rssi} dBm`);
  setText('heap', `${status.heap} bytes`);
  setText('uptime', `${status.uptime} s`);
  const relays = Array.isArray(status.relays) ? status.relays : demoStatus.relays;
  const relayContainer = $('relays');
  if (relayContainer) {
    relayContainer.innerHTML = relays.map((on, i) => `<button class="btn ${on ? 'bad' : 'ghost'}" type="button" onclick="relay(${i + 1},${!on})">Relay ${i + 1}: ${on ? 'ON' : 'OFF'}</button>`).join(' ');
  }
  last.push({ t: new Date().toLocaleTimeString(), temp: Number(status.temperature) || 0, hum: Number(status.humidity) || 0 });
  last = last.slice(-20);
  draw();
}

async function refresh() {
  try {
    const status = await api('/api/status');
    showPreviewNote(false);
    renderStatus(status);
  } catch (error) {
    // GitHub Pages cannot reach the ESP REST API unless the user supplies ?api=http://device.local/.
    if (!previewMode && isStaticPreview) toast('Static preview mode: add ?api=http://DEVICE-IP/ to connect to hardware');
    demoStatus.uptime += 3;
    showPreviewNote(true);
    renderStatus(demoStatus);
  }
}

async function relay(index, on) {
  if (previewMode) {
    demoStatus.relays[index - 1] = on;
    renderStatus(demoStatus);
    toast('Preview relay updated locally');
    return;
  }
  try {
    await api('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ relay: String(index), state: on ? 'ON' : 'OFF' })
    });
    toast('Relay updated');
    await refresh();
  } catch (error) {
    toast(`Relay update failed: ${error.message}`);
  }
}

function login() {
  token = 'admin';
  localStorage.setItem('sf_token', token);
  toast('Logged in as Admin');
}

function draw() {
  const canvas = $('chart');
  if (!canvas || !canvas.getContext) return;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = '#10b981';
  ctx.lineWidth = 2;
  ctx.beginPath();
  last.forEach((point, index) => {
    const x = index * (canvas.width / Math.max(1, last.length - 1));
    const y = canvas.height - Math.min(canvas.height - 8, Math.max(8, point.temp * 3));
    if (index) ctx.lineTo(x, y); else ctx.moveTo(x, y);
  });
  ctx.stroke();
}

function theme() {
  document.body.classList.toggle('dark');
  localStorage.setItem('sf_dark', String(document.body.classList.contains('dark')));
}

async function registerServiceWorker() {
  if (!('serviceWorker' in navigator) || !window.isSecureContext) return;
  try {
    await navigator.serviceWorker.register('./sw.js');
  } catch (error) {
    // Registration can fail on ESP HTTP or unsupported browsers; keep dashboard usable.
    console.info('Service worker not registered:', error.message);
  }
}

window.addEventListener('load', () => {
  if (localStorage.getItem('sf_dark') === 'true') document.body.classList.add('dark');
  refresh();
  window.setInterval(refresh, 3000);
  registerServiceWorker();
});
