// site/progrium.html in Chromium: serves the site with the built demo at
// demo/, and answers the page's GitHub requests itself (the tree from
// api.github.com and the files from raw.githubusercontent.com), so the
// test needs no network and no API quota. Waits for the session to say
// it's ready, then checks that the repository's files, without its
// dotfiles, are in the volume boot.rc's -v made, and that its folder
// window is open.
//
//     node test/web/progrium.mjs DEMO_DIR [SCREENSHOT.png]
//
// REPO_DIR=/path/to/a/git/checkout serves that checkout's HEAD in place
// of the small built-in repository. Needs Playwright with Chromium.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const top = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const demo = path.resolve(process.argv[2] ?? '.local/web/demo');
const shot = process.argv[3];

// The repository: path to [mode, bytes].
const files = new Map();
if (process.env.REPO_DIR) {
  const dir = process.env.REPO_DIR;
  const ls = execFileSync('git', ['-C', dir, 'ls-tree', '-r', '-z', 'HEAD'], { encoding: 'utf8' });
  for (const ent of ls.split('\0').filter(Boolean)) {
    const [meta, p] = ent.split('\t');
    const [mode, type] = meta.split(' ');
    if (type === 'blob') files.set(p, [mode, await readFile(path.join(dir, p))]);
  }
} else {
  const t = (s) => Buffer.from(s);
  files.set('README.md', ['100644', t('# test site\n')]);
  files.set('index.html', ['100644', t('<h1>hi</h1>\n')]);
  files.set('articles/2026/first-post.md', ['100644', t('First post.\n')]);
  files.set('_assets/style.css', ['100644', t('body { color: red }\n')]);
  files.set('.gitignore', ['100644', t('node_modules\n')]);
  files.set('.github/workflows/deploy.yml', ['100644', t('on: push\n')]);
}
const visible = [...files.keys()].filter((p) => !p.split('/').some((c) => c.startsWith('.')));
const tree = { sha: 'test', truncated: false, tree: [] };
const dirs = new Set();
for (const [p, [mode, b]] of files) {
  tree.tree.push({ path: p, mode, type: 'blob', size: b.length });
  for (let d = path.posix.dirname(p); d !== '.'; d = path.posix.dirname(d)) dirs.add(d);
}
for (const d of dirs) tree.tree.push({ path: d, mode: '040000', type: 'tree' });

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = createServer(async (req, res) => {
  const u = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  let p = u.startsWith('/demo/') ? path.join(demo, u.slice(6)) : path.join(top, 'site', u);
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
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const errors = [];
  let requests = 0;
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  const cors = { 'access-control-allow-origin': '*' };
  await page.route('https://api.github.com/**', (route) => {
    requests++;
    const u = new URL(route.request().url());
    if (u.pathname !== '/repos/progrium/progrium.xyz/git/trees/main') return route.fulfill({ status: 404, headers: cors, body: '{}' });
    route.fulfill({ status: 200, headers: { ...cors, 'content-type': 'application/json' }, body: JSON.stringify(tree) });
  });
  await page.route('https://raw.githubusercontent.com/**', (route) => {
    const u = decodeURIComponent(new URL(route.request().url()).pathname);
    const p = u.replace(/^\/progrium\/progrium\.xyz\/main\//, '');
    const f = files.get(p);
    if (!f) return route.fulfill({ status: 404, headers: cors, body: '' });
    route.fulfill({ status: 200, headers: cors, body: f[1] });
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/progrium.html?debug`);
  const term = () => page.locator('#term').textContent();
  const step = async (name, f) => {
    const t0 = Date.now();
    try {
      await f();
    } catch (e) {
      throw new Error(`${name}: ${e.message}\nterminal:\n${await term()}\nerrors: ${errors.join('\n')}`);
    }
    console.log(`ok: ${name} (${Date.now() - t0} ms)`);
  };
  await step('the page fetches the repository', () =>
    page.waitForFunction((n) => document.getElementById('term').textContent.includes(`${n} files OK`), visible.length, { timeout: 60_000 }));
  await step('the session starts', () => page.waitForFunction(() => document.getElementById('term').classList.contains('done'), null, { timeout: 180_000 }));
  await step('the volume has the files, no dotfiles, and its window is open', async () => {
    await page.waitForFunction(() => /debug: volume listed\n/.test(document.getElementById('term').textContent), null, { timeout: 60_000 });
    const t = await term();
    const listing = t.slice(t.indexOf('debug: volume progrium.xyz'), t.indexOf('debug: volume listed'));
    if (!/^progrium\.xyz$/m.test(listing)) throw new Error('no window titled progrium.xyz');
    const top = [...new Set(visible.map((p) => p.split('/')[0]))];
    for (const name of top)
      if (!new RegExp(`^${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`, 'm').test(listing)) throw new Error(`no ${name} in the volume`);
    if (/^\.git/m.test(listing)) throw new Error('a dotfile is in the volume');
    if (requests !== 1) throw new Error(`${requests} API requests, not 1`);
  });
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
