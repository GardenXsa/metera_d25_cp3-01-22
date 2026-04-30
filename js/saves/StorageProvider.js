// --- НИЗКОУРОВНЕВАЯ РАБОТА С ХРАНИЛИЩЕМ (Electron / LocalStorage / FSA) ---

async function requestDirectoryPermission() {
    if (!fsaApiAvailable) return null;
    try {
        const handle = await window.showDirectoryPicker({ mode: 'readwrite' });
        await saveDirectoryHandleToDB(handle); // Сохраняем полученный хэндл в IndexedDB
        directoryHandle = handle;
        console.log("Доступ к директории получен и сохранен:", directoryHandle.name);
        updateFSAStatus(directoryHandle, 'granted_new_selection');
        return directoryHandle;
    } catch (error) {
        if (error.name === 'AbortError') {
            console.log("Пользователь отменил выбор директории.");
        } else {
            console.error("Ошибка при запросе доступа к директории:", error);
        }
        updateFSAStatus(directoryHandle, 'selection_cancelled');
        return null;
    }
}

async function verifyDirectoryHandlePermission(handle) {
    if (!handle || typeof handle.queryPermission !== 'function') {
        console.error("Попытка проверить невалидный хэндл.");
        return false;
    }

    // Сначала тихо проверяем, есть ли у нас уже разрешение.
    if (await handle.queryPermission({ mode: 'readwrite' }) === 'granted') {
        return true;
    }

    // Если разрешения нет, запрашиваем его. Это вызовет всплывающее окно.
    if (await handle.requestPermission({ mode: 'readwrite' }) === 'granted') {
        return true;
    }

    // Пользователь отказал в доступе.
    return false;
}

async function getFileHandleFromDir(dirHandle, fileName, options = {}) {
    if (!dirHandle) {
        console.warn(`getFileHandleFromDir: dirHandle отсутствует для файла "${fileName}".`);
        return null;
    }
    try {
        const permission = await dirHandle.queryPermission({ mode: 'readwrite' });
        if (permission !== 'granted') {
            console.warn(`getFileHandleFromDir: Нет разрешения 'granted' для директории при попытке получить handle для ${fileName}. Статус: ${permission}. Попытка запроса...`);
            if (await dirHandle.requestPermission({ mode: 'readwrite' }) !== 'granted') {
                const now = Date.now();
                if (now - lastFSAErrorTime > FSA_ERROR_COOLDOWN) {
                    alert(t('fsa.permissionRequiredOperation', 'Для выполнения операции требуется разрешение на доступ к папке.'));
                    lastFSAErrorTime = now;
                }
                directoryHandle = null;
                updateFSAStatus(null, 'permission_revoked_on_operation');
                return null;
            }
            console.log("getFileHandleFromDir: Разрешение получено после повторного запроса.");
        }
        return await dirHandle.getFileHandle(fileName, options);
    } catch (e) {
        if (e.name === 'NotFoundError' && !options.create) {
            return null;
        }
        console.error(`getFileHandleFromDir: Ошибка получения fileHandle для "${fileName}":`, e);
        if (e.name === 'NotAllowedError' || e.name === 'SecurityError' || e.name === 'InvalidStateError') {
            directoryHandle = null;
            updateFSAStatus(null, 'permission_error_on_file_op');
            const now = Date.now();
            if (now - lastFSAErrorTime > FSA_ERROR_COOLDOWN) {
                alert(t('fsa.permissionRevokedError', 'Разрешение на доступ к папке было отозвано или утеряно. Пожалуйста, выберите папку заново.'));
                lastFSAErrorTime = now;
            }
        }
        return null;
    }
}

async function readFileFromFSA(fileName) {
    if (window.electronAPI && window.electronAPI.isElectron) {
        try {
            const data = await window.electronAPI.loadGame(fileName);
            if (data) {
                console.log(`[Electron] Файл "${fileName}" загружен.`);
                return data;
            } else {
                console.warn(`[Electron] Файл "${fileName}" не найден.`);
                return null;
            }
        } catch (error) {
            console.error(`[Electron] Ошибка чтения:`, error);
            return null;
        }
    }
    return null;
}

