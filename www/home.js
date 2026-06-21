'use strict';

let homeStations = [];
let homeState = null;
let homeSelectedStation = -1;
let homeRefreshTimer = null;

function homeDefaultLogo(mode) {
  if (mode === 'radio') return '/www/icons/radio.svg';
  if (mode === 'soubor' || mode === 'složka') return '/www/icons/music.svg';
  return '/www/icons/radio.svg';
}

function stationLogo(station) {
  return station && station.logo ? station.logo : '/www/icons/radio.svg';
}

function imageFallback(image, fallback) {
  image.onerror = null;
  image.src = fallback;
}

function renderHomeStations() {
  const host = document.getElementById('homeStations');
  const available = homeStations.filter(station => station.url);
  if (!available.length) {
    host.innerHTML = '<div class="empty warn">Nejdřív nastav alespoň jednu stanici na stránce Rádio.</div>';
    return;
  }

  host.innerHTML = available.map(station => {
    const active = Number(homeState?.radioIndex) === station.index;
    const selected = homeSelectedStation === station.index;
    return `<button type="button" class="station-tile ${active ? 'active' : ''} ${selected ? 'selected' : ''}" onclick="return homePlayStation(${station.index})">
      <img src="${Oris.esc(stationLogo(station))}" alt="" onerror="imageFallback(this,'/www/icons/radio.svg')">
      <span>${Oris.esc(station.name || ('Stanice ' + (station.index + 1)))}</span>
      ${active ? '<small>PRÁVĚ HRAJE</small>' : '<small>PŘEPNOUT</small>'}
    </button>`;
  }).join('');
}

function updateHomeBattery(state) {
  const widget = document.getElementById('homeBatteryWidget');
  const fill = document.getElementById('homeBatteryFill');
  const percent = document.getElementById('homeBatteryPercent');
  const voltage = document.getElementById('homeBatteryVoltage');

  widget.classList.remove('low', 'medium', 'disabled');
  if (!state.batteryEnabled) {
    widget.classList.add('disabled');
    fill.style.width = '0%';
    percent.textContent = '--';
    voltage.textContent = 'Měření vypnuto';
    return;
  }
  if (!state.batteryValid) {
    widget.classList.add('disabled');
    fill.style.width = '0%';
    percent.textContent = '?';
    voltage.textContent = 'Bez měření';
    return;
  }

  const value = Math.max(0, Math.min(100, Number(state.batteryPercent) || 0));
  fill.style.width = value + '%';
  percent.textContent = value + '%';
  voltage.textContent = Number(state.batteryVoltage).toFixed(2) + ' V';
  if (value <= 15) widget.classList.add('low');
  else if (value <= 35) widget.classList.add('medium');
}

function updateHomePlayer(state) {
  homeState = state;
  const mode = state.audio || 'připraveno';
  const cover = document.getElementById('homeCover');
  const title = document.getElementById('homeTitle');
  const subtitle = document.getElementById('homeSubtitle');
  const badge = document.getElementById('homeMode');
  const play = document.getElementById('homePlayPause');

  let coverPath = homeDefaultLogo(mode);
  let modeLabel = 'PŘIPRAVENO';
  let subtitleText = 'Vyber stanici a spusť přehrávání.';

  if (mode === 'radio') {
    coverPath = state.radioLogo || homeDefaultLogo('radio');
    modeLabel = state.audioPaused ? 'RÁDIO · PAUZA' : 'INTERNETOVÉ RÁDIO';
    subtitleText = state.audioPaused ? 'Přehrávání je pozastavené.' : 'Živý MP3 stream';
    if (Number(state.radioIndex) >= 0) homeSelectedStation = Number(state.radioIndex);
  } else if (mode === 'soubor' || mode === 'složka') {
    coverPath = homeDefaultLogo('soubor');
    modeLabel = state.audioPaused ? 'HUDBA · PAUZA' : (mode === 'složka' ? 'PLAYLIST' : 'HUDBA');
    subtitleText = mode === 'složka' ? 'Přehrávání MP3 ze složky' : 'Lokální MP3 soubor';
  }

  cover.src = coverPath;
  cover.onerror = () => imageFallback(cover, homeDefaultLogo(mode));
  title.textContent = state.audioTitle || 'Přehrávač připraven';
  subtitle.textContent = subtitleText;
  badge.textContent = modeLabel;
  document.getElementById('homeAudioStatus').textContent = state.audioDetail || state.audio || '';
  play.textContent = state.audioPaused ? '▶' : (state.audioPlaying ? 'Ⅱ' : '▶');
  play.title = state.audioPaused ? 'Pokračovat' : (state.audioPlaying ? 'Pozastavit' : 'Přehrát');

  updateHomeBattery(state);
  renderHomeStations();
}

