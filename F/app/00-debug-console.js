
(function() {
    const consoleDiv = document.createElement('div');
    consoleDiv.id = 'debug-console';
    consoleDiv.innerHTML = `
        <div id="debug-console-header">
            <strong>DEBUG / ERROR LOG</strong>
            <div>
                <button onclick="document.getElementById('debug-console-content').innerHTML=''" style="padding:2px 5px;margin-right:5px;cursor:pointer;">Clear</button>
                <button onclick="document.getElementById('debug-console').style.display='none'" style="padding:2px 5px;cursor:pointer;">Close</button>
            </div>
        </div>
        <div id="debug-console-content"></div>
    `;
    document.body.appendChild(consoleDiv);

    const contentDiv = document.getElementById('debug-console-content');
    
    function showConsole() {
        consoleDiv.style.display = 'block';
    }

    function logToScreen(msg, type = 'info') {
        const entry = document.createElement('div');
        entry.className = `log-entry ${type}`;
        const time = new Date().toLocaleTimeString();
        entry.textContent = `[${time}] ${msg}`;
        contentDiv.appendChild(entry);
        consoleDiv.scrollTop = consoleDiv.scrollHeight;
        if (type === 'error') showConsole();
    }

        const originalError = console.error;
    console.error = function(...args) {
        // 特定のONNX警告を無視するフィルタを追加
        if (args.length > 0 && typeof args[0] === 'string' && 
           (args[0].includes('VerifyOutputSizes') || args[0].includes('Unknown CPU vendor'))) {
            return;
        }

        originalError.apply(console, args);
        try {
            const msg = args.map(a => {

                if (a instanceof Error) return `${a.message}\n${a.stack}`;
                if (typeof a === 'object') return JSON.stringify(a, null, 2);
                return String(a);
            }).join(' ');
            logToScreen(msg, 'error');
        } catch(e) { logToScreen('Error logging error: ' + e, 'error'); }
    };
    
    window.onerror = function(message, source, lineno, colno, error) {
        const stack = error ? error.stack : 'No stack trace';
        const msg = `Global Error: ${message}\nLocation: ${source}:${lineno}:${colno}\nStack: ${stack}`;
        logToScreen(msg, 'error');
        return false;
    };

    window.addEventListener('unhandledrejection', function(event) {
        logToScreen(`Unhandled Promise Rejection: ${event.reason}`, 'error');
    });
})();