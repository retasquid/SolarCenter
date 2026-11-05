'use strict';

const PAGES = ['accueil', 'previsions', 'historique', 'parametres', 'systeme'];
const DEFAULT_PAGE = 'accueil';
const SYSTEM_POLL_MS = 5000;
const STATE_POLL_MS = 2000;     // repli si le WebSocket tombe
const WS_RETRY_MS = 10000;
const FETCH_TIMEOUT_MS = 4000;
const ACTIVE_W = 10;            // un flux est dessiné au-delà de 10 W
const HISTORY_MS = 2 * 3600 * 1000;

const SVG_NS = 'http://www.w3.org/2000/svg';
const $ = (sel) => document.querySelector(sel);

// ---------- Utilitaires ----------

async function getJson(url, opts) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
  try {
    const r = await fetch(url, Object.assign({ signal: ctl.signal, cache: 'no-store' }, opts));
    const j = await r.json().catch(() => ({}));
    if (!r.ok) {
      const e = new Error(j.error || 'HTTP ' + r.status);
      e.field = j.field;
      e.status = r.status;
      throw e;
    }
    return j;
  } finally {
    clearTimeout(timer);
  }
}

function svg(tag, attrs, parent) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const k in attrs) el.setAttribute(k, attrs[k]);
  if (parent) parent.append(el);
  return el;
}

function fillDl(id, rows) {
  const dl = document.getElementById(id);
  dl.replaceChildren();
  for (const [label, value, cls] of rows) {
    const dt = document.createElement('dt');
    const dd = document.createElement('dd');
    dt.textContent = label;
    dd.textContent = value;
    if (cls) dd.className = cls;
    dl.append(dt, dd);
  }
}

const fr = (v, d) => v.toFixed(d).replace('.', ',');
const ko = (b) => fr(b / 1024, 1) + ' Ko';

function fmtW(w) {
  const a = Math.abs(w);
  return a >= 1000 ? fr(a / 1000, a >= 10000 ? 1 : 2) + ' kW' : Math.round(a) + ' W';
}

