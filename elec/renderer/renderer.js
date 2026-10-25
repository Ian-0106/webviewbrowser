'use strict';

const tabsElement = document.querySelector('#tabs');
const newTabButton = document.querySelector('#new-tab');
const addressForm = document.querySelector('#address-form');
const addressInput = document.querySelector('#address');
const historyButton = document.querySelector('#history-button');
const downloadsButton = document.querySelector('#downloads-button');
const downloadCount = document.querySelector('#download-count');

let currentState = { activeTabId: null, tabs: [] };
let addressIsBeingEdited = false;

function render(state) {
  currentState = state;
  tabsElement.replaceChildren();

  for (const tab of state.tabs) {
    const tabElement = document.createElement('div');
    tabElement.className = `tab${tab.id === state.activeTabId ? ' active' : ''}`;
    tabElement.setAttribute('role', 'tab');
    tabElement.setAttribute('tabindex', tab.id === state.activeTabId ? '0' : '-1');
    tabElement.setAttribute('aria-selected', String(tab.id === state.activeTabId));
    tabElement.title = tab.title || 'New Tab';
    tabElement.addEventListener('click', () => window.browserApi.activateTab(tab.id));
    tabElement.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        window.browserApi.activateTab(tab.id);
      }
    });

    const title = document.createElement('span');
    title.className = 'tab-title';
    title.textContent = (tab.title || 'New Tab').slice(0, 30);

    const closeButton = document.createElement('button');
    closeButton.type = 'button';
    closeButton.className = 'tab-close';
    closeButton.textContent = '×';
    closeButton.title = `关闭 ${tab.title || 'New Tab'}`;
    closeButton.setAttribute('aria-label', closeButton.title);
    closeButton.addEventListener('click', (event) => {
      event.stopPropagation();
      window.browserApi.closeTab(tab.id);
    });

    tabElement.append(title, closeButton);
    tabsElement.append(tabElement);
  }

  const activeTab = state.tabs.find((tab) => tab.id === state.activeTabId);
  if (!addressIsBeingEdited) {
    addressInput.value = activeTab?.url ?? '';
  }
}

newTabButton.addEventListener('click', () => window.browserApi.createTab());
historyButton.addEventListener('click', () => window.browserApi.openActivity('history'));
downloadsButton.addEventListener('click', () => window.browserApi.openActivity('downloads'));

addressInput.addEventListener('focus', () => {
  addressIsBeingEdited = true;
});

addressInput.addEventListener('blur', () => {
  addressIsBeingEdited = false;
  const activeTab = currentState.tabs.find((tab) => tab.id === currentState.activeTabId);
  addressInput.value = activeTab?.url ?? '';
});

addressForm.addEventListener('submit', (event) => {
  event.preventDefault();
  addressIsBeingEdited = false;
  window.browserApi.navigate(addressInput.value);
  addressInput.blur();
});

window.browserApi.onState(render);
window.browserApi.onFocusAddress(() => {
  addressInput.focus();
  addressInput.select();
});

window.browserApi.onDownloadState(({ activeDownloadCount }) => {
  downloadCount.hidden = activeDownloadCount === 0;
  downloadCount.textContent = String(activeDownloadCount);
});

window.browserApi.getState().then(render);
