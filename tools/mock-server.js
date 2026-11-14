// Fake bridge for working on the web app without any hardware:
//   node tools/mock-server.js   ->  http://localhost:8080
// Serves web/ and imitates the firmware's JSON API with one pretend lamp.
const http = require('http');
const fs = require('fs');
const path = require('path');

const WEB = path.join(__dirname, '..', 'web');
const MIME = { '.html': 'text/html; charset=utf-8', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
const started = Date.now();

const db = {
  settings: { name: 'Home', city: 'Hyderabad', lat: 17.385, lon: 78.4867, tzOffsetMin: 330 },
  lamps: process.env.NO_LAMPS ? [] : [{ mac: 'a8bb50aabbcc', ip: '192.168.1.50', name: 'Lamp' }],
  routines: [],
  presets: [
    { id: 'focus', name: 'Focus', dimming: 100, temp: 5000 },
    { id: 'relax', name: 'Relax', dimming: 50, temp: 2700 },
    { id: 'movie', name: 'Movie', dimming: 20, temp: 2700 },
    { id: 'night', name: 'Night', dimming: 10, temp: 2700 },
  ],
};
const live = { a8bb50aabbcc: { online: true, state: true, dimming: 60, temp: 2700, sceneId: 0 } };
let lastRun = null;

function apply(target, a) {
  let n = 0;
  for (const lamp of db.lamps) {
    if (target !== 'all' && target !== lamp.mac) continue;
    const s = (live[lamp.mac] ||= { online: true, state: false, dimming: 50, temp: 2700, sceneId: 0 });
    delete s.fade;
    if (a.power === false) {
      if (a.fadeMin > 0 && s.state) s.fade = { to: 10, off: true, until: Date.now() + a.fadeMin * 60000 };
      else s.state = false;
    } else {
      s.state = true;
      if (a.sceneId) { s.sceneId = a.sceneId; delete s.r; delete s.g; delete s.b; s.temp = 0; }
      else if (a.r !== undefined) { Object.assign(s, { r: a.r, g: a.g, b: a.b, sceneId: 0, temp: 0 }); }
      else if (a.temp) { s.temp = a.temp; s.sceneId = 0; delete s.r; delete s.g; delete s.b; }
      if (a.dimming) s.dimming = Math.min(100, Math.max(10, a.dimming));
    }
    n++;
  }
  return n;
}

function state() {
  const local = new Date(Date.now() + db.settings.tzOffsetMin * 60000);
  const pad = (v) => String(v).padStart(2, '0');
  return {
    device: { version: 'mock', host: 'its-automated.local', ip: '127.0.0.1', ssid: 'MockNet', rssi: -52, uptime: Math.floor((Date.now() - started) / 1000), heap: 200000 },
    time: { synced: true, local: `${pad(local.getUTCHours())}:${pad(local.getUTCMinutes())}`, dow: local.getUTCDay(), date: local.toISOString().slice(0, 10), sunrise: '06:20', sunset: '17:41' },
    lamps: db.lamps.map((l) => {
      const s = { ...(live[l.mac] || { online: false, state: false, dimming: 0, temp: 0, sceneId: 0 }) };
      if (s.fade) { s.fade = { to: s.fade.to, off: s.fade.off, remainingSec: Math.max(0, Math.round((s.fade.until - Date.now()) / 1000)) }; }
      return { ...l, ...s };
    }),
    settings: db.settings, routines: db.routines, presets: db.presets,
    ...(lastRun ? { lastRun } : {}),
  };
}

const routes = {
  '/api/discover': () => {
    if (!db.lamps.length) { db.lamps.push({ mac: 'a8bb50aabbcc', ip: '192.168.1.50', name: 'Lamp' }); return { ok: true, found: 1 }; }
    return { ok: true, found: db.lamps.length };
  },
  '/api/lamps/add': (b) => { db.lamps.push({ mac: 'mock' + Date.now(), ip: b.ip, name: 'Lamp ' + (db.lamps.length + 1) }); return { ok: true }; },
  '/api/lamps/update': (b) => { const l = db.lamps.find((x) => x.mac === b.mac); if (!l) return { ok: false, error: 'unknown lamp' }; l.name = b.name; return { ok: true }; },
  '/api/lamps/remove': (b) => { db.lamps = db.lamps.filter((x) => x.mac !== b.mac); return { ok: true }; },
  '/api/control': (b) => { const n = apply(b.target || 'all', b.action || {}); return n ? { ok: true, lamps: n } : { ok: false, error: 'lamp did not respond' }; },
  '/api/sleep': (b) => {
    if (!b.minutes) { for (const l of db.lamps) if (live[l.mac]) delete live[l.mac].fade; return { ok: true }; }
    apply(b.target || 'all', { power: false, fadeMin: b.minutes }); return { ok: true };
  },
  '/api/routines': (b) => { db.routines = b.routines; return { ok: true }; },
  '/api/routines/run': (b) => {
    const r = db.routines.find((x) => x.id === b.id); if (!r) return { ok: false, error: 'unknown routine' };
    apply(r.target || 'all', r.action); lastRun = { name: r.name, at: Math.floor(Date.now() / 1000) }; return { ok: true };
  },
  '/api/presets': (b) => { db.presets = b.presets; return { ok: true }; },
  '/api/settings': (b) => { Object.assign(db.settings, b); return { ok: true }; },
  '/api/wifi/reset': () => ({ ok: true }),
};

http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  const json = (code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(obj)); };
  if (url === '/api/state') return json(200, state());
  if (routes[url] && req.method === 'POST') {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let parsed;
      try { parsed = JSON.parse(body || '{}'); } catch { return json(400, { ok: false, error: 'bad json' }); }
      const out = routes[url](parsed);
      json(out.ok === false ? 400 : 200, out);
    });
    return;
  }
  const file = path.join(WEB, path.normalize(url === '/' ? '/index.html' : url));
  if (!file.startsWith(WEB) || !fs.existsSync(file)) { res.writeHead(404); return res.end('not found'); }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
}).listen(process.env.PORT || 8080, () => console.log('mock bridge on http://localhost:' + (process.env.PORT || 8080)));