function duration(s) {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d} j ${h} h ${m} min`;
  if (h > 0) return `${h} h ${m} min`;
  return `${m} min ${s % 60} s`;
}

function age(s) {
  if (s === undefined) return 'jamais';
  if (s < 60) return `il y a ${s} s`;
  if (s < 3600) return `il y a ${Math.floor(s / 60)} min`;
  return `il y a ${Math.floor(s / 3600)} h`;
}

function rssiLabel(rssi) {
  if (!rssi) return '—';
  const q = rssi >= -60 ? 'bon' : rssi >= -70 ? 'correct' : rssi >= -80 ? 'faible' : 'très faible';
  return `${rssi} dBm (${q})`;
}

const dateTime = (epoch) => (epoch ? new Date(epoch * 1000).toLocaleString('fr-FR') : '—');
const dateOnly = (epoch) => new Date(epoch * 1000).toLocaleDateString('fr-FR');

function alertBox(boxId, list) {
  const box = document.getElementById(boxId);
  box.replaceChildren();
  for (const [text, cls] of list) {
    const div = document.createElement('div');
    div.className = 'alert' + (cls ? ' ' + cls : '');
    div.textContent = text;
    box.append(div);
  }
}

// ---------- Navigation ----------

function showPage() {
  const wanted = location.hash.slice(1);
  const id = PAGES.includes(wanted) ? wanted : DEFAULT_PAGE;
  for (const p of PAGES) document.getElementById(p).classList.toggle('active', p === id);
  document.querySelectorAll('.tabs a').forEach((a) => {
    if (a.getAttribute('href') === '#' + id) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
  if (id === 'parametres') loadConfig();
  if (id === 'previsions') loadForecast();
  if (id === 'historique') loadHistory();
}

// ---------- Diagramme des flux ----------

// Disposition (§11.3) : solaire en haut, réseau à gauche, batterie à droite, maison en bas,
// chauffe-eau à côté de la maison.
const NODES = {
  pv:   { x: 200, y: 62,  r: 34, label: 'Solaire',     color: 'var(--white)',    icon: 'pv' },
  grid: { x: 58,  y: 210, r: 34, label: 'Réseau',      color: 'var(--red)',      icon: 'grid' },
  batt: { x: 342, y: 210, r: 34, label: 'Batterie',    color: 'var(--red-soft)', icon: 'batt' },
  home: { x: 200, y: 352, r: 40, label: 'Maison',      color: 'var(--white)',    icon: 'home' },
  ce:   { x: 62,  y: 352, r: 28, label: 'Chauffe-eau', color: 'var(--red-soft)', icon: 'ce' },
};

// Chemins orthogonaux aux coins arrondis, orientés de la source vers la destination.
const FLOWS = {
  pv_home:   { d: 'M200,96 V312',                          color: 'var(--white)' },
  pv_grid:   { d: 'M188,96 V184 Q188,200 172,200 H92',      color: 'var(--white)', dash: '6 5' },
  pv_batt:   { d: 'M212,96 V184 Q212,200 228,200 H308',     color: 'var(--white)' },
  grid_home: { d: 'M92,220 H172 Q188,220 188,236 V314',     color: 'var(--red)' },
  grid_batt: { d: 'M92,210 H308',                           color: 'var(--red)' },
  batt_home: { d: 'M308,220 H228 Q212,220 212,236 V314',    color: 'var(--red-soft)' },
  home_ce:   { d: 'M160,352 H90',                           color: 'var(--red-soft)' },
};

// Icônes dessinées sur une grille de 24 unités.
const ICONS = {
  pv: 'M3 15h18l-3-10H6z M9 5l-1.2 10 M15 5l1.2 10 M4.5 10h15 M12 15v4 M8 20h8',
  grid: 'M12 2 6.5 22 M12 2l5.5 20 M7.8 8h8.4 M6.6 13h10.8 M5 19h14 M8 8l7.4 5 M16 8l-7.4 5',
  batt: 'M7 6h10v15H7z M10 3h4v3 M9.5 12h5 M12 9.5v5 M9.5 18h5',
  home: 'M3 11.5 12 4l9 7.5 M5.5 9.5V20h13V9.5 M10 20v-5.5h4V20',
  ce: 'M8 3h8a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z M12 8.5c-1.6 2.1-2.2 3.2-2.2 4.2a2.2 2.2 0 0 0 4.4 0c0-1-.6-2.1-2.2-4.2z',
};

const flowEls = {};
const nodeEls = {};
let lastState = null;
const history = [];     // [t_ms, {pv, grid, batt, home, ce}]

function buildDiagram() {
  const root = $('#flow');

  for (const [id, f] of Object.entries(FLOWS)) {
    const g = svg('g', { class: 'flow' }, root);
    svg('path', { d: f.d, class: 'flow-base' }, g);
    const line = svg('path', { d: f.d, class: 'flow-line', stroke: f.color }, g);
    if (f.dash) line.setAttribute('stroke-dasharray', f.dash);
    const dots = svg('g', { fill: f.color }, g);
    const dotEls = [0, 1, 2].map(() => svg('circle', { r: 3, cx: -10, cy: -10 }, dots));
    flowEls[id] = { line, dots, dotEls, len: line.getTotalLength(), w: 0, offset: 0 };
  }

  for (const [id, n] of Object.entries(NODES)) {
    const g = svg('g', { class: 'node', tabindex: 0, role: 'button' }, root);
    const title = svg('title', {}, g);
    title.textContent = n.label;
    if (id === 'home') {
      // Anneau de provenance : solaire, batterie, réseau.
      nodeEls.ring = ['pv', 'batt', 'grid'].map((src) => svg('circle', {
        cx: n.x, cy: n.y, r: n.r + 5, class: 'ring', stroke: NODES[src].color,
        transform: `rotate(-90 ${n.x} ${n.y})`,
      }, g));
    }
    if (id === 'batt') {
      svg('circle', { cx: n.x, cy: n.y, r: n.r + 5, class: 'ring-bg' }, g);
      nodeEls.soc = svg('circle', {
        cx: n.x, cy: n.y, r: n.r + 5, class: 'ring', stroke: 'var(--red-soft)',
        transform: `rotate(-90 ${n.x} ${n.y})`,
      }, g);
    }
    const circle = svg('circle', { cx: n.x, cy: n.y, r: n.r, class: 'node-bg', stroke: n.color }, g);
    const s = id === 'home' ? 1.7 : id === 'ce' ? 1.2 : 1.45;
    svg('path', {
      d: ICONS[n.icon], class: 'icon', stroke: n.color, 'stroke-width': 1.6 / s,
      transform: `translate(${n.x - 12 * s} ${n.y - 12 * s}) scale(${s})`,
    }, g);
    const text = svg('text', { x: n.x, y: valueY(id), class: 'val' }, g);
    const sub = svg('text', { x: n.x, y: valueY(id) + 17, class: 'sub' }, g);
    nodeEls[id] = { g, circle, text, sub };
    const open = () => openSpark(id);
    g.addEventListener('click', open);
    g.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); open(); } });
  }
  $('#spark-close').addEventListener('click', () => { $('#spark').hidden = true; sparkNode = null; });
  requestAnimationFrame(animate);
}

function valueY(id) {
  const n = NODES[id];
  return id === 'pv' ? n.y - n.r - 22 : n.y + n.r + (id === 'home' ? 24 : 20);
}

function setNode(id, value, sub, ok) {
  const e = nodeEls[id];
  e.text.textContent = value;
  e.sub.textContent = sub || '';
  e.g.classList.toggle('off', !ok);
}

function strokeWidth(w) {
  return 2 + 4 * Math.sqrt(Math.min(w, 3000) / 3000);
}

function setRing(circle, start, frac) {
  const c = 2 * Math.PI * circle.r.baseVal.value;
  circle.setAttribute('stroke-dasharray', `${Math.max(0, frac * c)} ${c}`);
  circle.setAttribute('stroke-dashoffset', -start * c);
}

function renderFlows(s) {
  const f = s.flows || {};
  for (const [id, el] of Object.entries(flowEls)) {
    const w = f.ok || s.grid.ok ? (f[id] || 0) : 0;
    el.w = w;
    const on = w > ACTIVE_W;
    el.line.style.display = on ? '' : 'none';
    el.dots.style.display = on ? '' : 'none';
    el.line.setAttribute('stroke-width', strokeWidth(w));
  }

  // Solaire
  setNode('pv', s.pv.configured ? (s.pv.ok ? fmtW(s.pv.w) : '—') : 'non configuré',
          s.pv.ok && !s.pv.eim ? 'micro-onduleurs' : '', s.pv.ok);

  // Réseau : → import, ← export (le sens n'est jamais porté par la seule couleur).
  const g = s.grid;
  const gTxt = !g.ok ? '—' : Math.abs(g.w) <= ACTIVE_W ? '0 W' : (g.w > 0 ? '→ ' : '← ') + fmtW(g.w);
  setNode('grid', gTxt, !g.ok ? 'Shelly indisponible' : g.w > ACTIVE_W ? 'import' : g.w < -ACTIVE_W ? 'export' : '', g.ok);

  // Batterie : ▲ charge, ▼ décharge, SOC.
  const b = s.batt;
  let bTxt = '—';
  if (!b.configured) bTxt = 'non configurée';
  else if (b.ok) bTxt = Math.abs(b.w) <= ACTIVE_W ? '0 W' : (b.w < 0 ? '▲ ' : '▼ ') + fmtW(b.w);
  setNode('batt', bTxt, b.soc_ok ? `SOC ${b.soc} %` : '', b.ok);
  setRing(nodeEls.soc, 0, b.soc_ok ? b.soc / 100 : 0);

  // Maison et anneau de provenance.
  const h = s.home;
  setNode('home', h.ok ? fmtW(h.w) : '—', h.ok && h.base !== h.w ? `hors chauffe-eau ${fmtW(h.base)}` : '', h.ok);
  const parts = [f.pv_home || 0, f.batt_home || 0, f.grid_home || 0];
  const tot = parts.reduce((a, v) => a + v, 0);
  let start = 0;
  nodeEls.ring.forEach((c, i) => {
    const frac = h.ok && tot > 0 ? parts[i] / tot : 0;
    setRing(c, start, frac);
    start += frac;
  });

  // Chauffe-eau
  const ce = s.ce;
  setNode('ce', ce.configured ? (ce.ok ? fmtW(ce.w) : '—') : 'non configuré',
          ce.ok ? (ce.relay ? 'relais ON' : 'relais OFF') : '', ce.ok);
}

let lastFrame = 0;
function animate(t) {
  const dt = Math.min(0.1, (t - lastFrame) / 1000);
  lastFrame = t;
  if (!document.hidden) {
    for (const el of Object.values(flowEls)) {
      if (el.w <= ACTIVE_W) continue;
      // Vitesse proportionnelle à la puissance, bornée.
      const speed = Math.min(140, 25 + 3 * Math.sqrt(el.w));
      el.offset = (el.offset + speed * dt) % el.len;
      el.dotEls.forEach((dot, i) => {
        const p = el.line.getPointAtLength((el.offset + (i * el.len) / 3) % el.len);
        dot.setAttribute('cx', p.x);
        dot.setAttribute('cy', p.y);
      });
    }
  }
  requestAnimationFrame(animate);
}

function renderFlowAlerts(s) {
  const list = [];
  if (s.fallback) list.push(['Compteur Shelly indisponible depuis plus de 30 s : mode repli.']);
  const names = { shelly: 'Shelly', envoy: 'Envoy', zendure: 'Zendure', tongou: 'Tongou' };
  const down = Object.entries(s.dev).filter(([, d]) => d.configured && !d.ok).map(([k]) => names[k]);
  if (down.length) list.push([`Lecture en échec : ${down.join(', ')}.`]);
  const unset = Object.entries(s.dev).filter(([, d]) => !d.configured).map(([k]) => names[k]);
  if (unset.length) list.push([`Non configuré : ${unset.join(', ')} (onglet Paramètres, section Appareils).`, 'info']);
  alertBox('flow-alerts', list);
}

// ---------- Mini-courbes (2 dernières heures, côté navigateur) ----------

let sparkNode = null;

function pushHistory(s) {
  const now = Date.now();
  history.push([now, {
    pv: s.pv.ok ? s.pv.w : null,
    grid: s.grid.ok ? s.grid.w : null,
    batt: s.batt.ok ? s.batt.w : null,
    home: s.home.ok ? s.home.w : null,
    ce: s.ce.ok ? s.ce.w : null,
  }]);
  while (history.length && history[0][0] < now - HISTORY_MS) history.shift();
}

function openSpark(id) {
  sparkNode = id;
  $('#spark').hidden = false;
  drawSpark();
}

function drawSpark() {
  if (!sparkNode) return;
  const pts = history.filter(([, v]) => v[sparkNode] !== null);
  const label = { pv: 'Solaire', grid: 'Réseau (> 0 import)', batt: 'Batterie (> 0 décharge)', home: 'Maison', ce: 'Chauffe-eau' };
  $('#spark-title').textContent = `${label[sparkNode]} — ${pts.length} points`;
  const root = $('#spark-svg');
  root.replaceChildren();
  if (pts.length < 2) {
    $('#spark-min').textContent = 'pas encore assez de données';
    $('#spark-max').textContent = '';
    return;
  }
  const vals = pts.map(([, v]) => v[sparkNode]);
  let lo = Math.min(0, ...vals);
  let hi = Math.max(0, ...vals);
  if (hi - lo < 1) hi = lo + 1;
  const t0 = pts[0][0];
  const span = Math.max(1, pts[pts.length - 1][0] - t0);
  const y = (v) => 76 - ((v - lo) / (hi - lo)) * 72;
  svg('line', { x1: 0, x2: 300, y1: y(0), y2: y(0), class: 'spark-zero' }, root);
  const d = pts.map(([t, v], i) => `${i ? 'L' : 'M'}${(((t - t0) / span) * 300).toFixed(1)},${y(v[sparkNode]).toFixed(1)}`).join(' ');
  svg('path', { d, class: 'spark-line', stroke: NODES[sparkNode].color }, root);
  $('#spark-min').textContent = 'min ' + (lo < 0 ? '−' : '') + fmtW(lo);
  $('#spark-max').textContent = 'max ' + fmtW(hi);
}

// ---------- Temps réel : WebSocket, repli sur /api/state ----------

let ws = null;
let pollTimer = null;

function onState(s) {
  lastState = s;
  pushHistory(s);
  if (unit === 'w') renderFlows(s);
  else setRing(nodeEls.soc, 0, s.batt.soc_ok ? s.batt.soc / 100 : 0);
  renderTariff(s.tariff);
  renderFlowAlerts(s);
  renderDevices(s);
  drawSpark();
}

function setConn(mode) {
  const c = $('#conn');
  const labels = { ws: 'en direct', poll: 'interrogation', off: 'hors ligne' };
  c.className = 'conn ' + (mode === 'off' ? 'ko' : 'ok');
  c.textContent = labels[mode];
}

async function pollState() {
  try {
    onState(await getJson('/api/state'));
    if (!ws || ws.readyState !== WebSocket.OPEN) setConn('poll');
  } catch (e) {
    setConn('off');
  }
}

function startPolling() {
  if (!pollTimer) pollTimer = setInterval(() => { if (!document.hidden) pollState(); }, STATE_POLL_MS);
}

function stopPolling() {
  clearInterval(pollTimer);
  pollTimer = null;
}

function connectWs() {
  try {
    ws = new WebSocket(`ws://${location.host}/ws`);
  } catch (e) {
    startPolling();
    setTimeout(connectWs, WS_RETRY_MS);
    return;
  }
  ws.onopen = () => { stopPolling(); setConn('ws'); };
  ws.onmessage = (e) => { try { onState(JSON.parse(e.data)); } catch (err) { /* trame ignorée */ } };
  ws.onclose = () => {
    ws = null;
    startPolling();
    setTimeout(connectWs, WS_RETRY_MS);
  };
}

