'use strict';
const textExts=new Set(['txt','md','ini','cfg','log','json','ock','xml','csv','html','htm','css','js','ino','cpp','h','hpp','c','py','sh','bat','yml','yaml']);
const imageExts=new Set(['jpg','jpeg','png','gif','bmp','webp','svg']);
const audioExts=new Set(['mp3','wav','ogg','m4a','aac','flac']);
const videoExts=new Set(['mp4','webm','mov','avi','mkv']);
async function loadPreview(){
  const q=Oris.query(),disk=q.get('disk')||'ffat',path=q.get('f')||'';const ext=(path.split('.').pop()||'').toLowerCase();
  document.getElementById('viewTitle').textContent='Náhled: '+(path.split('/').pop()||path);document.getElementById('viewPath').textContent=disk+': '+path;
  const raw='/raw?disk='+encodeURIComponent(disk)+'&f='+encodeURIComponent(path),down='/download?disk='+encodeURIComponent(disk)+'&f='+encodeURIComponent(path);
  let actions=`<a class="button ghost" href="/files?disk=${encodeURIComponent(disk)}&p=${encodeURIComponent(parent(path))}">Zpět</a><a class="button ghost" href="${down}">Stáhnout</a>`;
  if(textExts.has(ext))actions+=`<a class="button" href="/edit?disk=${encodeURIComponent(disk)}&f=${encodeURIComponent(path)}">Editovat</a>`;document.getElementById('viewActions').innerHTML=actions;
  const box=document.getElementById('preview');
  if(imageExts.has(ext))box.innerHTML=`<img src="${raw}" alt="">`;
  else if(audioExts.has(ext))box.innerHTML=`<audio controls autoplay src="${raw}"></audio>`;
  else if(videoExts.has(ext))box.innerHTML=`<video controls src="${raw}"></video>`;
  else if(textExts.has(ext)){try{const text=await Oris.fetchText(raw);box.innerHTML='<pre>'+Oris.esc(text)+'</pre>';}catch(e){box.innerHTML='<div class="bad">'+Oris.esc(e.message)+'</div>';}}
  else box.innerHTML=`<div class="empty">Pro tento typ není náhled. <a href="${down}">Stáhnout soubor</a>.</div>`;
}
function parent(path){const i=path.lastIndexOf('/');return i<=0?'/':path.slice(0,i);}
document.addEventListener('DOMContentLoaded',loadPreview);
