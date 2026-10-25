'use strict';

const ACTIVE_STATES = new Set(['waiting', 'progressing', 'paused']);
const navButtons = [...document.querySelectorAll('.nav-button')];
const sections = [...document.querySelectorAll('.section')];
const historyList = document.querySelector('#history-list');
const downloadsList = document.querySelector('#downloads-list');
const historySearch = document.querySelector('#history-search');
const downloadsSearch = document.querySelector('#downloads-search');
const downloadFilter = document.querySelector('#download-filter');
const activeCount = document.querySelector('#active-count');
const deleteHistorySelection = document.querySelector('#delete-history-selection');
const deleteDownloadSelection = document.querySelector('#delete-download-selection');
const clearHistory = document.querySelector('#clear-history');
const clearDownloads = document.querySelector('#clear-downloads');
const toast = document.querySelector('#toast');

let state = { history: [], downloads: [], activeDownloadCount: 0 };
let currentSection = 'history';
let highlightId = null;
const selectedHistory = new Set();
const selectedDownloads = new Set();

function setSection(section) {
  currentSection = section === 'downloads' ? 'downloads' : 'history';
  for (const button of navButtons) {
    button.classList.toggle('active', button.dataset.section === currentSection);
  }
  for (const element of sections) {
    element.classList.toggle('active', element.id === `${currentSection}-section`);
  }
  queueMicrotask(scrollToHighlight);
}

function showToast(message) {
  if (!message) return;
  toast.textContent = message;
  toast.hidden = false;
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => { toast.hidden = true; }, 3500);
}

function formatDateTime(value) {
  return new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(value));
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) return 'Unknown size';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const unit = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);
  const amount = value / (1024 ** unit);
  return `${amount >= 10 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[unit]}`;
}

function dayKey(value) {
  const date = new Date(value);
  return `${date.getFullYear()}-${date.getMonth()}-${date.getDate()}`;
}

function dayLabel(value) {
  const date = new Date(value);
  const today = new Date();
  const yesterday = new Date(today);
  yesterday.setDate(today.getDate() - 1);
  if (dayKey(date) === dayKey(today)) return 'Today';
  if (dayKey(date) === dayKey(yesterday)) return 'Yesterday';
  return new Intl.DateTimeFormat(undefined, { dateStyle: 'long' }).format(date);
}

function checkboxFor(id, selection, onChange, disabled = false) {
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.checked = selection.has(id);
  input.disabled = disabled;
  input.setAttribute('aria-label', 'Select record');
  input.addEventListener('change', () => {
    if (input.checked) selection.add(id); else selection.delete(id);
    onChange();
  });
  return input;
}

function actionButton(label, action, danger = false) {
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = label;
  if (danger) button.classList.add('danger');
  button.addEventListener('click', action);
  return button;
}

function renderHistory() {
  historyList.replaceChildren();
  const query = historySearch.value.trim().toLowerCase();
  const entries = state.history.filter((entry) => !query || `${entry.title} ${entry.url}`.toLowerCase().includes(query));
  if (entries.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = query ? 'No matching history.' : 'No browsing history yet.';
    historyList.append(empty);
    return;
  }
  let group = null;
  for (const entry of entries) {
    const key = dayKey(entry.visitedAt);
    if (key !== group) {
      group = key;
      const heading = document.createElement('h3');
      heading.className = 'group-title';
      heading.textContent = dayLabel(entry.visitedAt);
      historyList.append(heading);
    }
    const row = document.createElement('article');
    row.className = 'row';
    row.dataset.id = entry.id;
    row.append(checkboxFor(entry.id, selectedHistory, updateSelectionButtons));
    const content = document.createElement('div');
    content.className = 'row-main';
    const title = document.createElement('button');
    title.type = 'button';
    title.className = 'row-title';
    title.textContent = entry.title || entry.url;
    title.title = entry.url;
    title.addEventListener('click', () => window.activityApi.openHistoryItem(entry.url));
    const meta = document.createElement('div');
    meta.className = 'row-meta';
    meta.textContent = `${entry.url} · ${formatDateTime(entry.visitedAt)}`;
    content.append(title, meta);
    const actions = document.createElement('div');
    actions.className = 'row-actions';
    actions.append(actionButton('Delete', async () => {
      await window.activityApi.removeHistory([entry.id]);
      selectedHistory.delete(entry.id);
    }, true));
    row.append(content, actions);
    historyList.append(row);
  }
}

function matchesDownloadFilter(entry) {
  if (downloadFilter.value === 'active') return ACTIVE_STATES.has(entry.state);
  if (downloadFilter.value === 'completed') return entry.state === 'completed';
  if (downloadFilter.value === 'failed') return ['failed', 'interrupted', 'cancelled'].includes(entry.state);
  return true;
}

async function runDownloadAction(entry, action) {
  const result = await window.activityApi.downloadAction(entry.id, action);
  if (!result?.ok) showToast(result?.message || 'The action could not be completed.');
}