// ---------- Page Système ----------

const RESET_REASONS = {
  1: 'mise sous tension', 3: 'logiciel', 4: 'exception', 5: 'watchdog (interruption)',
  6: 'watchdog (tâche)', 7: 'watchdog', 8: 'sortie de veille', 9: 'baisse de tension',
};

const CFG_STATUS = {
  ok: 'chargée',
  migrated: 'migrée vers la nouvelle version (valeurs conservées)',
  first_boot: 'premier démarrage (valeurs par défaut)',
  reset_version: 'version changée : valeurs par défaut',
  reset_crc: 'corrompue : valeurs par défaut',
  reset_invalid: 'invalide : valeurs par défaut',
  reset_read: 'illisible : valeurs par défaut',
};

function tokenInfo(s) {
  if (!s.envoy_token_exp) return null;
  const days = Math.floor((s.envoy_token_exp - Date.now() / 1000) / 86400);
  return { days, date: dateOnly(s.envoy_token_exp) };
}

function renderAlerts(s) {
  const list = [];
  if (!s.admin_set) list.push(['Mot de passe administrateur non défini : ouvrez la page de configuration (/setup).']);
  if (s.cfg_status !== 'ok' && s.cfg_status !== 'first_boot' && s.cfg_status !== 'migrated') {
    list.push([`Configuration ${CFG_STATUS[s.cfg_status] || s.cfg_status}. Vérifiez les paramètres.`]);
  }
  if (!s.tariff_configured) list.push(['Tarif non saisi : aucune régulation basée sur les prix tant qu\'il manque.']);
  if (s.ntp_alert) {
    list.push([s.time_synced ? 'Heure non resynchronisée depuis plus de 24 h.'
                             : 'Heure non synchronisée (NTP) : régulation suspendue.']);
  }
  if (s.heap_min < s.heap_warn) list.push([`Mémoire : minimum libre ${ko(s.heap_min)}, sous le seuil de ${ko(s.heap_warn)}.`]);
  if (s.reset_reason === 9) list.push(['Dernier redémarrage dû à une baisse de tension : vérifiez l\'alimentation.']);
  const tk = tokenInfo(s);
  if (tk && tk.days < 0) list.push([`Jeton Envoy expiré le ${tk.date} : à renouveler dans les Paramètres.`]);
  else if (tk && tk.days <= 30) list.push([`Jeton Envoy : expire dans ${tk.days} jours (${tk.date}).`]);
  if (s.envoy_skip_cert) list.push(['Envoy : certificat auto-signé non vérifié (connexion chiffrée mais non authentifiée).', 'info']);
  if (s.observer) list.push(['Mode observateur : les décisions sont calculées, aucune commande n\'est envoyée.', 'info']);
  alertBox('alerts', list);
}

function renderSystem(s) {
  renderAlerts(s);
  const tk = tokenInfo(s);
  fillDl('sys-fw', [
    ['Version', s.version],
    ['ESP-IDF', s.idf],
    ['Fonctionnement', duration(s.uptime_s)],
    ['Dernier redémarrage', RESET_REASONS[s.reset_reason] || `code ${s.reset_reason}`, s.reset_reason === 9 ? 'warn' : ''],
    ['Jeton Envoy', tk ? `expire le ${tk.date}` : '—', tk && tk.days <= 30 ? 'warn' : ''],
  ]);
  fillDl('sys-mem', [
    ['Heap libre', ko(s.heap_free), s.heap_free < s.heap_warn ? 'warn' : ''],
    ['Heap minimum', ko(s.heap_min), s.heap_min < s.heap_warn ? 'warn' : ''],
    ['Seuil d\'alerte', ko(s.heap_warn), 'dim'],
  ]);
  fillDl('sys-net', [
    ['WiFi', s.wifi_connected ? 'connecté' : 'déconnecté', s.wifi_connected ? '' : 'warn'],
    ['Adresse IP', s.ip || '—'],
    ['Signal', rssiLabel(s.rssi)],
    ['Nom', s.hostname + '.local'],
    ['Portail SoftAP', s.ap_active ? 'ouvert' : 'fermé', s.ap_active ? 'warn' : 'dim'],
  ]);
  fillDl('sys-time', [
    ['Heure locale', s.time || '—'],
    ['NTP', s.time_synced ? 'synchronisé' : 'en attente', s.time_synced ? '' : 'warn'],
    ['Dernière synchro', dateTime(s.last_ntp_sync)],
  ]);
  fillDl('sys-hw', [
    ['Révision de puce', s.chip_rev],
    ['Secure Boot v2', s.secure_boot_v2 ? 'possible (puce ≥ v3.0)' : 'non (puce < v3.0)', 'dim'],
    ['Flash', ko(s.flash_size)],
  ]);
  fillDl('sys-cfg', [
    ['État', CFG_STATUS[s.cfg_status] || s.cfg_status],
    ['Tarif', s.tariff_configured ? 'saisi' : 'à saisir', s.tariff_configured ? '' : 'warn'],
    ['Mode', s.observer ? 'observateur' : 'pilotage'],
  ]);
}

function renderDevices(s) {
  const names = { shelly: 'Shelly Pro 3EM', envoy: 'Envoy', zendure: 'SolarFlow', tongou: 'Tongou' };
  const rows = [];
  for (const [k, d] of Object.entries(s.dev)) {
    let txt;
    let cls = '';
    if (!d.configured) { txt = 'non configuré'; cls = 'dim'; }
    else if (d.ok) txt = `OK, ${age(d.age_s)}`;
    else {
      txt = d.age_s === undefined ? 'jamais lu' : `en échec, ${age(d.age_s)}`;
      if (d.last_err) txt += ` (${d.last_err})`;
      cls = 'warn';
    }
    if (d.configured && d.errors) txt += ` · ${d.errors} err.`;
    rows.push([names[k], txt, cls]);
  }
  fillDl('sys-dev', rows);
}

async function pollSystem() {
  try {
    renderSystem(await getJson('/api/system'));
  } catch (e) {
    if (!ws) setConn('off');
  }
}

// ---------- Paramètres ----------

