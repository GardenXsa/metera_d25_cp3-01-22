// --- ЯДРО СИСТЕМЫ СОХРАНЕНИЙ (Логика, Таймеры, Сборка данных) ---

async function saveGame(slotType, slotId) {
    if (isWaitingForAI || !player) return false;
    showLoadingScreen('loadingScreen.saving', 'Подготовка к сохранению...');
    await yieldThread();

    try {
        const fileName = getSaveFileName(slotType, slotId);
        const isElectron = window.electronAPI && window.electronAPI.isElectron;
        let lines = [];
        let blockCount = 1;

        if (isElectron) await window.electronAPI.initSaveFile(fileName);

        const addBlock = async (name, id, data) => {
            updateLoadingText(`Сохраняем блок ${blockCount++}: ${name}...`);
            await yieldThread();
            const lineStr = JSON.stringify({ block: id, data: data }) + '\n';
            if (isElectron) {
                await window.electronAPI.appendSaveLine(fileName, lineStr);
            } else {
                lines.push(lineStr.trim());
            }
            await yieldThread();
        };

        if (isElectron && window.electronAPI.nexusGetFullState) {
            updateLoadingText('Синхронизация с ядром симуляции...');
            await yieldThread();
            const fullState = await window.electronAPI.nexusGetFullState();
            if (fullState && fullState.status === 'ok') {
                if (fullState.world) World = fullState.world;
                if (fullState.items) { ItemRegistry.clear(); fullState.items.forEach(([k, v]) => ItemRegistry.set(k, v)); }
                if (fullState.containers) { ContainerRegistry.clear(); fullState.containers.forEach(([k, v]) => ContainerRegistry.set(k, v)); }
            }
        }

        const metaData = {
            slotType, slotId, timestamp: new Date().toISOString(),
            playerData: { name: player.name, stats: { level: player.stats.level } }
        };
        
        await addBlock("Метаданные", "meta", metaData);
        await addBlock("Данные персонажа", "player", player);
        await addBlock("История диалогов", "history", conversationHistory);
        await addBlock("Реестр предметов", "item_registry", Array.from(ItemRegistry.entries()));
        await addBlock("Реестр контейнеров", "container_registry", Array.from(ContainerRegistry.entries()));

        if (typeof World !== 'undefined' && World) {
            await addBlock("Время и гомеостаз", "world_base", { time: World.time, homeostasis: World.homeostasis, lastDirectInjectionDay: World.lastDirectInjectionDay, needsGlobalEvent: World.needsGlobalEvent });
            await addBlock("Регионы мира", "world_regions", World.regions);
            await addBlock("Фракции", "world_factions", World.factions);
            await addBlock("Население (NPC)", "world_npcs", World.npcs);
            await addBlock("Правители и Интриги", "world_rulers", { rulers: World.rulers, intrigues: World.intrigues });
            await addBlock("Летопись и Погода", "world_misc", { news: World.news, weather: World.weather, animals: World.animals, gmInterventionHistory: World.gmInterventionHistory });
        }

        const allSaves = getAllSavesFromLocalStorage();
        let targetArray = allSaves[slotType] || [];
        const maxSlots = (slotType === 'manual') ? MAX_MANUAL_SAVES : MAX_AUTO_SAVES;
        const existingIdx = targetArray.findIndex(s => s.slotId === slotId);
        if (existingIdx !== -1) targetArray[existingIdx] = metaData;
        else {
            if (targetArray.length >= maxSlots && slotType === 'auto') {
                targetArray.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
                targetArray.shift();
            }
            targetArray.push(metaData);
        }
        allSaves[slotType] = targetArray;
        storeAllSavesToLocalStorage(allSaves);
        
        if (!isElectron) localStorage.setItem(`${SAVE_FILE_PREFIX}${slotType}_${slotId}_lines`, JSON.stringify(lines));

        hideLoadingScreen();
        return true;
    } catch (e) {
        console.error(e);
        hideLoadingScreen();
        return false;
    }
}

