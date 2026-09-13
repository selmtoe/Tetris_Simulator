// A game's return destination is fixed when it starts, independently of
// whether a replay or an older practice session happens to be open.
export function initialWorkflow() {
    return { mode: 'simulator', playOrigin: null, hasPractice: false, hasReplay: false, narrowPane: 'simulator' };
}

export function transition(state, event) {
    switch (event) {
        case 'practice':
            return { ...state, mode: 'split', focusPane: null, hasPractice: true, hasReplay: true, narrowPane: 'simulator' };
        case 'focus-simulator':
            return state.mode === 'split' ? { ...state, focusPane: 'simulator' } : state;
        case 'focus-viewer':
            return state.mode === 'split' ? { ...state, focusPane: 'viewer' } : state;
        case 'unfold':
            return { ...state, focusPane: null };
        case 'start':
            return { ...state, mode: 'playing', playOrigin: state.mode === 'split' ? 'practice' : 'simulator' };
        case 'return':
            return { ...state, mode: state.playOrigin === 'practice' ? 'split' : 'simulator', playOrigin: null, focusPane: null, narrowPane: 'simulator' };
        case 'replay':
            return { ...state, mode: 'viewer', hasReplay: true, playOrigin: null };
        case 'wide':
            return { ...state, mode: 'viewer' };
        case 'resume':
            return state.hasPractice ? { ...state, mode: 'split', narrowPane: 'simulator' } : state;
        case 'home':
            return { ...state, mode: 'simulator', hasPractice: false, playOrigin: null };
        case 'narrow-viewer':
            return { ...state, narrowPane: 'viewer' };
        case 'narrow-simulator':
            return { ...state, narrowPane: 'simulator' };
        default:
            return state;
    }
}
