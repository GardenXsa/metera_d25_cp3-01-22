// --- Константы и Настройки ---
const DEBUG_MODE = true;

const SAVE_FILE_PREFIX = 'meterea_save_';
const SAVE_FILE_EXTENSION = '.json';
let GEMINI_API_KEY = '';
const MAX_HISTORY = 12; // Количество пар (пользователь + модель) в истории для Gemini
const MAX_MANUAL_SAVES = 5;
const MAX_AUTO_SAVES = 20;
const AUTOSAVE_INTERVAL = 5 * 60 * 1000; // 5 минут
const MEMORY_SUMMARY_TURN = 29; // Ход, на котором GM делает выжимку памяти
const MEMORY_PRUNE_TURN = 30; // Ход, на котором история для GM очищается
const INITIAL_STAT_POINTS = 10;
const POINTS_PER_LEVEL = 4;
const SAVE_STORAGE_KEY = 'textRpgSaves_v3';
const ECHO_MEMORY_MAX_ITEMS = 20;
const ECHO_MEMORY_MAX_LENGTH = 200;
const DEFAULT_WORLD_ID = 'world_metera';
const LANGUAGE_STORAGE_KEY = 'textRpgLang_v1';
const DEFAULT_LANGUAGE = 'ru';
const SOUND_FOLDER_PATH = "assets/sound/";

const musicFiles = [
    'menu_theme.mp3'
];
let musicVolume = localStorage.getItem('musicVolume') !== null ? parseFloat(localStorage.getItem('musicVolume')) : 0.2;
let sfxVolume = localStorage.getItem('sfxVolume') !== null ? parseFloat(localStorage.getItem('sfxVolume')) : 0.5;

const backgroundFiles = [
    'Backgrounds_pixel.jpg', '13.jpg', '12.jpg', '14.jpg', '15.jpg', 'background-209.webp', 'background-210.webp', 'background-big-slime.webp'
];
const BACKGROUND_CHANGE_INTERVAL = 3.5 * 60 * 1000; // 3.5 минуты

const BASE_CLASS_STATS = {
    warrior: { str: 13, dex: 10, int: 8, con: 12, cha: 9, res: 12 },
    mage: { str: 8, dex: 11, int: 13, con: 9, cha: 11, res: 8 },
    rogue: { str: 10, dex: 13, int: 10, con: 10, cha: 9, res: 10 },
    bard: { str: 9, dex: 12, int: 11, con: 9, cha: 12, res: 9 },
    default: { str: 10, dex: 10, int: 10, con: 10, cha: 10, res: 10 }
};

const RACE_MODIFIERS = {
    human: { str: 1, dex: 1, int: 1, con: 1, cha: 1 },
    elf: { str: 0, dex: 2, int: 1, con: 0, cha: 0 },
    dwarf: { str: 1, dex: 0, int: 0, con: 2, cha: 0 }
};

let predefinedStatusEffects = {
    // --- Негативные эффекты (Дебаффы) ---
    "minor_burn_dot": {
        name: "Слабое горение",
        description: "Вы чувствуете легкий жар. Наносит небольшой урон огнем каждый ход.",
        effectsJSON: `[{"trigger":{"type":"on_turn_start","interval":1},"action":{"type":"modify_stat","stat":"hp","change":-2}}]`
    },
    "weak_poison_dot": {
        name: "Слабый яд",
        description: "Яд медленно действует в ваших жилах. Наносит урон и ослабляет.",
        effectsJSON: `[
            {"trigger":{"type":"on_turn_start","interval":1},"action":{"type":"modify_stat","stat":"hp","change":-1}},
            {"trigger":{"type":"on_apply"},"action":{"type":"modify_stat","stat":"str","change":-1}},
            {"trigger":{"type":"on_remove"},"action":{"type":"modify_stat","stat":"str","change":1}}
        ]`
    },
    "curse_of_clumsiness": {
        name: "Проклятие неуклюжести",
        description: "Ваши движения стали неловкими (-2 к Ловкости).",
        effectsJSON: `[
            {"trigger":{"type":"on_apply"},"action":{"type":"modify_stat","stat":"dex","change":-2}},
            {"trigger":{"type":"on_remove"},"action":{"type":"modify_stat","stat":"dex","change":2}}
        ]`
    },

    // --- Позитивные эффекты (Баффы) ---
    "blessing_of_might": {
        name: "Благословение силы",
        description: "Вы чувствуете прилив сил (+2 к Силе).",
        // --- КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: Используем modify_stat, а не set_stat ---
        effectsJSON: `[
            {"trigger":{"type":"on_apply"},"action":{"type":"modify_stat","stat":"str","change":2}},
            {"trigger":{"type":"on_remove"},"action":{"type":"modify_stat","stat":"str","change":-2}}
        ]`
    },
    "blessing_of_luck": {
        name: "Благословение удачи",
        description: "Вы чувствуете прикосновение удачи (+2 к Ловкости).",
        effectsJSON: `[
            {"trigger":{"type":"on_apply"},"action":{"type":"modify_stat","stat":"dex","change":2}},
            {"trigger":{"type":"on_remove"},"action":{"type":"modify_stat","stat":"dex","change":-2}}
        ]`
    },
    "minor_regeneration": {
        name: "Слабая регенерация",
        description: "Ваши раны медленно затягиваются (+1 HP каждый ход).",
        effectsJSON: `[{"trigger":{"type":"on_turn_start","interval":1},"action":{"type":"modify_stat","stat":"hp","change":1}}]`
    }
};

const standardItemDescriptions = {
    'sword_short': () => t('itemDescriptions.shortSword', null, 'Простой, но надежный короткий меч. Базовое оружие ближнего боя.'),
    'shield_wooden': () => t('itemDescriptions.woodenShield', null, 'Круглый деревянный щит. Обеспечивает минимальную защиту.'),
    'potion_heal_small': () => t('itemDescriptions.smallHealthPotion', null, 'Маленькая склянка с красной жидкостью. Восстанавливает немного здоровья.'),
    'staff_simple': () => t('itemDescriptions.simpleStaff', null, 'Гладкий деревянный посох. Помогает фокусировать магическую энергию.'),
    'robe_novice': () => t('itemDescriptions.noviceRobe', null, 'Простая роба, которую носят начинающие маги. Практически не защищает.'),
    'mana_potion_small': () => t('itemDescriptions.smallManaPotion', null, 'Маленькая склянка с синей жидкостью. Восстанавливает немного маны.'),
    'dagger_basic': () => t('itemDescriptions.basicDagger', null, 'Обычный кинжал. Быстрое, но слабое оружие.'),
    'leather_armor_light': () => t('itemDescriptions.lightLeatherArmor', null, 'Легкий доспех из обработанной кожи. Дает небольшую защиту, не стесняя движений.'),
    'lockpicks': () => t('itemDescriptions.lockpicks', null, 'Набор тонких металлических инструментов для вскрытия замков.'),
    'lute_simple': () => t('itemDescriptions.simpleLute', null, 'Простая лютня. Инструмент для бардовских песен и заклинаний.'),
    'colorful_clothes': () => t('itemDescriptions.colorfulClothes', null, 'Яркая и удобная одежда, подходящая для выступлений.'),
    'gold': () => t('itemDescriptions.gold', null, 'Блестящие золотые монеты. Основная валюта.')
};

