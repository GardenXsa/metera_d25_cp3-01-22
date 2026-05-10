// --- Глобальные переменные ---
let settingsReturnScreen = 'main-menu'; // Экран для возврата из настроек

let recognition;
let isRecognizing = false;

let narrators = [];
let currentNarratorIndex = 0;
let tempPlayer = null; // Для временного хранения персонажа
let preloadedWorldData = null;
let directoryHandle = null;
let lastFSAErrorTime = 0; // Для предотвращения спама alert'ами
const FSA_ERROR_COOLDOWN = 10000; // 10 секунд



let itemsReferenceData = null; // Will store the array of item objects
let gmFeedbackMessages = [];
let playerActionQueue = []; // <-- [НОВАЯ ПЕРЕМЕННАЯ] Будет хранить действия игрока
let turnRollMemory = {}; // Античит: запоминает броски в текущем ходу
let nextInternalItemId = 1;
let itemTooltipElement = null; // Для кастомных DnD тултипов
let nextInternalEntityId = 1;
let nextInternalSkillId = 1;
let nextInternalMapMarkerId = 1;
let draggedItemData = null;
let combatSystemRulesData = "Загрузка правил...";
let autoTesterPromptTemplate = "";
const BUILT_IN_KEY_STORAGE_FLAG = 'useBuiltInApiKey_v1';



// Проверяем, запущены ли мы в Electron
const hasElectronAPI = window.electronAPI && window.electronAPI.isElectron;

// Определяем доступность файловой системы (Electron ИЛИ Браузер)
let fsaApiAvailable = hasElectronAPI || (typeof window.showDirectoryPicker === 'function');

if (hasElectronAPI) {
    console.log("Режим Electron обнаружен. Используем нативные сохранения.");
}

// --- Утилиты для асинхронных сохранений (Streaming) ---
let autoSaveIntervalMs = parseInt(localStorage.getItem('autoSaveInterval')) || 300000;
const yieldThread = () => new Promise(resolve => setTimeout(resolve, 50));

function updateLoadingText(text) {
    const loadingTextEl = document.getElementById('loading-text');
    if (loadingTextEl) loadingTextEl.textContent = text;
}

// --- T3: CORE REGISTRIES ---
const ItemRegistry = new Map();
const ContainerRegistry = new Map();

function generateUUID() {
    if (typeof crypto !== 'undefined' && crypto.randomUUID) {
        return crypto.randomUUID();
    }
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
        var r = Math.random() * 16 | 0, v = c == 'x' ? r : (r & 0x3 | 0x8);
        return v.toString(16);
    });
}