// Bornes identiques à cfg_validate() (components/config/cfg_validate.c).
const SCHEMA = [
  { sec: 'tariff', title: 'Tarif', fields: [
    { k: 'type', l: 'Type de tarif', t: 'select', opts: [['hphc', 'Heures pleines / heures creuses'], ['base', 'Base']] },
    { k: 'hc', l: 'Plages heures creuses', t: 'slots', help: 'HH:MM-HH:MM, jusqu\'à 4 plages (ex. 22:00-06:00).' },
    { k: 'price_hp', l: 'Prix HP', t: 'num', min: 0, max: 2, step: 0.0001, u: '€/kWh' },
    { k: 'price_hc', l: 'Prix HC', t: 'num', min: 0, max: 2, step: 0.0001, u: '€/kWh' },
    { k: 'price_base', l: 'Prix base', t: 'num', min: 0, max: 2, step: 0.0001, u: '€/kWh' },
    { k: 'price_sell', l: 'Prix de revente', t: 'num', min: 0, max: 1, step: 0.0001, u: '€/kWh' },
  ] },
  { sec: 'pv', title: 'Installation PV', fields: [
    { k: 'kwc', l: 'Puissance crête', t: 'num', min: 0.1, max: 36, step: 0.01, u: 'kWc' },
    { k: 'lat', l: 'Latitude', t: 'num', min: -90, max: 90, step: 0.0001, u: '°' },
    { k: 'lon', l: 'Longitude', t: 'num', min: -180, max: 180, step: 0.0001, u: '°' },
    { k: 'tilt', l: 'Inclinaison', t: 'num', min: 0, max: 90, step: 1, u: '°' },
    { k: 'azimuth', l: 'Azimut', t: 'num', min: -180, max: 180, step: 1, u: '°', help: 'Convention d\'Open-Meteo à vérifier (0 = sud ?).' },
  ] },
  { sec: 'batt', title: 'Batterie', fields: [
    { k: 'capa_kwh', l: 'Capacité utile', t: 'num', min: 0.1, max: 50, step: 0.01, u: 'kWh' },
    { k: 'p_charge_max', l: 'Charge max', t: 'num', min: 0, max: 2999, step: 1, u: 'W', help: 'À vérifier sur la fiche du SF 1600 AC+.' },
    { k: 'p_discharge_max', l: 'Décharge max', t: 'num', min: 0, max: 2999, step: 1, u: 'W' },
    { k: 'eta', l: 'Rendement aller-retour', t: 'num', min: 0.5, max: 1, step: 0.01 },
    { k: 'soc_min', l: 'SOC min', t: 'int', min: 0, max: 99, u: '%' },
    { k: 'soc_max', l: 'SOC max', t: 'int', min: 1, max: 100, u: '%' },
    { k: 'price_eur', l: 'Prix de la batterie', t: 'num', min: 0, max: 20000, step: 1, u: '€' },
    { k: 'cycles', l: 'Cycles garantis', t: 'int', min: 1, max: 65535 },
    { k: 'commissioning', l: 'Mise en service', t: 'date' },
    { k: 'arbitrage', l: 'Arbitrage réseau (charge en HC)', t: 'bool' },
  ] },
  { sec: 'wh', title: 'Chauffe-eau', fields: [
    { k: 'power_w', l: 'Puissance nominale', t: 'num', min: 100, max: 3999, step: 1, u: 'W' },
    { k: 'full_heat_min', l: 'Durée de chauffe complète', t: 'int', min: 10, max: 600, u: 'min', help: 'Valeur initiale, apprise ensuite.' },
    { k: 'shower', l: 'Heure des douches', t: 'time' },
    { k: 'forecast_margin', l: 'Marge de prévision solaire', t: 'num', min: 0.3, max: 1, step: 0.05 },
    { k: 'start_margin_min', l: 'Marge de démarrage', t: 'int', min: 0, max: 120, u: 'min' },
    { k: 'catchup_hp', l: 'Rattrapage en heures pleines', t: 'bool' },
    { k: 'solar_on_w', l: 'Seuil de démarrage solaire', t: 'num', min: 100, max: 3999, step: 1, u: 'W' },
    { k: 'min_on_min', l: 'Durée min ON', t: 'int', min: 1, max: 120, u: 'min' },
    { k: 'min_off_min', l: 'Durée min OFF', t: 'int', min: 1, max: 120, u: 'min' },
  ] },
  { sec: 'roi', title: 'Rentabilité', fields: [
    { k: 'yield_kwh_kwc', l: 'Productible', t: 'num', min: 100, max: 3000, step: 1, u: 'kWh/kWc/an' },
    { k: 'month_frac', l: 'Répartition mensuelle (%)', t: 'months', help: 'Janvier → décembre, total = 100.' },
  ] },
  { sec: 'dev', title: 'Appareils', fields: [
    { k: 'shelly_ip', l: 'IP Shelly Pro 3EM', t: 'ip' },
    { k: 'shelly_mode', l: 'Profil du Shelly', t: 'select', opts: [['auto', 'Automatique'], ['triphase', 'Triphasé (EM)'], ['monophase', 'Monophasé (EM1)']] },
    { k: 'shelly_channel', l: 'Canal du réseau (monophasé)', t: 'int', min: 0, max: 2, help: 'Canal EM1 qui mesure l\'arrivée du réseau (0 à 2).' },
    { k: 'envoy_ip', l: 'IP Envoy', t: 'ip' },
    { k: 'envoy_fw7', l: 'Envoy firmware ≥ 7 (HTTPS + jeton)', t: 'bool' },
    { k: 'envoy_token', l: 'Jeton Envoy', t: 'secret', area: true, help: 'Jeton propriétaire (~1 an). Vide = conserver l\'actuel.' },
    { k: 'envoy_skip_cert', l: 'Accepter le certificat auto-signé', t: 'bool' },
    { k: 'zendure_ip', l: 'IP SolarFlow', t: 'ip' },
    { k: 'zendure_sn', l: 'N° de série SolarFlow', t: 'text', max: 31 },
    { k: 'tongou_ip', l: 'IP Tongou', t: 'ip' },
    { k: 'tongou_type', l: 'Firmware du Tongou', t: 'select', opts: [['openbeken', 'OpenBeken (API HTTP)'], ['tuya', 'Tuya d\'origine (protocole local chiffré)']] },
    { k: 'tuya_version', l: 'Version du protocole Tuya', t: 'select', opts: [['3.3', '3.3'], ['3.4', '3.4'], ['3.5', '3.5']] },
    { k: 'tuya_id', l: 'Identifiant Tuya (device id)', t: 'text', max: 23, pattern: '[A-Za-z0-9]*' },
    { k: 'tuya_key', l: 'Clé locale Tuya', t: 'secret', help: '16 caractères. Vide = conserver.' },
    { k: 'tuya_dp_switch', l: 'DP du relais', t: 'int', min: 1, max: 255 },
    { k: 'tuya_dp_power', l: 'DP de la puissance', t: 'int', min: 1, max: 255, help: 'À vérifier avec tools/tuya_ref.py probe.' },
    { k: 'tuya_power_div', l: 'Diviseur de puissance', t: 'int', min: 1, max: 1000, help: 'W = valeur brute ÷ diviseur (souvent 10).' },
  ] },
  { sec: 'sys', title: 'Système', fields: [
    { k: 'poll_ms', l: 'Période de sondage du Shelly', t: 'int', min: 1000, max: 10000, u: 'ms' },
    { k: 'observer', l: 'Mode observateur (aucune commande)', t: 'bool' },
    { k: 'hostname', l: 'Nom réseau', t: 'text', max: 31, pattern: '[a-z0-9]([a-z0-9-]*[a-z0-9])?', help: 'Redémarrage si modifié.' },
    { k: 'wifi_ssid', l: 'WiFi (SSID)', t: 'text', max: 32, help: 'Redémarrage si modifié.' },
    { k: 'wifi_pass', l: 'Mot de passe WiFi', t: 'secret', help: 'Vide = conserver.' },
    { k: 'admin_pass', l: 'Nouveau mot de passe admin', t: 'secret', help: '8 caractères min. Vide = conserver.' },
  ] },
];

