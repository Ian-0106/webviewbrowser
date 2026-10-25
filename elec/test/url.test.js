'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { normalizeUrl } = require('../src/url');

test('empty and whitespace-only input does not navigate', () => {
  assert.equal(normalizeUrl(''), '');
  assert.equal(normalizeUrl('   \r\n'), '');
});

test('supported URLs are preserved', () => {
  assert.equal(normalizeUrl(' https://example.com/path '), 'https://example.com/path');
  assert.equal(normalizeUrl('http://localhost:8080'), 'http://localhost:8080');
  assert.equal(normalizeUrl('about:blank'), 'about:blank');
  assert.equal(normalizeUrl('file:///C:/demo.html'), 'file:///C:/demo.html');
});

test('host-like input receives an HTTPS scheme', () => {
  assert.equal(normalizeUrl('example.com'), 'https://example.com');
  assert.equal(normalizeUrl('docs.example.com/page'), 'https://docs.example.com/page');
});

test('other input becomes a Bing search', () => {
  assert.equal(normalizeUrl('electron browser'), 'https://cn.bing.com/search?q=electron+browser');
  assert.equal(normalizeUrl('浏览器'), 'https://cn.bing.com/search?q=浏览器');
});
