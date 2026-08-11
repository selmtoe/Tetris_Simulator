import { elements, state } from './state.js';

export function requestSnapshot(mode) {
    state.pendingSnapshot = { editor: null, sim: null, mode };
    elements.editorCustomFrame.contentWindow.postMessage({ type: 'requestState' }, '*');
    elements.simFrame.contentWindow.postMessage({ type: 'requestState' }, '*');
}
