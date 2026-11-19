// Installed in the static build. Native URLs still work in iframes and with
// ?standalone=1; shared viewer links enter the integrated viewer by default.
(() => {
    const parameters = new URLSearchParams(location.search);
    if (window.parent !== window || parameters.get('workspace') === '1' || parameters.has('standalone') || parameters.has('clean') || parameters.get('view') === 'editor' || parameters.has('league')) return;
    const role = document.currentScript.dataset.app;
    const url = new URL(role === 'viewer' ? '../hub/' : './hub/', location.href);
    url.search = location.search;
    url.searchParams.set('entry', role);
    url.hash = location.hash;
    location.replace(url);
})();
