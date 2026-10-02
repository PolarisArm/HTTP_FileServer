#include <Arduino.h>


const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Sender</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: sans-serif; max-width: 460px; margin: 30px auto; padding: 0 16px; background: #f0f0f0; }
  h2 { margin: 0 0 4px; }
  #clock { margin-bottom: 12px; font-size: 0.9rem; color: #555; }
  .card { background: #fff; border-radius: 10px; padding: 16px; margin-bottom: 14px; box-shadow: 0 1px 4px rgba(0,0,0,0.15); }
  .card h3 { margin: 0 0 8px; font-size: 1rem; color: #333; }
  input, button { width: 100%; padding: 10px; margin: 5px 0; font-size: 1rem; }
  button { cursor: pointer; background: #1a73e8; color: #fff; border: none; border-radius: 6px; }
  button:active { background: #1557b0; }
  .status { margin-top: 6px; font-size: 0.85rem; color: #444; word-wrap: break-word; }
  #ip { border: 2px solid #1a73e8; }
  .row { display: flex; align-items: center; gap: 8px; padding: 6px 0; border-bottom: 1px solid #eee; }
  .row a { flex: 1; word-break: break-all; color: #1a73e8; text-decoration: none; }
  .row small { color: #777; white-space: nowrap; }
  .row button { width: auto; margin: 0; padding: 4px 10px; font-size: 0.85rem; background: #d93025; }
</style>
</head>
<body>
  <h2>ESP32 Sender</h2>
  <div id="clock"></div>

  <div class="card">
    <input id="ip" type="text" placeholder="ESP32 IP" value="192.168.0.211">
  </div>

  <!-- 1. TEXT -->
  <div class="card">
    <h3>1. Send Text (form)</h3>
    <input id="msg" type="text" placeholder="Message">
    <button onclick="sendText()">Send Text</button>
    <p id="statusText" class="status"></p>
  </div>

  <!-- 2. FILE -->
  <div class="card">
    <h3>2. Send File</h3>
    <input id="file" type="file">
    <button onclick="sendFile()">Send File</button>
    <p id="statusFile" class="status"></p>
  </div>

  <!-- 3. FILES ON SD -->
  <div class="card">
    <h3>3. Files on SD Card</h3>
    <button onclick="loadList()">Refresh</button>
    <div id="list"></div>
    <p id="statusList" class="status"></p>
  </div>

<script>
function ip() { return document.getElementById('ip').value.trim(); }
function base() { return 'http://' + ip(); }

function tick() { document.getElementById('clock').textContent = new Date().toLocaleString(); }
setInterval(tick, 1000); tick();

function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}

async function sendText() {
  const s = document.getElementById('statusText');
  try {
    const res = await fetch(base() + '/filePage/text', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'msg=' + encodeURIComponent(document.getElementById('msg').value)
    });
    s.textContent = 'Sent! Response: ' + await res.text();
  } catch (e) { s.textContent = 'Error: ' + e.message; }
}

async function sendFile() {
  const s = document.getElementById('statusFile');
  const file = document.getElementById('file').files[0];
  if (!file) { s.textContent = 'Pick a file first'; return; }
  s.textContent = 'Uploading ' + file.name + ' ...';
  try {
    const res = await fetch(base() + '/upload/' + encodeURIComponent(file.name), {
      method: 'POST',
      headers: { 'Content-Type': file.type || 'application/octet-stream' },
      body: file
    });
    s.textContent = res.ok ? 'Uploaded ' + file.name : 'Failed (' + res.status + ')';
    loadList();
  } catch (e) { s.textContent = 'Error: ' + e.message; }
}

async function delFile(name) {
  if (!confirm('Delete ' + name + '?')) return;
  const s = document.getElementById('statusList');
  try {
    const res = await fetch(base() + '/filePage/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'msg=' + encodeURIComponent(name)
    });
    s.textContent = res.ok ? 'Deleted ' + name : 'Delete failed (' + res.status + ')';
    loadList();
  } catch (e) { s.textContent = 'Error: ' + e.message; }
}

async function loadList() {
  const box = document.getElementById('list');
  const s = document.getElementById('statusList');
  try {
    const res = await fetch(base() + '/filePage/list');
    const files = await res.json();
    box.innerHTML = '';
    if (!files.length) { s.textContent = 'No files'; return; }
    s.textContent = files.length + ' file(s)';
    files.forEach(f => {
      const row = document.createElement('div'); row.className = 'row';
      const a = document.createElement('a');
      a.textContent = f.name;
      a.href = base() + '/filePage/inload?file=' + encodeURIComponent(f.name);
      a.target = '_blank';
      const sz = document.createElement('small'); sz.textContent = fmtSize(f.size);
      const b = document.createElement('button'); b.textContent = 'Del';
      b.onclick = () => delFile(f.name);
      row.append(a, sz, b);
      box.appendChild(row);
    });
  } catch (e) { s.textContent = 'Error: ' + e.message; }
}

loadList();
</script>
</body>
</html>
)rawliteral";
