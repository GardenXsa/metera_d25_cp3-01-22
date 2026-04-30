const { app, BrowserWindow, ipcMain, protocol } = require('electron');
const path = require('path');
const fs = require('fs');
const http = require('http');
const { spawn } = require('child_process');

const USER_DATA = app.getPath('userData');
const SAVES_DIR = path.join(USER_DATA, 'saves');
const SETTINGS_FILE = path.join(USER_DATA, 'settings.json');

if (!fs.existsSync(SAVES_DIR)) fs.mkdirSync(SAVES_DIR, { recursive: true });

const PORT = 30007; 

// ============================================================================
// NEXUS ENGINE PROCESS MANAGEMENT
// ============================================================================

let engineProcess = null;
let engineReady = false;
let commandQueue = [];
let currentResolve = null;

function getEnginePath() {
    const exeName = process.platform === 'win32' ? 'meterea_engine.exe' : 'meterea_engine';
    if (app.isPackaged) {
        return path.join(process.resourcesPath, 'engine', exeName);
    } else {
        return path.join(__dirname, 'engine', exeName);
    }
}

function startEngine() {
    return new Promise((resolve, reject) => {
        const enginePath = getEnginePath();
        console.log(`[Nexus] Запуск движка: ${enginePath}`);
        
        if (!fs.existsSync(enginePath)) {
            const errorMsg = `Движок не найден: ${enginePath}. Скомпилируйте его в папке engine/`;
            console.error(`[Nexus Error] ${errorMsg}`);
            resolve({ status: 'error', message: errorMsg });
            return;
        }

        engineProcess = spawn(enginePath, [], {
            stdio: ['pipe', 'pipe', 'pipe'],
            windowsHide: true
        });

        let stdoutBuffer = '';

        engineProcess.stdout.on('data', (data) => {
            stdoutBuffer += data.toString();
            
            // Обрабатываем полные JSON сообщения (разделенные \n)
            const lines = stdoutBuffer.split('\n');
            stdoutBuffer = lines.pop() || ''; // Оставляем неполную строку в буфере

            for (const line of lines) {
                if (!line.trim()) continue;
                try {
                    const response = JSON.parse(line);
                    
                    // Если это сообщение о прогрессе, пробрасываем в рендерер и НЕ резолвим промис
                    if (response.status === 'progress') {
                        const wins = BrowserWindow.getAllWindows();
                        if (wins.length > 0) {
                            wins[0].webContents.send('nexus-progress-update', response.message);
                        }
                        continue;
                    }

                    if (currentResolve) {
                        const resolve = currentResolve;
                        currentResolve = null;
                        resolve(response);
                        processQueue();
                    }
                } catch (e) {
                    console.error('[Nexus Parse Error]', e, line);
                }
            }
        });

        engineProcess.stderr.on('data', (data) => {
            console.error('[Nexus Stderr]', data.toString());
        });

        engineProcess.on('close', (code) => {
            console.log(`[Nexus] Движок завершен с кодом ${code}`);
            engineProcess = null;
            engineReady = false;
        });

        engineProcess.on('error', (err) => {
            console.error('[Nexus Error]', err);
            reject(err);
        });

        // Ждем немного чтобы процесс успел стартовать
        setTimeout(() => {
            engineReady = true;
            resolve({ status: 'ok', message: 'Engine started' });
        }, 500);
    });
}

function sendCommand(command, params = {}) {
    return new Promise((resolve, reject) => {
        if (!engineProcess || !engineReady) {
            resolve({ status: 'error', message: 'Engine not ready' });
            return;
        }

        const message = JSON.stringify({ command, ...params }) + '\n';
        commandQueue.push({ message, resolve, reject });
        processQueue();
    });
}

function processQueue() {
    if (commandQueue.length === 0 || currentResolve !== null) return;
    
    const cmd = commandQueue.shift();
    currentResolve = cmd.resolve;
    
    try {
        engineProcess.stdin.write(cmd.message);
    } catch (e) {
        currentResolve = null;
        cmd.reject(e);
        processQueue();
    }
}

async function initEngine() {
    if (!engineProcess) {
        await startEngine();
    }
    return await sendCommand('init');
}

async function buildWorld(playerId, era, initialAgents, globalLocations) {
    return await sendCommand('buildWorld', { player_id: playerId, era: era, initial_agents: initialAgents, global_locations: globalLocations });
}

async function simulateTicks(world, ticks) {
    return await sendCommand('simulateTicks', { world, ticks });
}

async function preSimulate(world, ticks) {
    return await sendCommand('preSimulate', { world, ticks });
}

async function syncState(world, items, containers) {
    return await sendCommand('syncState', { world, items, containers });
}

async function getFullState() {
    return await sendCommand('getFullState', {});
}

async function gmIntervention(commandObj) {
    return await sendCommand('gmIntervention', commandObj);
}

// ============================================================================
// IPC HANDLERS FOR NEXUS ENGINE
// ============================================================================

ipcMain.handle('nexus-init', async () => {
    return await initEngine();
});

