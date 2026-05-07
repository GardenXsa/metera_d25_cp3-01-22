// --- Элементы DOM ---



// НОВЫЕ ЭЛЕМЕНТЫ ДЛЯ СИСТЕМЫ НЕКСУС
const traitsList = document.getElementById('traits-list');
const holdingsList = document.getElementById('holdings-list');

const fsaSelectDirectoryButton = document.getElementById('fsa-select-directory-button');
const fsaStatusElement = document.getElementById('fsa-status');

// Экран Выбора Рассказчика
const narratorSelectionScreen = document.getElementById('narrator-selection-screen');
const narratorCard = document.getElementById('narrator-card');
const narratorName = document.getElementById('narrator-name');
const narratorDesc = document.getElementById('narrator-desc');
const narratorPrevButton = document.getElementById('narrator-prev');
const narratorNextButton = document.getElementById('narrator-next');
const confirmNarratorButton = document.getElementById('confirm-narrator-button');
const worldSetupScreen = document.getElementById('world-setup-screen');
const worldYearsSlider = document.getElementById('world-years-slider');
const worldYearsValue = document.getElementById('world-years-value');
const worldAgentsSlider = document.getElementById('world-agents-slider');
const worldAgentsValue = document.getElementById('world-agents-value');
const confirmWorldSetupButton = document.getElementById('confirm-world-setup-button');

// Элементы Загрузки (НОВОЕ)
const loadingOverlay = document.getElementById('loading-overlay');
const loadingText = document.getElementById('loading-text');

// Меню и Экраны
const ttsVoiceSelectorGroup = document.getElementById('tts-voice-selector-group');
const ttsVoiceSelect = document.getElementById('tts-voice-select');
const calculationLog = document.getElementById('calculation-log');
const gmNotesPanel = document.getElementById('gm-notes-panel');
const gmNotesContent = document.getElementById('gm-notes-content');
const screens = document.querySelectorAll('.menu-screen');
const mainMenu = document.getElementById('main-menu');
const settingsMenu = document.getElementById('settings-menu');
const characterCreationScreen = document.getElementById('character-creation-screen');
const loadGameScreen = document.getElementById('load-game-screen');
const helpScreen = document.getElementById('help-screen');
const gameInterface = document.getElementById('game-interface');
const apiKeyStatus = document.getElementById('api-key-status');
const backgroundContainer = document.getElementById('background-container');
const languageSelect = document.getElementById('language-select');

// Кнопки Главного Меню
const newGameButton = document.getElementById('new-game-button');
const loadGameButton = document.getElementById('load-game-button');
const mainSettingsButton = document.getElementById('main-settings-button');
const helpButton = document.getElementById('help-button');
const communityButton = document.getElementById('community-button'); // <-- ДОБАВЛЕНО
const useUserKeyButton = document.getElementById('use-user-key-button');
const useBuiltInKeyButton = document.getElementById('use-builtin-key-button');

// Настройки
const apiKeyInput = document.getElementById('api-key-input');
const saveSettingsButton = document.getElementById('save-settings-button');

// Создание Персонажа
const charNameInput = document.getElementById('char-name-input');
const charRaceSelect = document.getElementById('char-race-select');
const charClassSelect = document.getElementById('char-class-select');
const charEraSelect = document.getElementById('char-era-select');
const charStartModeSelect = document.getElementById('char-start-mode-select');
const eraDescriptionBox = document.getElementById('era-description-box'); // <-- ДОБАВЛЕНО
const charDescInput = document.getElementById('char-desc-input');
const statDistributionSection = document.getElementById('stat-distribution');
const creationStatPointsDisplay = document.getElementById('stat-points-available');
const statButtons = document.querySelectorAll('.stat-button');
const createStatDisplays = {
    str: document.getElementById('create-stat-str'),
    dex: document.getElementById('create-stat-dex'),
    int: document.getElementById('create-stat-int'),
    con: document.getElementById('create-stat-con'),
    cha: document.getElementById('create-stat-cha'),
};
const startGameButton = document.getElementById('start-game-button');
const creationError = document.getElementById('creation-error');

// Загрузка Игры
let lastUserMessageForRetry = null;
const manualSaveSlotsList = document.getElementById('manual-save-slots');
const autoSaveSlotsList = document.getElementById('auto-save-slots');
const maxManualSavesDisplay = document.getElementById('max-manual-saves');
const maxAutoSavesDisplay = document.getElementById('max-auto-saves');

// Кнопки Назад
const backButtons = document.querySelectorAll('.back-button');

// Игровой Интерфейс
const gameLog = document.getElementById('game-log');
const userInput = document.getElementById('user-input');
const sendButton = document.getElementById('send-button');
const voiceInputButton = document.getElementById('voice-input-button'); // <--- НОВЫЙ ЭЛЕМЕНТ
const gameTitle = document.getElementById('game-title');
const inGameMenuButton = document.getElementById('in-game-menu-button');
const collapsiblePanels = document.querySelectorAll('.collapsible-panel');

