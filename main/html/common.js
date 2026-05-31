// ─────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────
let deviceId    = null;
let ws          = null;
let textTimeout = null;
let wsPendingQueue = [];

// Reconnect guard — each page sets this to its own "am I active?" variable
// by assigning:  wsReconnectGuard = () => isSimulating;
// common.js default: never auto-reconnect (safe fallback).
let wsReconnectGuard = () => false;

// ─────────────────────────────────────────────
// Device ID from URL
// ─────────────────────────────────────────────

/**
 * Extract the device-id segment from the current URL path.
 *
 * @param {string} [prefix='central']  The path segment that precedes the id.
 *                                     e.g. 'central' matches /central/<id>
 *                                          'sim'     matches /sim/<id>
 * @returns {string|null}
 */
function getDeviceIdFromURL(prefix = 'central') {
  const m = window.location.pathname.match(
    new RegExp('\\/' + prefix + '\\/([^/]+)')
  );
  return m ? m[1] : null;
}

// ─────────────────────────────────────────────
// Device info
// ─────────────────────────────────────────────
function loadDeviceInfo() {
  if (!deviceId) return;
  fetch(`/api/device/${deviceId}/info`)
    .then(r => r.json())
    .then(d => {
      if (d.name) document.getElementById('deviceName').textContent = d.name;
      const img = document.getElementById('deviceImage');
      img.src     = `/static/${deviceId}/icon`;
      img.onload  = () => { img.style.display = 'block'; document.getElementById('iconFallback').style.display = 'none'; };
      img.onerror = () => {};
    })
    .catch(() => {});
}

// ─────────────────────────────────────────────
// Text notification
// ─────────────────────────────────────────────
function showTextNotification(text) {
  const el = document.getElementById('textNotification');
  el.textContent = text;
  el.classList.remove('show', 'fadeout');
  requestAnimationFrame(() => el.classList.add('show'));
  clearTimeout(textTimeout);
  textTimeout = setTimeout(() => {
    el.classList.replace('show', 'fadeout');
    setTimeout(() => el.classList.remove('fadeout'), 300);
  }, 2000);
}

// ─────────────────────────────────────────────
// WebSocket
// ─────────────────────────────────────────────
function wsSend(obj) {
  if (ws?.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
    addWsLog(JSON.stringify(obj));
  } else {
    wsPendingQueue.push(obj);       // queue instead of drop
    addEspLog('I', 'web', 'WS not open yet — message queued');
    if (!ws || ws.readyState === WebSocket.CLOSED) {
      connectWebSocket();           // ensure connection is in progress
    }
  }
}

function connectWebSocket() {
  if (ws?.readyState === WebSocket.OPEN) return;
  const url = `ws://${window.location.hostname}/ws`;
  try {
    ws = new WebSocket(url);
    ws.onopen = () => {
      addEspLog('I', 'web', 'WebSocket connected');
      const el = document.getElementById('wsStatus');
      el.textContent = 'WS On'; el.className = 'ws-status ws-connected';

      // Flush any queued messages
      while (wsPendingQueue.length > 0) {
        const queued = wsPendingQueue.shift();
        ws.send(JSON.stringify(queued));
        addWsLog(JSON.stringify(queued));
      }
      if (typeof window.afterWebSocketOpen === 'function') {
        try { window.afterWebSocketOpen(); } catch (_) {}
      }
    };
    
    ws.onmessage = (event) => {
      let skipRawWsLog = false;
      try {
        if (event.data && event.data.trim().startsWith('{')) {
          const p = JSON.parse(event.data);
          if (p && p.type === 'ble_sim_trace') {
            skipRawWsLog = true;
          }
        }
      } catch (_) {}
      if (!skipRawWsLog) {
        addWsLog(event.data.length > 120 ? event.data.substring(0, 120) + '…' : event.data);
      }
      // Skip empty or non-JSON frames
      if (!event.data || !event.data.trim().startsWith('{')) {
        addEspLog('I', 'web', `WS raw frame (non-JSON): ${event.data.substring(0, 40)}`);
        return;
      }
      try {
        handleWsMessage(JSON.parse(event.data));
      } catch(e) {
        addEspLog('E', 'web', `WS parse error: ${e.message}`);
      }
    };
    ws.onerror = () => addEspLog('E', 'web', 'WebSocket error');
    ws.onclose = () => {
      addEspLog('W', 'web', 'WebSocket disconnected');
      const el = document.getElementById('wsStatus');
      el.textContent = 'WS Off'; el.className = 'ws-status ws-disconnected';
      wsPendingQueue = [];   // clear stale queued messages

      if (wsReconnectGuard?.()) setTimeout(connectWebSocket, 3000);
    };
  } catch(e) {
    addEspLog('E', 'web', `WS create error: ${e.message}`);
    if (wsReconnectGuard()) setTimeout(connectWebSocket, 3000);
  }
}

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────
function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
