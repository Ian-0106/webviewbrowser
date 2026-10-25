'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const HISTORY_MAX_AGE_MS = 90 * 24 * 60 * 60 * 1000;
const DOWNLOAD_MAX_AGE_MS = 365 * 24 * 60 * 60 * 1000;
const HISTORY_MAX_ITEMS = 10_000;
const DOWNLOAD_MAX_ITEMS = 5_000;
const ACTIVE_DOWNLOAD_STATES = new Set(['waiting', 'progressing', 'paused']);
const DOWNLOAD_STATES = new Set([...ACTIVE_DOWNLOAD_STATES, 'completed', 'cancelled', 'failed', 'interrupted']);

function isText(value, max = 32768) {
  return typeof value === 'string' && value.length <= max && !value.includes('\0');
}

function validHistory(entry) {
  return entry && isText(entry.id, 128) && entry.id.length > 0 && isText(entry.title, 65536)
    && isText(entry.url) && isWebUrl(entry.url) && Number.isFinite(Date.parse(entry.visitedAt));
}

function validDownload(entry) {
  return entry && isText(entry.id, 128) && entry.id.length > 0 && isText(entry.filename, 4096)
    && isText(entry.url) && isText(entry.savePath) && DOWNLOAD_STATES.has(entry.state)
    && Number.isSafeInteger(entry.receivedBytes) && entry.receivedBytes >= 0
    && Number.isSafeInteger(entry.totalBytes) && entry.totalBytes >= -1
    && Number.isFinite(Date.parse(entry.startedAt))
    && (entry.finishedAt == null || Number.isFinite(Date.parse(entry.finishedAt)));
}

function defaultData() {
  return { version: 1, history: [], downloads: [] };
}

function isWebUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === 'http:' || url.protocol === 'https:';
  } catch {
    return false;
  }
}

function compareNewest(a, b) {
  return Date.parse(b.startedAt || b.visitedAt || 0) - Date.parse(a.startedAt || a.visitedAt || 0);
}

class ActivityStore {
  constructor(filePath, options = {}) {
    this.filePath = filePath;
    this.now = options.now || (() => new Date());
    this.data = defaultData();
    this.lastHistoryByTab = new Map();
    this.load();
  }

  load() {
    let malformed = false;
    try {
      const parsed = JSON.parse(fs.readFileSync(this.filePath, 'utf8'));
      if (!parsed || !Array.isArray(parsed.history) || !Array.isArray(parsed.downloads)) {
        throw new Error('Invalid activity storage schema');
      }
      this.data = {
        version: 1,
        history: parsed.history.filter(validHistory),
        downloads: parsed.downloads.filter(validDownload),
      };
      malformed = this.data.history.length !== parsed.history.length || this.data.downloads.length !== parsed.downloads.length;
      if (malformed) {
        fs.copyFileSync(this.filePath, `${this.filePath}.corrupt-${Date.now()}`);
      }
    } catch (error) {
      if (error.code !== 'ENOENT') {
        try {
          fs.renameSync(this.filePath, `${this.filePath}.corrupt-${Date.now()}`);
        } catch {
          // Recovery must never prevent the browser from opening.
        }
      }
      this.data = defaultData();
    }

    let changed = malformed;
    for (const item of this.data.downloads) {
      if (ACTIVE_DOWNLOAD_STATES.has(item.state)) {
        item.state = 'interrupted';
        item.interruptReason = item.interruptReason || 'Application exited before the download finished';
        item.finishedAt = item.finishedAt || this.now().toISOString();
        changed = true;
      }
    }
    changed = this.prune() || changed;
    if (changed) {
      this.persist();
    }
  }

  persist() {
    fs.mkdirSync(path.dirname(this.filePath), { recursive: true });
    const temporaryPath = `${this.filePath}.tmp`;
    fs.writeFileSync(temporaryPath, `${JSON.stringify(this.data, null, 2)}\n`, 'utf8');
    try {
      fs.renameSync(temporaryPath, this.filePath);
    } catch {
      fs.copyFileSync(temporaryPath, this.filePath);
      fs.unlinkSync(temporaryPath);
    }
  }

