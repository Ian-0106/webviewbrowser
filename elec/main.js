'use strict';

const fs = require('node:fs');
const path = require('node:path');
const {
  app,
  BrowserWindow,
  dialog,
  ipcMain,
  Menu,
  Notification,
  session,
  shell,
  WebContentsView,
} = require('electron');
const { normalizeUrl } = require('./src/url');
const { ActivityStore, ACTIVE_DOWNLOAD_STATES, isWebUrl } = require('./src/activity-store');

const APP_NAME = 'WebView Browser Electron';
const APP_ID = 'com.webviewbrowser.electron';
const TOAST_ACTIVATOR_CLSID = '{923A6F87-8D59-4EBE-8F7D-6D1511D7E30C}';
const CHROME_HEIGHT = 80;
const INITIAL_URL = 'https://bilibili.com';
const NEW_TAB_URL = 'https://cn.bing.com';

let mainWindow = null;
let activityWindow = null;
let activityStore = null;
let nextTabId = 1;
let activeTabId = null;
let allowMainClose = false;
let pendingNotificationActivation = null;
const tabs = new Map();
const activeDownloads = new Map();
const pendingRetries = new Map();
const lastDownloadUpdates = new Map();

app.setName(APP_NAME);
app.setAppUserModelId(APP_ID);
app.setToastActivatorCLSID(TOAST_ACTIVATOR_CLSID);

const hasSingleInstanceLock = app.requestSingleInstanceLock();
if (!hasSingleInstanceLock) {
  app.quit();
}

function getTabState() {
  return {
    activeTabId,
    tabs: [...tabs.values()].map(({ id, title, url }) => ({ id, title, url })),
  };
}

function sendTabState() {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('tabs:state', getTabState());
  }
}

function getActivityState() {
  const downloads = activityStore.listDownloads().map((entry) => ({
    ...entry,
    exists: Boolean(entry.savePath && fs.existsSync(entry.savePath)),
  }));
  return {
    history: activityStore.listHistory(),
    downloads,
    activeDownloadCount: activityStore.activeDownloadCount(),
  };
}

function broadcastActivityState() {
  const state = getActivityState();
  if (activityWindow && !activityWindow.isDestroyed()) {
    activityWindow.webContents.send('activity:state', state);
  }
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('chrome:download-state', {
      activeDownloadCount: state.activeDownloadCount,
    });
  }
}

function updateWindowTitle() {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  const activeTab = tabs.get(activeTabId);
  mainWindow.setTitle(activeTab?.title ? `${APP_NAME} - ${activeTab.title}` : APP_NAME);
}

function layoutActiveTab() {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  const { width, height } = mainWindow.getContentBounds();
  for (const tab of tabs.values()) {
    const isActive = tab.id === activeTabId;
    tab.view.setVisible(isActive);
    if (isActive) {
      tab.view.setBounds({ x: 0, y: CHROME_HEIGHT, width, height: Math.max(0, height - CHROME_HEIGHT) });
    }
  }
}

