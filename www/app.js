'use strict';

const Oris = {
  esc(value) {
    return String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  },
  query() {
    return new URLSearchParams(location.search);
  },
  async fetchJson(url, options) {
    const response = await fetch(url, Object.assign({credentials:'same-origin', cache:'no-store'}, options || {}));
    if (!response.ok) throw new Error((await response.text()) || ('HTTP ' + response.status));
    return response.json();
  },
  async fetchText(url, options) {
    const response = await fetch(url, Object.assign({credentials:'same-origin', cache:'no-store'}, options || {}));
    const text = await response.text();
    if (!response.ok) throw new Error(text || ('HTTP ' + response.status));
    return text;
  },
  formBody(values) {
    const body = new URLSearchParams();
    Object.keys(values).forEach(key => body.set(key, values[key] ?? ''));
    return body;
  },
  niceBytes(bytes) {
    bytes = Number(bytes) || 0;
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 ** 2) return (bytes / 1024).toFixed(1) + ' KB';
    if (bytes < 1024 ** 3) return (bytes / 1024 ** 2).toFixed(1) + ' MB';
    return (bytes / 1024 ** 3).toFixed(2) + ' GB';
  },
  toast(message, type) {
    const host = document.getElementById('toastHost');
    if (!host || !message) return;
    const toast = document.createElement('div');
    toast.className = 'toast ' + (type || '');
    toast.textContent = message;
    host.appendChild(toast);
    setTimeout(() => toast.remove(), 3400);
  },
  async postForm(url, values) {
    return this.fetchText(url, {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:this.formBody(values)
    });
  }
};

function mountShell() {
  if (!document.querySelector('header.app-header')) {
    document.body.insertAdjacentHTML('afterbegin', `
      <header class="app-header">
        <div class="nav-wrap">
          <a class="brand" href="/"><span class="brand-mark">O</span><span>ORIS radio</span></a>
          <button class="nav-toggle secondary" type="button" aria-expanded="false" onclick="toggleMainNav(this)">Menu</button>
          <nav id="mainNav" class="main-nav">
            <a data-nav="home" href="/">Přehrávač</a>
            <a data-nav="files" href="/files">Soubory</a>
            <a data-nav="radio" href="/radio">Rádio</a>
            <a data-nav="games" href="/games">Hry</a>
            <a data-nav="config" href="/config">Konfigurace</a>
            <a data-nav="update" href="/update">Firmware</a>
          </nav>
        </div>
      </header>`);
  }
  if (!document.getElementById('globalStatus')) {
    document.body.insertAdjacentHTML('beforeend', `
      <div id="globalStatus" class="statusbar">
        <span id="barHeap">Heap: …</span><span id="barPsram">PSRAM: …</span>
        <span id="barUsb">USB: …</span><span id="barRssi">Wi-Fi: …</span>
        <span id="barBattery">Baterie: …</span><span id="barAudio">Audio: …</span><span id="barUptime">Uptime: …</span>
        <span id="barAudioDetail" class="detail"></span>
      </div>`);
  }
  if (!document.getElementById('toastHost')) {
    document.body.insertAdjacentHTML('beforeend','<div id="toastHost" class="toast-host"></div>');
  }
  markActiveNavigation();
}

function toggleMainNav(button) {
  const nav = document.getElementById('mainNav');
  if (!nav) return;
  const open = nav.classList.toggle('open');
  if (button) button.setAttribute('aria-expanded', open ? 'true' : 'false');
}

function markActiveNavigation() {
  const path = location.pathname;
  let key = path === '/' ? 'home' : 'files';
  if (path.startsWith('/radio')) key = 'radio';
  else if (path.startsWith('/games') || path.startsWith('/game/')) key = 'games';
  else if (path.startsWith('/config') || path.startsWith('/wifi/')) key = 'config';
  else if (path.startsWith('/update') || path.startsWith('/firmware')) key = 'update';
  document.querySelectorAll('[data-nav]').forEach(link => link.classList.toggle('active', link.dataset.nav === key));
}

function setStatusText(id, value, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = value;
  if (cls !== undefined) el.className = cls || '';
}

