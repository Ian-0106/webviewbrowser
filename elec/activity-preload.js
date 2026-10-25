'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('activityApi', {
  getState: () => ipcRenderer.invoke('activity:get-state'),
  openHistoryItem: (url) => ipcRenderer.send('history:open', url),
  removeHistory: (ids) => ipcRenderer.invoke('history:remove', ids),
  clearHistory: () => ipcRenderer.invoke('history:clear'),
  downloadAction: (id, action) => ipcRenderer.invoke('downloads:action', id, action),
  removeDownloads: (ids) => ipcRenderer.invoke('downloads:remove', ids),
  clearDownloads: () => ipcRenderer.invoke('downloads:clear'),
  trashDownload: (id) => ipcRenderer.invoke('downloads:trash', id),
  onState: (callback) => {
    const listener = (_event, state) => callback(state);
    ipcRenderer.on('activity:state', listener);
    return () => ipcRenderer.removeListener('activity:state', listener);
  },
  onNavigate: (callback) => {
    const listener = (_event, target) => callback(target);
    ipcRenderer.on('activity:navigate', listener);
    return () => ipcRenderer.removeListener('activity:navigate', listener);
  },
});