const MONTHS = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'];
const IP_RE = /^((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)$/;
const SLOT_RE = /^([01]\d|2[0-3]):[0-5]\d-([01]\d|2[0-3]):[0-5]\d$/;

const dirty = new Set();
let cfgLoaded = false;

function el(tag, attrs, parent) {
  const e = document.createElement(tag);
  for (const k in attrs) {
    if (k === 'text') e.textContent = attrs[k];
    else e.setAttribute(k, attrs[k]);
  }
  if (parent) parent.append(e);
  return e;
}

function fid(sec, k, i) {
  return `f-${sec}-${k}` + (i !== undefined ? `-${i}` : '');
}

function buildForm() {
  const root = $('#cfg-sections');
  for (const s of SCHEMA) {
    const card = el('fieldset', { class: 'card', 'data-sec': s.sec }, root);
    el('legend', { text: s.title }, card);
    for (const f of s.fields) {
      const row = el('div', { class: 'field' + (f.t === 'bool' ? ' check' : ''), id: fid(s.sec, f.k) + '-row' }, card);
      const id = fid(s.sec, f.k);
      if (f.t === 'bool') {
        el('input', { type: 'checkbox', id }, row);
        el('label', { for: id, text: f.l }, row);
      } else {
        el('label', { for: f.t === 'slots' || f.t === 'months' ? fid(s.sec, f.k, 0) : id, text: f.l + (f.u ? ` (${f.u})` : '') }, row);
        if (f.t === 'select') {
          const sel = el('select', { id }, row);
          for (const [v, l] of f.opts) el('option', { value: v, text: l }, sel);
        } else if (f.t === 'slots') {
          const box = el('div', { class: 'multi' }, row);
          for (let i = 0; i < 4; i++) el('input', { id: fid(s.sec, f.k, i), placeholder: '—', inputmode: 'numeric', maxlength: 11, 'aria-label': `Plage ${i + 1}` }, box);
        } else if (f.t === 'months') {
          const box = el('div', { class: 'multi months' }, row);
          MONTHS.forEach((m, i) => el('input', { id: fid(s.sec, f.k, i), type: 'number', min: 0, max: 100, step: 0.1, 'aria-label': m, title: m }, box));
        } else if (f.t === 'secret' && f.area) {
          el('textarea', { id, rows: 3, spellcheck: 'false', autocomplete: 'off' }, row);
        } else {
          const type = { num: 'number', int: 'number', secret: 'password', time: 'time', date: 'date' }[f.t] || 'text';
          const attrs = { id, type, autocomplete: f.t === 'secret' ? 'new-password' : 'off' };
          if (f.min !== undefined) attrs.min = f.min;
          if (f.max !== undefined) attrs[f.t === 'text' ? 'maxlength' : 'max'] = f.max;
          if (f.step !== undefined) attrs.step = f.step;
          if (f.t === 'int') attrs.step = 1;
          if (f.pattern) attrs.pattern = f.pattern;
          if (f.t === 'ip') { attrs.inputmode = 'decimal'; attrs.placeholder = 'non configuré'; }
          el('input', attrs, row);
        }
      }
      if (f.help) el('small', { text: f.help }, row);
    }
    card.addEventListener('input', () => dirty.add(s.sec));
    card.addEventListener('change', () => dirty.add(s.sec));
  }
  $('#cfg-form').addEventListener('submit', saveConfig);
}

function fillForm(cfg) {
  for (const s of SCHEMA) {
    const v = cfg[s.sec] || {};
    for (const f of s.fields) {
      const id = fid(s.sec, f.k);
      const val = v[f.k];
      if (f.t === 'bool') document.getElementById(id).checked = !!val;
      else if (f.t === 'slots') for (let i = 0; i < 4; i++) document.getElementById(fid(s.sec, f.k, i)).value = (val || [])[i] || '';
      else if (f.t === 'months') for (let i = 0; i < 12; i++) document.getElementById(fid(s.sec, f.k, i)).value = (val || [])[i] ?? '';
      else if (f.t === 'secret') {
        const e = document.getElementById(id);
        e.value = '';
        e.placeholder = val === '***' ? 'défini — laisser vide pour conserver' : 'non défini';
      } else document.getElementById(id).value = val ?? '';
    }
  }
  if (cfg.tariff && !cfg.tariff.configured) {
    setMsg('Tarif non saisi : renseignez la section Tarif.', true);
  }
  dirty.clear();
}

async function loadConfig() {
  if (cfgLoaded && dirty.size) return;     // ne pas écraser une saisie en cours
  try {
    fillForm(await getJson('/api/config'));
    cfgLoaded = true;
  } catch (e) {
    setMsg('Configuration illisible : ' + e.message, true);
  }
}

function setMsg(text, bad) {
  const m = $('#cfg-msg');
  m.textContent = text;
  m.className = bad ? 'bad' : 'good';
}

function markField(field) {
  document.querySelectorAll('.field.bad').forEach((r) => r.classList.remove('bad'));
  if (!field) return;
  const [sec, k] = field.split('.');
  const row = document.getElementById(fid(sec, k) + '-row');
  if (row) {
    row.classList.add('bad');
    row.scrollIntoView({ block: 'center', behavior: 'smooth' });
  }
}

// Lit une section ; lève {field, msg} sur la première valeur invalide.
function readSection(s) {
  const out = {};
  for (const f of s.fields) {
    const id = fid(s.sec, f.k);
    const field = `${s.sec}.${f.k}`;
    const fail = (msg) => { throw { field, msg: `${f.l} : ${msg}` }; };
    if (f.t === 'bool') { out[f.k] = document.getElementById(id).checked; continue; }
    if (f.t === 'slots') {
      const list = [];
      for (let i = 0; i < 4; i++) {
        const v = document.getElementById(fid(s.sec, f.k, i)).value.trim();
        if (!v) continue;
        if (!SLOT_RE.test(v)) fail('format HH:MM-HH:MM attendu');
        list.push(v);
      }
      out[f.k] = list;
      continue;
    }
    if (f.t === 'months') {
      const list = [];
      for (let i = 0; i < 12; i++) {
        const v = parseFloat(document.getElementById(fid(s.sec, f.k, i)).value);
        if (!(v >= 0 && v <= 100)) fail(`${MONTHS[i]} entre 0 et 100`);
        list.push(v);
      }
      const sum = list.reduce((a, v) => a + v, 0);
      if (Math.abs(sum - 100) > 0.1) fail(`total ${fr(sum, 1)} % au lieu de 100 %`);
      out[f.k] = list;
      continue;
    }
    const e = document.getElementById(id);
    const v = e.value.trim();
    if (f.t === 'num' || f.t === 'int') {
      const n = Number(v);
      if (v === '' || !Number.isFinite(n)) fail('nombre attendu');
      if (f.t === 'int' && !Number.isInteger(n)) fail('nombre entier attendu');
      if (n < f.min || n > f.max) fail(`entre ${f.min} et ${f.max}`);
      out[f.k] = n;
    } else if (f.t === 'ip') {
      if (v && !IP_RE.test(v)) fail('adresse IPv4 attendue');
      out[f.k] = v;
    } else if (f.t === 'secret') {
      if (f.k === 'admin_pass' && v && v.length < 8) fail('8 caractères minimum');
      if (f.k === 'wifi_pass' && v && v.length < 8) fail('8 caractères minimum');
      if (f.k === 'tuya_key' && v && v.length !== 16) fail('16 caractères exactement');
      out[f.k] = v || '***';
    } else if (f.t === 'time') {
      if (!/^\d\d:\d\d$/.test(v)) fail('heure HH:MM attendue');
      out[f.k] = v;
    } else {
      if (e.validity && !e.validity.valid) fail('valeur invalide');
      out[f.k] = v;
    }
  }
  return out;
}

async function saveConfig(ev) {
  ev.preventDefault();
  markField(null);
  const pass = $('#cfg-admin').value;
  if (!pass) { setMsg('Saisissez le mot de passe administrateur.', true); $('#cfg-admin').focus(); return; }
  if (!dirty.size) { setMsg('Aucune modification.', false); return; }
  const body = {};
  try {
    for (const s of SCHEMA) if (dirty.has(s.sec)) body[s.sec] = readSection(s);
  } catch (e) {
    markField(e.field);
    setMsg(e.msg, true);
    return;
  }
  $('#cfg-save').disabled = true;
  try {
    const r = await getJson('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Admin-Password': pass },
      body: JSON.stringify(body),
    });
    dirty.clear();
    cfgLoaded = false;
    setMsg(r.reboot_required ? 'Enregistré. Redémarrage en cours pour appliquer le réseau…' : 'Enregistré.', false);
    if (!r.reboot_required) loadConfig();
  } catch (e) {
    markField(e.field);
    setMsg(e.status === 401 ? 'Mot de passe administrateur incorrect.' : `Refusé : ${e.message}` + (e.field ? ` (${e.field})` : ''), true);
  } finally {
    $('#cfg-save').disabled = false;
  }
}

// ---------- Graphiques SVG (barres empilées, courbes, survol) ----------

const fmtKwh = (wh) => fr(wh / 1000, Math.abs(wh) < 10000 ? 2 : 1) + ' kWh';
const fmtEur = (v) => (v < 0 ? '−' : '') + fr(Math.abs(v), 2) + ' €';
const hhmm = (t) => new Date(t * 1000).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
const dayLabel = (t) => new Date(t * 1000).toLocaleDateString('fr-FR', { weekday: 'short', day: 'numeric' });

function niceStep(range, n) {
  const raw = range / n;
  const p = Math.pow(10, Math.floor(Math.log10(raw)));
  const f = raw / p;
  return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * p;
}

