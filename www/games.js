'use strict';
async function createGamesDir(disk){try{await Oris.postForm('/games/mkdir',{disk});location.reload();}catch(e){Oris.toast(e.message,'bad');}}
async function loadGames(){
  try{
    const data=await Oris.fetchJson('/api/games.json');
    document.getElementById('gamesRoot').innerHTML=(data.disks||[]).map(d=>{
      if(!d.available)return `<div class="card"><h2>${Oris.esc(d.title)}</h2><p class="warn">Disk není dostupný.</p></div>`;
      if(!d.hasGamesDir)return `<div class="card"><h2>${Oris.esc(d.title)}</h2><p class="small">Složka /games zatím neexistuje.</p><button onclick="createGamesDir('${d.disk}')">Vytvořit /games</button></div>`;
      const cards=(d.items||[]).map(g=>`<div class="game-card"><h3>🎮 ${Oris.esc(g.name)}</h3><div class="small">${Oris.esc(g.type)}<br>${Oris.esc(g.path)}</div><a class="button" href="${g.url}">Spustit</a></div>`).join('')||'<div class="empty">Žádná hra nenalezena.</div>';
      return `<div class="card"><h2>${Oris.esc(d.title)}</h2><div class="game-grid">${cards}</div><p class="small"><a href="/files?disk=${encodeURIComponent(d.disk)}&p=%2Fgames">Spravovat /games</a></p></div>`;
    }).join('');
  }catch(e){document.getElementById('gamesRoot').innerHTML='<div class="card bad">'+Oris.esc(e.message)+'</div>';}
}
window.createGamesDir=createGamesDir;document.addEventListener('DOMContentLoaded',loadGames);
