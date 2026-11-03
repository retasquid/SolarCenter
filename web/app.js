'use strict';

const PAGES = ['accueil', 'previsions', 'historique', 'parametres', 'systeme'];
const DEFAULT_PAGE = 'systeme';   // l'accueil prendra le relais au M1
const SYSTEM_POLL_MS = 5000;
const FETCH_TIMEOUT_MS = 4000;

const $ = (sel) => document.querySelector(sel);

// ---------- Navigation ----------

function showPage() {
  const wanted = location.hash.slice(1);
  const id = PAGES.includes(wanted) ? wanted : DEFAULT_PAGE;
  for (const p of PAGES) {
    document.getElementById(p).classList.toggle('active', p === id);
  }
  document.querySelectorAll('.tabs a').forEach((a) => {
    if (a.getAttribute('href') === '#' + id) {
      a.setAttribute('aria-current', 'page');
    } else {
      a.removeAttribute('aria-current');
    }
  });
}

// ---------- Utilitaires ----------

async function getJson(url) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), FETCH_TIMEOUT_MS);
  try {
    const r = await fetch(url, { signal: ctl.signal, cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return await r.json();
  } finally {
    clearTimeout(timer);
  }
}

// rows : [libellé, valeur, classe facultative] ; tout passe par textContent.
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

const ko = (b) => (b / 1024).toFixed(1).replace('.', ',') + ' Ko';

function duration(s) {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d} j ${h} h ${m} min`;
  if (h > 0) return `${h} h ${m} min`;
  return `${m} min ${s % 60} s`;
}

function rssiLabel(rssi) {
  if (!rssi) return '—';
  const q = rssi >= -60 ? 'bon' : rssi >= -70 ? 'correct' : rssi >= -80 ? 'faible' : 'très faible';
  return `${rssi} dBm (${q})`;
}

function dateTime(epoch) {
  if (!epoch) return '—';
  return new Date(epoch * 1000).toLocaleString('fr-FR');
}

const RESET_REASONS = {
  1: 'mise sous tension', 3: 'logiciel', 4: 'exception', 5: 'watchdog (interruption)',
  6: 'watchdog (tâche)', 7: 'watchdog', 8: 'sortie de veille', 9: 'baisse de tension',
};

const CFG_STATUS = {
  ok: 'chargée',
  first_boot: 'premier démarrage (valeurs par défaut)',
  reset_version: 'version changée : valeurs par défaut',
  reset_crc: 'corrompue : valeurs par défaut',
  reset_invalid: 'invalide : valeurs par défaut',
  reset_read: 'illisible : valeurs par défaut',
};

// ---------- Page Système ----------

function renderAlerts(s) {
  const list = [];
  if (!s.admin_set) {
    list.push(['Mot de passe administrateur non défini : ouvrez la page de configuration (/setup).']);
  }
  if (s.cfg_status !== 'ok' && s.cfg_status !== 'first_boot') {
    list.push([`Configuration ${CFG_STATUS[s.cfg_status] || s.cfg_status}. Vérifiez les paramètres.`]);
  }
  if (!s.tariff_configured) {
    list.push(['Tarif non saisi : aucune régulation basée sur les prix tant qu\'il manque.']);
  }
  if (s.ntp_alert) {
    list.push([s.time_synced ? 'Heure non resynchronisée depuis plus de 24 h.'
                             : 'Heure non synchronisée (NTP) : régulation suspendue.']);
  }
  if (s.heap_min < s.heap_warn) {
    list.push([`Mémoire : minimum libre ${ko(s.heap_min)}, sous le seuil de ${ko(s.heap_warn)}.`]);
  }
  if (s.observer) {
    list.push(['Mode observateur : les décisions sont calculées, aucune commande n\'est envoyée.', 'info']);
  }
  const box = $('#alerts');
  box.replaceChildren();
  for (const [text, cls] of list) {
    const div = document.createElement('div');
    div.className = 'alert' + (cls ? ' ' + cls : '');
    div.textContent = text;
    box.append(div);
  }
}

function renderSystem(s) {
  renderAlerts(s);
  fillDl('sys-fw', [
    ['Version', s.version],
    ['ESP-IDF', s.idf],
    ['Fonctionnement', duration(s.uptime_s)],
    ['Dernier redémarrage', RESET_REASONS[s.reset_reason] || `code ${s.reset_reason}`],
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
    ['Secure Boot v2', s.secure_boot_v2 ? 'possible' : 'non (puce < v3.0)', 'dim'],
    ['Flash', ko(s.flash_size)],
  ]);
  fillDl('sys-cfg', [
    ['État', CFG_STATUS[s.cfg_status] || s.cfg_status],
    ['Tarif', s.tariff_configured ? 'saisi' : 'à saisir', s.tariff_configured ? '' : 'warn'],
    ['Mode', s.observer ? 'observateur' : 'pilotage'],
  ]);
}

function setConn(ok) {
  const c = $('#conn');
  c.className = 'conn ' + (ok ? 'ok' : 'ko');
  c.textContent = ok ? 'en ligne' : 'hors ligne';
}

async function pollSystem() {
  try {
    renderSystem(await getJson('/api/system'));
    setConn(true);
  } catch (e) {
    setConn(false);
  }
}

// ---------- Démarrage ----------

window.addEventListener('hashchange', showPage);
showPage();
pollSystem();
setInterval(() => { if (!document.hidden) pollSystem(); }, SYSTEM_POLL_MS);
