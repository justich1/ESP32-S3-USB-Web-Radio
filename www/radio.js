'use strict';

let radioData = null;

function radioLogo(station) {
  return station.logo || '/www/icons/radio.svg';
}

function renderRadioStations(data) {
  let html = '<table><thead><tr><th>#</th><th>Logo</th><th>Název</th><th>HTTP MP3 URL</th><th>Akce</th></tr></thead><tbody>';
  (data.radioStations || []).forEach(station => {
    const logo = radioLogo(station);
    html += `<tr>
      <td>${station.index + 1}</td>
      <td>
        <div class="station-logo-editor">
          <img src="${Oris.esc(logo)}" alt="Logo stanice" onerror="this.onerror=null;this.src='/www/icons/radio.svg'">
          <input id="stationLogoFile_${station.index}" type="file" accept=".png,.jpg,.jpeg,.webp,.gif,.svg,image/*">
          <div class="button-row">
            <button class="secondary" type="button" onclick="return uploadStationLogo(${station.index})">Nahrát logo</button>
            ${station.logo ? `<button class="danger" type="button" onclick="return deleteStationLogo(${station.index})">Smazat</button>` : ''}
          </div>
        </div>
      </td>
      <td><input name="radio_name_${station.index}" value="${Oris.esc(station.name)}" placeholder="Název stanice"></td>
      <td><input name="radio_url_${station.index}" value="${Oris.esc(station.url)}" placeholder="http://server/stream.mp3"></td>
      <td class="actions">${station.url ? `<button type="button" onclick="return playRadioNoReload(${station.index})">Play</button>` : '<span class="small">není URL</span>'}</td>
    </tr>`;
  });
  html += '</tbody></table>';
  document.getElementById('radioStations').innerHTML = html;
}

async function loadRadio() {
  try {
    radioData = await Oris.fetchJson('/api/config.json');
    renderRadioStations(radioData);
    updateAudioVolumeDisplay(radioData.audioVolume);
    updateAudioLabels(radioData.audioStatus);
  } catch (error) {
    document.getElementById('radioStations').innerHTML = '<div class="empty bad">' + Oris.esc(error.message) + '</div>';
  }
}

function uploadStationLogo(index) {
  const input = document.getElementById('stationLogoFile_' + index);
  if (!input?.files?.length) {
    Oris.toast('Vyber soubor s logem.', 'bad');
    return false;
  }

  const file = input.files[0];
  if (file.size > 512 * 1024) {
    Oris.toast('Logo je větší než 512 KB.', 'bad');
    return false;
  }

  const modal = ensureUploadModal();
  const fill = document.getElementById('uploadFill');
  const pct = document.getElementById('uploadPercent');
  const err = document.getElementById('uploadError');
  document.getElementById('uploadFileName').textContent = file.name + ' (' + Oris.niceBytes(file.size) + ')';
  fill.style.width = '0%';
  pct.textContent = '0 %';
  err.style.display = 'none';
  modal.classList.add('show');

  const data = new FormData();
  data.append('logo', file);
  const xhr = new XMLHttpRequest();
  xhr.upload.onprogress = event => {
    if (!event.lengthComputable) return;
    const value = Math.round(event.loaded / event.total * 100);
    fill.style.width = value + '%';
    pct.textContent = value + ' %';
  };
  xhr.onload = () => {
    if (xhr.status >= 200 && xhr.status < 300) {
      fill.style.width = '100%';
      pct.textContent = 'Logo uloženo, obnovuji…';
      setTimeout(() => location.reload(), 450);
    } else {
      err.style.display = 'block';
      err.textContent = xhr.responseText || ('Upload selhal: HTTP ' + xhr.status);
    }
  };
  xhr.onerror = () => {
    err.style.display = 'block';
    err.textContent = 'Upload selhal: chyba spojení.';
  };
  xhr.open('POST', '/radio/logo?i=' + encodeURIComponent(index), true);
  xhr.send(data);
  return false;
}

async function deleteStationLogo(index) {
  if (!confirm('Smazat logo této stanice?')) return false;
  try {
    const text = await Oris.postForm('/radio/logo/delete', {i:index});
    Oris.toast(text, 'good');
    setTimeout(() => location.reload(), 300);
  } catch (error) {
    Oris.toast(error.message, 'bad');
  }
  return false;
}

window.uploadStationLogo = uploadStationLogo;
window.deleteStationLogo = deleteStationLogo;
document.addEventListener('DOMContentLoaded', loadRadio);