function renderDownloads() {
  downloadsList.replaceChildren();
  const query = downloadsSearch.value.trim().toLowerCase();
  const entries = state.downloads.filter((entry) => {
    const textMatches = !query || `${entry.filename} ${entry.url}`.toLowerCase().includes(query);
    return textMatches && matchesDownloadFilter(entry);
  });
  if (entries.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = query || downloadFilter.value !== 'all' ? 'No matching downloads.' : 'No downloads yet.';
    downloadsList.append(empty);
    return;
  }
  for (const entry of entries) {
    const active = ACTIVE_STATES.has(entry.state);
    const row = document.createElement('article');
    row.className = 'row';
    row.dataset.id = entry.id;
    row.append(checkboxFor(entry.id, selectedDownloads, updateSelectionButtons, active));
    const content = document.createElement('div');
    content.className = 'row-main';
    const title = document.createElement('div');
    title.className = 'row-title';
    title.textContent = entry.filename;
    const meta = document.createElement('div');
    meta.className = 'row-meta';
    const status = document.createElement('span');
    status.className = `status ${entry.state}`;
    status.textContent = entry.state;
    meta.append(status, document.createTextNode(`${formatBytes(entry.totalBytes)} · ${formatDateTime(entry.startedAt)}`));
    const path = document.createElement('div');
    path.className = 'row-path';
    path.textContent = entry.exists || active ? (entry.savePath || entry.url) : `File missing · ${entry.savePath || entry.url}`;
    content.append(title, meta, path);
    if (active) {
      const progress = document.createElement('progress');
      progress.max = Math.max(entry.totalBytes, 1);
      progress.value = Math.min(entry.receivedBytes, progress.max);
      progress.setAttribute('aria-label', `${entry.filename} download progress`);
      content.append(progress);
    }
    const actions = document.createElement('div');
    actions.className = 'row-actions';
    if (entry.state === 'progressing' || entry.state === 'waiting') actions.append(actionButton('Pause', () => runDownloadAction(entry, 'pause')));
    if (entry.state === 'paused') actions.append(actionButton('Resume', () => runDownloadAction(entry, 'resume')));
    if (active) actions.append(actionButton('Cancel', () => runDownloadAction(entry, 'cancel'), true));
    if (['failed', 'interrupted', 'cancelled'].includes(entry.state)) actions.append(actionButton('Retry', () => runDownloadAction(entry, 'retry')));
    if (entry.state === 'completed' && entry.exists) actions.append(actionButton('Open file', () => runDownloadAction(entry, 'open-file')));
    if (entry.savePath) actions.append(actionButton('Open folder', () => runDownloadAction(entry, 'open-folder')));
    if (!active) {
      actions.append(actionButton('Delete record', async () => {
        await window.activityApi.removeDownloads([entry.id]);
        selectedDownloads.delete(entry.id);
      }));
    }
    if (entry.state === 'completed' && entry.exists) {
      actions.append(actionButton('Delete file', async () => {
        const result = await window.activityApi.trashDownload(entry.id);
        if (!result?.ok) showToast(result?.message || 'The file could not be deleted.');
      }, true));
    }
    row.append(content, actions);
    downloadsList.append(row);
  }
  scrollToHighlight();
}

function updateSelectionButtons() {
  deleteHistorySelection.disabled = selectedHistory.size === 0;
  deleteDownloadSelection.disabled = selectedDownloads.size === 0;
}

function scrollToHighlight() {
  if (!highlightId || currentSection !== 'downloads') return;
  const row = downloadsList.querySelector(`[data-id="${CSS.escape(highlightId)}"]`);
  if (row) {
    row.classList.add('highlight');
    row.scrollIntoView({ block: 'center', behavior: 'smooth' });
    highlightId = null;
  } else if (highlightId === 'missing') {
    showToast('That download record no longer exists.');
    highlightId = null;
  }
}

function render(nextState) {
  state = nextState;
  for (const id of [...selectedHistory]) if (!state.history.some((item) => item.id === id)) selectedHistory.delete(id);
  for (const id of [...selectedDownloads]) if (!state.downloads.some((item) => item.id === id)) selectedDownloads.delete(id);
  activeCount.hidden = state.activeDownloadCount === 0;
  activeCount.textContent = String(state.activeDownloadCount);
  updateSelectionButtons();
  renderHistory();
  renderDownloads();
}

for (const button of navButtons) button.addEventListener('click', () => setSection(button.dataset.section));
historySearch.addEventListener('input', renderHistory);
downloadsSearch.addEventListener('input', renderDownloads);
downloadFilter.addEventListener('change', renderDownloads);
deleteHistorySelection.addEventListener('click', async () => {
  if (!confirm(`Delete ${selectedHistory.size} selected history record(s)?`)) return;
  await window.activityApi.removeHistory([...selectedHistory]);
  selectedHistory.clear();
});
clearHistory.addEventListener('click', async () => {
  if (!state.history.length || !confirm(`Clear all ${state.history.length} history record(s)?`)) return;
  await window.activityApi.clearHistory();
});
deleteDownloadSelection.addEventListener('click', async () => {
  if (!confirm(`Delete ${selectedDownloads.size} selected download record(s)? Files will be kept.`)) return;
  await window.activityApi.removeDownloads([...selectedDownloads]);
  selectedDownloads.clear();
});
clearDownloads.addEventListener('click', async () => {
  const count = state.downloads.filter((entry) => !ACTIVE_STATES.has(entry.state)).length;
  if (!count || !confirm(`Clear ${count} finished download record(s)? Files will be kept.`)) return;
  await window.activityApi.clearDownloads();
});

window.activityApi.onState(render);
window.activityApi.onNavigate(({ section, highlightId: targetId }) => {
  highlightId = targetId;
  setSection(section);
});
window.activityApi.getState().then(render);
