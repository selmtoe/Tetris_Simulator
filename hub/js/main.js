import { importFromOfficial, initializeBridge } from './bridge.js';
import {
    applyLayoutMode,
    initializeSplitter,
    onSplitterDrag,
    startSplitterDrag,
    stopSplitterDrag,
    swapSides,
    switchTab,
    toggleSplitMode
} from './layout.js';
import {
    cancelFabHold,
    initializeFabDrag,
    onFabDrag,
    startFabHold,
    stopFabDrag,
    toggleMenu
} from './menu.js';
import {
    deleteAllFiles,
    deleteFile,
    escapeHtml,
    exportData,
    exportIndividual,
    finalizeSave,
    importData,
    loadFile,
    loadFromUrlInput,
    loadStateFromSharedHash,
    openFileManager,
    renameFile,
    renderFileList,
    saveCurrentState,
    shareIndividual,
    switchSaveTab,
    triggerImport
} from './saves.js';
import {
    loadSettings,
    openSettings,
    saveHubSettings,
    triggerDevMode,
    updateAutoSaveInterval
} from './settings.js';
import { requestSnapshot } from './snapshot.js';
import { elements, hubSettings, state } from './state.js';
import { showToast } from './toast.js';
import {
    initWindows,
    makeWindowDraggable,
    makeWindowResizable,
    minimizeWindow,
    renderTaskbar,
    resetWindowLayouts,
    restoreWindow,
    saveWindowLayouts,
    toggleMaximizeWindow
} from './windows.js';

Object.assign(window, {
    applyLayoutMode,
    cancelFabHold,
    deleteAllFiles,
    deleteFile,
    escapeHtml,
    exportData,
    exportIndividual,
    finalizeSave,
    importData,
    importFromOfficial,
    initWindows,
    loadFile,
    loadFromUrlInput,
    loadSettings,
    makeWindowDraggable,
    makeWindowResizable,
    minimizeWindow,
    onFabDrag,
    onSplitterDrag,
    openFileManager,
    openSettings,
    renameFile,
    renderFileList,
    renderTaskbar,
    requestSnapshot,
    resetWindowLayouts,
    restoreWindow,
    saveCurrentState,
    saveHubSettings,
    saveWindowLayouts,
    shareIndividual,
    showToast,
    startFabHold,
    startSplitterDrag,
    stopFabDrag,
    stopSplitterDrag,
    swapSides,
    switchSaveTab,
    switchTab,
    toggleMaximizeWindow,
    toggleMenu,
    toggleSplitMode,
    triggerDevMode,
    triggerImport,
    updateAutoSaveInterval
});

initializeBridge();
initializeFabDrag();
initializeSplitter();

// start.bat opens the Hub, where the simulator lives in an iframe.  Keyboard
// events only reach that iframe after it has focus, so forward the PC shortcut
// when the visible simulator is not currently focused.
function isTextEntryTarget(target) {
    return target && (
        target.tagName === 'INPUT' ||
        target.tagName === 'TEXTAREA' ||
        target.tagName === 'SELECT' ||
        target.isContentEditable
    );
}

function isSimulatorVisible() {
    const simulatorWindow = document.getElementById('win-sim');
    if (!simulatorWindow || simulatorWindow.classList.contains('minimized')) return false;
    if (hubSettings.layoutMode === 'tab') return state.activeTab === 'sim';
    return simulatorWindow.classList.contains('active');
}

document.addEventListener('keydown', event => {
    if (event.defaultPrevented || event.repeat || isTextEntryTarget(event.target)) {
        return;
    }
    if (!isSimulatorVisible()) return;

    try {
        const finder = elements.simFrame.contentWindow?.PCFinder;
        if (typeof finder?.searchIfBoundKey !== 'function' || !finder.searchIfBoundKey(event.key)) return;
        event.preventDefault();
        event.stopPropagation();
    } catch (error) {
        // The iframe may still be loading. In that case the normal in-game
        // handler will be available as soon as it is ready.
        console.warn('Unable to forward the PC shortcut to the simulator:', error);
    }
}, true);

window.addEventListener('load', () => {
    loadSettings();
    initWindows();
    loadStateFromSharedHash();
});

window.addEventListener('blur', () => {
    if (elements.fabMenu.classList.contains('open')) {
        toggleMenu();
    }
});

if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
        navigator.serviceWorker.register('./sw.js');
    });
}
