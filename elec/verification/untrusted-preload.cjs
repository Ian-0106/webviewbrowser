'use strict';

// Test-only bridge; verification/ is excluded from packaged application files.
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('securityProbe', {
  invoke: (channel, ...args) => ipcRenderer.invoke(channel, ...args),
});