async function writeFileToFSA(fileName, data) {
    // Если мы в Electron
    if (window.electronAPI && window.electronAPI.isElectron) {
        try {
            // Вызываем метод из main.js через preload
            const result = await window.electronAPI.saveGame(fileName, data);

            if (result.success) {
                console.log(`[Electron] Файл "${fileName}" успешно сохранен.`);
                return true;
            } else {
                console.error(`[Electron] Ошибка сохранения:`, result.error);
                // Показываем твое красивое окно ошибки, если есть, или alert
                if (typeof showCustomAlert === 'function') {
                    showCustomAlert(`Ошибка сохранения файла: ${result.error}`);
                } else {
                    alert(`Ошибка сохранения: ${result.error}`);
                }
                return false;
            }
        } catch (error) {
            console.error(`[Electron] Критическая ошибка записи:`, error);
            return false;
        }
    }

    // Если это веб - функция просто не сработает (вернет false)
    return false;
}

async function deleteFileFromFSA(fileName) {
    if (window.electronAPI && window.electronAPI.isElectron) {
        try {
            const success = await window.electronAPI.deleteSave(fileName);
            console.log(`[Electron] Удаление "${fileName}": ${success}`);
            return success;
        } catch (error) {
            console.error(`[Electron] Ошибка удаления:`, error);
            return false;
        }
    }
    return false;
}

async function listSaveFilesFromFSA() {
    // Эта функция теперь ТОЛЬКО для Electron, так как веб-версия не использует FSA.
    if (window.electronAPI && window.electronAPI.isElectron) {
        try {
            // Получаем "сырой" список файлов из main.js
            const rawSaves = await window.electronAPI.listSaves();
            console.log(`[Electron listSaves] Получено ${rawSaves.length} записей из main.js.`);

            // Преобразуем в формат, который ожидает остальная часть приложения
            return rawSaves.map(save => {
                const nameParts = save.filename.substring(SAVE_FILE_PREFIX.length, save.filename.length - SAVE_FILE_EXTENSION.length).split('_');
                const type = nameParts[0];
                const id = parseInt(nameParts[1], 10);
                return {
                    slotType: type,
                    slotId: id,
                    timestamp: save.timestamp,
                    playerData: save.playerData,
                    historyData: save.historyData,
                    fileName: save.filename // Важно сохранить имя файла!
                };
            }).filter(s => s.slotType && !isNaN(s.slotId)); // Отфильтровываем некорректные имена

        } catch (error) {
            console.error("[Electron] Ошибка получения списка сохранений из main.js:", error);
            return [];
        }
    }
    // Для веб-версии возвращаем пустой массив, так как она не использует эту функцию.
    return [];
}

function getSaveFileName(slotType, slotId) {
    const fileName = `${SAVE_FILE_PREFIX}${slotType}_${slotId}${SAVE_FILE_EXTENSION}`;
    console.log(`[getSaveFileName] Сгенерировано имя файла: ${fileName} для слота ${slotType} #${slotId}`);
    return fileName;
}

function updateFSAStatus(grantedAndHandleExists) {
    if (!fsaStatusElement) return;

    if (window.electronAPI && window.electronAPI.isElectron) {
        fsaStatusElement.textContent = t('fsa.directorySelected', { dirName: 'Локальное хранилище игры' });
        fsaStatusElement.style.color = '#2ecc71'; // Зеленый
        if (fsaSelectDirectoryButton) fsaSelectDirectoryButton.style.display = 'none';
        return;
    }

    if (!fsaApiAvailable) {
        fsaStatusElement.textContent = "Сохранения работают через LocalStorage (Браузер)";
        fsaStatusElement.style.color = '#f1c40f'; // Желтый
        if (fsaSelectDirectoryButton) fsaSelectDirectoryButton.style.display = 'none';
        return;
    }

    if (grantedAndHandleExists && directoryHandle) {
        fsaStatusElement.textContent = t('fsa.directorySelected', { dirName: directoryHandle.name });
        fsaStatusElement.style.color = '#2ecc71';
        if (fsaSelectDirectoryButton) fsaSelectDirectoryButton.textContent = t('settingsMenu.fsaChangeDirectory', 'Сменить папку');
    } else if (localStorage.getItem('fsaDirSelected') === 'true' && !directoryHandle) {
        // Пользователь ранее выбирал папку, но хэндл не активен (например, после перезагрузки страницы)
        fsaStatusElement.textContent = t('fsa.permissionGrantedPreviouslyNeedsAction', 'Папка была выбрана. Нажмите "Выбрать папку", чтобы подтвердить.');
        fsaStatusElement.style.color = '#f1c40f';
        if (fsaSelectDirectoryButton) fsaSelectDirectoryButton.textContent = t('settingsMenu.fsaSelectDirectory', 'Выбрать папку для сохранений');
    } else {
        fsaStatusElement.textContent = t('fsa.directoryNotSelected', 'Папка для сохранений не выбрана. Используется localStorage.');
        fsaStatusElement.style.color = '#e74c3c';
        if (fsaSelectDirectoryButton) fsaSelectDirectoryButton.textContent = t('settingsMenu.fsaSelectDirectory', 'Выбрать папку для сохранений');
    }
}