ipcMain.handle('nexus-build-world', async (event, playerId, era, initialAgents, globalLocations) => {
    return await buildWorld(playerId, era, initialAgents, globalLocations);
});

ipcMain.handle('nexus-simulate', async (event, world, ticks) => {
    return await simulateTicks(world, ticks);
});

ipcMain.handle('nexus-presimulate', async (event, world, ticks) => {
    return await preSimulate(world, ticks);
});

ipcMain.handle('nexus-sync-state', async (event, world, items, containers) => {
    return await syncState(world, items, containers);
});

ipcMain.handle('nexus-get-full-state', async () => {
    return await getFullState();
});

ipcMain.handle('nexus-gm-intervention', async (event, commandObj) => {
    return await gmIntervention(commandObj);
});

// ============================================================================
// EXISTING CODE...
// ============================================================================ 

function isSafeFileName(filename) {
    return /^[a-zA-Z0-9_-]+\.json$/.test(filename);
}

const server = http.createServer((req, res) => {
    let urlPath = decodeURI(req.url.split('?')[0]);
    let filePath = path.join(__dirname, urlPath === '/' ? 'index.html' : urlPath);

    fs.readFile(filePath, (err, data) => {
        if (err) {
            res.statusCode = 404;
            res.end('Not Found');
            return;
        }
        const ext = path.extname(filePath).toLowerCase();
        const mimeTypes = {
            '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
            '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpg',
            '.jpeg': 'image/jpeg', '.mp3': 'audio/mpeg', '.wav': 'audio/wav'
        };
        res.setHeader('Content-Type', mimeTypes[ext] || 'application/octet-stream');
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.end(data);
    });
});

server.listen(PORT, '127.0.0.1', () => {
    console.log(`[SERVER] Static origin: http://127.0.0.1:${PORT}`);
});

function createWindow () {
  const win = new BrowserWindow({
    width: 1280, height: 800,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      webSecurity: false,
      preload: path.join(__dirname, 'preload.js')
    }
  });
  win.loadURL(`http://127.0.0.1:${PORT}`);
}

app.whenReady().then(createWindow);

ipcMain.handle('save-settings', async (event, data) => {
    try {
        fs.writeFileSync(SETTINGS_FILE, JSON.stringify(data, null, 2));
        return true;
    } catch (e) { return false; }
});

ipcMain.handle('load-settings', async () => {
    try {
        if (fs.existsSync(SETTINGS_FILE)) {
            return JSON.parse(fs.readFileSync(SETTINGS_FILE, 'utf-8'));
        }
    } catch (e) { return null; }
    return null;
});

ipcMain.handle('save-game', async (event, filename, data) => {
    if (!isSafeFileName(filename)) return { success: false };
    try {
        fs.writeFileSync(path.join(SAVES_DIR, filename), JSON.stringify(data, null, 2));
        return { success: true };
    } catch (error) { return { success: false }; }
});

ipcMain.handle('load-game', async (event, filename) => {
    if (!isSafeFileName(filename)) return null;
    try {
        const filePath = path.join(SAVES_DIR, filename);
        if (fs.existsSync(filePath)) return JSON.parse(fs.readFileSync(filePath, 'utf-8'));
    } catch (e) { return null; }
});

// --- НОВЫЕ ПОТОКОВЫЕ МЕТОДЫ (STREAMING) ---
ipcMain.handle('init-save-file', async (event, filename) => {
    if (!isSafeFileName(filename)) return false;
    try {
        await fs.promises.writeFile(path.join(SAVES_DIR, filename), '');
        return true;
    } catch (e) { return false; }
});

ipcMain.handle('append-save-line', async (event, filename, line) => {
    if (!isSafeFileName(filename)) return false;
    try {
        await fs.promises.appendFile(path.join(SAVES_DIR, filename), line);
        return true;
    } catch (e) { return false; }
});

ipcMain.handle('get-file-size', async (event, filename) => {
    if (!isSafeFileName(filename)) return 0;
    try {
        const stats = await fs.promises.stat(path.join(SAVES_DIR, filename));
        return stats.size;
    } catch (e) { return 0; }
});

ipcMain.handle('read-save-chunk', async (event, filename, position, size) => {
    if (!isSafeFileName(filename)) return "";
    try {
        const fd = await fs.promises.open(path.join(SAVES_DIR, filename), 'r');
        const buffer = Buffer.alloc(size);
        const { bytesRead } = await fd.read(buffer, 0, size, position);
        await fd.close();
        return buffer.toString('utf-8', 0, bytesRead);
    } catch (e) { return ""; }
});