  prune() {
    const now = this.now().getTime();
    const oldHistoryLength = this.data.history.length;
    const oldDownloadsLength = this.data.downloads.length;

    this.data.history = this.data.history
      .filter((entry) => now - Date.parse(entry.visitedAt) <= HISTORY_MAX_AGE_MS)
      .sort(compareNewest)
      .slice(0, HISTORY_MAX_ITEMS);

    const active = this.data.downloads.filter((entry) => ACTIVE_DOWNLOAD_STATES.has(entry.state));
    const terminal = this.data.downloads
      .filter((entry) => !ACTIVE_DOWNLOAD_STATES.has(entry.state))
      .filter((entry) => now - Date.parse(entry.finishedAt || entry.startedAt) <= DOWNLOAD_MAX_AGE_MS)
      .sort(compareNewest)
      .slice(0, DOWNLOAD_MAX_ITEMS);
    this.data.downloads = [...active, ...terminal].sort(compareNewest);

    return oldHistoryLength !== this.data.history.length || oldDownloadsLength !== this.data.downloads.length;
  }

  recordHistory({ tabId, url, title }) {
    if (!isWebUrl(url)) {
      return null;
    }
    const now = this.now().toISOString();
    const previousId = this.lastHistoryByTab.get(tabId);
    const previous = this.data.history.find((entry) => entry.id === previousId);
    if (previous && previous.url === url) {
      previous.title = title || previous.title || url;
      previous.visitedAt = now;
      this.data.history.sort(compareNewest);
      this.persist();
      return previous.id;
    }

    const entry = {
      id: crypto.randomUUID(),
      title: title || url,
      url,
      visitedAt: now,
    };
    this.data.history.unshift(entry);
    this.lastHistoryByTab.set(tabId, entry.id);
    this.prune();
    this.persist();
    return entry.id;
  }

  updateHistoryTitle(id, title) {
    const entry = this.data.history.find((candidate) => candidate.id === id);
    if (!entry || !title || entry.title === title) {
      return false;
    }
    entry.title = title;
    this.persist();
    return true;
  }

  listHistory() {
    return [...this.data.history].sort(compareNewest);
  }

  removeHistory(ids) {
    const selected = new Set(ids);
    const before = this.data.history.length;
    this.data.history = this.data.history.filter((entry) => !selected.has(entry.id));
    for (const [tabId, historyId] of this.lastHistoryByTab) {
      if (selected.has(historyId)) {
        this.lastHistoryByTab.delete(tabId);
      }
    }
    if (this.data.history.length !== before) {
      this.persist();
    }
    return before - this.data.history.length;
  }

  clearHistory() {
    const count = this.data.history.length;
    this.data.history = [];
    this.lastHistoryByTab.clear();
    if (count > 0) {
      this.persist();
    }
    return count;
  }

  createDownload(values) {
    const entry = {
      id: values.id || crypto.randomUUID(),
      filename: values.filename || 'download',
      url: values.url || '',
      savePath: values.savePath || '',
      state: values.state || 'waiting',
      receivedBytes: values.receivedBytes || 0,
      totalBytes: values.totalBytes || 0,
      startedAt: values.startedAt || this.now().toISOString(),
      finishedAt: values.finishedAt || null,
      interruptReason: values.interruptReason || '',
      retryOf: values.retryOf || null,
    };
    this.data.downloads.unshift(entry);
    this.prune();
    this.persist();
    return { ...entry };
  }

  updateDownload(id, changes) {
    const entry = this.data.downloads.find((candidate) => candidate.id === id);
    if (!entry) {
      return null;
    }
    Object.assign(entry, changes);
    this.data.downloads.sort(compareNewest);
    this.persist();
    return { ...entry };
  }

  getDownload(id) {
    const entry = this.data.downloads.find((candidate) => candidate.id === id);
    return entry ? { ...entry } : null;
  }

  listDownloads() {
    return [...this.data.downloads].sort(compareNewest);
  }

  removeDownloads(ids, { terminalOnly = false } = {}) {
    const selected = new Set(ids);
    const before = this.data.downloads.length;
    this.data.downloads = this.data.downloads.filter((entry) => {
      if (!selected.has(entry.id)) {
        return true;
      }
      return terminalOnly && ACTIVE_DOWNLOAD_STATES.has(entry.state);
    });
    if (this.data.downloads.length !== before) {
      this.persist();
    }
    return before - this.data.downloads.length;
  }

  clearTerminalDownloads() {
    const before = this.data.downloads.length;
    this.data.downloads = this.data.downloads.filter((entry) => ACTIVE_DOWNLOAD_STATES.has(entry.state));
    if (this.data.downloads.length !== before) {
      this.persist();
    }
    return before - this.data.downloads.length;
  }

  activeDownloadCount() {
    return this.data.downloads.filter((entry) => ACTIVE_DOWNLOAD_STATES.has(entry.state)).length;
  }
}

module.exports = {
  ActivityStore,
  ACTIVE_DOWNLOAD_STATES,
  isWebUrl,
};
