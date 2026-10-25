'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { app, BrowserWindow, dialog, session, shell } = require('electron');
const { ActivityStore } = require('../src/activity-store');

process.on('uncaughtException', (error) => { console.error(error); app.exit(1); });

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'webview-security-smoke-'));
app.setPath('userData', root);
fs.mkdirSync(path.join(root, 'session'));
app.setPath('sessionData', path.join(root, 'session'));
const file = path.join(root, 'download.txt');
fs.writeFileSync(file, 'harmless verification fixture');
const store = new ActivityStore(path.join(root, 'activity.json'));
const record = store.createDownload({ filename: 'download.txt', savePath: file,
  url: 'https://example.com/download', state: 'completed', finishedAt: new Date().toISOString() });
const directoryRecord = store.createDownload({ filename: 'directory', savePath: root,
  state: 'completed', finishedAt: new Date().toISOString() });
let deletedPaths = [];
let confirmationCount = 0;
let confirmationResponse = 0;
shell.trashItem = async (target) => { deletedPaths.push(target); };
dialog.showMessageBox = async () => {
  confirmationCount += 1;
  return { response: confirmationResponse };
};

async function waitFor(getValue, label) {
  const limit = Date.now() + 15000;
  while (Date.now() < limit) {
    const value = getValue();
    if (value) return value;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Timed out waiting for ${label}`);
}

const server = http.createServer((request, response) => {
  response.setHeader('Content-Type', 'text/html; charset=utf-8');
  if (request.url.startsWith('/frame')) {
    response.end('<!doctype html><title>Child</title><script>setTimeout(()=>history.replaceState({},"","/frame#changed"),200)</script>');
  } else {
    response.end('<!doctype html><title>Security test</title><iframe src="/frame"></iframe>');
  }
});

async function run() {
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const home = `http://127.0.0.1:${server.address().port}/home`;
  // Redirect only the application's initial page, keeping the verification offline.
  app.whenReady().then(() => session.defaultSession.webRequest.onBeforeRequest({
    urls: ['https://bilibili.com/*', 'https://www.bilibili.com/*'],
  }, (_details, callback) => callback({ redirectURL: home })));
  require('../main');
  await app.whenReady();
  const chromeUrl = pathToFileURL(path.join(__dirname, '../renderer/index.html')).href;
  const chrome = await waitFor(() => BrowserWindow.getAllWindows().find((window) =>
    window.webContents.getURL() === chromeUrl && !window.webContents.isLoadingMainFrame()), 'browser chrome');
  const tabs = await waitFor(() => chrome.contentView.children.find((view) =>
    view.webContents?.getURL() === home && !view.webContents.isLoadingMainFrame()), 'offline web tab');
  await new Promise((resolve) => setTimeout(resolve, 500));
  const initial = await chrome.webContents.executeJavaScript('window.browserApi.getState()');
  assert.equal(initial.tabs[0].url, home, 'a child-frame history change must not spoof the address bar');
  assert.equal(await chrome.webContents.executeJavaScript('typeof window.activityApi'), 'undefined');
  assert.equal(await tabs.webContents.executeJavaScript('typeof window.browserApi'), 'undefined');
  assert.equal(await tabs.webContents.executeJavaScript('typeof window.activityApi'), 'undefined');

  await chrome.webContents.executeJavaScript('window.browserApi.openActivity("history")');
  const activity = await waitFor(() => BrowserWindow.getAllWindows().find((window) =>
    window.webContents.getURL().endsWith('/renderer/activity.html') && !window.webContents.isLoadingMainFrame()), 'activity window');
  assert.equal(await activity.webContents.executeJavaScript('typeof window.browserApi'), 'undefined');
  const state = await activity.webContents.executeJavaScript('window.activityApi.getState()');
  assert.ok(state.downloads.some((entry) => entry.id === record.id), 'legitimate activity IPC must work');

  const hostile = new BrowserWindow({ show: false, webPreferences: {
    contextIsolation: true, sandbox: true, nodeIntegration: false,
    preload: path.join(__dirname, 'untrusted-preload.cjs'),
  } });
  await hostile.loadFile(path.join(__dirname, '../renderer/activity.html'));
  for (const channel of ['activity:get-state', 'history:clear', 'downloads:clear', 'downloads:trash']) {
    const result = await hostile.webContents.executeJavaScript(
      `window.securityProbe.invoke(${JSON.stringify(channel)}, ${JSON.stringify(record.id)}).then(()=>false,()=>true)`);
    assert.equal(result, true, `foreign window must not call ${channel}, even at the same file URL`);
  }

  const cspBlocked = await activity.webContents.executeJavaScript(`new Promise((resolve) => {
    window.__securityInlineRan = false;
    const script = document.createElement('script');
    script.textContent = 'window.__securityInlineRan = true';
    document.body.append(script);
    setTimeout(() => resolve(!window.__securityInlineRan), 50);
  })`);
  assert.equal(cspBlocked, true, 'the internal UI CSP must block inline scripts');
  const countBefore = (await chrome.webContents.executeJavaScript('window.browserApi.getState()')).tabs.length;
  await tabs.webContents.executeJavaScript('window.open("file:///C:/nonexistent-security-fixture.html")');
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal((await chrome.webContents.executeJavaScript('window.browserApi.getState()')).tabs.length,
    countBefore, 'website popups must not open local files');

  let result = await activity.webContents.executeJavaScript(`window.activityApi.trashDownload(${JSON.stringify(directoryRecord.id)})`);
  assert.equal(result.ok, false);
  assert.equal(confirmationCount, 0, 'directory deletion must fail before confirmation');
  result = await activity.webContents.executeJavaScript(`window.activityApi.trashDownload(${JSON.stringify(record.id)})`);
  assert.equal(result.ok, true);
  assert.equal(confirmationCount, 1);
  assert.deepEqual(deletedPaths, [], 'cancelled native confirmation must not delete files');
  confirmationResponse = 1;
  await activity.webContents.executeJavaScript(`window.activityApi.trashDownload(${JSON.stringify(record.id)})`);
  assert.deepEqual(deletedPaths, [file]);
  assert.equal(fs.existsSync(file), true, 'the test intercepts shell deletion');
  console.log('Electron security integration: PASS (sender, frame/address, popup, CSP, least privilege, native deletion confirmation)');
}

run().then(() => {
  server.close();
  app.exit(0);
}).catch((error) => {
  console.error(error);
  server.close();
  app.exit(1);
});