ipcMain.handle('list-saves', async () => {
    try {
        const files = fs.readdirSync(SAVES_DIR).filter(f => f.endsWith('.json'));
        const results = [];
        for (const file of files) {
            try {
                const filePath = path.join(SAVES_DIR, file);
                const stats = fs.statSync(filePath);
                
                const fd = fs.openSync(filePath, 'r');
                const buffer = Buffer.alloc(1024);
                const bytesRead = fs.readSync(fd, buffer, 0, 1024, 0);
                fs.closeSync(fd);
                
                const chunk = buffer.toString('utf-8', 0, bytesRead);
                
                if (chunk.startsWith('{"block":"meta"')) {
                    const firstLine = chunk.split('\n')[0];
                    const meta = JSON.parse(firstLine).data;
                    results.push({ filename: file, timestamp: meta.timestamp, playerData: meta.playerData });
                } else {
                    const nameMatch = chunk.match(/"name"\s*:\s*"([^"]+)"/);
                    const levelMatch = chunk.match(/"level"\s*:\s*(\d+)/);
                    const tsMatch = chunk.match(/"timestamp"\s*:\s*"([^"]+)"/);
                    
                    results.push({ 
                        filename: file, 
                        timestamp: tsMatch ? tsMatch[1] : stats.mtime.toISOString(), 
                        playerData: {
                            name: nameMatch ? nameMatch[1] : "Герой",
                            stats: { level: levelMatch ? parseInt(levelMatch[1]) : "?" }
                        }
                    });
                }
            } catch (err) {
                console.error(`[Save] Ошибка чтения превью файла ${file}:`, err.message);
            }
        }
        return results;
    } catch (e) { 
        console.error("[Save] Ошибка чтения папки:", e);
        return []; 
    }
});

ipcMain.handle('delete-save', async (event, filename) => {
    if (!isSafeFileName(filename)) return false;
    try {
        const filePath = path.join(SAVES_DIR, filename);
        if (fs.existsSync(filePath)) {
            fs.unlinkSync(filePath);
            return true;
        }
    } catch (e) { return false; }
    return false;
});

ipcMain.handle('speak-text', async (event, text, voiceModel) => {
    const { spawn } = require('child_process');
    const path = require('path');
    const fs = require('fs');

    return new Promise((resolve) => {
        let ttsDir;
        if (app.isPackaged) {
            ttsDir = path.join(process.resourcesPath, 'app.asar.unpacked', 'assets', 'tts');
        } else {
            ttsDir = path.join(__dirname, 'assets', 'tts');
        }
        
        const piperExe = path.join(ttsDir, process.platform === 'win32' ? 'piper.exe' : 'piper');
        const modelPath = path.join(ttsDir, voiceModel);
        const outputPath = path.join(app.getPath('temp'), `tts_${Date.now()}.wav`);

        if (!fs.existsSync(piperExe)) {
            resolve({ success: false, error: `Piper не найден: ${piperExe}` });
            return;
        }
        
        if (!fs.existsSync(modelPath)) {
            resolve({ success: false, error: `Модель не найдена: ${modelPath}` });
            return;
        }

        let errorOutput = '';

        const env = {
            ...process.env,
            ESPEAK_DATA_PATH: ttsDir
        };
        if (process.platform === 'win32') {
            env.PATH = `${ttsDir};${process.env.PATH || ''}`;
        } else {
            env.LD_LIBRARY_PATH = `${ttsDir}:${process.env.LD_LIBRARY_PATH || ''}`;
        }

        const piper = spawn(piperExe, [
            '--model', modelPath, 
            '--output_file', outputPath
        ], { 
            cwd: ttsDir, 
            env: env,
            windowsHide: true 
        });

        piper.stderr.on('data', (data) => {
            errorOutput += data.toString();
        });

        piper.stdin.setDefaultEncoding('utf-8');
        piper.stdin.write(text);
        piper.stdin.end();

        piper.on('close', (code) => {
            if (code === 0) {
                resolve({ success: true, audioPath: `file://${outputPath}` });
            } else {
                console.error(`[Piper Error] Code: ${code}, Details: ${errorOutput}`);
                resolve({ success: false, error: `Piper exit code: ${code}. Details: ${errorOutput}` });
            }
        });
        
        piper.on('error', (err) => resolve({ success: false, error: err.message }));
    });
});

ipcMain.handle('gemini-request', async (event, model, apiKey, contents) => {
    const { net } = require('electron');
    return new Promise((resolve, reject) => {
        const requestBody = JSON.stringify({ 
            contents, 
            generationConfig: { maxOutputTokens: 8192, temperature: 0.8, topP: 0.95 }, 
            safetySettings: [ 
                { category: "HARM_CATEGORY_HARASSMENT", threshold: "BLOCK_NONE" }, 
                { category: "HARM_CATEGORY_HATE_SPEECH", threshold: "BLOCK_NONE" }, 
                { category: "HARM_CATEGORY_SEXUALLY_EXPLICIT", threshold: "BLOCK_NONE" }, 
                { category: "HARM_CATEGORY_DANGEROUS_CONTENT", threshold: "BLOCK_NONE" } 
            ] 
        });
        const request = net.request({
            method: 'POST', protocol: 'https:', hostname: 'generativelanguage.googleapis.com',
            path: `/v1beta/models/${model}:generateContent?key=${apiKey}`,
            headers: { 'Content-Type': 'application/json' }
        });
        request.on('response', (res) => {
            let body = '';
            res.on('data', chunk => body += chunk);
            res.on('end', () => resolve(JSON.parse(body)));
        });
        request.on('error', e => reject(e));
        request.write(requestBody);
        request.end();
    });
});