async function loadGame(slotType, slotId) {
    showLoadingScreen('loadingScreen.loading', 'Открытие потока данных...');
    await yieldThread();

    const fileName = getSaveFileName(slotType, slotId);
    const isElectron = window.electronAPI && window.electronAPI.isElectron;
    let rawPlayer = null, rawHistory = [], rawWorld = {};
    let loadedSuccessfully = false;

    try {
        if (isElectron) {
            const fileSize = await window.electronAPI.getFileSize(fileName);
            if (fileSize > 0) {
                const CHUNK_SIZE = 512 * 1024;
                let position = 0;
                let leftover = "";
                let blockCount = 1;

                let firstChunk = await window.electronAPI.readSaveChunk(fileName, 0, 1024);
                if (!firstChunk.startsWith('{"block":')) {
                    throw new Error("LEGACY_SAVE");
                }

                while (position < fileSize) {
                    const chunk = await window.electronAPI.readSaveChunk(fileName, position, CHUNK_SIZE);
                    position += CHUNK_SIZE;
                    
                    const lines = (leftover + chunk).split('\n');
                    leftover = lines.pop();

                    for (const line of lines) {
                        if (!line.trim()) continue;
                        
                        if (!line.startsWith('{"block":')) {
                            throw new Error("LEGACY_SAVE");
                        }

                        const parsed = JSON.parse(line);
                        updateLoadingText(`Читаем блок ${blockCount++}: ${parsed.block}...`);
                        await yieldThread();

                        switch(parsed.block) {
                            case 'player': rawPlayer = parsed.data; break;
                            case 'history': rawHistory = parsed.data; break;
                            case 'item_registry': ItemRegistry.clear(); parsed.data.forEach(([k, v]) => ItemRegistry.set(k, v)); break;
                            case 'container_registry': ContainerRegistry.clear(); parsed.data.forEach(([k, v]) => ContainerRegistry.set(k, v)); break;
                            case 'world_base': Object.assign(rawWorld, parsed.data); break;
                            case 'world_regions': rawWorld.regions = parsed.data; break;
                            case 'world_factions': rawWorld.factions = parsed.data; break;
                            case 'world_npcs': rawWorld.npcs = parsed.data; break;
                            case 'world_rulers': rawWorld.rulers = parsed.data.rulers; rawWorld.intrigues = parsed.data.intrigues; break;
                            case 'world_misc': Object.assign(rawWorld, parsed.data); break;
                        }
                    }
                }
                if (leftover.trim()) {
                    const parsed = JSON.parse(leftover);
                    switch(parsed.block) {
                        case 'player': rawPlayer = parsed.data; break;
                        case 'history': rawHistory = parsed.data; break;
                            case 'item_registry': ItemRegistry.clear(); parsed.data.forEach(([k, v]) => ItemRegistry.set(k, v)); break;
                            case 'container_registry': ContainerRegistry.clear(); parsed.data.forEach(([k, v]) => ContainerRegistry.set(k, v)); break;
                        case 'world_base': Object.assign(rawWorld, parsed.data); break;
                        case 'world_regions': rawWorld.regions = parsed.data; break;
                        case 'world_factions': rawWorld.factions = parsed.data; break;
                        case 'world_npcs': rawWorld.npcs = parsed.data; break;
                        case 'world_rulers': rawWorld.rulers = parsed.data.rulers; rawWorld.intrigues = parsed.data.intrigues; break;
                        case 'world_misc': Object.assign(rawWorld, parsed.data); break;
                    }
                }
                loadedSuccessfully = true;
            }
        } else {
            const lsData = localStorage.getItem(`${SAVE_FILE_PREFIX}${slotType}_${slotId}_lines`);
            if (lsData) {
                const lines = JSON.parse(lsData);
                let blockCount = 1;
                for (const line of lines) {
                    const parsed = JSON.parse(line);
                    updateLoadingText(`Читаем блок ${blockCount++}: ${parsed.block}...`);
                    await yieldThread();
                    switch(parsed.block) {
                        case 'player': rawPlayer = parsed.data; break;
                        case 'history': rawHistory = parsed.data; break;
                            case 'item_registry': ItemRegistry.clear(); parsed.data.forEach(([k, v]) => ItemRegistry.set(k, v)); break;
                            case 'container_registry': ContainerRegistry.clear(); parsed.data.forEach(([k, v]) => ContainerRegistry.set(k, v)); break;
                        case 'world_base': Object.assign(rawWorld, parsed.data); break;
                        case 'world_regions': rawWorld.regions = parsed.data; break;
                        case 'world_factions': rawWorld.factions = parsed.data; break;
                        case 'world_npcs': rawWorld.npcs = parsed.data; break;
                        case 'world_rulers': rawWorld.rulers = parsed.data.rulers; rawWorld.intrigues = parsed.data.intrigues; break;
                        case 'world_misc': Object.assign(rawWorld, parsed.data); break;
                    }
                }
                loadedSuccessfully = true;
            } else {
                throw new Error("LEGACY_SAVE");
            }
        }
    } catch (e) {
        if (e.message === "LEGACY_SAVE") {
            updateLoadingText('Конвертация старого сохранения...');
            await yieldThread();
            let oldData;
            if (isElectron) {
                oldData = await window.electronAPI.loadGame(fileName);
            } else {
                const allSaves = getAllSavesFromLocalStorage();
                const targetArray = allSaves[slotType];
                oldData = targetArray.find(save => save.slotId === slotId);
            }
            
            if (oldData) {
                rawPlayer = oldData.playerData || oldData.player;
                rawHistory = oldData.historyData || oldData.history;
                rawWorld = oldData.worldData || oldData.world;
                loadedSuccessfully = true;
            }
        } else {
            console.error(e);
        }
    }

    if (!loadedSuccessfully || !rawPlayer) {
        hideLoadingScreen();
        alert(t('loadGame.errorSaveNotFound', { slotId: slotId }));
        return;
    }

    try {
        player = structuredClone(rawPlayer);

        // T3 Migration
        if (!player.container_backpack) {
            player.container_backpack = CoreInventorySystem.createContainer("player_backpack", "player", 100, 30);
            for (let key in player.inventory) {
                let oldItem = player.inventory[key];
                CoreInventorySystem.createItem(oldItem.aiIdentifier || oldItem.id, oldItem.quantity || 1, player.container_backpack, oldItem);
            }
        }
        if (!player.container_equipment) {
            player.container_equipment = CoreInventorySystem.createContainer("player_equipment", "player", 50, 10);
            for (let slot in player.equipment) {
                let oldItem = player.equipment[slot];
                if (oldItem) {
                    let newItemId = CoreInventorySystem.createItem(oldItem.aiIdentifier || oldItem.id, oldItem.quantity || 1, player.container_equipment, oldItem);
                    ItemRegistry.get(newItemId).slot_index = slot;
                }
            }
        }

        // Устранение утечки памяти (T3)
        delete player.inventory;
        delete player.equipment;

        updateLoadingText('Инициализация симуляции мира...');
        await yieldThread();
        if (rawWorld && Object.keys(rawWorld).length > 0) { 
            World = structuredClone(rawWorld); 
            worldWorker.postMessage({ action: 'init', ECONOMY_ITEMS, CRAFTING_RECIPES, FACILITY_NAMES });
            if (window.electronAPI && window.electronAPI.nexusSyncState) {
                await window.electronAPI.nexusSyncState(World, Array.from(ItemRegistry.entries()), Array.from(ContainerRegistry.entries()));
            }
        } else { 
            World = await initWorldSimulator(); 
        }

        updateLoadingText('Чтение лора и истории...');
        await yieldThread();
        await loadActiveEraLore(player.era);
        await loadGlobalLocations(DEFAULT_WORLD_ID, currentLanguage, player.era);
        conversationHistory = structuredClone(rawHistory || []);

        updateLoadingText('Восстановление интерфейса...');
        await yieldThread();

        player.stats = player.stats || {};
        player.inventory = player.inventory || {};
        player.equipment = player.equipment || {};
        player.holdings = player.holdings || {};
        player.bankAccount = player.bankAccount || { deposit: 0, loan: 0, loanDays: 0 };
        player.quests = player.quests || {};
        player.skills = player.skills || {};
        player.mapMarkers = player.mapMarkers || {};
        player.subLocations = player.subLocations || {};
        player.statusEffects = player.statusEffects || {};
        player.visibleEntities = player.visibleEntities || {};
        player.allKnownEntities = player.allKnownEntities || {};
        player.visitedLocations = player.visitedLocations || [];
        player.localMap = player.localMap || null;
        player.gameLogHistory = player.gameLogHistory || [];
        player.calcLogHistory = player.calcLogHistory || [];
        player.gmErrors = player.gmErrors || [];
        player.echoMemory = player.echoMemory || { items: [], maxItems: ECHO_MEMORY_MAX_ITEMS, version: 1 };
        player.gmNotes = player.gmNotes || {};
        player.memoryArchives = player.memoryArchives || {};
        player.archiveSummaries = player.archiveSummaries || {};
        player.factionData = player.factionData || { global: t('factions.global', null, 'Общая') };
        player.nexusData = player.nexusData || {};
        syncPlayerContainerBindings();
        syncPlayerGoldFromInventory();

        if (typeof player.stats.reputation === 'number' || player.stats.reputation === undefined) {
            player.stats.reputation = { global: player.stats.reputation || 0 };
        }

        currentSaveSlot = { type: slotType, id: slotId };
        isMapInitialized = false;

        stopMenuMusic();
        initializeGameInterface();
        setActiveScreen('game-interface');
        displaySavedChatHistory();

        addLogMessage(t('gameInterface.log.gameLoaded', { slotType: slotType, slotId: slotId }), "system-message");
        hideLoadingScreen();
    } catch (e) {
        console.error(`Ошибка загрузки:`, e);
        hideLoadingScreen();
        alert(t('loadGame.errorLoad', { slotId: slotId, message: e.message }));
        setActiveScreen('main-menu');
    }
}

