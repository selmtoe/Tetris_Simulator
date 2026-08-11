import { elements } from './state.js';

export function showToast(message) {
    elements.toast.textContent = message;
    elements.toast.style.opacity = 1;
    setTimeout(() => {
        elements.toast.style.opacity = 0;
    }, 2000);
}
