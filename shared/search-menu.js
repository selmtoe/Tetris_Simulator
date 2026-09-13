/* The same hover/tap menu in both tools. A body layer avoids clipping by
   the viewer's animated Page shell and horizontal action row. */
document.addEventListener('DOMContentLoaded', () => {
    document.querySelectorAll('[data-search-menu]').forEach(trigger => {
        const panel = document.getElementById(trigger.dataset.searchMenu);
        if (!panel) return;
        document.body.append(panel);
        let pinned = false, timer;
        const position = () => {
            const rect = trigger.getBoundingClientRect();
            panel.style.left = `${Math.max(8, Math.min(innerWidth - panel.offsetWidth - 8, rect.right - panel.offsetWidth))}px`;
            panel.style.top = `${Math.max(8, Math.min(innerHeight - panel.scrollHeight - 8, rect.bottom + 5))}px`;
        };
        const open = () => {
            clearTimeout(timer);
            position();
            panel.inert = false;
            panel.classList.add('is-open');
            trigger.setAttribute('aria-expanded', 'true');
        };
        const close = () => {
            clearTimeout(timer);
            pinned = false;
            panel.classList.remove('is-open');
            panel.inert = true;
            trigger.setAttribute('aria-expanded', 'false');
        };
        trigger.setAttribute('aria-controls', panel.id);
        trigger.setAttribute('aria-expanded', 'false');
        panel.inert = true;
        [trigger, panel].forEach(element => {
            element.addEventListener('pointerenter', event => {
                if (event.pointerType === 'mouse' || event.pointerType === 'pen') open();
            });
            element.addEventListener('pointerleave', () => {
                timer = setTimeout(() => { if (!pinned && !panel.contains(document.activeElement)) close(); }, 180);
            });
            element.addEventListener('keydown', event => {
                event.stopPropagation();
                if (event.key === 'Escape') { event.preventDefault(); close(); trigger.focus(); }
                if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
                    event.preventDefault(); open();
                    const buttons = [...panel.querySelectorAll('button:not(:disabled)')];
                    const index = buttons.indexOf(document.activeElement);
                    buttons[(index + (event.key === 'ArrowDown' ? 1 : buttons.length - 1)) % buttons.length]?.focus();
                }
            });
        });
        trigger.addEventListener('click', event => {
            event.stopPropagation();
            if (pinned) close(); else { pinned = true; open(); }
        });
        panel.addEventListener('click', event => {
            if (event.target.closest('button')) close();
        });
        document.addEventListener('pointerdown', event => {
            if (!panel.contains(event.target) && !trigger.contains(event.target)) close();
        });
        document.addEventListener('focusin', event => {
            if (!panel.contains(event.target) && !trigger.contains(event.target)) close();
        });
        window.addEventListener('resize', close);
        window.addEventListener('scroll', close, true);
        const surface = trigger.closest('#game-controls, #viewer-container');
        if (surface) new MutationObserver(() => {
            if (!trigger.getClientRects().length) close();
        }).observe(surface, { attributes: true, attributeFilter: ['style'] });
        // The outer Page menu must remain available while its submenu is used.
        panel.addEventListener('pointerenter', () => document.getElementById('viewer-controls')?.dispatchEvent(new PointerEvent('pointerenter', { pointerType: 'mouse' })));
    });
});