function activateTab(id) {
  const tab = tabs.get(id);
  if (!tab || !mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  activeTabId = id;
  mainWindow.contentView.addChildView(tab.view);
  layoutActiveTab();
  updateWindowTitle();
  sendTabState();
  tab.view.webContents.focus();
}

function focusMainWindow() {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  if (mainWindow.isMinimized()) {
    mainWindow.restore();
  }
  mainWindow.show();
  mainWindow.focus();
}

function handleShortcut(input) {
  if (!input.control || input.type !== 'keyDown') {
    return false;
  }
  const key = input.key.toLowerCase();
  if (key === 't') {
    createTab(NEW_TAB_URL);
  } else if (key === 'w') {
    closeTab(activeTabId);
  } else if (key === 'l') {
    mainWindow?.webContents.focus();
    mainWindow?.webContents.send('chrome:focus-address');
  } else if (key === 'h') {
    openActivity('history');
  } else if (key === 'j') {
    openActivity('downloads');
  } else if (key === 'tab') {
    cycleTab(input.shift ? -1 : 1);
  } else {
    return false;
  }
  return true;
}

function attachShortcutHandler(webContents) {
  webContents.on('before-input-event', (event, input) => {
    if (handleShortcut(input)) {
      event.preventDefault();
    }
  });
}

function recordTabHistory(tab) {
  if (!tab || tab.view.webContents.isDestroyed()) {
    return;
  }
  const url = tab.view.webContents.getURL();
  const title = tab.view.webContents.getTitle() || tab.title || url;
  tab.lastHistoryId = activityStore.recordHistory({ tabId: tab.id, url, title });
  broadcastActivityState();
}

function createTab(targetUrl = NEW_TAB_URL) {
  if (!mainWindow || mainWindow.isDestroyed()) {
    return null;
  }
  const id = nextTabId++;
  const url = normalizeUrl(targetUrl) || NEW_TAB_URL;
  const view = new WebContentsView({
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true, webSecurity: true },
  });
  const tab = { id, title: 'New Tab', url, view, lastHistoryId: null };
  tabs.set(id, tab);
  mainWindow.contentView.addChildView(view);
  view.setBackgroundColor('#ffffff');
  attachShortcutHandler(view.webContents);

  view.webContents.on('page-title-updated', (_event, title) => {
    if (!tabs.has(id)) {
      return;
    }
    tab.title = title || 'New Tab';
    if (tab.lastHistoryId) {
      activityStore.updateHistoryTitle(tab.lastHistoryId, tab.title);
      broadcastActivityState();
    }
    updateWindowTitle();
    sendTabState();
  });

  const syncUrl = (_event, currentUrl) => {
    if (!tabs.has(id) || !currentUrl) {
      return;
    }
    tab.url = currentUrl;
    sendTabState();
  };
  view.webContents.on('did-navigate', syncUrl);
  view.webContents.on('did-navigate-in-page', (event, currentUrl, isMainFrame) => {
    syncUrl(event, currentUrl);
    if (isMainFrame) {
      recordTabHistory(tab);
    }
  });
  view.webContents.on('did-finish-load', () => recordTabHistory(tab));
  view.webContents.setWindowOpenHandler(({ url: popupUrl }) => {
    createTab(popupUrl || NEW_TAB_URL);
    return { action: 'deny' };
  });
  view.webContents.on('destroyed', () => {
    if (tabs.get(id)?.view === view) {
      tabs.delete(id);
    }
  });

  activateTab(id);
  view.webContents.loadURL(url).catch((error) => {
    if (!view.webContents.isDestroyed()) {
      console.error(`Failed to load ${url}:`, error.message);
    }
  });
  return id;
}

function closeTab(id) {
  const tab = tabs.get(id);
  if (!tab || !mainWindow || mainWindow.isDestroyed()) {
    return;
  }
  const ids = [...tabs.keys()];
  const closedIndex = ids.indexOf(id);
  const wasActive = activeTabId === id;
  mainWindow.contentView.removeChildView(tab.view);
  tabs.delete(id);
  if (!tab.view.webContents.isDestroyed()) {
    tab.view.webContents.close();
  }
  if (tabs.size === 0) {
    activeTabId = null;
    mainWindow.close();
    return;
  }
  if (wasActive) {
    const remainingIds = [...tabs.keys()];
    activateTab(remainingIds[Math.min(closedIndex, remainingIds.length - 1)]);
  } else {
    sendTabState();
  }
}

function cycleTab(offset) {
  const ids = [...tabs.keys()];
  if (ids.length < 2) {
    return;
  }
  const currentIndex = ids.indexOf(activeTabId);
  activateTab(ids[(currentIndex + offset + ids.length) % ids.length]);
}

function navigateActiveTab(input) {
  const url = normalizeUrl(input);
  const tab = tabs.get(activeTabId);
  if (!url || !tab || tab.view.webContents.isDestroyed()) {
    return;
  }
  tab.url = url;
  sendTabState();
  tab.view.webContents.loadURL(url).catch((error) => {
    if (!tab.view.webContents.isDestroyed()) {
      console.error(`Failed to load ${url}:`, error.message);
    }
  });
}

