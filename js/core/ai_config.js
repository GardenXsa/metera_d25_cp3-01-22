// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ НАСТРОЕК AI ---
let currentApiProvider = 'gemini'; // 'gemini', 'llmost', 'openrouter', 'local'
let usePromptCaching = true;
let useThinkingMode = false;
let thinkingBudget = 2048;
let reasoningEffort = 'medium';
let allowNSFW = false;


// Gemini
let geminiApiKeys = [];
let geminiApiKey = ''; // Единый активный ключ
let currentGeminiKeyIndex = 0;
let geminiModelId = 'gemini-1.5-flash-latest';

// user-input
let lastUserPrompt = "";

// LLMost
let llmostApiKey = '';
let llmostModelId = 'openai/gpt-4';

// OpenRouter
let openrouterApiKey = '';
let openrouterModelId = 'anthropic/claude-3-haiku';
let deepseekApiKey = '';
let deepseekModelId = 'deepseek-chat';

// Local (LM Studio)
let localApiUrl = 'http://localhost:1234/v1/chat/completions';
let localModelId = 'local-model';

// Image Generation Settings
let imgApiProvider = 'pollinations';
let imgApiKey = '';
let imgModelId = 'dall-e-3';
let enableImageGeneration = true;

let enableDeepSetup = false;
const enableWorldSim = true; // Ядро игры, всегда включено
let isSimulatingWorld = false;
let IS_PRE_SIMULATING = false;


let lowSpecMode = localStorage.getItem('lowSpecMode') === 'true';

// --- AI ИГРОК (АВТО-ТЕСТЕР) ---
let aiPlayerEnabled = false;
let aiPlayerProvider = 'openrouter';
let aiPlayerModelId = 'google/gemma-2-9b-it:free';
let aiPlayerApiKey = '';
let aiPlayerLocalUrl = 'http://localhost:1234/v1/chat/completions';
let isAutoTesting = false;
let aiPlayerTurnLimit = 20;
let aiPlayerCurrentTurns = 0;