// Hachures : second codage de la batterie (rouge clair) et de l'export (blanc), voir légende.
function hatchDefs(root) {
  const defs = svg('defs', {}, root);
  for (const [id, color] of [['h-batt', 'var(--red-soft)'], ['h-exp', 'var(--white)']]) {
    const p = svg('pattern', { id, width: 5, height: 5, patternUnits: 'userSpaceOnUse', patternTransform: 'rotate(45)' }, defs);
    svg('rect', { width: 5, height: 5, fill: 'var(--surface)' }, p);
    svg('line', { x1: 0, y1: 0, x2: 0, y2: 5, stroke: color, 'stroke-width': 2.4 }, p);
  }
}

function legend(el, items) {
  el.replaceChildren();
  for (const it of items) {
    const span = document.createElement('span');
    const sw = document.createElement('i');
    sw.className = 'sw' + (it.hatch ? ' hatch' : '') + (it.line ? ' line' : '') + (it.dash ? ' dash' : '');
    sw.style.setProperty('--c', it.color);
    span.append(sw, it.name);
    el.append(span);
  }
}

const tip = () => $('#tip');

function showTip(ev, html) {
  const t = tip();
  t.replaceChildren(...html);
  t.hidden = false;
  const r = t.getBoundingClientRect();
  let x = ev.clientX + 14;
  if (x + r.width > window.innerWidth - 8) x = ev.clientX - r.width - 14;
  t.style.left = Math.max(8, x) + 'px';
  t.style.top = Math.max(8, ev.clientY - r.height - 12) + 'px';
}

function tipRow(color, name, value, hatch) {
  const d = document.createElement('div');
  const sw = document.createElement('i');
  sw.className = 'sw' + (hatch ? ' hatch' : '');
  sw.style.setProperty('--c', color);
  const n = document.createElement('span');
  n.textContent = name;
  const v = document.createElement('b');
  v.textContent = value;
  d.append(sw, n, v);
  return d;
}

/* cfg : { t[], step (s), bars:[{name, vals[], color, fill, hatch, neg}], lines:[{name, vals[], color, dash}],
 *         fmt(v), xMode:'hour'|'day', yMin, yMax, height } */
function drawChart(box, cfg) {
  const W = Math.max(280, Math.round(box.clientWidth || 600));
  const H = cfg.height || 210;
  const m = { l: 46, r: 8, t: 10, b: 22 };
  const pw = W - m.l - m.r;
  const ph = H - m.t - m.b;
  const n = cfg.t.length;
  const bars = cfg.bars || [];
  const lines = cfg.lines || [];

  let hi = cfg.yMax ?? 0;
  let lo = cfg.yMin ?? 0;
  for (let i = 0; i < n; i++) {
    let pos = 0, neg = 0;
    for (const b of bars) {
      const v = b.vals[i];
      if (v == null) continue;
      if (b.neg) neg -= v; else pos += v;
    }
    hi = Math.max(hi, pos);
    lo = Math.min(lo, neg);
    for (const l of lines) {
      const v = l.vals[i];
      if (v != null) { hi = Math.max(hi, v); lo = Math.min(lo, v); }
    }
  }
  if (hi - lo < 1) hi = lo + 1;
  if (cfg.yMax == null) hi += (hi - lo) * 0.05;     // marge : la courbe ne colle pas au bord
  const step = niceStep(hi - lo, 4);
  hi = Math.ceil(hi / step) * step;
  lo = Math.floor(lo / step) * step;
  const y = (v) => m.t + ((hi - v) / (hi - lo)) * ph;
  const bw = pw / Math.max(1, n);
  const x = (i) => m.l + i * bw;

  const root = document.createElementNS(SVG_NS, 'svg');
  root.setAttribute('width', W);
  root.setAttribute('height', H);
  root.setAttribute('viewBox', `0 0 ${W} ${H}`);
  root.setAttribute('aria-hidden', 'true');
  hatchDefs(root);

  for (let v = lo; v <= hi + step / 2; v += step) {
    svg('line', { x1: m.l, x2: W - m.r, y1: y(v), y2: y(v), class: v === 0 ? 'axis0' : 'grid' }, root);
    const t = svg('text', { x: m.l - 6, y: y(v) + 4, class: 'tick', 'text-anchor': 'end' }, root);
    t.textContent = cfg.fmt(v, true);
  }
  // Graduations horaires (toutes les 3 h) ou journalières.
  for (let i = 0; i < n; i++) {
    const d = new Date(cfg.t[i] * 1000);
    const mark = cfg.xMode === 'day' ? d.getHours() === 0 && d.getMinutes() === 0
                                     : d.getMinutes() === 0 && d.getHours() % 3 === 0;
    if (!mark) continue;
    svg('line', { x1: x(i), x2: x(i), y1: m.t, y2: H - m.b, class: 'grid' }, root);
    const t = svg('text', { x: x(i) + 2, y: H - 6, class: 'tick' }, root);
    t.textContent = cfg.xMode === 'day' ? dayLabel(cfg.t[i]) : d.getHours() + ' h';
  }

  const gap = bw > 3 ? 1 : 0;
  for (let i = 0; i < n; i++) {
    let up = 0, down = 0;
    for (const b of bars) {
      const v = b.vals[i];
      if (v == null || v <= 0) continue;
      const from = b.neg ? down : up;
      const to = b.neg ? down - v : up + v;
      if (b.neg) down = to; else up = to;
      const top = Math.min(y(from), y(to));
      svg('rect', {
        x: x(i) + gap / 2, y: top, width: Math.max(0.6, bw - gap), height: Math.max(0.5, Math.abs(y(from) - y(to))),
        fill: b.hatch ? `url(#${b.hatch})` : b.color, 'fill-opacity': b.opacity ?? 1,
      }, root);
    }
  }
  for (const l of lines) {
    let d = '';
    let pen = false;
    for (let i = 0; i < n; i++) {
      const v = l.vals[i];
      if (v == null) { pen = false; continue; }
      d += `${pen ? 'L' : 'M'}${(x(i) + bw / 2).toFixed(1)},${y(v).toFixed(1)}`;
      pen = true;
    }
    const p = svg('path', { d, class: 'cline', stroke: l.color }, root);
    if (l.dash) p.setAttribute('stroke-dasharray', '5 4');
  }

  // Survol : ligne de repère et infobulle (cible = toute la zone du graphique).
  const cross = svg('line', { y1: m.t, y2: H - m.b, class: 'cross', visibility: 'hidden' }, root);
  const hit = svg('rect', { x: m.l, y: m.t, width: pw, height: ph, fill: 'transparent' }, root);
  const move = (ev) => {
    const r = root.getBoundingClientRect();
    const i = Math.floor((ev.clientX - r.left - m.l) / bw);
    if (i < 0 || i >= n) return;
    cross.setAttribute('x1', x(i) + bw / 2);
    cross.setAttribute('x2', x(i) + bw / 2);
    cross.setAttribute('visibility', 'visible');
    const head = document.createElement('div');
    head.className = 'tip-head';
    head.textContent = (cfg.xMode === 'day' ? dayLabel(cfg.t[i]) + ' ' : '') + hhmm(cfg.t[i]);
    const rows = [head];
    for (const b of bars) {
      const v = b.vals[i];
      rows.push(tipRow(b.color, b.name, v == null ? '—' : cfg.fmt(b.neg ? -v : v), !!b.hatch));
    }
    for (const l of lines) rows.push(tipRow(l.color, l.name, l.vals[i] == null ? '—' : cfg.fmt(l.vals[i])));
    showTip(ev, rows);
  };
  hit.addEventListener('pointermove', move);
  hit.addEventListener('pointerdown', move);
  hit.addEventListener('pointerleave', () => { cross.setAttribute('visibility', 'hidden'); tip().hidden = true; });

  box.replaceChildren(root);
  box.chartCfg = cfg;
  box.chartWidth = W;
  chartResize.observe(box);
}

// Redessine un graphique quand la largeur de son conteneur change (barre de défilement qui
// apparaît, rotation, panneau) : le SVG est dessiné à la largeur mesurée.
const chartResize = new ResizeObserver((entries) => {
  for (const e of entries) {
    const box = e.target;
    const w = Math.max(280, Math.round(box.clientWidth));
    if (box.chartCfg && box.offsetParent !== null && Math.abs(w - box.chartWidth) > 2) {
      requestAnimationFrame(() => drawChart(box, box.chartCfg));
    }
  }
});