async function openActivity(section = 'history', highlightId = null) {
  if (!activityWindow || activityWindow.isDestroyed()) {
    activityWindow = new BrowserWindow({
      width: 980,
      height: 720,
      minWidth: 720,
      minHeight: 500,
      show: false,
      title: `Activity Center - ${APP_NAME}`,
      backgroundColor: '#f7f8fa',
      webPreferences: {
        preload: path.join(__dirname, 'preload.js'),
        contextIsolation: true,
        nodeIntegration: false,
        sandbox: true,
      },
    });
    activityWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    activityWindow.webContents.on('will-navigate', (event) => event.preventDefault());
    activityWindow.on('closed', () => {
      activityWindow = null;
    });
    await activityWindow.loadFile(path.join(__dirname, 'renderer', 'activity.html'));
  }
  activityWindow.webContents.send('activity:navigate', { section, highlightId });
  activityWindow.webContents.send('activity:state', getActivityState());
  if (activityWindow.isMinimized()) {
    activityWindow.restore();
  }
  activityWindow.show();
  activityWindow.focus();
}

function enqueueRetry(url, originalId) {
  const queue = pendingRetries.get(url) || [];
  queue.push(originalId);
  pendingRetries.set(url, queue);
}

function consumeRetry(url) {
  const queue = pendingRetries.get(url);
  if (!queue?.length) {
    return null;
  }
  const retryOf = queue.shift();
  if (queue.length === 0) {
    pendingRetries.delete(url);
  }
  return retryOf;
}

function trackDownload(item) {
  const url = item.getURL();
  const record = activityStore.createDownload({
    filename: item.getFilename(),
    url,
    savePath: item.getSavePath(),
    state: item.isPaused() ? 'paused' : 'progressing',
    totalBytes: item.getTotalBytes(),
    receivedBytes: item.getReceivedBytes(),
    retryOf: consumeRetry(url),
  });
  activeDownloads.set(record.id, item);
  lastDownloadUpdates.set(record.id, Date.now());
  broadcastActivityState();

  item.on('updated', (_event, state) => {
    const now = Date.now();
    if (state === 'progressing' && now - (lastDownloadUpdates.get(record.id) || 0) < 250) {
      return;
    }
    lastDownloadUpdates.set(record.id, now);
    const normalizedState = item.isPaused() ? 'paused' : state === 'interrupted' ? 'interrupted' : 'progressing';
    activityStore.updateDownload(record.id, {
      filename: item.getFilename(),
      savePath: item.getSavePath(),
      state: normalizedState,
      totalBytes: item.getTotalBytes(),
      receivedBytes: item.getReceivedBytes(),
      interruptReason: normalizedState === 'interrupted' ? 'The download was interrupted' : '',
    });
    broadcastActivityState();
  });

  item.once('done', (_event, state) => {
    const normalizedState = state === 'completed' ? 'completed' : state === 'cancelled' ? 'cancelled' : 'failed';
    const updated = activityStore.updateDownload(record.id, {
      filename: item.getFilename(),
      savePath: item.getSavePath(),
      state: normalizedState,
      totalBytes: item.getTotalBytes(),
      receivedBytes: item.getReceivedBytes(),
      finishedAt: new Date().toISOString(),
      interruptReason: normalizedState === 'failed' ? 'The download could not be completed' : '',
    });
    activeDownloads.delete(record.id);
    lastDownloadUpdates.delete(record.id);
    broadcastActivityState();
    if (normalizedState === 'completed' || normalizedState === 'failed') {
      showDownloadNotification(updated);
    }
  });
}

function escapeXml(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&apos;');
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) {
    return '0 B';
  }
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const unit = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);
  const amount = value / (1024 ** unit);
  return `${amount >= 10 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[unit]}`;
}

