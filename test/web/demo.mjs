// The website's live demo (site/demo/, scripts/web.sh) in Chromium: serves
// the built demo directory, opens its page, and waits for the session to
// say it's ready (web/session.rc: Finder registered with uxd and showing
// the home volume's window). Then it double-clicks README.txt in that
// window and checks that acme opened it, through the plumber, and that
// opening Sample in the Applications volume (which session.rc's -d does
// through Finder's files) launches the sample app. The Pages workflow
// runs it before deploying.
//
//     node test/web/demo.mjs DIR [SCREENSHOT.png]
//
// Needs Playwright with Chromium.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const dir = path.resolve(process.argv[2] ?? '.local/web/demo');
const shot = process.argv[3];
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer(async (req, res) => {
  let p = path.join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (p.endsWith('/')) p += 'index.html';
  try {
    const body = await readFile(p);
    res.writeHead(200, { 'content-type': types[path.extname(p)] ?? 'application/octet-stream' });
    res.end(body);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));

const browser = await chromium.launch();
let failed = false;
try {
  const page = await browser.newPage({ viewport: { width: 1024, height: 800 } });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  // ?debug: the session's progress and its processes go to the log too.
  await page.goto(`http://127.0.0.1:${server.address().port}/?debug`);
  const log = () => page.locator('#log').textContent();
  const step = async (name, f) => {
    const t0 = Date.now();
    try {
      await f();
    } catch (e) {
      throw new Error(`${name}: ${e.message}\nstatus: ${await page.locator('#status').textContent()}\nlog:\n${await log()}\nerrors: ${errors.join('\n')}`);
    }
    console.log(`ok: ${name} (${Date.now() - t0} ms)`);
  };

  await step('the session starts', () => page.waitForFunction(() => document.getElementById('status').hidden, null, { timeout: 180_000 }));
  // session.rc's -d listing, a few seconds after ready, shows every
  // service still running.
  await step('the services keep running', async () => {
    await page.waitForFunction(() => /uxmeta\.glenda/.test(document.getElementById('log').textContent), null, { timeout: 60_000 });
    const t = await log();
    for (const want of [/\/srv\/uxd\.glenda\b/, /\/srv\/plumb\./, /\/mnt\/ux\/apps\/finder-/, / 8\.finder\n/])
      if (!want.test(t)) throw new Error(`no ${want} in the log`);
  });
  await step('a double-click in Finder plumbs README.txt to acme', async () => {
    const box = await page.locator('#screen').boundingBox();
    // The home window, where Finder places a volume's first window: one
    // click makes it current, as rio's first click does, then a double
    // click on README.txt, its third icon.
    await page.mouse.click(box.x + 300, box.y + 300);
    await page.waitForTimeout(1000);
    await page.mouse.click(box.x + 284, box.y + 98);
    await page.waitForTimeout(80);
    await page.mouse.click(box.x + 284, box.y + 98);
    await page.waitForFunction(() => /\bacme\n/.test(document.getElementById('log').textContent), null, { timeout: 60_000 });
  });
  await step('opening Sample in Applications launches the sample app', () =>
    page.waitForFunction(() => /debug: sample\n/.test(document.getElementById('log').textContent), null, { timeout: 60_000 }));
  if (errors.length) throw new Error(`page errors:\n${errors.join('\n')}`);
  if (shot) await page.screenshot({ path: shot });
} catch (e) {
  console.error(`FAIL: ${e.message}`);
  failed = true;
} finally {
  await browser.close();
  server.close();
}
process.exit(failed ? 1 : 0);
