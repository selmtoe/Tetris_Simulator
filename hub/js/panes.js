// Only presentation lives here: folding a pane never ends a practice session.
export function createPanes({getFlow, canPair, commit, changed, resized}) {
    const $ = id => document.getElementById(id);
    const divider = $('workspace-divider');
    const edge = $('pane-edge'), panes = [$('simulator-pane'), $('viewer-pane')];
    const reduced = matchMedia('(prefers-reduced-motion: reduce)');
    let ratio = 50, drag = null, timer, first = true, lastView, suppressEdgeClick = false;
    // The cue and the released position must always agree, for both handles.
    const destination = percent => percent <= 15 ? 'viewer' : percent >= 85 ? 'simulator' : 'both';
    function setRatio(value) {
        ratio = Math.min(84.99, Math.max(15.01, value));
        divider.setAttribute('aria-valuenow', String(Math.round(ratio)));
    }
    function size(percent, animate = false) {
        document.body.dataset.paneMotion = String(animate && !reduced.matches);
        panes[0].style.flexBasis = `${percent}%`;
        panes[1].style.flexBasis = `${100 - percent}%`;
        divider.style.left = `${percent}%`;
        document.body.style.setProperty('--reveal-visible', `${innerWidth * percent / 100}px`);
    }
    function render() {
        if (drag) return;
        clearTimeout(timer);
        const flow = getFlow();
        const paired = flow.mode === 'split';
        const view = paired ? flow.focusPane || 'both'
            : flow.mode === 'viewer' ? 'viewer' : 'simulator';
        const animate = !first && flow.mode !== 'playing' && (lastView !== view || document.body.dataset.dragSettling === 'true');
        delete document.body.dataset.dragSettling;
        document.body.dataset.paneView = view;
        document.body.dataset.paired = String(paired);
        panes.forEach(pane => { pane.hidden = false; });
        panes[0].inert = view === 'viewer'; panes[1].inert = view === 'simulator';
        void panes[0].offsetWidth;
        size(view === 'both' ? ratio : view === 'viewer' ? 0 : 100, animate);
        divider.hidden = view !== 'both';
        edge.hidden = view === 'both' || flow.mode === 'playing' || !canPair();
        edge.dataset.side = view === 'viewer' ? 'left' : 'right';
        edge.title = view === 'viewer' ? 'タップでシミュレータを小さく表示・ドラッグで幅を調整' : 'タップでビューワーを小さく表示・ドラッグで幅を調整';
        edge.setAttribute('aria-label', edge.title);
        function finish() {
            panes[0].hidden = view === 'viewer'; panes[1].hidden = view === 'simulator';
            resized();
        }
        if (animate && !reduced.matches) timer = setTimeout(finish, 260); else finish();
        first = false; lastView = view;
    }
    function begin(event, opening) {
        if (event.button !== 0 || drag) return;
        suppressEdgeClick = false;
        clearTimeout(timer);
        const view = document.body.dataset.paneView;
        drag = {id:event.pointerId, x:event.clientX, start:ratio, percent:ratio, moved:false, opening, view};
        event.currentTarget.setPointerCapture(event.pointerId);
        document.body.dataset.dragging = 'true';
        if (opening) {
            panes.forEach(pane => { pane.hidden = false; });
            document.body.dataset.revealing = view === 'viewer' ? 'simulator' : 'viewer';
            document.body.style.setProperty('--reveal-width', `${innerWidth * (view === 'viewer' ? ratio : 100 - ratio) / 100}px`);
        }
    }
    function progress(event) {
        if (!drag || drag.id !== event.pointerId) return;
        if (Math.abs(event.clientX - drag.x) > 5) drag.moved = true;
        if (!drag.moved) return;
        const rect = $('workspace-panes').getBoundingClientRect();
        drag.percent = Math.min(98, Math.max(2, (event.clientX - rect.left) / rect.width * 100));
        size(drag.percent);
        if (drag.opening) divider.hidden = false;
        const target = destination(drag.percent);
        // The named pane stays; CSS dims only the one about to be stowed.
        document.body.dataset.snapPane = target === 'both' ? '' : target;
        $('divider-hint').textContent = target === 'viewer' ? '離すとシミュレータを収納' : target === 'simulator' ? '離すとビューワーを収納' : '';
    }
    function end(event, cancelled = false) {
        if (!drag || event.pointerId !== drag.id) return;
        const completed = drag; drag = null;
        suppressEdgeClick = completed.opening && (cancelled || completed.moved);
        delete document.body.dataset.dragging; delete document.body.dataset.snapPane; delete document.body.dataset.revealing;
        document.body.dataset.dragSettling = 'true';
        $('divider-hint').textContent = '';
        if (cancelled || !completed.moved) { setRatio(completed.start); render(); return; }
        const target = destination(completed.percent);
        if (target === 'both') setRatio(completed.percent);
        commit(target);
    }
    for (const [element, opening] of [[divider,false],[edge,true]]) {
        element.addEventListener('contextmenu', event => event.preventDefault());
        element.addEventListener('selectstart', event => event.preventDefault());
        element.addEventListener('pointerdown', event => begin(event, opening));
        element.addEventListener('pointermove', progress);
        element.addEventListener('pointerup', event => end(event));
        element.addEventListener('pointercancel', event => end(event, true));
        element.addEventListener('lostpointercapture', event => end(event, true));
    }
    divider.addEventListener('keydown', event => {
        if (!['ArrowLeft','ArrowRight','Home'].includes(event.key)) return;
        event.preventDefault();
        if (event.ctrlKey && event.key !== 'Home') commit(event.key === 'ArrowLeft' ? 'simulator' : 'viewer');
        else { setRatio(event.key === 'Home' ? 50 : ratio + (event.key === 'ArrowLeft' ? -5 : 5)); render(); changed(); }
    });
    function revealMinimum() {
        if (edge.hidden || getFlow().mode === 'playing' || !canPair()) return;
        // Keep a visible strip above the 15% stow zone. A tap never needs an
        // edge swipe, which Android can reserve for system back navigation.
        setRatio(document.body.dataset.paneView === 'viewer' ? 20 : 80);
        commit('both');
    }
    edge.addEventListener('click', event => {
        if (suppressEdgeClick && event.detail !== 0) { suppressEdgeClick = false; return; }
        revealMinimum();
    });
    edge.addEventListener('keydown', event => {
        const view = document.body.dataset.paneView;
        // Enter/Space use the native button click; the inward arrow also opens.
        if (event.key !== (view === 'viewer' ? 'ArrowRight' : 'ArrowLeft')) return;
        event.preventDefault(); revealMinimum();
    });
    return {render, setRatio};
}
