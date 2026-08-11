import { elements, hubSettings, state } from './state.js';
import { showToast } from './toast.js';

export function initializeFabDrag() {
    elements.mainToggle.addEventListener('mousedown', startFabHold);
    elements.mainToggle.addEventListener('touchstart', startFabHold, { passive: false });
    elements.mainToggle.addEventListener('touchmove', (event) => {
        event.preventDefault();
    }, { passive: false });
}

export function startFabHold(event) {
    state.fabHoldTimer = setTimeout(() => {
        state.isFabDragging = true;
        state.wasFabDragged = true;

        const rect = elements.fabMenu.getBoundingClientRect();
        state.fabInitialLeft = rect.left;
        state.fabInitialTop = rect.top;
        state.fabStartX = event.touches ? event.touches[0].clientX : event.clientX;
        state.fabStartY = event.touches ? event.touches[0].clientY : event.clientY;

        elements.fabMenu.style.bottom = 'auto';
        elements.fabMenu.style.right = 'auto';
        elements.fabMenu.style.left = `${state.fabInitialLeft}px`;
        elements.fabMenu.style.top = `${state.fabInitialTop}px`;

        document.addEventListener('mousemove', onFabDrag);
        document.addEventListener('touchmove', onFabDrag, { passive: false });
        document.addEventListener('mouseup', stopFabDrag);
        document.addEventListener('touchend', stopFabDrag);

        document.querySelectorAll('iframe').forEach((frame) => {
            frame.style.pointerEvents = 'none';
        });

        showToast('位置変更モード');
        if (elements.fabMenu.classList.contains('open')) {
            elements.fabMenu.classList.remove('open');
            elements.mainToggle.classList.remove('open');
        }
    }, 500);

    document.addEventListener('mouseup', cancelFabHold);
    document.addEventListener('touchend', cancelFabHold);
}

export function cancelFabHold() {
    if (state.fabHoldTimer) {
        clearTimeout(state.fabHoldTimer);
        state.fabHoldTimer = null;
    }
    document.removeEventListener('mouseup', cancelFabHold);
    document.removeEventListener('touchend', cancelFabHold);
}

export function onFabDrag(event) {
    if (!state.isFabDragging) return;

    event.preventDefault();
    const clientX = event.touches ? event.touches[0].clientX : event.clientX;
    const clientY = event.touches ? event.touches[0].clientY : event.clientY;

    elements.fabMenu.style.left = `${state.fabInitialLeft + (clientX - state.fabStartX)}px`;
    elements.fabMenu.style.top = `${state.fabInitialTop + (clientY - state.fabStartY)}px`;
}

export function stopFabDrag() {
    state.isFabDragging = false;
    document.removeEventListener('mousemove', onFabDrag);
    document.removeEventListener('touchmove', onFabDrag);
    document.removeEventListener('mouseup', stopFabDrag);
    document.removeEventListener('touchend', stopFabDrag);

    document.querySelectorAll('iframe').forEach((frame) => {
        frame.style.pointerEvents = '';
    });

    const buttons = elements.fabMenu.querySelectorAll('.fab-btn');
    buttons.forEach((button) => {
        button.style.transition = 'none';
    });

    const mainToggleRect = elements.mainToggle.getBoundingClientRect();
    const wasExpandDown = elements.fabMenu.classList.contains('expand-down');
    const shouldExpandDown = mainToggleRect.top < window.innerHeight / 2;

    if (shouldExpandDown) {
        elements.fabMenu.classList.add('expand-down');
    } else {
        elements.fabMenu.classList.remove('expand-down');
    }

    if (wasExpandDown !== shouldExpandDown) {
        elements.fabMenu.offsetHeight;
        const newMainToggleRect = elements.mainToggle.getBoundingClientRect();
        const diffY = mainToggleRect.top - newMainToggleRect.top;
        const currentTop = parseFloat(elements.fabMenu.style.top || 0);

        if (!Number.isNaN(diffY) && Math.abs(diffY) < 1000) {
            elements.fabMenu.style.top = `${currentTop + diffY}px`;
        }
    }

    requestAnimationFrame(() => {
        buttons.forEach((button) => {
            button.style.transition = '';
        });
    });

    setTimeout(() => {
        state.wasFabDragged = false;
    }, 100);
}

export function toggleMenu() {
    if (state.wasFabDragged) return;

    elements.fabMenu.classList.toggle('open');
    elements.mainToggle.classList.toggle('open');

    if (hubSettings.layoutMode === 'window') {
        if (elements.fabMenu.classList.contains('open')) {
            elements.taskbar.classList.remove('hidden');
        } else {
            elements.taskbar.classList.add('hidden');
        }
    }
}
