'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');

function isTrustedSender(event, window, pagePath) {
  try {
    if (!window || window.isDestroyed()) return false;
    const contents = window.webContents;
    // A matching URL alone does not authenticate another tab or a child frame.
    return event.sender === contents && !contents.isDestroyed()
      && event.senderFrame === contents.mainFrame
      && event.senderFrame.url === pathToFileURL(pagePath).href;
  } catch {
    return false;
  }
}

function isLocalAbsolutePath(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= 32768
    && !value.includes('\0') && path.isAbsolute(value)
    && !/^(?:\\\\|\/\/)/.test(value);
}

function isRegularDownloadFile(value) {
  if (!isLocalAbsolutePath(value)) return false;
  try {
    // Never turn a single-file action into an operation on a directory or link.
    return fs.lstatSync(value).isFile();
  } catch {
    return false;
  }
}

module.exports = { isTrustedSender, isLocalAbsolutePath, isRegularDownloadFile };
