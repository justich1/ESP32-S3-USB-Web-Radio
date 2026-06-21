'use strict';
let fileState={disk:'ffat',path:'/',parent:'/',available:false,items:[]};
const textExt=new Set(['txt','md','ini','cfg','log','json','ock','xml','csv','html','htm','css','js','ino','cpp','h','hpp','c','py','sh','bat','yml','yaml']);
function filesUrl(disk,path){return '/files?disk='+encodeURIComponent(disk)+'&p='+encodeURIComponent(path||'/');}
function fileActionUrl(base,path){return base+'?disk='+encodeURIComponent(fileState.disk)+'&f='+encodeURIComponent(path);}
function jsArg(value){return JSON.stringify(String(value??'')).replace(/'/g,'\\u0027').replace(/</g,'\\u003c');}
function submitForm(url,values){const form=document.createElement('form');form.method='POST';form.action=url;form.hidden=true;Object.entries(values||{}).forEach(([name,value])=>{const input=document.createElement('input');input.name=name;input.value=value;form.appendChild(input);});document.body.appendChild(form);form.submit();}
async function loadFiles(){
  const q=Oris.query(); fileState.disk=q.get('disk')||'ffat'; fileState.path=q.get('p')||'/';
  document.getElementById('diskSelect').value=fileState.disk;
  try{
    fileState=await Oris.fetchJson('/api/files.json?disk='+encodeURIComponent(fileState.disk)+'&p='+encodeURIComponent(fileState.path));
    renderFiles();
  }catch(e){document.getElementById('fileList').innerHTML='<div class="empty bad">'+Oris.esc(e.message)+'</div>';}
}
function renderFiles(){
  const {disk,path,parent,available,items,title}=fileState;
  document.getElementById('pathTitle').innerHTML='<b>'+Oris.esc(title)+':</b> '+Oris.esc(path);
  document.getElementById('diskHint').textContent='Aktuální disk: '+title;
  document.getElementById('rootLink').href=filesUrl(disk,'/');
  const parentLink=document.getElementById('parentLink'); parentLink.href=filesUrl(disk,parent||'/'); parentLink.hidden=path==='/';
  document.getElementById('uploadForm').action='/upload?disk='+encodeURIComponent(disk)+'&p='+encodeURIComponent(path);
  document.getElementById('playFolderButton').disabled=!available;
  document.getElementById('mkdirButton').disabled=!available; document.getElementById('createButton').disabled=!available;
  document.getElementById('formatFfatBox').hidden=disk!=='ffat'; document.getElementById('usbFormatHint').hidden=disk!=='usb0';
  if(!available){document.getElementById('fileList').innerHTML='<div class="empty warn">Disk není dostupný.</div>';return;}
  let html='<div class="table-wrap"><table><thead><tr><th>Název</th><th>Velikost</th><th>Akce</th></tr></thead><tbody>';
  if(path!=='/') html+=rowForParent(parent);
  (items||[]).forEach(item=>html+=rowForItem(item));
  if(path==='/' && !(items||[]).length) html+='<tr><td colspan="3" class="empty">Složka je prázdná.</td></tr>';
  html+='</tbody></table></div>'; document.getElementById('fileList').innerHTML=html;
}
function rowForParent(parent){return `<tr><td><div class="file-name"><span class="file-icon">📁</span><a href="${filesUrl(fileState.disk,parent)}">..</a></div></td><td>nahoru</td><td></td></tr>`;}
function rowForItem(item){
  const name=Oris.esc(item.name), path=encodeURIComponent(item.path), rawPath=Oris.esc(item.path), rawName=Oris.esc(item.name);
  let actions='';
  if(item.dir){
    actions+=`<a class="button ghost" href="${filesUrl(fileState.disk,item.path)}">Otevřít</a>`;
    actions+=`<button class="secondary" onclick='return renameEntry(${jsArg(fileState.disk)},${jsArg(item.path)},${jsArg(fileState.path)},${jsArg(item.name)})'>Přejmenovat</button>`;
    actions+=`<a class="button danger" href="/delete?disk=${encodeURIComponent(fileState.disk)}&f=${path}&p=${encodeURIComponent(fileState.path)}" onclick="return confirm('Smazat složku včetně obsahu?')">Smazat</a>`;
  }else{
    actions+=`<a class="button ghost" href="/view?disk=${encodeURIComponent(fileState.disk)}&f=${path}">Náhled</a>`;
    if((item.ext||'').toLowerCase()==='mp3') actions+=`<button onclick="return playAudioNoReload('/audio/play?disk=${encodeURIComponent(fileState.disk)}&f=${path}')">Play</button>`;
    if(textExt.has((item.ext||'').toLowerCase())) actions+=`<a class="button ghost" href="/edit?disk=${encodeURIComponent(fileState.disk)}&f=${path}">Edit</a>`;
    actions+=`<a class="button ghost" href="/download?disk=${encodeURIComponent(fileState.disk)}&f=${path}">Stáhnout</a>`;
    actions+=`<button class="secondary" onclick='return renameEntry(${jsArg(fileState.disk)},${jsArg(item.path)},${jsArg(fileState.path)},${jsArg(item.name)})'>Přejmenovat</button>`;
    actions+=`<a class="button danger" href="/delete?disk=${encodeURIComponent(fileState.disk)}&f=${path}&p=${encodeURIComponent(fileState.path)}" onclick="return confirm('Smazat soubor?')">Smazat</a>`;
  }
  return `<tr><td><div class="file-name"><span class="file-icon">${item.dir?'📁':'📄'}</span><span>${name}</span></div></td><td>${item.dir?'složka':Oris.niceBytes(item.size)}</td><td class="actions">${actions}</td></tr>`;
}
document.addEventListener('DOMContentLoaded',()=>{
  document.getElementById('openDiskButton').onclick=()=>location.href=filesUrl(document.getElementById('diskSelect').value,'/');
  document.getElementById('usbRemountButton').onclick=()=>submitForm('/usb/remount',{});
  document.getElementById('playFolderButton').onclick=()=>playAudioNoReload('/audio/play_folder?disk='+encodeURIComponent(fileState.disk)+'&p='+encodeURIComponent(fileState.path));
  document.getElementById('mkdirButton').onclick=()=>{const name=document.getElementById('mkdirName').value.trim();if(name)submitForm('/mkdir',{disk:fileState.disk,p:fileState.path,name});};
  document.getElementById('createButton').onclick=()=>{const name=document.getElementById('createName').value.trim();if(name)submitForm('/create',{disk:fileState.disk,p:fileState.path,name});};
  document.getElementById('formatButton').onclick=()=>{if(confirm('Opravdu formátovat interní FFat?'))submitForm('/format',{disk:'ffat',confirm:document.getElementById('formatConfirm').value});};
  loadFiles();
});