async function autoSaveGame() {
    if (!player || !gameInterface.classList.contains('active-screen') || isWaitingForAI || IS_PRE_SIMULATING || tempPlayer !== null) return;

    let nextAutoSaveId = 1;
    if (window.electronAPI && window.electronAPI.isElectron) {
        const autoSaves = (await listSaveFilesFromFSA()).filter(s => s.slotType === 'auto');
        if (autoSaves.length > 0) {
            if (autoSaves.length < MAX_AUTO_SAVES) {
                const usedIds = new Set(autoSaves.map(s => s.slotId));
                for (let i = 1; i <= MAX_AUTO_SAVES + 1; i++) { if (!usedIds.has(i)) { nextAutoSaveId = i; break; } }
            } else {
                autoSaves.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
                nextAutoSaveId = autoSaves[0].slotId;
            }
        }
    } else {
        const allSavesForAuto = getAllSavesFromLocalStorage();
        const autoSaves = allSavesForAuto.auto || [];
        if (autoSaves.length > 0) {
            if (autoSaves.length < MAX_AUTO_SAVES) {
                const usedIds = new Set(autoSaves.map(s => s.slotId));
                for (let i = 1; i <= MAX_AUTO_SAVES + 1; i++) { if (!usedIds.has(i)) { nextAutoSaveId = i; break; } }
            } else {
                autoSaves.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
                nextAutoSaveId = autoSaves[0].slotId;
            }
        }
    }

    console.log(`Попытка автосохранения в слот ${nextAutoSaveId}...`);
    const savedSuccessfully = await saveGame('auto', nextAutoSaveId);

    if (savedSuccessfully) {
        const finalSource = (window.electronAPI && window.electronAPI.isElectron) ? t('loadGame.sourceFileSystem') : t('loadGame.sourceLocalStorage');
        addLogMessage(t('gameInterface.log.autoSaveSuccess', { slotId: nextAutoSaveId, source: finalSource }), "command-feedback");
    } else {
        addLogMessage(t('gameInterface.log.autoSaveFailed', { slotId: nextAutoSaveId }), "system-message");
    }
}

function startAutoSaveTimer() {
    stopAutoSaveTimer();
    if (autoSaveIntervalMs > 0) {
        autoSaveTimer = setInterval(autoSaveGame, autoSaveIntervalMs);
        console.log(`Таймер автосохранения запущен с интервалом ${autoSaveIntervalMs / 1000}с`);
    } else {
        console.log("Автосохранение отключено в настройках.");
    }
}

function stopAutoSaveTimer() {
    if (autoSaveTimer) {
        clearInterval(autoSaveTimer);
        autoSaveTimer = null;
        console.log("Таймер автосохранения остановлен.");
    }
}