function showDownloadNotification(record) {
  if (!record || !Notification.isSupported()) {
    return;
  }
  const completed = record.state === 'completed';
  const title = completed ? 'Download complete' : 'Download failed';
  const size = record.totalBytes > 0 ? ` (${formatBytes(record.totalBytes)})` : '';
  const body = completed ? `${record.filename}${size}` : `${record.filename}: ${record.interruptReason}`;
  const action = completed ? 'open' : 'retry';
  const actionLabel = completed ? 'Open file' : 'Retry';
  const toastXml = `<toast launch="download:${escapeXml(record.id)}:show"><visual><binding template="ToastGeneric"><text>${escapeXml(title)}</text><text>${escapeXml(body)}</text></binding></visual><actions><action content="${actionLabel}" arguments="download:${escapeXml(record.id)}:${action}" activationType="foreground" /></actions></toast>`;
  new Notification({ id: record.id, toastXml }).show();
}

function retryDownload(id) {
  const record = activityStore.getDownload(id);
  if (!record?.url) {
    return false;
  }
  enqueueRetry(record.url, id);
  session.defaultSession.downloadURL(record.url);
  return true;
}

async function activateDownloadNotification(argumentsValue) {
  const match = /^download:([^:]+):(show|open|retry)$/.exec(argumentsValue || '');
  if (!match) {
    return;
  }
  const [, id, action] = match;
  const record = activityStore?.getDownload(id);
  if (action === 'open' && record?.savePath && fs.existsSync(record.savePath)) {
    await shell.openPath(record.savePath);
    return;
  }
  if (action === 'retry' && record) {
    retryDownload(id);
    return;
  }
  focusMainWindow();
  await openActivity('downloads', record ? id : 'missing');
}

function installIpcHandlers() {
  ipcMain.handle('tabs:get-state', () => getTabState());
  ipcMain.on('tabs:create', () => createTab(NEW_TAB_URL));
  ipcMain.on('tabs:activate', (_event, id) => activateTab(Number(id)));
  ipcMain.on('tabs:close', (_event, id) => closeTab(Number(id)));
  ipcMain.on('tabs:navigate', (_event, value) => navigateActiveTab(value));
  ipcMain.on('activity:open', (_event, section) => openActivity(section));
  ipcMain.handle('activity:get-state', () => getActivityState());
  ipcMain.on('history:open', (_event, url) => {
    if (!isWebUrl(url)) {
      return;
    }
    focusMainWindow();
    createTab(url);
  });
  ipcMain.handle('history:remove', (_event, ids) => {
    const removed = activityStore.removeHistory(Array.isArray(ids) ? ids : []);
    broadcastActivityState();
    return removed;
  });
  ipcMain.handle('history:clear', () => {
    const removed = activityStore.clearHistory();
    broadcastActivityState();
    return removed;
  });
  ipcMain.handle('downloads:action', async (_event, id, action) => {
    const record = activityStore.getDownload(id);
    const item = activeDownloads.get(id);
    if (!record) {
      return { ok: false, message: 'The download record no longer exists.' };
    }
    if (action === 'pause' && item && !item.isPaused() && item.canResume()) {
      item.pause();
    } else if (action === 'resume' && item?.canResume()) {
      item.resume();
    } else if (action === 'cancel' && item) {
      item.cancel();
    } else if (action === 'retry') {
      return { ok: retryDownload(id) };
    } else if (action === 'open-file') {
      if (!record.savePath || !fs.existsSync(record.savePath)) {
        return { ok: false, message: 'The downloaded file no longer exists.' };
      }
      const error = await shell.openPath(record.savePath);
      return { ok: !error, message: error };
    } else if (action === 'open-folder') {
      if (!record.savePath) {
        return { ok: false, message: 'No saved file path is available.' };
      }
      if (fs.existsSync(record.savePath)) {
        shell.showItemInFolder(record.savePath);
      } else {
        const error = await shell.openPath(path.dirname(record.savePath));
        return { ok: !error, message: error };
      }
    } else {
      return { ok: false, message: 'This action is not available.' };
    }
    return { ok: true };
  });
  ipcMain.handle('downloads:remove', (_event, ids) => {
    const removed = activityStore.removeDownloads(Array.isArray(ids) ? ids : [], { terminalOnly: true });
    broadcastActivityState();
    return removed;
  });
  ipcMain.handle('downloads:clear', () => {
    const removed = activityStore.clearTerminalDownloads();
    broadcastActivityState();
    return removed;
  });
  ipcMain.handle('downloads:trash', async (_event, id) => {
    const record = activityStore.getDownload(id);
    if (!record) {
      return { ok: false, message: 'The download record no longer exists.' };
    }
    if (ACTIVE_DOWNLOAD_STATES.has(record.state)) {
      return { ok: false, message: 'Cancel the active download before deleting it.' };
    }
    if (!record.savePath || !fs.existsSync(record.savePath)) {
      return { ok: false, message: 'The downloaded file no longer exists.' };
    }
    await shell.trashItem(record.savePath);
    activityStore.removeDownloads([id]);
    broadcastActivityState();
    return { ok: true };
  });
}

