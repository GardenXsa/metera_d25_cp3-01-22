// Словарь типов для красивого отображения в UI
const typeDictionary = {
    empty: "Трава",
    water: "Водоем",
    tree_pine: "Хвойное дерево",
    tree_oak: "Дуб",
    road_dirt: "Грунтовая дорога",
    road_paved: "Мощеная улица",
    road_cross: "Перекресток",
    fountain: "Фонтан",
    wall: "Крепостная стена (Гор.)",
    wall_v: "Крепостная стена (Верт.)",
    gate: "Городские ворота",
    house_small: "Малый дом",
    house_large: "Большой дом",
    manor: "Усадьба",
    tavern: "Таверна",
    market_red: "Палатка (Красная)",
    market_blue: "Палатка (Синяя)",
    blacksmith: "Кузница"
};

let selectedElement = null;

window.onload = () => {
    // Загружаем тестовые данные из test_data.js при старте
    document.getElementById('jsonInput').value = JSON.stringify(window.INITIAL_DATA, null, 2);
    buildMap(window.INITIAL_DATA);
};

function generateFromInput() {
    try {
        const data = JSON.parse(document.getElementById('jsonInput').value);
        buildMap(data);
    } catch (e) {
        alert("СИСТЕМНАЯ ОШИБКА: Неверный формат JSON\n" + e.message);
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
            
            let typeClass = "type-empty";
            let tileData = { name: "Пустырь", type: "empty", x: x, y: y };

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

    document.getElementById('infoName').innerText = `[Зона] ${data.name || "Без названия"}`;
    
    let detailsHtml = `
        <div class="info-row"><span class="info-label">Тип объекта:</span> ${typeDictionary[data.type] || data.type}</div>
        <div class="info-row"><span class="info-label">Координаты:</span> X:${data.x} Y:${data.y}</div>
    `;

    if (data.owner) detailsHtml += `<div class="info-row"><span class="info-label">Владелец:</span> ${data.owner}</div>`;
    if (data.price) detailsHtml += `<div class="info-row"><span class="info-label">Ст-сть земли:</span> 🪙 ${data.price}</div>`;
    if (data.id) detailsHtml += `<div class="info-row" style="color: #666; margin-top: 15px;"><span class="info-label">ID:</span> ${data.id}</div>`;

    document.getElementById('infoDetails').innerHTML = detailsHtml;
}