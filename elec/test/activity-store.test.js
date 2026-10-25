'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ActivityStore } = require('../src/activity-store');

function withStore(callback, now = new Date('2026-10-25T10:00:00.000Z')) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'webview-browser-store-'));
  try {
    const filePath = path.join(directory, 'activity.json');
    return callback(new ActivityStore(filePath, { now: () => now }), filePath);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

test('history only accepts web URLs and merges consecutive visits in one tab', () => {
  withStore((store) => {
    assert.equal(store.recordHistory({ tabId: 1, url: 'file:///tmp/a.html', title: 'Local' }), null);
    const first = store.recordHistory({ tabId: 1, url: 'https://example.com', title: 'Example' });
    const again = store.recordHistory({ tabId: 1, url: 'https://example.com', title: 'Updated' });
    const otherTab = store.recordHistory({ tabId: 2, url: 'https://example.com', title: 'Example' });

    assert.equal(first, again);
    assert.notEqual(first, otherTab);
    assert.equal(store.listHistory().length, 2);
    assert.equal(store.listHistory().find((entry) => entry.id === first).title, 'Updated');
  });
});

test('corrupt storage is recovered without losing the corrupt evidence file', () => {
  withStore((_store, filePath) => {
    fs.writeFileSync(filePath, '{not json', 'utf8');
    const recovered = new ActivityStore(filePath);
    assert.deepEqual(recovered.listHistory(), []);
    assert.equal(fs.readdirSync(path.dirname(filePath)).some((name) => name.includes('.corrupt-')), true);
  });
});

test('unfinished downloads become interrupted after a restart', () => {
  withStore((store, filePath) => {
    const item = store.createDownload({ filename: 'large.zip', state: 'progressing' });
    const restarted = new ActivityStore(filePath, { now: () => new Date('2026-10-25T10:05:00.000Z') });
    assert.equal(restarted.getDownload(item.id).state, 'interrupted');
  });
});

test('clearing terminal downloads preserves active work', () => {
  withStore((store) => {
    store.createDownload({ filename: 'done.zip', state: 'completed', finishedAt: '2026-10-25T09:00:00.000Z' });
    store.createDownload({ filename: 'active.zip', state: 'progressing' });
    assert.equal(store.clearTerminalDownloads(), 1);
    assert.equal(store.listDownloads().length, 1);
    assert.equal(store.listDownloads()[0].filename, 'active.zip');
  });
});

test('valid JSON with malformed records is recovered without crashing the main process', () => {
  withStore((store, filePath) => {
    const valid = store.createDownload({ filename: 'kept.txt', state: 'completed' });
    const data = JSON.parse(fs.readFileSync(filePath, 'utf8'));
    data.history.push(null, { id: 'bad-history', url: 'javascript:alert(1)' });
    data.downloads.push(null, { ...valid, id: 'bad-path', savePath: 42 });
    fs.writeFileSync(filePath, JSON.stringify(data));
    const recovered = new ActivityStore(filePath);
    assert.equal(recovered.listHistory().length, 0);
    assert.deepEqual(recovered.listDownloads().map((entry) => entry.id), [valid.id]);
    assert.ok(fs.readdirSync(path.dirname(filePath)).some((name) => name.includes('.corrupt-')));
  });
});