const fmtWaxis = (v, axis) => (axis && Math.abs(v) >= 1000 ? fr(v / 1000, 1) + ' k' : Math.round(v) + (axis ? '' : ' W'));

function segment(groupSel, attr, onChange) {
  const btns = document.querySelectorAll(`${groupSel} button`);
  btns.forEach((b) => b.addEventListener('click', () => {
    btns.forEach((o) => o.setAttribute('aria-pressed', o === b ? 'true' : 'false'));
    onChange(b.dataset[attr]);
  }));
}

// ---------- Accueil : jauge, gains, tarif, diagramme en kWh ----------

let unit = 'w';
let gaugePeriod = 'day';
const statsCache = {};

async function loadStats(period) {
  statsCache[period] = await getJson('/api/stats?period=' + period);
  return statsCache[period];
}

function drawGauge(v, sub) {
  const g = $('#gauge');
  g.replaceChildren();
  const arc = 'M20,100 A80,80 0 0 1 180,100';
  svg('path', { d: arc, class: 'gauge-bg' }, g);
  const len = Math.PI * 80;
  const color = v == null ? 'var(--line)' : v < 0.5 ? 'var(--red)' : v <= 0.8 ? 'var(--red-soft)' : 'var(--white)';
  svg('path', { d: arc, class: 'gauge-val', stroke: color, 'stroke-dasharray': `${(v || 0) * len} ${len}` }, g);
  const t = svg('text', { x: 100, y: 94, class: 'gauge-num' }, g);
  t.textContent = v == null ? '—' : Math.round(v * 100) + ' %';
  $('#gauge-label').textContent = sub;
}

function renderGauge(st) {
  const p = st.autoprod;
  const sub = st.kwh.pv > 0 ? `Autoproduction ${p == null ? '—' : Math.round(p * 100) + ' %'} · PV ${fmtKwh(st.kwh.pv * 1000)}`
                            : 'Pas encore de production mesurée sur la période';
  drawGauge(st.autoconso, sub);
}

function renderGains(day, month) {
  const row = (label, st) => {
    if (!st) return [label, '—'];
    const g = st.gain;
    return [label, `${fmtEur(g.total)} (batterie ${fmtEur(g.batt_net)}, chauffe-eau ${fmtEur(g.ce)})`];
  };
  fillDl('gains', [row('Aujourd\'hui', day), row('30 jours', month)]);

  const roi = (day || month || {}).roi;
  if (!roi) return;
  const bar = $('#roi-bar span');
  const pct = roi.price > 0 ? Math.max(0, Math.min(1, roi.cum_net / roi.price)) : 0;
  bar.style.width = (pct * 100).toFixed(1) + '%';
  $('#roi-bar').setAttribute('aria-valuenow', Math.round(pct * 100));
  $('#roi-pct').textContent = roi.price > 0 ? `${fmtEur(roi.cum_net)} / ${fmtEur(roi.price)}` : '';
  let txt;
  if (!(roi.price > 0)) txt = 'Prix de la batterie à saisir dans les Paramètres.';
  else if (roi.payback_years == null) txt = 'Pas encore de gain projeté.';
  else {
    const c = roi.commissioning ? new Date(roi.commissioning) : null;
    const y0 = c ? c.getFullYear() + c.getMonth() / 12 : null;
    // Année de rentabilité si la mise en service est connue, sinon durée.
    const when = (yrs) => (y0 == null ? `${Math.round(yrs)} ans` : `${Math.round(y0 + yrs)}`);
    txt = y0 == null ? `Batterie rentabilisée dans ~${when(roi.payback_years)}`
                     : `Batterie rentabilisée en ~${when(roi.payback_years)}`;
    if (roi.estimate) {
      txt += ` — estimation (${roi.months_observed} mois observé${roi.months_observed > 1 ? 's' : ''}, ` +
             `fourchette ${when(roi.payback_years * (1 - roi.spread))} à ${when(roi.payback_years * (1 + roi.spread))})`;
    }
    txt += ` · ${fmtEur(roi.annual)}/an projetés.`;
  }
  $('#roi-txt').textContent = txt;
}

function renderTariff(t) {
  const el = $('#tariff');
  if (!t || !t.configured) { el.textContent = 'Tarif non saisi (Paramètres → Tarif).'; return; }
  if (t.price === undefined) { el.textContent = 'En attente de l\'heure (NTP).'; return; }
  const name = !t.hphc ? 'Tarif base' : t.hc ? 'Heures creuses' : 'Heures pleines';
  let txt = `${name} · ${fr(t.price, 4)} €/kWh`;
  if (t.next_min) {
    const h = Math.floor(t.next_min / 60), mn = t.next_min % 60;
    txt += ` · ${t.next_hc ? 'HC' : 'HP'} dans ${h ? h + ' h ' : ''}${String(mn).padStart(h ? 2 : 1, '0')} min`;
  }
  el.textContent = txt;
  el.classList.toggle('hc', !!t.hc);
}

// Diagramme en énergies du jour : mêmes chemins, valeurs de /api/stats.
function renderFlowsKwh(st) {
  const k = st.kwh;
  const wh = {
    pv_home: k.pv_home, pv_batt: k.pv_batt, pv_grid: k.pv_grid, grid_home: k.grid_home,
    grid_batt: k.grid_batt, batt_home: k.batt_home, home_ce: k.ce,
  };
  for (const [id, el] of Object.entries(flowEls)) {
    const v = (wh[id] || 0) * 1000;
    el.w = v > ACTIVE_W ? 60 + Math.sqrt(v) : 0;      // vitesse indicative en mode énergie
    el.line.style.display = v > ACTIVE_W ? '' : 'none';
    el.dots.style.display = v > ACTIVE_W ? '' : 'none';
    el.line.setAttribute('stroke-width', 2 + 4 * Math.sqrt(Math.min(v, 10000) / 10000));
  }
  const s = lastState || { pv: {}, batt: {}, ce: {}, grid: {} };
  setNode('pv', fmtKwh(k.pv * 1000), 'production', s.pv.configured !== false);
  setNode('grid', '→ ' + fmtKwh(k.import * 1000), '← ' + fmtKwh(k.export * 1000) + ' exportés', true);
  setNode('batt', '▲ ' + fmtKwh(k.batt_ch * 1000), '▼ ' + fmtKwh(k.batt_dis * 1000), s.batt.configured !== false);
  setNode('home', fmtKwh(k.home * 1000), '', true);
  setNode('ce', fmtKwh(k.ce * 1000), k.ce > 0 ? `dont local ${fmtKwh(k.ce_local * 1000)}` : '', s.ce.configured !== false);
  const parts = [k.pv_home, k.batt_home, k.grid_home];
  const tot = parts.reduce((a, v) => a + v, 0);
  let start = 0;
  nodeEls.ring.forEach((c, i) => {
    const frac = tot > 0 ? parts[i] / tot : 0;
    setRing(c, start, frac);
    start += frac;
  });
}

async function refreshHome() {
  try {
    const day = await loadStats('day');
    if (unit === 'kwh') renderFlowsKwh(day);
    renderGains(day, statsCache['30d']);
    if (gaugePeriod === 'day') renderGauge(day);
    else renderGauge(await loadStats(gaugePeriod));
    renderGains(day, await loadStats('30d'));
  } catch (e) {
    $('#gauge-label').textContent = e.status === 503 ? 'En attente de l\'heure (NTP).' : 'Statistiques indisponibles.';
  }
}

// ---------- Prévisions ----------

let fcDay = 0;

