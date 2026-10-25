'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const test = require('node:test');
const { isTrustedSender, isLocalAbsolutePath, isRegularDownloadFile } = require('../src/security');

test('IPC requires the expected live window, its main frame, and exact packaged page', () => {
  const page = path.resolve('renderer/activity.html');
  const frame = { url: pathToFileURL(page).href };
  const contents = { mainFrame: frame, isDestroyed: () => false };
  const window = { webContents: contents, isDestroyed: () => false };
  assert.equal(isTrustedSender({ sender: contents, senderFrame: frame }, window, page), true);
  assert.equal(isTrustedSender({ sender: { ...contents }, senderFrame: frame }, window, page), false);
  assert.equal(isTrustedSender({ sender: contents, senderFrame: { ...frame } }, window, page), false);
  frame.url = 'https://example.com/activity.html';
  assert.equal(isTrustedSender({ sender: contents, senderFrame: frame }, window, page), false);
  frame.url = `${pathToFileURL(page).href}?untrusted=1`;
  assert.equal(isTrustedSender({ sender: contents, senderFrame: frame }, window, page), false);
  assert.equal(isTrustedSender({ sender: contents, senderFrame: null }, window, page), false);
  assert.equal(isTrustedSender({}, null, page), false);
  window.isDestroyed = () => true;
  assert.equal(isTrustedSender({ sender: contents, senderFrame: frame }, window, page), false);
});

test('file actions reject directories, links, relative paths, network paths and NULs', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'webview-security-'));
  try {
    const file = path.join(directory, 'download.txt');
    fs.writeFileSync(file, 'harmless verification fixture');
    assert.equal(isRegularDownloadFile(file), true);
    assert.equal(isRegularDownloadFile(directory), false);
    assert.equal(isRegularDownloadFile(`${file}\0another-file`), false);
    assert.equal(isLocalAbsolutePath('relative.txt'), false);
    assert.equal(isLocalAbsolutePath('\\\\server\\share\\file.txt'), false);
    assert.equal(isLocalAbsolutePath('\\\\?\\C:\\file.txt'), false);
    assert.equal(isLocalAbsolutePath('//server/share/file.txt'), false);
    const link = path.join(directory, 'linked-directory');
    fs.symlinkSync(directory, link, process.platform === 'win32' ? 'junction' : 'dir');
    assert.equal(isRegularDownloadFile(link), false);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