function buildApplicationMenu() {
  return Menu.buildFromTemplate([
    {
      label: 'Activity',
      submenu: [
        { label: 'History', accelerator: 'CmdOrCtrl+H', click: () => openActivity('history') },
        { label: 'Downloads', accelerator: 'CmdOrCtrl+J', click: () => openActivity('downloads') },
      ],
    },
    {
      label: 'Help',
      submenu: [{
        label: 'About...',
        click: () => dialog.showMessageBox(mainWindow, {
          type: 'info', title: 'About', message: `${APP_NAME}\nVersion ${app.getVersion()}`, buttons: ['OK'],
        }),
      }],
    },
  ]);
}

function confirmMainWindowClose(event) {
  if (allowMainClose || !activityStore || activityStore.activeDownloadCount() === 0) {
    return;
  }
  const count = activityStore.activeDownloadCount();
  const choice = dialog.showMessageBoxSync(mainWindow, {
    type: 'warning',
    title: 'Downloads in progress',
    message: `${count} download${count === 1 ? ' is' : 's are'} still in progress.`,
    detail: 'Exiting now will interrupt active downloads. You can retry them from Downloads next time.',
    buttons: ['Keep browsing', 'Exit and interrupt'],
    defaultId: 0,
    cancelId: 0,
  });
  if (choice === 0) {
    event.preventDefault();
    return;
  }
  allowMainClose = true;
  for (const item of activeDownloads.values()) {
    item.cancel();
  }
}

async function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1100,
    height: 760,
    minWidth: 480,
    minHeight: 320,
    show: false,
    backgroundColor: '#ffffff',
    title: APP_NAME,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  Menu.setApplicationMenu(buildApplicationMenu());
  attachShortcutHandler(mainWindow.webContents);
  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  mainWindow.webContents.on('will-navigate', (event) => event.preventDefault());
  mainWindow.on('resize', layoutActiveTab);
  mainWindow.on('close', confirmMainWindowClose);
  mainWindow.on('closed', () => {
    for (const tab of tabs.values()) {
      if (!tab.view.webContents.isDestroyed()) {
        tab.view.webContents.close();
      }
    }
    tabs.clear();
    activeTabId = null;
    mainWindow = null;
    if (activityWindow && !activityWindow.isDestroyed()) {
      activityWindow.close();
    }
  });
  await mainWindow.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  createTab(INITIAL_URL);
  mainWindow.show();
  broadcastActivityState();
}

if (hasSingleInstanceLock) {
  app.on('second-instance', () => focusMainWindow());
  Notification.handleActivation((details) => {
    if (!activityStore) {
      pendingNotificationActivation = details.arguments;
      return;
    }
    activateDownloadNotification(details.arguments).catch((error) => console.error(error));
  });
  app.whenReady().then(async () => {
    activityStore = new ActivityStore(path.join(app.getPath('userData'), 'activity.json'));
    session.defaultSession.setPermissionCheckHandler(() => false);
    session.defaultSession.setPermissionRequestHandler((_webContents, _permission, callback) => callback(false));
    session.defaultSession.on('will-download', (_event, item) => trackDownload(item));
    installIpcHandlers();
    await createWindow();
    if (pendingNotificationActivation) {
      const activation = pendingNotificationActivation;
      pendingNotificationActivation = null;
      await activateDownloadNotification(activation);
    }
    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) {
        createWindow();
      }
    });
  });
}

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});