let volumeTimer = null;
function updateAudioVolumeDisplay(v) {
  v = Math.max(0, Math.min(100, parseInt(v, 10) || 0));
  document.querySelectorAll('[data-audio-volume-label]').forEach(el => el.textContent = v + ' %');
  document.querySelectorAll('[data-audio-volume-range]').forEach(el => { if (document.activeElement !== el) el.value = v; });
}
function updateAudioLabels(text) {
  document.querySelectorAll('[data-audio-status]').forEach(el => el.textContent = text || '');
}
function setAudioVolume(v) {
  updateAudioVolumeDisplay(v);
  clearTimeout(volumeTimer);
  volumeTimer = setTimeout(async () => {
    try { updateAudioVolumeDisplay(await Oris.fetchText('/audio/volume?v=' + encodeURIComponent(v))); }
    catch (e) { Oris.toast(e.message, 'bad'); }
    finally { volumeTimer = null; }
  }, 120);
}
async function audioAction(url, playing = true) {
  try {
    const text = await Oris.fetchText(url, {method:'POST'});
    updateAudioLabels(text);
    Oris.toast(text, 'good');
  } catch (e) { Oris.toast(e.message, 'bad'); }
  return false;
}
async function playAudioNoReload(url) {
  try { const text = await Oris.fetchText(url); updateAudioLabels(text); Oris.toast(text, 'good'); }
  catch (e) { Oris.toast(e.message, 'bad'); }
  return false;
}
async function playRadioNoReload(index) {
  try { const text = await Oris.fetchText('/radio/play?i=' + encodeURIComponent(index), {method:'POST'}); updateAudioLabels(text); Oris.toast(text, 'good'); }
  catch (e) { Oris.toast(e.message, 'bad'); }
  return false;
}
function stopAudioAjax() { return audioAction('/audio/stop_ajax', false); }

async function refreshBottomStatus() {
  try {
    const st = await Oris.fetchJson('/status.json');
    setStatusText('barHeap', `Heap: ${st.heapUsed} / ${st.heapTotal}`);
    setStatusText('barPsram', `PSRAM: ${st.psramUsed} / ${st.psramTotal}`, st.psramOk ? 'good' : 'warn');
    setStatusText('barUsb', 'USB: ' + st.usb, st.usbOk ? 'good' : 'warn');
    setStatusText('barRssi', st.wifiConnected ? `Wi-Fi: ${st.rssi} dBm` : 'Wi-Fi: jen AP', st.wifiConnected ? 'good' : 'warn');
    if (!st.batteryEnabled) {
      setStatusText('barBattery', 'Baterie: vypnuto', '');
    } else if (st.batteryValid) {
      setStatusText('barBattery', `Baterie: ${st.batteryPercent} % / ${Number(st.batteryVoltage).toFixed(2)} V`, st.batteryPercent <= 15 ? 'bad' : (st.batteryPercent <= 30 ? 'warn' : 'good'));
    } else {
      setStatusText('barBattery', 'Baterie: bez měření', 'warn');
    }
    setStatusText('barAudio', 'Audio: ' + st.audio, st.audioPlaying ? 'good' : 'warn');
    setStatusText('barUptime', 'Uptime: ' + st.uptime);
    setStatusText('barAudioDetail', (st.audioPlaying ? '▶ ' : '') + (st.audioDetail || st.audio || ''));
    updateAudioLabels(st.audioDetail || st.audio || '');
    if (typeof st.audioVolume !== 'undefined' && !volumeTimer) updateAudioVolumeDisplay(st.audioVolume);
  } catch (_) {}
}

function renameEntry(disk, path, returnPath, oldName) {
  const newName = prompt('Nový název:', oldName || '');
  if (newName === null) return false;
  const value = newName.trim();
  if (!value || value === oldName) return false;
  if (value.includes('/') || value.includes('\\') || value.includes('..')) {
    Oris.toast('Název nesmí obsahovat /, \\ ani ..', 'bad'); return false;
  }
  const form = document.createElement('form');
  form.method = 'POST'; form.action = '/rename'; form.hidden = true;
  Object.entries({disk, f:path, p:returnPath || '/', name:value}).forEach(([name,val]) => {
    const input = document.createElement('input'); input.name = name; input.value = val; form.appendChild(input);
  });
  document.body.appendChild(form); form.submit(); return false;
}

