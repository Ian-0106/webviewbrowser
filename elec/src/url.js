'use strict';

const SEARCH_URL = 'https://cn.bing.com/search?q=';

function normalizeUrl(input) {
  const value = String(input ?? '').trim();
  if (!value) {
    return '';
  }

  if (
    value.startsWith('http://') ||
    value.startsWith('https://') ||
    value.startsWith('about:') ||
    value.startsWith('file://')
  ) {
    return value;
  }

  if (!value.includes(' ') && value.includes('.')) {
    return `https://${value}`;
  }

  return `${SEARCH_URL}${value.replaceAll(' ', '+')}`;
}

module.exports = { normalizeUrl, SEARCH_URL };