function getAllSavesFromLocalStorage() {
    // --- ЖЕСТКАЯ БЛОКИРОВКА ---
    // Если мы в Electron, эта функция НИЧЕГО не делает и возвращает пустой результат.
    if (window.electronAPI && window.electronAPI.isElectron) {
        return { manual: [], auto: [] };
    }
    // --- Конец блокировки ---

    // Этот код выполнится только в веб-версии
    const savesJson = localStorage.getItem(SAVE_STORAGE_KEY);
    try {
        let saves = JSON.parse(savesJson);
        if (!saves || typeof saves !== 'object') {
            saves = { manual: [], auto: [] };
        }
        saves.manual = (saves.manual || []).filter(s => s && s.playerData && s.timestamp);
        saves.auto = (saves.auto || []).filter(s => s && s.playerData && s.timestamp);
        return saves;
    } catch (e) {
        console.error("Ошибка разбора сохранений из localStorage:", e);
        return { manual: [], auto: [] };
    }
}

function storeAllSavesToLocalStorage(saves) {
    // --- ЖЕСТКАЯ БЛОКИРОВКА ---
    // Если мы в Electron, эта функция НИЧЕГО не делает.
    if (window.electronAPI && window.electronAPI.isElectron) {
        return;
    }
    // --- Конец блокировки ---

    // Этот код выполнится только в веб-версии
    try {
        saves.manual = (saves.manual || []).filter(s => s && s.playerData && s.timestamp);
        saves.auto = (saves.auto || []).filter(s => s && s.playerData && s.timestamp);
        localStorage.setItem(SAVE_STORAGE_KEY, JSON.stringify(saves));
    } catch (e) {
        console.error("Ошибка сохранения в localStorage:", e);
        const errorMsg = t("loadGame.errorStorageFull");
        addLogMessage(errorMsg, "system-message");
        alert(errorMsg);
    }
}

async function saveDirectoryHandleToDB(dirHandle) {
    if (!window.indexedDB) {
        console.warn("IndexedDB не поддерживается. Хэндл не будет сохранен.");
        return;
    }
    const db = await new Promise((resolve, reject) => {
        const request = indexedDB.open("FileSystemDB", 1);
        request.onupgradeneeded = () => request.result.createObjectStore("handles");
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
    });
    const tx = db.transaction("handles", "readwrite");
    await tx.objectStore("handles").put(dirHandle, "saveDirectory");
    await tx.done;
    console.log("Хэндл директории сохранен в IndexedDB.");
}

async function getDirectoryHandleFromDB() {
    if (!window.indexedDB) return null;
    try {
        const db = await new Promise((resolve, reject) => {
            const request = indexedDB.open("FileSystemDB", 1);
            request.onupgradeneeded = () => request.result.createObjectStore("handles");
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
        });
        const tx = db.transaction("handles", "readonly");
        const handle = await tx.objectStore("handles").get("saveDirectory");
        await tx.done;
        if (handle) {
            console.log("Хэндл директории успешно загружен из IndexedDB.");
        }
        return handle;
    } catch (error) {
        console.error("Ошибка при загрузке хэндла из IndexedDB:", error);
        return null;
    }
}

