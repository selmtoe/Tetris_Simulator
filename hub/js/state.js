const getById = (id) => document.getElementById(id);

export const elements = Object.freeze({
    editorCustomFrame: getById('iframe-editor-custom'),
    editorOfficialFrame: getById('iframe-editor-official'),
    simFrame: getById('iframe-sim'),
    desktopArea: getById('desktop-area'),
    splitter: getById('splitter'),
    fabMenu: getById('fabMenu'),
    mainToggle: document.querySelector('.main-toggle'),
    taskbar: getById('taskbar'),
    settingsModal: getById('hubSettingsModal'),
    fileModal: getById('fileModal'),
    toast: getById('toast')
});

export const state = {
    activeTab: 'editor',
    currentFileTab: 'manual',
    pendingSnapshot: { editor: null, sim: null, mode: 'manual' },
    autoSaveTimer: null,
    maxZIndex: 10,
    fabHoldTimer: null,
    isFabDragging: false,
    fabStartX: 0,
    fabStartY: 0,
    fabInitialLeft: 0,
    fabInitialTop: 0,
    wasFabDragged: false,
    isDraggingSplitter: false,
    devModeCount: 0
};

export const hubSettings = {
    layoutMode: 'tab',
    defaultEditor: 'custom',
    autoSaveInterval: 300000
};

export const winStates = {
    custom: { minimized: false, maximized: false, name: 'エディタ', prevRect: null },
    official: { minimized: false, maximized: false, name: 'fumen-for-mobile', prevRect: null },
    sim: { minimized: false, maximized: false, name: 'シミュレータ', prevRect: null }
};

const windowElementIds = {
    custom: 'win-custom-editor',
    official: 'win-official-editor',
    sim: 'win-sim'
};

export function getAppWindow(id) {
    return getById(windowElementIds[id]);
}
