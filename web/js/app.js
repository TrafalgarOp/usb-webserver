document.addEventListener('DOMContentLoaded', () => {
    // 1. Sidebar Navigation Routing
    const navButtons = document.querySelectorAll('.nav-btn');
    const views = document.querySelectorAll('.instrument-view');

    navButtons.forEach(btn => {
        btn.addEventListener('click', () => {
            const targetId = btn.getAttribute('data-target');

            navButtons.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');

            views.forEach(v => {
                v.classList.remove('active');
                if (v.id === targetId) {
                    v.classList.add('active');
                    // Resize canvas if switching to an instrument view
                    triggerCanvasResize(targetId);
                }
            });
        });
    });

    // 2. Initialize Logic Analyzer / Oscilloscope Canvases
    initWaveformCanvas('logicCanvas');
    initWaveformCanvas('oscopeCanvas');

    document.getElementById('gpio-mode').addEventListener('change', (e) => {
        const stateGroup = document.getElementById('state-group');
        stateGroup.style.display = e.target.value === 'input' ? 'none' : 'block';
    });

    document.getElementById('apply-gpio-btn').addEventListener('click', async () => {
        const port = document.getElementById('gpio-port').value;
        const pin = document.getElementById('gpio-pin').value;
        const mode = document.getElementById('gpio-mode').value;
        const pull = document.getElementById('gpio-pull').value;
        const state = document.getElementById('gpio-state').value;

        const statusEl = document.getElementById('gpio-status');
        statusEl.textContent = "TRANSMITTING CONFIG...";
        statusEl.style.color = "var(--accent-orange, #d29922)";

        try {
            await DeviceAPI.setGPIO({
                port,
                pin,
                mode,
                pull,
                state: mode === 'output' ? state : undefined
            });
            statusEl.textContent = `SUCCESS: Port ${port == 0 ? 'A' : 'B'} Pin ${pin} Updated [${mode.toUpperCase()}]`;
            statusEl.style.color = "var(--accent-green, #3fb950)";
        } catch (err) {
            statusEl.textContent = "ERROR: " + err.message;
            statusEl.style.color = "#f85149";
        }
    });
});

function initWaveformCanvas(canvasId) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;

    const ctx = canvas.getContext('2d');

    // Auto-fit canvas to parent container sizing
    function resize() {
        canvas.width = canvas.parentElement.clientWidth;
        canvas.height = canvas.parentElement.clientHeight;
        drawGrid(ctx, canvas.width, canvas.height);
    }

    window.addEventListener('resize', resize);
    resize();
}

function drawGrid(ctx, width, height) {
    ctx.fillStyle = '#0a0c0e';
    ctx.fillRect(0, 0, width, height);

    // Draw engineering grid lines
    ctx.strokeStyle = '#1f242d';
    ctx.lineWidth = 1;

    const gridSize = 40;
    for (let x = 0; x < width; x += gridSize) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, height);
        ctx.stroke();
    }
    for (let y = 0; y < height; y += gridSize) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
    }
}

function triggerCanvasResize(viewId) {
    if (viewId === 'logic-view') initWaveformCanvas('logicCanvas');
    if (viewId === 'oscope-view') initWaveformCanvas('oscopeCanvas');
}