'use strict';
let configData=null;
function setField(name,value){const el=document.querySelector(`[name="${name}"]`);if(!el)return;if(el.type==='checkbox')el.checked=!!value;else el.value=value??'';}
function dbLabel(value){value=Number(value)||0;return (value>0?'+':'')+value+' dB';}
function updateEqLabels(){
  const preamp=document.getElementById('audioEqPreampRange');
  const output=document.getElementById('audioOutputGainRange');
  const curve=document.getElementById('audioVolumeCurveRange');
  if(preamp)document.getElementById('audioEqPreampLabel').textContent=dbLabel(preamp.value);
  if(output)document.getElementById('audioOutputGainLabel').textContent=dbLabel(output.value);
  if(curve)document.getElementById('audioVolumeCurveLabel').textContent=Number(curve.value).toFixed(1)+'×';
  for(let i=0;i<10;i++){
    const input=document.getElementById('eqBandRange'+i);
    const label=document.getElementById('eqBandLabel'+i);
    if(input&&label)label.textContent=dbLabel(input.value);
  }
}
function setEqBands(values){
  for(let i=0;i<10;i++){
    const input=document.getElementById('eqBandRange'+i);
    if(input)input.value=Number(values[i]??0);
  }
  updateEqLabels();
}
function resetEq(){setEqBands([0,0,0,0,0,0,0,0,0,0]);const p=document.getElementById('audioEqPreampRange');if(p)p.value=0;updateEqLabels();}
function applyEqPreset(name){
  const presets={
    flat:[0,0,0,0,0,0,0,0,0,0],
    bass:[5,5,4,3,1,0,-1,-1,0,1],
    voice:[-5,-4,-2,0,2,4,4,2,0,-2],
    loudness:[5,4,3,1,-1,-2,-1,1,3,4]
  };
  setEqBands(presets[name]||presets.flat);
}
function applySafeGain(){
  const output=document.getElementById('audioOutputGainRange');
  const curve=document.getElementById('audioVolumeCurveRange');
  if(output)output.value=-18;
  if(curve)curve.value=2.0;
  updateEqLabels();
}
function updateConfigVolume(value){document.getElementById('configAudioVolumeLabel').textContent=value+' %';}
function renderStatus(d){
  const ffatFree=Math.max(0,d.ffatTotal-d.ffatUsed);
  const cards=[
    ['AP hotspot',d.apRunning?d.apSsid:'Vypnuto',d.apRunning?(`IP ${d.apIp}`):('Režim '+({0:'vždy zapnuto',1:'automaticky',2:'vždy vypnuto'}[Number(d.apMode)]||d.apMode)),d.apRunning?'good':'warn'],
    ['Wi-Fi klient',d.wifiConnected?d.wifiSsid:'Nepřipojeno',d.wifiConnected?`${d.wifiIp} · ${d.wifiRssi} dBm`:d.wifiStatus,d.wifiConnected?'good':'warn'],
    ['mDNS',d.mdnsName+'.local',d.mdnsStarted?'aktivní':'čeká',d.mdnsStarted?'good':'warn'],
    ['Interní FFat',Oris.niceBytes(d.ffatUsed)+' / '+Oris.niceBytes(d.ffatTotal),'Volno '+Oris.niceBytes(ffatFree),''],
    ['USB disk',d.usbMounted?'Připojeno':'Nepřipojeno',d.usbStatus,d.usbMounted?'good':'warn'],
    ['Baterie',!d.batteryEnabled?'Měření vypnuto':(d.batteryValid?(d.batteryPercent+' %'):'Bez měření'),d.batteryValid?(Number(d.batteryVoltage).toFixed(2)+' V · ADC '+d.batteryRawMv+' mV'):'GPIO '+d.batteryPin,d.batteryValid?(d.batteryPercent<=15?'bad':(d.batteryPercent<=30?'warn':'good')):'warn'],
    ['Audio',d.audioOutput,d.audioStatus,d.audioReady?'good':'warn']
  ];
  document.getElementById('statusCards').innerHTML=cards.map(c=>`<div class="metric"><div class="metric-title">${Oris.esc(c[0])}</div><div class="metric-value ${c[3]}">${Oris.esc(c[1])}</div><div class="metric-detail">${Oris.esc(c[2])}</div></div>`).join('');
}
function renderSavedWifi(d){
  const list=d.savedWifi||[];
  if(!list.length){document.getElementById('savedWifiList').innerHTML='<div class="empty warn">Zatím není uložená žádná Wi-Fi.</div>';return;}
  let html='<table><thead><tr><th>#</th><th>SSID</th><th>Stav</th><th>Akce</th></tr></thead><tbody>';
  list.forEach(w=>{html+=`<tr><td>${w.index+1}</td><td>${Oris.esc(w.ssid)}</td><td class="${w.connected?'good':''}">${w.connected?'připojeno':'uloženo'}</td><td class="actions"><form method="POST" action="/wifi/connect" style="display:inline"><input type="hidden" name="i" value="${w.index}"><button class="secondary" type="submit">Připojit</button></form><form method="POST" action="/wifi/delete" style="display:inline" onsubmit="return confirm('Smazat uloženou Wi-Fi?')"><input type="hidden" name="i" value="${w.index}"><button class="danger" type="submit">Smazat</button></form></td></tr>`;});
  html+='</tbody></table>';document.getElementById('savedWifiList').innerHTML=html;
}
async function loadConfig(){
  try{
    const d=configData=await Oris.fetchJson('/api/config.json');
    renderStatus(d);renderSavedWifi(d);
    setField('ap_mode',d.apMode);setField('ap_ssid',d.apSsid);setField('ap_pass',d.apPass);setField('mdns_name',d.mdnsName);setField('web_user',d.webUser);setField('web_pass',d.webPass);setField('ftp_enabled',d.ftpEnabled);setField('ftp_user',d.ftpUser);setField('ftp_pass',d.ftpPass);setField('ftp_disk',d.ftpDisk);setField('rgb_enabled',d.rgbEnabled);setField('audio_volume',d.audioVolume);setField('audio_eq_enabled',d.audioEqEnabled?1:0);setField('audio_eq_preamp_db',d.audioEqPreampDb??0);setField('audio_eq_auto_headroom',d.audioEqAutoHeadroom?1:0);setField('audio_output_gain_db',d.audioOutputGainDb??-12);setField('audio_volume_curve',d.audioVolumeCurve??1.8);setField('battery_enabled',d.batteryEnabled);setField('battery_divider_ratio',d.batteryDividerRatio);setField('battery_calibration',d.batteryCalibration);
    let bands=Array.isArray(d.audioEqBands)?d.audioEqBands:null;
    if(!bands)bands=[d.audioBassDb||0,d.audioBassDb||0,d.audioBassDb||0,d.audioBassDb||0,0,0,0,d.audioTrebleDb||0,d.audioTrebleDb||0,d.audioTrebleDb||0];
    setEqBands(bands);
    const first=(d.radioStations||[])[0]||{};setField('radio_name',first.name);setField('radio_url',first.url);
    document.getElementById('mdnsUrl').value='http://'+d.mdnsName+'.local/';updateConfigVolume(d.audioVolume);updateEqLabels();
  }catch(e){Oris.toast(e.message,'bad');}
}
window.updateEqLabels=updateEqLabels;window.resetEq=resetEq;window.applyEqPreset=applyEqPreset;window.applySafeGain=applySafeGain;window.updateConfigVolume=updateConfigVolume;
document.addEventListener('DOMContentLoaded',loadConfig);

async function loadSmartSpeakerConfig() {
  try {
    const response = await fetch('/api/config.json', {
      cache: 'no-store',
      credentials: 'same-origin'
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const config = await response.json();

    const enabled = document.getElementById('smartSpeakerEnabled');
    const name = document.getElementById('smartSpeakerName');
    const status = document.getElementById('smartSpeakerStatus');

    if (enabled) {
      enabled.checked = Boolean(config.smartSpeakerEnabled);
    }

    if (name) {
      name.textContent = config.mdnsName || 'oris-radio';
    }

    if (status) {
      status.textContent = config.smartSpeakerStatus || 'Neaktivní';
    }
  } catch (error) {
    console.error('Načtení nastavení síťového reproduktoru selhalo:', error);
  }
}

document.addEventListener('DOMContentLoaded', loadSmartSpeakerConfig);