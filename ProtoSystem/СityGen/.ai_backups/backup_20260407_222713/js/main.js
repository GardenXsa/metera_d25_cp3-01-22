const typeDictionary = {
    void: "Неизведанная тьма",
    dirt: "Сырая земля",
    grass_dead: "Мертвая трава",
    mud: "Вязкая грязь",
    water_deep: "Глубокая темная вода",
    tree_dead: "Мертвое дерево",
    tree_pine_dark: "Мрачная сосна",
    bush_dry: "Колючий кустарник",
    stone_floor: "Каменный пол",
    wood_floor: "Сгнившие доски",
    wall_stone: "Каменная кладка",
    wall_cave: "Стена пещеры",
    wall_wood: "Деревянный частокол",
    door_wood: "Тяжелая дверь",
    campfire: "Костер",
    torch: "Настенный факел",
    chest: "Старый сундук",
    table: "Грубый стол",
    bones: "Останки",
    blood: "Запекшаяся кровь"
};

let selectedElement = null;

window.onload = () => {
    document.getElementById('jsonInput').value = JSON.stringify(window.INITIAL_DATA, null, 2);
    buildMap(window.INITIAL_DATA);
};

function generateFromInput() {
    try {
        const data = JSON.parse(document.getElementById('jsonInput').value);
        buildMap(data);
    } catch (e) {
        alert("ОШИБКА АУГУРА: Искажение данных (Неверный JSON)\n" + e.message);
    }
}

function buildMap(data) {
    const args = data.args;
    const gridContainer = document.getElementById('mapGrid');
    const [width, height] = args.size.split('x').map(Number);

    gridContainer.style.gridTemplateColumns = `repeat(${width}, 48px)`;
    gridContainer.style.gridTemplateRows = `repeat(${height}, 48px)`;
    gridContainer.innerHTML = ''; 

    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const plotData = args.plots.find(p => p.x === x && p.y === y);
            const cell = document.createElement('div');
            
            let typeClass = "type-void";
            let tileData = { name: "Пустота", type: "void", x: x, y: y };

            if (plotData) {
                typeClass = `type-${plotData.type}`;
                tileData = plotData;
            }

            cell.className = `tile ${typeClass}`;
            cell.onclick = () => selectPlot(cell, tileData);
            
            gridContainer.appendChild(cell);
        }
    }
}

function selectPlot(element, data) {
    if (selectedElement) selectedElement.classList.remove('selected');
    
    element.classList.add('selected');
    selectedElement = element;

    document.getElementById('infoName').innerText = `[ ${data.name || "Неизвестно"} ]`;
    
    let detailsHtml = `
        <div class="info-row"><span class="info-label">Объект:</span> ${typeDictionary[data.type] || data.type}</div>
        <div class="info-row"><span class="info-label">Позиция:</span> [X:${data.x} | Y:${data.y}]</div>
    `;

    if (data.desc) detailsHtml += `<div class="info-row" style="margin-top: 10px; font-style: italic; color: #a0a0a0;">"${data.desc}"</div>`;
    if (data.loot) detailsHtml += `<div class="info-row" style="margin-top: 10px; color: #8b6b4a;"><span class="info-label">Замечено:</span> ${data.loot}</div>`;
    if (data.id) detailsHtml += `<div class="info-row" style="color: #444; margin-top: 15px; font-size: 10px;"><span class="info-label">SYS_ID:</span> ${data.id}</div>`;

    document.getElementById('infoDetails').innerHTML = detailsHtml;
}