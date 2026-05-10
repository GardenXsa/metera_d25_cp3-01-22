const typeDictionary = {
    void: "Неизведанная тьма", dirt: "Сырая земля", grass_dead: "Мертвая трава", mud: "Вязкая грязь", water_deep: "Глубокая темная вода", tree_dead: "Мертвое дерево", tree_pine_dark: "Мрачная сосна", bush_dry: "Колючий кустарник", stone_floor: "Каменный пол", wood_floor: "Сгнившие доски", wall_stone: "Каменная кладка", wall_cave: "Стена пещеры", wall_wood: "Деревянный частокол", door_wood: "Тяжелая дверь", campfire: "Костер", torch: "Настенный факел", chest: "Старый сундук", table: "Грубый стол", bones: "Останки", blood: "Запекшаяся кровь",
    d_wall: "Стена темницы", d_wall_moss: "Замшелая стена", d_wall_crack: "Треснувшая стена", d_wall_iron: "Железная перегородка", d_wall_bars: "Тюремная решетка", d_floor: "Пол подземелья", d_floor_blood: "Окровавленный пол", d_floor_grate: "Ржавая решетка в полу", d_door: "Укрепленная дверь", d_door_locked: "Запертая дверь", d_stairs_up: "Лестница наверх", d_stairs_down: "Лестница вниз", d_pillar: "Каменная колонна", d_barrel: "Бочка", d_crate: "Ящик", d_webs: "Паутина", d_spikes: "Ловушка с шипами", d_pit: "Глубокая яма", d_chains: "Цепи на стене", d_skeleton: "Скелет узника",
    c_wall_brick: "Кирпичная стена", c_wall_plank: "Стена из досок", c_wall_rich: "Обои с узором", c_floor_cobble: "Брусчатка", c_floor_wood: "Паркет", c_floor_carpet: "Красный ковер", c_door_front: "Входная дверь", c_door_rich: "Резная дверь", c_bed: "Кровать", c_bookshelf: "Книжный шкаф", c_wardrobe: "Шкаф", c_desk: "Письменный стол", c_chair: "Стул", c_fireplace: "Камин", c_anvil: "Наковальня", c_forge: "Горн", c_fountain: "Фонтан", c_statue: "Статуя героя", c_sign: "Вывеска", c_cart: "Повозка",
    n_grass: "Зеленая трава", n_grass_tall: "Высокая трава", n_sand: "Песок", n_snow_ground: "Снег", n_ice_floor: "Лед", n_water_shallow: "Мелководье", n_tree_oak: "Дуб", n_tree_birch: "Береза", n_stump: "Пень", n_bush: "Куст", n_bush_berry: "Ягодный куст", n_flower_red: "Красный цветок", n_flower_blue: "Синий цветок", n_mushroom_brown: "Коричневый гриб", n_mushroom_glow: "Светящийся гриб", n_rock_small: "Камень", n_rock_large: "Валун", n_log: "Поваленное бревно", n_vines: "Лианы", n_nest: "Птичье гнездо",
    h_wall_obsidian: "Обсидиановая стена", h_wall_flesh: "Стена из плоти", h_wall_bone: "Костяная стена", h_floor_ash: "Пепел", h_floor_lava: "Лава", h_floor_blood: "Озеро крови", h_door_demon: "Демонические врата", h_altar: "Алтарь жертвоприношений", h_pentagram: "Пентаграмма", h_fire_blue: "Адское пламя", h_cages: "Подвешенные клетки", h_spikes_bone: "Костяные шипы", h_statue_gargoyle: "Статуя горгульи", h_eye: "Глаз Бездны", h_rune_red: "Красная руна", h_rune_purple: "Пурпурная руна", h_portal: "Портал в пустоту", h_crystal_dark: "Темный кристалл", h_tentacle: "Щупальце", h_maw: "Зубастая пасть",
    s_wall_ice: "Ледяная стена", s_wall_snow: "Снежный вал", s_door_frozen: "Смерзшаяся дверь", s_tree_pine: "Заснеженная сосна", s_snowman: "Снеговик", s_crystal_ice: "Ледяной кристалл", s_campfire_dead: "Потухший костер", s_frozen_body: "Замерзший труп", m_wall_void: "Стена Пустоты", m_wall_runic: "Руническая стена", m_floor_stars: "Звездный пол", m_floor_energy: "Энергетическая сетка", m_portal_blue: "Синий портал", m_crystal_blue: "Магический кристалл", m_altar_arcane: "Мистический алтарь", m_book: "Книга заклинаний", m_orb: "Светящаяся сфера", m_pillar_float: "Парящая колонна",
    temple: "Храм", office: "Лавка/Офис", farms: "Фермы", lumbermills: "Лесопилка", mines: "Шахта", forges: "Кузница", smelters: "Плавильня", weavers: "Ткацкая", bakeries: "Пекарня", smokehouses: "Коптильня", alchemists: "Алхимик", banks: "Банк", mills: "Мельница", tailors: "Портной", jewelers: "Ювелир", house: "Жилой дом", tavern: "Таверна", market: "Рынок", road: "Дорога"
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

    // Умный выбор фона по умолчанию на основе описания
    let defaultType = "void";
    let defaultName = "Неизведанная тьма";
    const desc = (args.description || "").toLowerCase();
    if (desc.includes("лес") || desc.includes("лагерь") || desc.includes("полян") || desc.includes("дорог") || desc.includes("тракт")) { defaultType = "n_grass"; defaultName = "Трава"; }
    else if (desc.includes("пещер") || desc.includes("шахт") || desc.includes("гор")) { defaultType = "stone_floor"; defaultName = "Каменный пол"; }
    else if (desc.includes("подземел") || desc.includes("крипт") || desc.includes("темниц") || desc.includes("руин")) { defaultType = "d_floor"; defaultName = "Пол подземелья"; }
    else if (desc.includes("дом") || desc.includes("таверн") || desc.includes("комнат") || desc.includes("хижин") || desc.includes("здани")) { defaultType = "wood_floor"; defaultName = "Деревянный пол"; }
    else if (desc.includes("улиц") || desc.includes("город") || desc.includes("площад")) { defaultType = "c_floor_cobble"; defaultName = "Брусчатка"; }

    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const plotData = args.plots.find(p => p.x === x && p.y === y);
            const cell = document.createElement('div');
            
            let typeClass = `type-${defaultType}`;
            let tileData = { name: defaultName, type: defaultType, x: x, y: y };

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