async function loadHomeStations() {
  try {
    const data = await Oris.fetchJson('/api/config.json');
    homeStations = data.radioStations || [];
    const first = homeStations.find(station => station.url);
    if (homeSelectedStation < 0 && first) homeSelectedStation = first.index;
    updateAudioVolumeDisplay(data.audioVolume);
    renderHomeStations();
  } catch (error) {
    document.getElementById('homeStations').innerHTML = '<div class="empty bad">' + Oris.esc(error.message) + '</div>';
  }
}

async function refreshHomeStatus() {
  try {
    const state = await Oris.fetchJson('/status.json');
    updateHomePlayer(state);
    if (typeof state.audioVolume !== 'undefined' && !volumeTimer) updateAudioVolumeDisplay(state.audioVolume);
  } catch (_) {}
}

async function homePlayStation(index) {
  homeSelectedStation = Number(index);
  renderHomeStations();
  try {
    const text = await Oris.fetchText('/radio/play?i=' + encodeURIComponent(index), {method:'POST'});
    Oris.toast(text, 'good');
    setTimeout(refreshHomeStatus, 250);
  } catch (error) {
    Oris.toast(error.message, 'bad');
  }
  return false;
}

function availableHomeStations() {
  return homeStations.filter(station => station.url);
}

function relativeStation(delta) {
  const available = availableHomeStations();
  if (!available.length) return false;
  let current = available.findIndex(station => station.index === homeSelectedStation);
  if (current < 0 && Number(homeState?.radioIndex) >= 0) current = available.findIndex(station => station.index === Number(homeState.radioIndex));
  if (current < 0) current = 0;
  current = (current + delta + available.length) % available.length;
  return homePlayStation(available[current].index);
}

async function homePlaylistStep(direction) {
  const url = direction < 0 ? '/audio/prev' : '/audio/next';
  try {
    const text = await Oris.fetchText(url, {method:'POST'});
    Oris.toast(text, 'good');
    setTimeout(refreshHomeStatus, 180);
  } catch (error) {
    Oris.toast(error.message, 'bad');
  }
  return false;
}

function homePreviousStation() {
  if (homeState?.audio === 'složka') return homePlaylistStep(-1);
  return relativeStation(-1);
}

function homeNextStation() {
  if (homeState?.audio === 'složka') return homePlaylistStep(1);
  return relativeStation(1);
}

async function homeTogglePlay() {
  if (homeState?.audioPlaying || homeState?.audioPaused) {
    try {
      const text = await Oris.fetchText('/audio/toggle_pause', {method:'POST'});
      Oris.toast(text, 'good');
      setTimeout(refreshHomeStatus, 180);
    } catch (error) { Oris.toast(error.message, 'bad'); }
    return false;
  }

  const available = availableHomeStations();
  if (!available.length) {
    Oris.toast('Není nastavená žádná stanice.', 'bad');
    return false;
  }
  const selected = available.find(station => station.index === homeSelectedStation) || available[0];
  return homePlayStation(selected.index);
}

async function homeStop() {
  try {
    const text = await Oris.fetchText('/audio/stop_ajax', {method:'POST'});
    Oris.toast(text, 'good');
    setTimeout(refreshHomeStatus, 180);
  } catch (error) { Oris.toast(error.message, 'bad'); }
  return false;
}

window.imageFallback = imageFallback;
window.homePlayStation = homePlayStation;
window.homePreviousStation = homePreviousStation;
window.homeNextStation = homeNextStation;
window.homeTogglePlay = homeTogglePlay;
window.homeStop = homeStop;

document.addEventListener('DOMContentLoaded', () => {
  loadHomeStations();
  refreshHomeStatus();
  homeRefreshTimer = setInterval(refreshHomeStatus, 1200);
});
