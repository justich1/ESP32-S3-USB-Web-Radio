'use strict';
async function loadEditor(){
  const q=Oris.query(),disk=q.get('disk')||'ffat',path=q.get('f')||'';const raw='/raw?disk='+encodeURIComponent(disk)+'&f='+encodeURIComponent(path);
  document.getElementById('editTitle').textContent='Editor: '+(path.split('/').pop()||path);document.getElementById('editPath').textContent=disk+': '+path;
  document.getElementById('editForm').action='/save?disk='+encodeURIComponent(disk)+'&f='+encodeURIComponent(path);
  document.getElementById('cancelEdit').href='/view?disk='+encodeURIComponent(disk)+'&f='+encodeURIComponent(path);
  try{document.getElementById('editor').value=await Oris.fetchText(raw);document.getElementById('editor').placeholder='';}catch(e){Oris.toast(e.message,'bad');document.getElementById('editor').disabled=true;}
}
document.addEventListener('DOMContentLoaded',loadEditor);
