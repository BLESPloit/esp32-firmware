let wsLogEnabled    = false;
let espLogEnabled   = false;
let infoLogEnabled  = false; 
let infoTags        = [];


// ─────────────────────────────────────────────
// Debug logging
// ─────────────────────────────────────────────
function addWsLog(msg) {
  if (!wsLogEnabled) return;
  _appendLog('debug', `WS ← ${msg}`);
}

function addEspLog(level, tag, msg) {
  _appendLog(level, `${tag} [${level}] ${msg}`);
}

function _appendLog(cls, text) {
  const log = document.getElementById('debugLog');
  const ts  = new Date().toLocaleTimeString('en-US', { hour12: false });
  const div = document.createElement('div');
  div.className   = `log-entry log-${cls}`;
  div.textContent = `${ts} ${text}`;
  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
}


// ─────────────────────────────────────────────
// Show/hide INFO row and tag manager
// ─────────────────────────────────────────────
function updateEspGroupVisibility() {
    const infoRow = document.getElementById('espInfoLogRow');
    const tagPanel = document.getElementById('tagManagerPanel');
  // INFO logs checkbox row: visible only when ESP logs are enabled
    if (infoRow)  infoRow.style.display  = espLogEnabled ? '' : 'none';
  // Tag manager: visible only when both ESP logs AND INFO logs are enabled
    if (tagPanel) tagPanel.style.display = (espLogEnabled && infoLogEnabled) ? '' : 'none';
}


// ─────────────────────────────────────────────
// ESP log filter API
// ─────────────────────────────────────────────
async function loadWslogFilter() {
  try {
    const r = await fetch('/api/log/filter');
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const cfg = await r.json();
    espLogEnabled  = !!cfg.enabled;
    infoLogEnabled = !!cfg.info_enabled; 
    infoTags       = Array.isArray(cfg.allowed_info_tags)
                       ? cfg.allowed_info_tags.slice()
                       : [];
    document.getElementById('chkEspLog').checked     = espLogEnabled;
    document.getElementById('chkEspInfoLog').checked = infoLogEnabled;
    updateEspGroupVisibility(); 
    renderTagChips();
    addEspLog('I', 'wslog',
      `filter loaded — enabled=${espLogEnabled}, info=${infoLogEnabled}, tags=[${infoTags.join(', ')}]`);
  } catch(e) {
    addEspLog('W', 'wslog', `GET /api/log/filter failed: ${e.message}`);
  }
}

async function pushWslogFilter() {
  const body = {
    enabled:           espLogEnabled,
    info_enabled:      infoLogEnabled,
    allowed_info_tags: infoTags,
  };
  try {
    const r = await fetch('/api/log/filter', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(body),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    addEspLog('I', 'wslog',
      `filter updated — enabled=${espLogEnabled}, info=${infoLogEnabled}, tags=[${infoTags.join(', ')}]`);
  } catch(e) {
    addEspLog('E', 'wslog', `POST /api/log/filter failed: ${e.message}`);
  }
}

// ─────────────────────────────────────────────
// Tag chip rendering
// ─────────────────────────────────────────────
function renderTagChips() {
  const listEl  = document.getElementById('tagList');
  const emptyEl = document.getElementById('tagListEmpty');
  if (!listEl || !emptyEl) return; 
  listEl.querySelectorAll('.tag-chip').forEach(el => el.remove());

  if (infoTags.length === 0) {
    emptyEl.style.display = '';
  } else {
    emptyEl.style.display = 'none';
    infoTags.forEach((tag, idx) => {
      const chip = document.createElement('span');
      chip.className = 'tag-chip';
      chip.innerHTML =
        `${escHtml(tag)}<button type="button" title="Remove" data-idx="${idx}">&times;</button>`;
      chip.querySelector('button').addEventListener('click', () => removeTag(idx));
      listEl.appendChild(chip);
    });
  }
}

function addTag() {
  const input = document.getElementById('newTagInput');
  const tag   = input.value.trim();
  if (!tag || infoTags.includes(tag)) return;
  infoTags.push(tag);
  input.value = '';
  renderTagChips();
  pushWslogFilter();
}

function removeTag(idx) {
  infoTags.splice(idx, 1);
  renderTagChips();
  pushWslogFilter();
}


// ─────────────────────────────────────────────
// Debug-controls event listeners
// ─────────────────────────────────────────────
if (document.getElementById('chkWsLog')) {
    document.getElementById('chkWsLog').addEventListener('change', function () {
        wsLogEnabled = this.checked;
        addEspLog('I', 'web', 'WS log ' + (wsLogEnabled ? 'enabled' : 'disabled'));
    });
    document.getElementById('chkEspLog').addEventListener('change', function () {
        espLogEnabled = this.checked;
        if (!espLogEnabled) { infoLogEnabled = false; document.getElementById('chkEspInfoLog').checked = false; }
        updateEspGroupVisibility();
        pushWslogFilter();
    });
    document.getElementById('chkEspInfoLog').addEventListener('change', function () {
        infoLogEnabled = this.checked;
        updateEspGroupVisibility();
        pushWslogFilter();
    });
    document.getElementById('addTagBtn').addEventListener('click', addTag);
    document.getElementById('newTagInput').addEventListener('keydown', function(e) {
        if (e.key === 'Enter') addTag();
    });
}