// Элементы для отображения состояния игрока
const levelInfoDiv = document.getElementById('level-info');
const characterSheetPanel = document.querySelector('.character-sheet');
const charNameDisplay = document.getElementById('character-name');
const charRaceDisplay = document.getElementById('character-race');
const charClassDisplay = document.getElementById('character-class');
const levelDisplay = document.getElementById('stat-level');
const xpDisplay = document.getElementById('stat-xp');
const xpNextDisplay = document.getElementById('stat-xp-next');
const inGameStatPointsDisplay = document.getElementById('stat-points-available-display');
const turnDisplay = document.getElementById('stat-turn');
const hpDisplay = document.getElementById('stat-hp');
const maxHpDisplay = document.getElementById('stat-max-hp');
const manaDisplay = document.getElementById('stat-mana');
const maxManaDisplay = document.getElementById('stat-max-mana');
const strDisplay = document.getElementById('stat-str');
const dexDisplay = document.getElementById('stat-dex');
const intDisplay = document.getElementById('stat-int');
const conDisplay = document.getElementById('stat-con');
const chaDisplay = document.getElementById('stat-cha');
const goldDisplay = document.getElementById('stat-gold');
const reputationMarker = document.getElementById('reputation-marker');
const reputationValueTextDisplay = document.getElementById('stat-reputation-value-text');
const locationDisplay = document.getElementById('stat-location');
const locationStatLine = document.getElementById('location-stat-line');
const journeyContainer = document.getElementById('journey-container');
const journeyDest = document.getElementById('journey-dest');
const journeyProgressText = document.getElementById('journey-progress-text');
const journeyProgressBar = document.getElementById('journey-progress-bar');
const journeyLoading = document.getElementById('journey-loading');
const journeyEventArea = document.getElementById('journey-event-area');
const journeyEventText = document.getElementById('journey-event-text');
const journeyEventActions = document.getElementById('journey-event-actions');
const travelControls = document.getElementById('travel-controls');
const travelFastForwardBtn = document.getElementById('travel-fast-forward-btn');
const travelPauseBtn = document.getElementById('travel-pause-btn');
const travelCancelBtn = document.getElementById('travel-cancel-btn');
const journeyContinueBtn = document.getElementById('journey-continue-btn');
const inventoryList = document.getElementById('inventory-list');
const inventoryCount = document.getElementById('inventory-count');
const inventoryCapacity = document.getElementById('inventory-capacity');
const questList = document.getElementById('quest-list');
const skillsList = document.getElementById('skills-list');
const statusEffectsList = document.getElementById('status-effects-list'); // НОВОЕ
const statIncreaseButtons = document.querySelectorAll('.stat-increase-button');
const reputationDisplayWrapper = document.querySelector('.reputation-display-wrapper');
const reputationModal = document.getElementById('reputation-modal');

// Элементы Карты
const globalLocationsList = document.getElementById('global-locations-list');
const customLocationsList = document.getElementById('custom-locations-list');

// Элементы окна ошибки ИИ
const aiErrorModal = document.getElementById('ai-error-modal');
const aiErrorMessage = document.getElementById('ai-error-message');
const aiErrorRetryBtn = document.getElementById('ai-error-retry-btn');
const aiErrorCancelBtn = document.getElementById('ai-error-cancel-btn');
const aiErrorDetailsToggle = document.getElementById('ai-error-details-toggle');
const aiErrorDetailsContent = document.getElementById('ai-error-details-content');

// Элементы Панели Окружения (НОВОЕ)
const environmentList = document.getElementById('environment-list');
let entityTooltip = null; // Будет создан динамически

// Элементы Игрового Меню
const menuOverlay = document.getElementById('menu-overlay');
const inGameMenu = document.getElementById('in-game-menu');
const inGameSaveButton = document.getElementById('in-game-save-button');
const inGameSettingsButton = document.getElementById('in-game-settings-button'); // <-- [НОВЫЙ ЭЛЕМЕНТ]
const inGameExitButton = document.getElementById('in-game-exit-button');
const closeInGameMenuButton = document.getElementById('close-in-game-menu-button');
const settingsBackButton = document.getElementById('settings-back-button'); // <-- [НОВЫЙ ЭЛЕМЕНТ]

// Музыка
const audioPlayer = document.getElementById('background-music-player');
const toggleMusicButton = document.getElementById('toggle-music-button');
const toggleMusicIcon = toggleMusicButton?.querySelector('i');

// TTS (Text-to-Speech)
const TTS_VOICE_STORAGE_KEY = 'textRpgTTSVoice_v1';
const toggleTTSButton = document.getElementById('toggle-tts-button');
const toggleTTSIcon = toggleTTSButton?.querySelector('i');

