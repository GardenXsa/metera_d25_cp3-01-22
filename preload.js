const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
    isElectron: true,
    saveGame: (filename, data) => ipcRenderer.invoke('save-game', filename, data),
    loadGame: (filename) => ipcRenderer.invoke('load-game', filename),
    listSaves: () => ipcRenderer.invoke('list-saves'),
    deleteSave: (filename) => ipcRenderer.invoke('delete-save', filename),
    saveWorldState: (filename, data) => ipcRenderer.invoke('save-world-state', filename, data),
    loadWorldState: (filename) => ipcRenderer.invoke('load-world-state', filename),
    listWorlds: () => ipcRenderer.invoke('list-worlds'),
    deleteWorld: (filename) => ipcRenderer.invoke('delete-world', filename),

    initSaveFile: (filename) => ipcRenderer.invoke('init-save-file', filename),
    appendSaveLine: (filename, line) => ipcRenderer.invoke('append-save-line', filename, line),
    getFileSize: (filename) => ipcRenderer.invoke('get-file-size', filename),
    readSaveChunk: (filename, position, size) => ipcRenderer.invoke('read-save-chunk', filename, position, size),

    speakText: (text, voiceModel) => ipcRenderer.invoke('speak-text', text, voiceModel),
    sendGeminiRequest: (model, apiKey, contents) => ipcRenderer.invoke('gemini-request', model, apiKey, contents),
    getSavePath: () => ipcRenderer.invoke('get-save-path')
,
    nexusInit: (forceRestart) => ipcRenderer.invoke('nexus-init', forceRestart),
    nexusBuildWorld: (playerId, era, initialAgents, globalLocations, startDay) => ipcRenderer.invoke('nexus-build-world', playerId, era, initialAgents, globalLocations, startDay),

    nexusBootstrap: (days, startDay) => ipcRenderer.invoke('nexus-bootstrap', days, startDay),
    nexusSimulate: (world, ticks) => ipcRenderer.invoke('nexus-simulate', world, ticks),
    nexusPreSimulate: (world, ticks) => ipcRenderer.invoke('nexus-presimulate', world, ticks),
    nexusSyncState: (world, items, containers) => ipcRenderer.invoke('nexus-sync-state', world, items, containers),
    nexusGetFullState: () => ipcRenderer.invoke('nexus-get-full-state'),
    nexusGetWorldMap: () => ipcRenderer.invoke('nexus-get-world-map'),
    nexusGmIntervention: (commandObj) => ipcRenderer.invoke('nexus-gm-intervention', commandObj)
,
    nexusInventoryCommand: (params) => ipcRenderer.invoke('nexus-inventory-command', params)
,
    nexusStartTrek: (startId, destId) => ipcRenderer.invoke('nexus-start-trek', startId, destId),
    nexusPauseTrek: () => ipcRenderer.invoke('nexus-pause-trek'),
    nexusResumeTrek: () => ipcRenderer.invoke('nexus-resume-trek'),
    nexusCancelTrek: () => ipcRenderer.invoke('nexus-cancel-trek'),
    nexusInteractTrekObject: (type, id) => ipcRenderer.invoke('nexus-interact-trek-object', type, id),
    nexusManageBusiness: (params) => ipcRenderer.invoke('nexus-manage-business', params)
,
    onNexusProgress: (callback) => ipcRenderer.on('nexus-progress-update', (event, message) => callback(message))
});