async function loadForecast() {
  let fc, hist;
  try {
    [fc, hist] = await Promise.all([getJson('/api/forecast'), getJson('/api/history?range=24h').catch(() => null)]);
  } catch (e) {
    alertBox('fc-alerts', [[`Prévisions indisponibles : ${e.message}`]]);
    return;
  }
  const al = [];
  if (!fc.coords) al.push(['Latitude et longitude non renseignées : pas de prévision météo (Paramètres → Installation PV).']);
  else if (!fc.ok) al.push(['Prévision Open-Meteo pas encore reçue.']);
  else al.push([`Prévision Open-Meteo reçue à ${hhmm(fc.fetched)}.`, 'info']);
  alertBox('fc-alerts', al);

  const rows = fc.rows.slice(fcDay * 48, fcDay * 48 + 48);
  const t = rows.map((r) => r[0]);
  const W = (wh) => (wh == null ? null : wh * 3600 / fc.slot_s);
  const actual = {};
  if (hist && fcDay === 0) for (const r of hist.rows) actual[r[0]] = r;
  const act = (i, f) => { const r = actual[t[i]]; return r && r[9] >= 80 ? W(f(r)) : null; };

  const pvRaw = rows.map((r) => W(r[3]));
  const pv = rows.map((r) => W(r[4]));
  const pvAct = t.map((_, i) => act(i, (r) => r[1]));
  legend($('#fc-pv-legend'), [
    { name: 'Mesuré', color: 'var(--white)' },
    { name: 'Prévu (modèle)', color: 'var(--white)', line: true },
    { name: 'Repli physique', color: 'var(--text-3)', line: true, dash: true },
  ]);
  drawChart($('#fc-pv'), {
    t, step: fc.slot_s, fmt: fmtWaxis,
    bars: [{ name: 'Mesuré', vals: pvAct, color: 'var(--white)', opacity: 0.4 }],
    lines: [{ name: 'Prévu', vals: pv, color: 'var(--white)' }, { name: 'Repli', vals: pvRaw, color: 'var(--text-3)', dash: true }],
  });

  const baseAct = t.map((_, i) => act(i, (r) => r[2] - r[3]));
  legend($('#fc-base-legend'), [
    { name: 'Mesurée', color: 'var(--red-soft)' },
    { name: 'Prévue (modèle)', color: 'var(--red-soft)', line: true },
    { name: 'Profil moyen', color: 'var(--text-3)', line: true, dash: true },
  ]);
  drawChart($('#fc-base'), {
    t, step: fc.slot_s, fmt: fmtWaxis,
    bars: [{ name: 'Mesurée', vals: baseAct, color: 'var(--red-soft)', opacity: 0.45 }],
    lines: [{ name: 'Prévue', vals: rows.map((r) => W(r[6])), color: 'var(--red-soft)' },
            { name: 'Profil', vals: rows.map((r) => W(r[5])), color: 'var(--text-3)', dash: true }],
  });

  const sumKwh = (vals) => vals.reduce((a, v) => a + (v || 0), 0) / 1000 * fc.slot_s / 3600;
  const quality = (mdl, total) => [
    ['Total prévu', fr(total, 1) + ' kWh'],
    ['Écart moyen (7 j)', mdl.mae7 == null ? 'pas encore de mesure' : `${mdl.mae7} Wh par créneau (${mdl.n7} créneaux)`],
    ['Apprentissage', mdl.ready ? `${mdl.samples} échantillons` : `rodage : ${mdl.samples} / 96 (repli utilisé)`, mdl.ready ? '' : 'dim'],
  ];
  fillDl('fc-pv-q', quality(fc.pv, sumKwh(pv)));
  fillDl('fc-base-q', quality(fc.base, sumKwh(rows.map((r) => W(r[6])))));
}

// ---------- Historique ----------

let histRange = '24h';
let histData = null;

function histSeries(h) {
  // 7 jours : regroupement par heure (2 créneaux) pour rester lisible sur mobile.
  const k = h.range === '7d' ? 2 : 1;
  const step = h.slot_s * k;
  const out = { t: [], step, pv: [], dis: [], imp: [], ch: [], exp: [], soc: [], home: [] };
  for (let i = 0; i + k <= h.rows.length; i += k) {
    const grp = h.rows.slice(i, i + k);
    const sum = (c) => grp.reduce((a, r) => a + r[c], 0) * 3600 / step;
    out.t.push(grp[0][0]);
    out.pv.push(sum(1)); out.home.push(sum(2)); out.imp.push(sum(4)); out.exp.push(sum(5));
    out.ch.push(sum(6)); out.dis.push(sum(7));
    const soc = grp[grp.length - 1][8];
    out.soc.push(soc);
  }
  return out;
}

function renderHistory() {
  const h = histData;
  if (!h) return;
  const s = histSeries(h);
  if (!s.t.length) {
    $('#hist-chart').textContent = 'Pas encore de créneau enregistré (le premier est clos au bout de 30 min).';
    $('#hist-totals').textContent = '';
    $('#soc-card').hidden = true;
    return;
  }
  const bars = [
    { name: 'Solaire', vals: s.pv, color: 'var(--white)' },
    { name: 'Batterie (décharge)', vals: s.dis, color: 'var(--red-soft)', hatch: 'h-batt' },
    { name: 'Réseau (import)', vals: s.imp, color: 'var(--red)' },
    { name: 'Batterie (charge)', vals: s.ch, color: 'var(--red-soft)', hatch: 'h-batt', neg: true },
    { name: 'Réseau (export)', vals: s.exp, color: 'var(--white)', hatch: 'h-exp', neg: true },
  ];
  legend($('#hist-legend'), bars.map((b) => ({ name: b.name, color: b.color, hatch: !!b.hatch })));
  drawChart($('#hist-chart'), { t: s.t, step: s.step, bars, fmt: fmtWaxis, xMode: h.range === '7d' ? 'day' : 'hour' });

  const kwh = (vals) => fmtKwh(vals.reduce((a, v) => a + v, 0) * s.step / 3600);
  $('#hist-totals').textContent = `Production ${kwh(s.pv)} · Maison ${kwh(s.home)} · Import ${kwh(s.imp)} · ` +
                                  `Export ${kwh(s.exp)} · Batterie ▲ ${kwh(s.ch)} ▼ ${kwh(s.dis)}`;

  const hasSoc = s.soc.some((v) => v != null);
  $('#soc-card').hidden = !hasSoc;
  if (hasSoc) {
    drawChart($('#soc-chart'), {
      t: s.t, step: s.step, height: 130, yMin: 0, yMax: 100, xMode: h.range === '7d' ? 'day' : 'hour',
      fmt: (v) => Math.round(v) + (v === 100 || v === 0 ? '' : '') + ' %',
      lines: [{ name: 'SOC', vals: s.soc, color: 'var(--red-soft)' }],
    });
  }
  if ($('#hist-table').open) buildHistTable();
}

function buildHistTable() {
  const h = histData;
  const wrap = $('#hist-table .table-wrap');
  if (!h) return;
  const table = document.createElement('table');
  const head = ['Créneau', 'Solaire (Wh)', 'Maison (Wh)', 'Chauffe-eau (Wh)', 'Import (Wh)', 'Export (Wh)',
                'Charge (Wh)', 'Décharge (Wh)', 'SOC (%)', 'Couverture (%)'];
  const tr = document.createElement('tr');
  for (const c of head) { const th = document.createElement('th'); th.textContent = c; tr.append(th); }
  table.append(tr);
  for (const r of h.rows.slice().reverse()) {
    const row = document.createElement('tr');
    const cells = [`${dayLabel(r[0])} ${hhmm(r[0])}`, ...r.slice(1, 8).map((v) => Math.round(v)), r[8] ?? '—', r[9]];
    for (const c of cells) { const td = document.createElement('td'); td.textContent = c; row.append(td); }
    table.append(row);
  }
  wrap.replaceChildren(table);
}

async function loadHistory() {
  try {
    histData = await getJson('/api/history?range=' + histRange);
    renderHistory();
  } catch (e) {
    $('#hist-chart').textContent = e.status === 503 ? 'En attente de l\'heure (NTP).' : `Historique indisponible : ${e.message}`;
  }
}

// ---------- Démarrage ----------

segment('.flow-card .seg', 'unit', (u) => {
  unit = u;
  if (u === 'kwh' && statsCache.day) renderFlowsKwh(statsCache.day);
  else if (u === 'w' && lastState) renderFlows(lastState);
  refreshHome();
});
segment('#gauge-period', 'p', (p) => { gaugePeriod = p; refreshHome(); });
segment('#fc-day', 'd', (d) => { fcDay = Number(d); loadForecast(); });
segment('#hist-range', 'r', (r) => { histRange = r; loadHistory(); });
$('#hist-table').addEventListener('toggle', () => { if ($('#hist-table').open) buildHistTable(); });

buildDiagram();
buildForm();
window.addEventListener('hashchange', showPage);
showPage();
pollState();
connectWs();
pollSystem();
refreshHome();
setInterval(() => { if (!document.hidden) pollSystem(); }, SYSTEM_POLL_MS);
setInterval(() => { if (!document.hidden) refreshHome(); }, 60000);
