/* Virtual controller input, layout persistence, and touch rendering. */

const virtualController = (() => {

    let container, canvas, ctx, editControls;

    let mode = 'play';
    let layouts = {};
    let buttons = {};
    let selectedButtonId = null;
    let isDraggingButton = false;
    let isDraggingSlider = false;
    const activeTouches = {};

   
    const defaultLayout = {
        portrait: {
            'left':      { label: '←', x: 0.12, y: 0.82, r: 0.08 },
            'right':     { label: '→', x: 0.32, y: 0.82, r: 0.08 },
            'softDrop':  { label: '↓', x: 0.22, y: 0.92, r: 0.07 },
            'hardDrop':  { label: '↑', x: 0.22, y: 0.72, r: 0.07 },
            'rotateCCW': { label: 'L',  x: 0.65, y: 0.85, r: 0.10 },
            'rotateCW':  { label: 'R',  x: 0.85, y: 0.85, r: 0.10 },
            'hold':      { label: 'H',  x: 0.75, y: 0.68, r: 0.09 },
        },
        landscape: {
            'left':      { label: '←', x: 0.10, y: 0.75, r: 0.08 },
            'right':     { label: '→', x: 0.25, y: 0.75, r: 0.08 },
            'softDrop':  { label: '↓', x: 0.175,y: 0.90, r: 0.07 },
            'hardDrop':  { label: '↑', x: 0.175,y: 0.60, r: 0.07 },
            'rotateCCW': { label: 'L',  x: 0.80, y: 0.85, r: 0.10 },
            'rotateCW':  { label: 'R',  x: 0.90, y: 0.65, r: 0.10 },
            'hold':      { label: 'H',  x: 0.75, y: 0.60, r: 0.09 },
        }
    };
    
    function init() {
        container = document.getElementById('virtual-controller-container');
        canvas = document.getElementById('virtualControllerCanvas');
        ctx = canvas.getContext('2d');
        editControls = document.getElementById('vc-edit-controls');

        loadLayouts();
        handleResize();

        window.addEventListener('resize', handleResize);
        window.addEventListener('orientationchange', handleResize);

        canvas.addEventListener('touchstart', handleStart, { passive: false });
        canvas.addEventListener('touchmove', handleMove, { passive: false });
        canvas.addEventListener('touchend', handleEnd, { passive: false });
        canvas.addEventListener('touchcancel', handleEnd, { passive: false });
        

        let isMouseDown = false;
        const convertMouseEvent = (e) => ({ changedTouches: [{ clientX: e.clientX, clientY: e.clientY, identifier: -1 }], preventDefault: () => {} });
        canvas.addEventListener('mousedown', (e) => { isMouseDown = true; handleStart(convertMouseEvent(e)); });
        canvas.addEventListener('mousemove', (e) => { if (isMouseDown) handleMove(convertMouseEvent(e)); });
        window.addEventListener('mouseup', (e) => { if (isMouseDown) { isMouseDown = false; handleEnd(convertMouseEvent(e)); } });
    }

    function show() { if (container) container.style.display = 'block'; handleResize(); }
    function hide() { if (container) container.style.display = 'none'; }

    function startEditMode() {
        show();
        mode = 'edit';
        editControls.style.display = 'flex';
        selectedButtonId = null;
        draw();
    }

    function endEditMode() {
        mode = 'play';
        editControls.style.display = 'none';
        saveLayouts();
        if (gameState !== 'PLAYING') {
            hide();
        }
        draw();
    }
    
    function handleResize() {
        if (!canvas) return;
        canvas.width = window.innerWidth;
        canvas.height = window.innerHeight;

        const orientation = (canvas.width > canvas.height) ? 'landscape' : 'portrait';
       
        if (!layouts[orientation] || Object.keys(layouts[orientation]).length === 0) {
            layouts[orientation] = JSON.parse(JSON.stringify(defaultLayout[orientation]));
        }
        
        buttons = layouts[orientation];
       
        for(const id in buttons) {
            if (buttons[id].isPressed === undefined) {
                buttons[id].isPressed = false;
            }
        }
        
        draw();
    }

    function draw() {
        if (!ctx) return;
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        const shortSide = Math.min(canvas.width, canvas.height);

        for (const id in buttons) {
            const btn = buttons[id];
            const x = btn.x * canvas.width;
            const y = btn.y * canvas.height;
            const r = btn.r * shortSide;

            ctx.beginPath();
            ctx.arc(x, y, r, 0, 2 * Math.PI);
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.7)';
            ctx.lineWidth = 3;
            if (btn.isPressed || (mode === 'edit' && id === selectedButtonId)) {
                ctx.fillStyle = 'rgba(255, 255, 255, 0.4)';
                ctx.fill();
            }
            ctx.stroke();

            ctx.fillStyle = 'white';
            ctx.font = `bold ${r * 0.7}px sans-serif`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(btn.label, x, y);
        }

        if (mode === 'edit' && selectedButtonId) {
            drawSizeSlider(shortSide);
        }
    }

    function drawSizeSlider(shortSide) {
        const centerX = canvas.width / 2;
        const y = canvas.height / 2;
        const sliderWidth = shortSide * 0.5;
        const minR = 0.04, maxR = 0.20;

        ctx.beginPath();
        ctx.moveTo(centerX - sliderWidth / 2, y);
        ctx.lineTo(centerX + sliderWidth / 2, y);
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.5)';
        ctx.lineWidth = 5;
        ctx.stroke();

        const btn = buttons[selectedButtonId];
        const ratio = (btn.r - minR) / (maxR - minR);
        const handleX = (centerX - sliderWidth / 2) + sliderWidth * ratio;
        
        ctx.beginPath();
        ctx.arc(handleX, y, shortSide * 0.03, 0, 2 * Math.PI);
        ctx.fillStyle = 'white';
        ctx.fill();
    }

    function handleStart(e) {
        e.preventDefault();
        const touches = e.changedTouches;
        const shortSide = Math.min(canvas.width, canvas.height);

        for (let i = 0; i < touches.length; i++) {
            const touch = touches[i];
            const touchX = touch.clientX, touchY = touch.clientY;

            if (mode === 'play') {
                const buttonId = getButtonAt(touchX, touchY, shortSide);
                if (buttonId) {
                    pressButton(buttonId);
                    activeTouches[touch.identifier] = buttonId;
                }
            } else {
                const sliderHit = checkSliderHit(touchX, touchY, shortSide);
                if (sliderHit) {
                    isDraggingSlider = true;
                    activeTouches[touch.identifier] = 'slider';
                    updateSlider(touchX, shortSide);
                    return;
                }
                const buttonId = getButtonAt(touchX, touchY, shortSide);
                if (buttonId) {
                    selectedButtonId = buttonId;
                    isDraggingButton = true;
                    activeTouches[touch.identifier] = buttonId;
                } else {
                    selectedButtonId = null;
                }
            }
        }
        draw();
    }

    function handleMove(e) {
        e.preventDefault();
        const touches = e.changedTouches;
        const shortSide = Math.min(canvas.width, canvas.height);

        for (let i = 0; i < touches.length; i++) {
            const touch = touches[i];
            const touchId = touch.identifier;
            const touchX = touch.clientX, touchY = touch.clientY;

            if (mode === 'play') {
                const prevButtonId = activeTouches[touchId];
                const currentButtonId = getButtonAt(touchX, touchY, shortSide);
                if (prevButtonId !== currentButtonId) {
                    if (prevButtonId) releaseButton(prevButtonId);
                    if (currentButtonId) pressButton(currentButtonId);
                    activeTouches[touchId] = currentButtonId;
                }
            } else {
                if (isDraggingSlider && activeTouches[touchId] === 'slider') {
                    updateSlider(touchX, shortSide);
                } else if (isDraggingButton && activeTouches[touchId] === selectedButtonId) {
                    const btn = buttons[selectedButtonId];
                    btn.x = touchX / canvas.width;
                    btn.y = touchY / canvas.height;
                }
            }
        }
        draw();
    }

    function handleEnd(e) {
        e.preventDefault();
        const touches = e.changedTouches;
        for (let i = 0; i < touches.length; i++) {
            const touchId = touches[i].identifier;
            if (mode === 'play') {
                const buttonId = activeTouches[touchId];
                if (buttonId) releaseButton(buttonId);
            } else {
                if (isDraggingButton) isDraggingButton = false;
                if (isDraggingSlider) isDraggingSlider = false;
            }
            delete activeTouches[touchId];
        }
        draw();
    }

    function getButtonAt(x, y, shortSide) {
     
        const buttonIds = Object.keys(buttons).reverse();
        for (const id of buttonIds) {
            const btn = buttons[id];
            const btnX = btn.x * canvas.width;
            const btnY = btn.y * canvas.height;
            const btnR = btn.r * shortSide;
            if (Math.sqrt((x - btnX) ** 2 + (y - btnY) ** 2) <= btnR) {
                return id;
            }
        }
        return null;
    }

    function pressButton(id) {
        const btn = buttons[id];
        if (btn && !btn.isPressed) {
            btn.isPressed = true;

            if (['hardDrop', 'rotateCW', 'rotateCCW', 'hold'].includes(id)) {
                if(players.length > 0) players[0].handlePress(id);
            }
        }
    }
    
    function releaseButton(id) {
        const btn = buttons[id];
        if (btn) btn.isPressed = false;
    }

    function checkSliderHit(x, y, shortSide) {
        if (!selectedButtonId) return false;
        const centerX = canvas.width / 2, sliderY = canvas.height / 2;
        const sliderWidth = shortSide * 0.5, handleRadius = shortSide * 0.03;
        const minR = 0.04, maxR = 0.20;
        const ratio = (buttons[selectedButtonId].r - minR) / (maxR - minR);
        const handleX = (centerX - sliderWidth / 2) + sliderWidth * ratio;
        return Math.sqrt((x - handleX) ** 2 + (y - sliderY) ** 2) <= handleRadius;
    }

    function updateSlider(touchX, shortSide) {
        if (!selectedButtonId) return;
        const centerX = canvas.width / 2, sliderWidth = shortSide * 0.5;
        const sliderStart = centerX - sliderWidth / 2;
        const clampedX = Math.max(sliderStart, Math.min(touchX, sliderStart + sliderWidth));
        const newRatio = (clampedX - sliderStart) / sliderWidth;
        const minR = 0.04, maxR = 0.20;
        buttons[selectedButtonId].r = minR + (maxR - minR) * newRatio;
    }

    function saveLayouts() {
        try {
            localStorage.setItem('tetrisVirtualPadLayouts', JSON.stringify(layouts));
        } catch (e) {
            console.error("Failed to save virtual pad layouts:", e);
        }
    }

    function loadLayouts() {
        try {
            const savedLayouts = JSON.parse(localStorage.getItem('tetrisVirtualPadLayouts'));
            if (savedLayouts && savedLayouts.portrait && savedLayouts.landscape) {
                layouts = savedLayouts;
            } else {
                layouts = JSON.parse(JSON.stringify(defaultLayout));
            }
        } catch (e) {
            console.error("Failed to load virtual pad layouts:", e);
            layouts = JSON.parse(JSON.stringify(defaultLayout));
        }
    }

    async function copyLayoutsToClipboard() {
        try {
            await navigator.clipboard.writeText(JSON.stringify(layouts, null, 2));
            alert('ボタン配置をクリップボードにコピーしました。');
        } catch (err) {
            alert('コピーに失敗しました。');
            console.error('Failed to copy layouts: ', err);
        }
    }

    async function importLayoutsFromClipboard() {
        if (!confirm('クリップボードからボタン配置を読み込みますか？現在の配置は上書きされます。')) return;
        try {
            const text = await navigator.clipboard.readText();
            const newLayouts = JSON.parse(text);
            if (newLayouts && newLayouts.portrait && newLayouts.landscape) {
                layouts = newLayouts;
                saveLayouts();
                handleResize();
                alert('ボタン配置をインポートしました。');
            } else {
                alert('無効なデータ形式です。');
            }
        } catch (err) {
            alert('インポートに失敗しました。');
            console.error('Failed to paste layouts: ', err);
        }
    }
    
    return {
        init,
        show,
        hide,
        startEditMode,
        endEditMode,
        isButtonPressed: (action) => buttons[action]?.isPressed || false,
        copyLayoutsToClipboard,
        importLayoutsFromClipboard
    };
})();