function ensureUploadModal() {
  let modal = document.getElementById('uploadModal');
  if (modal) return modal;
  document.body.insertAdjacentHTML('beforeend', `<div id="uploadModal" class="upload-modal"><div class="upload-box"><div class="upload-title">Nahrávání souboru</div><div id="uploadFileName" class="upload-file"></div><div class="upload-bar"><div id="uploadFill" class="upload-fill"></div></div><div id="uploadPercent" class="upload-percent">0 %</div><div id="uploadError" class="upload-error"></div></div></div>`);
  return document.getElementById('uploadModal');
}
function uploadWithModal(form) {
  const fileInput = form.querySelector('input[type=file]');
  if (!fileInput?.files?.length) { Oris.toast('Vyber soubor k nahrání.', 'bad'); return false; }
  const modal = ensureUploadModal();
  const fill = document.getElementById('uploadFill');
  const pct = document.getElementById('uploadPercent');
  const err = document.getElementById('uploadError');
  document.getElementById('uploadFileName').textContent = fileInput.files[0].name + ' (' + Oris.niceBytes(fileInput.files[0].size) + ')';
  fill.style.width='0%'; pct.textContent='0 %'; err.style.display='none'; modal.classList.add('show');
  const xhr = new XMLHttpRequest();
  xhr.upload.onprogress = e => { if (e.lengthComputable) { const p=Math.round(e.loaded/e.total*100); fill.style.width=p+'%'; pct.textContent=`${p} % — ${Oris.niceBytes(e.loaded)} / ${Oris.niceBytes(e.total)}`; } };
  xhr.onload = () => { if (xhr.status >= 200 && xhr.status < 400) { fill.style.width='100%'; pct.textContent='Hotovo, obnovuji…'; setTimeout(()=>location.reload(),500); } else { err.style.display='block'; err.textContent='Upload selhal: HTTP '+xhr.status; } };
  xhr.onerror = () => { err.style.display='block'; err.textContent='Upload selhal: chyba spojení.'; };
  xhr.open(form.method || 'POST', form.action, true); xhr.send(new FormData(form)); return false;
}

let wifiScanTimer = null;
async function startWifiScan() {
  const button = document.getElementById('wifiScanButton');
  const status = document.getElementById('wifiScanStatus');
  if (button) button.disabled = true;
  if (status) status.textContent = 'Spouštím vyhledávání…';
  try { await Oris.fetchText('/wifi/scan/start', {method:'POST'}); pollWifiScan(); }
  catch (e) { if (status) status.textContent=e.message; if (button) button.disabled=false; }
  return false;
}
async function pollWifiScan() {
  const button = document.getElementById('wifiScanButton');
  const status = document.getElementById('wifiScanStatus');
  try {
    const data = await Oris.fetchJson('/wifi/scan.json');
    if (data.running) { if (status) status.textContent='Vyhledávám…'; wifiScanTimer=setTimeout(pollWifiScan,500); return; }
    if (button) button.disabled=false;
    if (!data.ok) { if (status) status.textContent=data.error || 'Scan selhal'; return; }
    const select=document.getElementById('wifiScanList');
    if (select) {
      select.innerHTML='';
      (data.networks || []).forEach(n => { const o=document.createElement('option'); o.value=n.ssid; o.textContent=`${n.ssid} (${n.rssi} dBm, ${n.secure?'zabezpečená':'otevřená'})`; select.appendChild(o); });
      if (!select.options.length) select.add(new Option('Žádná síť nenalezena',''));
      const input=document.getElementById('wifiSsidInput'); if (input && select.value) input.value=select.value;
    }
    if (status) status.textContent='Nalezeno sítí: ' + (data.networks || []).length;
  } catch (e) { if (button) button.disabled=false; if (status) status.textContent=e.message; }
}

window.Oris=Oris;
window.toggleMainNav=toggleMainNav;
window.setAudioVolume=setAudioVolume;
window.updateAudioVolumeDisplay=updateAudioVolumeDisplay;
window.updateAudioLabels=updateAudioLabels;
window.audioAction=audioAction;
window.playAudioNoReload=playAudioNoReload;
window.playRadioNoReload=playRadioNoReload;
window.stopAudioAjax=stopAudioAjax;
window.renameEntry=renameEntry;
window.uploadWithModal=uploadWithModal;
window.startWifiScan=startWifiScan;

document.addEventListener('DOMContentLoaded', () => {
  mountShell();
  refreshBottomStatus();
  setInterval(refreshBottomStatus, 2000);
});
