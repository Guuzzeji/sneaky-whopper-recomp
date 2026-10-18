// Drive the web build in Chrome for testing, over the DevTools protocol:
//   node tools/web_drive.mjs <url> <seconds> <outdir> [key@sec ...]
// Launches Chrome with a throwaway profile, prints the page's console (and
// its workers'), saves a screenshot every 5 s to <outdir>, presses the given
// keys at the given times (e.g. Enter@10), then closes Chrome.
import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, secs = '60', out = '.', ...keys] = process.argv.slice(2);
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const port = 9333;
mkdirSync(out, { recursive: true });
const profile = mkdtempSync(join(tmpdir(), 'skweb-'));
const chrome = spawn(CHROME, [`--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
  '--no-first-run', '--no-default-browser-check', '--window-size=1400,1200', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
  { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ver;
for (let i = 0; i < 50 && !ver; i++) {
  try { ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(200); }
}
const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = targets.find((t) => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const pending = new Map();
const sessions = [];
const pausedWaiters = new Map();
const send = (method, params = {}, sessionId) => new Promise((res) => {
  const msg = { id: ++id, method, params };
  if (sessionId) msg.sessionId = sessionId;
  pending.set(msg.id, res);
  ws.send(JSON.stringify(msg));
});
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
ws.addEventListener('message', async (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result ?? m.error); pending.delete(m.id); return; }
  if (m.method === 'Runtime.consoleAPICalled')
    console.log(stamp(), m.params.args.map((a) => a.value ?? a.description ?? '').join(' '));
  else if (m.method === 'Runtime.exceptionThrown')
    console.log(stamp(), 'EXCEPTION', m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text);
  else if (m.method === 'Debugger.paused' && pausedWaiters.has(m.sessionId)) {
    pausedWaiters.get(m.sessionId)(m.params.callFrames);
    pausedWaiters.delete(m.sessionId);
  } else if (m.method === 'Target.attachedToTarget') {
    const sid = m.params.sessionId;
    sessions.push({ sid, url: m.params.targetInfo.url, type: m.params.targetInfo.type });
    await send('Runtime.enable', {}, sid);
    await send('Runtime.runIfWaitingForDebugger', {}, sid);
  }
});
await send('Runtime.enable');
await send('Page.enable');
await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true });
await send('Page.navigate', { url });

const presses = keys.map((k) => { const [key, at] = k.split('@'); return { key, at: +at }; });
const vk = { KeyZ: 90, KeyX: 88, Enter: 13, ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Space: 32, KeyW: 87 };
let shot = 0;
for (let s = 1; s <= +secs; s++) {
  await sleep(1000);
  for (const p of presses.filter((p) => p.at === s && p.key === 'PAUSE')) {
    // Every worker's stack: which thread waits on what.
    for (const ses of sessions) {
      await send('Debugger.enable', {}, ses.sid);
      const got = new Promise((r) => { pausedWaiters.set(ses.sid, r); setTimeout(() => r(null), 300); });
      await send('Debugger.pause', {}, ses.sid);
      const frames = await got;
      if (frames) console.log(stamp(), `[stack ${ses.type} ${ses.sid.slice(0, 6)}]`,
        frames ? frames.slice(0, 14).map((f) => f.functionName || '?').join(' < ') : '(no pause)');
      await send('Debugger.resume', {}, ses.sid);
    }
  }
  for (const p of presses.filter((p) => p.at === s && p.key !== 'PAUSE')) {
    const code = vk[p.key] ?? 0;
    console.log(stamp(), `[drive] key ${p.key}`);
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: p.key, code: p.key, windowsVirtualKeyCode: code });
    await sleep(150);
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: p.key, code: p.key, windowsVirtualKeyCode: code });
  }
  if (s % 5 === 0) {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    if (r?.data) writeFileSync(join(out, `shot_${String(shot++).padStart(3, '0')}.png`), Buffer.from(r.data, 'base64'));
  }
}
chrome.kill();
process.exit(0);
