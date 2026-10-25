'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('browserApi', {
  getState: () => ipcRenderer.invoke('tabs:get-state'),
  createTab: () => ipcRenderer.send('tabs:create'),
  activateTab: (id) => ipcRenderer.send('tabs:activate', id),
  closeTab: (id) => ipcRenderer.send('tabs:close', id),
  navigate: (value) => ipcRenderer.send('tabs:navigate', value),
  openActivity: (section) => ipcRenderer.send('activity:open', section),
  onState: (callback) => {
    const listener = (_event, state) => callback(state);
    ipcRenderer.on('tabs:state', listener);
    return () => ipcRenderer.removeListener('tabs:state', listener);
  },
  onFocusAddress: (callback) => {
    const listener = () => callback();
    ipcRenderer.on('chrome:focus-address', listener);
    return () => ipcRenderer.removeListener('chrome:focus-address', listener);
  },
  onDownloadState: (callback) => {
    const listener = (_event, state) => callback(state);
    ipcRenderer.on('chrome:download-state', listener);
    return () => ipcRenderer.removeListener('chrome:download-state', listener);
  },
});
