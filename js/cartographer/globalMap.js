window.Cartographer = {
    mapContext: null,
    mapCanvas: null,
    mapTooltipElement: null,
    mapState: {
        zoom: 1.2,
        offsetX: 0,
        offsetY: 0,
        isDragging: false,
        isFollowingPlayer: false,
        lastMouseX: 0,
        lastMouseY: 0
    },
    isMapInitialized: false,
    hoveredMapPoint: null,
    mapControlsInitialized: false,
    bgCacheCanvas: null,
    lastGenerationTick: -1,
    currentFilter: 'none',
    filterCacheCanvas: null,
    animationFrameId: null,
    TILE_SIZE: 10,

        /**
     * Инициализирует модуль картографии, привязывает Canvas и настраивает события.
     */
    init: function() {
        this.mapCanvas = document.getElementById('visual-map');
        this.mapTooltipElement = document.getElementById('map-tooltip');
        if (this.mapCanvas) {
            this.mapContext = this.mapCanvas.getContext('2d');
        }
        this.setupMapControls();
        this.setupFilters();
    },

    /**
     * Настраивает UI-кнопки для переключения фильтров отображения карты.
     */
        /**
     * Настраивает UI-кнопки для переключения фильтров отображения карты.
     */
    setupFilters: function() {
        const btns = document.querySelectorAll('.map-filter-btn');
        btns.forEach(btn => {
            btn.addEventListener('click', (e) => {
                btns.forEach(b => b.classList.remove('active'));
                const target = e.currentTarget;
                target.classList.add('active');
                this.currentFilter = target.dataset.filter;
                if (typeof World !== 'undefined' && World && World.map) {
                    this.updateFilterCache(World.map);
                }
                this.render();
            });
        });
    },

    /**
     * Настраивает обработчики событий мыши для панорамирования, зума и взаимодействия с маркерами.
     */
    setupMapControls: function() {
        if (this.mapControlsInitialized || !this.mapCanvas || !this.mapTooltipElement) return;

        console.log("Инициализация управления картой (Nexus Cartographer)...");

        let isClick = false;
        const handleMouseDown = (e) => {
            this.mapState.isDragging = true;
            this.mapState.isFollowingPlayer = false;
            isClick = true;
            this.mapState.lastMouseX = e.offsetX;
            this.mapState.lastMouseY = e.offsetY;
            this.mapCanvas.style.cursor = 'grabbing';
        };

        const handleMouseUp = () => {
            this.mapState.isDragging = false;
            this.mapCanvas.style.cursor = 'grab';
            if (isClick && this.hoveredMapPoint) {
                const targetX = (this.hoveredMapPoint.x + 0.5) * this.TILE_SIZE * this.mapState.zoom;
                const targetY = (this.hoveredMapPoint.y + 0.5) * this.TILE_SIZE * this.mapState.zoom;
                this.mapState.offsetX = (this.mapCanvas.width / 2) - targetX;
                this.mapState.offsetY = (this.mapCanvas.height / 2) - targetY;
                this.render();

                // Клик по порту теперь обрабатывается через боковую панель
            }
        };

        const handleMouseLeave = () => {
            this.mapState.isDragging = false;
            this.hoveredMapPoint = null;
            this.mapCanvas.style.cursor = 'default';
            this.mapTooltipElement.style.display = 'none';
            this.mapTooltipElement.style.opacity = '0';
            this.render();
        };

        const handleMouseMove = (e) => {
            if (this.mapState.isDragging) {
                isClick = false;
                const dx = e.offsetX - this.mapState.lastMouseX;
                const dy = e.offsetY - this.mapState.lastMouseY;
                this.mapState.offsetX += dx;
                this.mapState.offsetY += dy;

                const maxOffset = 3000 * this.mapState.zoom;
                this.mapState.offsetX = Math.max(Math.min(this.mapState.offsetX, maxOffset), -maxOffset + this.mapCanvas.width);
                this.mapState.offsetY = Math.max(Math.min(this.mapState.offsetY, maxOffset), -maxOffset + this.mapCanvas.height);

                this.mapState.lastMouseX = e.offsetX;
                this.mapState.lastMouseY = e.offsetY;
                this.mapTooltipElement.style.display = 'none';
                this.mapTooltipElement.style.opacity = '0';
            } else {
                let closestDist = 15;
                let monsterFound = null;
                if (typeof World !== 'undefined' && World && World.monsters) {
                    for (const m of World.monsters) {
                        if (m.health <= 0 || !m.is_visible_on_map) continue;
                        const markerScreenX = (m.lair_x + 0.5) * this.TILE_SIZE * this.mapState.zoom + this.mapState.offsetX;
                        const markerScreenY = (m.lair_y + 0.5) * this.TILE_SIZE * this.mapState.zoom + this.mapState.offsetY;
                        const dist = Math.hypot(markerScreenX - e.offsetX, markerScreenY - e.offsetY);
                        if (dist < closestDist) {
                            monsterFound = m;
                            closestDist = dist;
                        }
                    }
                }

                if (monsterFound) {
                    this.hoveredMapPoint = { id: monsterFound.id, isMonster: true, x: monsterFound.lair_x, y: monsterFound.lair_y };
                    this.mapCanvas.style.cursor = 'pointer';
                    let mHtml = `<h4 style="color:#e74c3c; border-bottom: 1px solid #e74c3c; margin:0 0 5px 0; padding-bottom:5px;"><i class="fas fa-skull"></i> ${monsterFound.name}</h4>`;
                    mHtml += `<p style="color:#f1c40f; font-weight:bold; margin:0 0 5px 0; font-size:0.9em;">Угроза: Экстремальная (Ур. ${monsterFound.level})</p>`;
                    mHtml += `<p style="margin:0 0 5px 0; color:#bdc3c7; font-size:0.9em;">Тип: ${monsterFound.type}</p>`;
                    mHtml += `<div style="width:100%; height:8px; background:rgba(0,0,0,0.5); border-radius:4px; margin-bottom:5px; border:1px solid #e74c3c;"><div style="height:100%; background:#e74c3c; border-radius:3px; width:${(monsterFound.health/monsterFound.maxHealth)*100}%;"></div></div>`;
                    mHtml += `<p style="margin:0; font-size:0.85em; color:#ecf0f1;">Атака: <span style="color:#e74c3c">${monsterFound.attack}</span> | Защита: <span style="color:#3498db">${monsterFound.defense}</span></p>`;
                    this.mapTooltipElement.innerHTML = mHtml;
                    this.mapTooltipElement.style.display = 'block';
                    this.mapTooltipElement.style.opacity = '1';
                    
                    let newX = e.clientX + 15;
                    let newY = e.clientY + 15;
                    if (newX + this.mapTooltipElement.offsetWidth > window.innerWidth) newX = e.clientX - this.mapTooltipElement.offsetWidth - 15;
                    if (newY + this.mapTooltipElement.offsetHeight > window.innerHeight) newY = e.clientY - this.mapTooltipElement.offsetHeight - 15;
                    this.mapTooltipElement.style.left = `${newX}px`;
                    this.mapTooltipElement.style.top = `${newY}px`;
                    this.render();
                    return;
                }

                let pointFound = null;
                // Радиус срабатывания строго в экранных пикселях
                
                let allPoints = [];
                if (typeof World !== 'undefined' && World && World.map && World.map.locations) {
                    allPoints = [...Object.values(World.map.locations)];
                }
                if (typeof player !== 'undefined' && player && player.mapMarkers) {
                    allPoints = [...allPoints, ...Object.values(player.mapMarkers)];
                }

                for (const point of allPoints) {
                    // Вычисляем реальные экранные координаты маркера (как при отрисовке)
                    const markerScreenX = (point.x + 0.5) * this.TILE_SIZE * this.mapState.zoom + this.mapState.offsetX;
                    const markerScreenY = (point.y + 0.5) * this.TILE_SIZE * this.mapState.zoom + this.mapState.offsetY;

                    // Считаем расстояние от курсора до маркера в пикселях экрана
                    const dist = Math.hypot(markerScreenX - e.offsetX, markerScreenY - e.offsetY);
                    
                    if (dist < closestDist) {
                        pointFound = point;
                        closestDist = dist;
                    }
                }

                this.hoveredMapPoint = pointFound;

                if (pointFound) {
                    this.mapCanvas.style.cursor = 'pointer';
                    
                    let subLocsHtml = '';
                    if (typeof player !== 'undefined' && player && player.subLocations && pointFound.id) {
                        const subs = Object.values(player.subLocations).filter(sub => sub.parentId === pointFound.id);
                        if (subs.length > 0) {
                            subLocsHtml = '<div style="margin-top: 8px; border-top: 1px solid rgba(243, 229, 171, 0.2); padding-top: 5px;">';
                            subLocsHtml += '<strong style="color: #aeb6bf; font-size: 0.85em;">Открытые места:</strong><ul style="margin: 3px 0 0 0; padding-left: 15px; font-size: 0.9em; color: #bdc3c7;">';
                            subs.forEach(sub => {
                                let displayName = sub.name;
                                if (displayName === sub.type) {
                                    displayName = typeof getFacilityName === 'function' ? getFacilityName(sub.type, typeof player !== 'undefined' && player ? player.era : 'rebirth') : sub.name;
                                }
                                subLocsHtml += `<li>${displayName}</li>`;
                            });
                            subLocsHtml += '</ul></div>';
                        }
                    }

                    let residentsHtml = '';
                    if (typeof player !== 'undefined' && player && (player.visitedLocations.includes(pointFound.name) || player.visitedLocations.some(l => pointFound.name.includes(l)))) {
                        const residents = Object.values(player.allKnownEntities).filter(e => e.boundTo === pointFound.name && e.stats.hp > 0);
                        if (residents.length > 0) {
                            residentsHtml = '<div style="margin-top: 8px; border-top: 1px dashed rgba(243, 229, 171, 0.4); padding-top: 5px;">';
                            residentsHtml += '<strong style="color: #f1c40f; font-size: 0.85em;">Известные жители:</strong><ul style="margin: 3px 0 0 0; padding-left: 15px; font-size: 0.85em; color: #ecf0f1;">';
                            residents.forEach(res => {
                                residentsHtml += `<li>${res.name}</li>`;
                            });
                            residentsHtml += '</ul></div>';
                        }
                    }

                                        let resourcesHtml = '';
                    let statsHtml = '';
                    if (typeof World !== 'undefined' && World && World.regions && World.regions[pointFound.id]) {
                        const reg = World.regions[pointFound.id];
                        
                        let actualFactionId = reg.factionId;
                        let factionName = "Ничья земля";
                        let repText = '';
                        let isPlayerFaction = false;

                        if (actualFactionId && World.factions && World.factions[actualFactionId]) {
                            factionName = World.factions[actualFactionId].name;
                            if (World.factions[actualFactionId].rulerId === 'player') {
                                factionName = "👑 [ВАША] " + factionName;
                                isPlayerFaction = true;
                            }
                            if (typeof player !== 'undefined' && player && player.stats && player.stats.reputation) {
                                const rep = player.stats.reputation[actualFactionId] || 0;
                                repText = ` | Реп: <span style="color: ${rep >= 0 ? '#2ecc71' : '#e74c3c'}">${rep}</span>`;
                            }
                        }
                        
                        let popText = reg.population > 0 ? reg.population : "<span style='color:#e74c3c; font-style:italic;'>Заброшено</span>";
                        let occText = reg.isOccupied ? `<div style="color:#e74c3c; font-weight:bold; margin-top:2px;">⚠️ Оккупировано (${reg.occupierFactionId})</div>` : '';
                        
                        let armyText = '';
                        let armiesHere = [];
                        for (let fid in World.factions) {
                            World.factions[fid].armies.forEach(a => {
                                if (a.location === pointFound.id || a.destination === pointFound.id) {
                                    let aName = World.factions[fid].name;
                                    if (World.factions[fid].rulerId === 'player') aName = "👑 Ваша армия";
                                    armiesHere.push(`${aName} (${a.size} чел.)`);
                                }
                            });
                        }
                        if (armiesHere.length > 0) {
                            armyText = `<div style="color:#e67e22; margin-top:4px; padding-top:4px; border-top: 1px dashed rgba(230, 126, 34, 0.3);"><b>⚔️ Армии:</b> ${armiesHere.join(', ')}</div>`;
                        }

                        statsHtml = `<div style="margin-top: 8px; font-size: 0.85em; color: #ecf0f1;">
                                        <div><strong style="color: #3498db;">Фракция:</strong> <span style="color:${isPlayerFaction ? '#2ecc71' : '#ecf0f1'}">${factionName}</span>${repText}</div>
                                        ${occText}
                                        <div style="display:grid; grid-template-columns: 1fr 1fr; gap: 4px; margin-top: 6px; background: rgba(0,0,0,0.3); padding: 6px; border-radius: 4px;">
                                            <div><strong style="color: #f1c40f;">Население:</strong> ${popText}</div>
                                            <div><strong style="color: #e74c3c;">Угроза:</strong> ${reg.threat_level}%</div>
                                            <div><strong style="color: #9b59b6;">Стаб-ть:</strong> ${reg.stability}%</div>
                                            <div><strong style="color: #e67e22;">Волнения:</strong> ${reg.unrest}%</div>
                                            <div><strong style="color: #2ecc71;">Казна:</strong> ${Math.floor(reg.moneySupply)} з.</div>
                                            <div><strong style="color: #aeb6bf;">Зарплата:</strong> ${reg.average_wage} з.</div>
                                        </div>
                                        ${armyText}
                                     </div>`;

                        if (World.port_facilities && World.port_facilities[pointFound.id]) {
                            const port = World.port_facilities[pointFound.id];
                            let blockade = port.is_blockaded ? ' <span style="color:#e74c3c">[БЛОКАДА]</span>' : '';
                            statsHtml += `<div style="margin-top: 4px; font-size: 0.85em; color: #ecf0f1;"><strong style="color: #3498db;">⚓ Порт:</strong> Ур. ${port.level} (${port.type})${blockade}</div>`;
                        }

                        if (reg.available_raw_resources && reg.available_raw_resources.length > 0) {
                            const icons = reg.available_raw_resources.map(r => typeof getResourceIcon === 'function' ? getResourceIcon(r) : "📦").join(' ');
                            resourcesHtml = `<div style="margin-top: 8px; border-top: 1px dashed rgba(243, 229, 171, 0.4); padding-top: 5px;">
                                <strong style="color: #2ecc71; font-size: 0.85em;">Сырье:</strong> <span style="font-size: 1.1em; letter-spacing: 2px;">${icons}</span>
                            </div>`;
                        }
                    }

                    this.mapTooltipElement.innerHTML = `<h4>${pointFound.name}</h4><p>${pointFound.description || ''}</p>${statsHtml}${resourcesHtml}${subLocsHtml}${residentsHtml}`;
                    this.mapTooltipElement.style.display = 'block';
                    this.mapTooltipElement.style.opacity = '1';

                    let newX = e.clientX + 15;
                    let newY = e.clientY + 15;

                    if (newX + this.mapTooltipElement.offsetWidth > window.innerWidth) newX = e.clientX - this.mapTooltipElement.offsetWidth - 15;
                    if (newY + this.mapTooltipElement.offsetHeight > window.innerHeight) newY = e.clientY - this.mapTooltipElement.offsetHeight - 15;

                    this.mapTooltipElement.style.left = `${newX}px`;
                    this.mapTooltipElement.style.top = `${newY}px`;
                } else {
                    this.mapCanvas.style.cursor = 'grab';
                    this.mapTooltipElement.style.display = 'none';
                    this.mapTooltipElement.style.opacity = '0';
                }
            }
            this.render();
        };

        const handleWheel = (e) => {
            e.preventDefault();
            const zoomIntensity = 0.1;
            const scroll = e.deltaY < 0 ? 1 : -1;
            const zoomFactor = Math.exp(scroll * zoomIntensity);
            const newZoom = Math.max(0.5, Math.min(3, this.mapState.zoom * zoomFactor)); // Пункт 4.3.2 ТЗ
            const mouseX = e.offsetX;
            const mouseY = e.offsetY;
            const worldX = (mouseX - this.mapState.offsetX) / this.mapState.zoom;
            const worldY = (mouseY - this.mapState.offsetY) / this.mapState.zoom;
            this.mapState.offsetX = mouseX - worldX * newZoom;
            this.mapState.offsetY = mouseY - worldY * newZoom;
            this.mapState.zoom = newZoom;
            this.render();
        };

        this.mapCanvas.addEventListener('mousedown', handleMouseDown);
        this.mapCanvas.addEventListener('mouseup', handleMouseUp);
        this.mapCanvas.addEventListener('mouseleave', handleMouseLeave);
        this.mapCanvas.addEventListener('mousemove', handleMouseMove);
        this.mapCanvas.addEventListener('wheel', handleWheel);

        const centerBtn = document.getElementById('center-map-btn');
        if (centerBtn) {
            centerBtn.addEventListener('click', () => {
                this.mapState.isFollowingPlayer = true;
                this.render();
            });
        }

        this.mapControlsInitialized = true;
    },

        /**
     * Преобразует экранные координаты в мировые координаты тайлов.
     * @param {number} screenX - X координата на экране
     * @param {number} screenY - Y координата на экране
     * @returns {Object} Объект с мировыми координатами {x, y}
     */
    screenToWorld: function(screenX, screenY) {
        return {
            x: (screenX - this.mapState.offsetX) / (this.TILE_SIZE * this.mapState.zoom),
            y: (screenY - this.mapState.offsetY) / (this.TILE_SIZE * this.mapState.zoom)
        };
    },

        /**
     * Отрисовывает маркер локации или игрока на карте.
     * @param {CanvasRenderingContext2D} ctx - Контекст отрисовки
     * @param {Object} pos - Экранные координаты {x, y}
     * @param {string} type - Тип локации (city, village, ruins и т.д.)
     * @param {boolean} isPlayer - Флаг, является ли маркер позицией игрока
     * @param {string|null} factionId - ID фракции для отрисовки политического флага
     */
        /**
     * Отрисовывает маркер локации или игрока на карте.
     */
    drawMapMarker: function(ctx, pos, type, isPlayer, factionId = null) {
        ctx.lineWidth = 2;
        ctx.strokeStyle = '#2c3e50';

        if (factionId && factionId !== 'none') {
            let hash = 0;
            for (let i = 0; i < factionId.length; i++) {
                hash = factionId.charCodeAt(i) + ((hash << 5) - hash);
            }
            const hue = Math.abs(hash) % 360;
            ctx.fillStyle = `hsl(${hue}, 70%, 50%)`;
            ctx.fillRect(pos.x - 12, pos.y - 12, 6, 6);
            ctx.strokeRect(pos.x - 12, pos.y - 12, 6, 6);
        }

        if (isPlayer) {
            // Пульсация маркера игрока (Пункт 4.4.2 ТЗ)
            const pulse = (Math.sin(Date.now() / 200) + 1) / 2;
            const currentRadius = 6 + pulse * 3;
            ctx.fillStyle = `rgba(231, 76, 60, ${0.6 + pulse * 0.4})`;
            ctx.beginPath();
            ctx.arc(pos.x, pos.y, currentRadius, 0, 2 * Math.PI);
            ctx.fill();
            ctx.strokeStyle = '#fff';
            ctx.stroke();
        } else {
            ctx.beginPath();
            if (type === 'road') {
                ctx.fillStyle = '#7d6b5d';
                ctx.beginPath();
                ctx.moveTo(pos.x - 4, pos.y + 5);
                ctx.lineTo(pos.x, pos.y - 6);
                ctx.lineTo(pos.x + 4, pos.y + 5);
                ctx.fill();
            } else if (type === 'city') {
                ctx.fillStyle = '#f1c40f';
                ctx.rect(pos.x - 8, pos.y - 6, 16, 12);
                ctx.fill();
                ctx.stroke();
                ctx.fillStyle = '#d35400';
                ctx.beginPath();
                ctx.moveTo(pos.x - 10, pos.y - 6);
                ctx.lineTo(pos.x, pos.y - 14);
                ctx.lineTo(pos.x + 10, pos.y - 6);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'village') {
                ctx.fillStyle = '#bdc3c7';
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 5, 0, 2 * Math.PI);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'ruins') {
                ctx.fillStyle = '#7f8c8d';
                ctx.beginPath();
                ctx.moveTo(pos.x - 6, pos.y + 6);
                ctx.lineTo(pos.x - 4, pos.y - 4);
                ctx.lineTo(pos.x, pos.y + 2);
                ctx.lineTo(pos.x + 4, pos.y - 6);
                ctx.lineTo(pos.x + 6, pos.y + 6);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'fort') {
                ctx.fillStyle = '#95a5a6';
                ctx.fillRect(pos.x - 7, pos.y - 7, 14, 14);
                ctx.strokeRect(pos.x - 7, pos.y - 7, 14, 14);
            } else if (type === 'camp') {
                ctx.fillStyle = '#d35400';
                ctx.beginPath();
                ctx.moveTo(pos.x, pos.y - 6);
                ctx.lineTo(pos.x - 6, pos.y + 6);
                ctx.lineTo(pos.x + 6, pos.y + 6);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'anomaly') {
                ctx.fillStyle = '#9b59b6';
                ctx.beginPath();
                ctx.moveTo(pos.x, pos.y - 7);
                ctx.lineTo(pos.x + 7, pos.y);
                ctx.lineTo(pos.x, pos.y + 7);
                ctx.lineTo(pos.x - 7, pos.y);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'observatory') {
                ctx.fillStyle = '#2980b9';
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 6, Math.PI, 0);
                ctx.fill();
                ctx.stroke();
            } else if (type === 'mountain') {
                ctx.fillStyle = '#95a5a6';
                ctx.beginPath();
                ctx.moveTo(pos.x - 8, pos.y + 6);
                ctx.lineTo(pos.x, pos.y - 8);
                ctx.lineTo(pos.x + 8, pos.y + 6);
                ctx.fill();
                ctx.stroke();
            } else {
                ctx.fillStyle = '#27ae60';
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 6, 0, 2 * Math.PI);
                ctx.fill();
                ctx.stroke();
            }
        }
    },

    /**
     * Отрисовывает розу ветров (компас) в указанных координатах.
     * @param {CanvasRenderingContext2D} ctx - Контекст отрисовки
     * @param {number} x - X координата центра
     * @param {number} y - Y координата центра
     * @param {number} radius - Радиус компаса
     */
    drawCompassRose: function(ctx, x, y, radius) {
        ctx.strokeStyle = 'rgba(93, 74, 54, 0.7)';
        ctx.fillStyle = 'rgba(93, 74, 54, 0.7)';
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(x, y - radius); ctx.lineTo(x, y + radius);
        ctx.moveTo(x - radius, y); ctx.lineTo(x + radius, y);
        ctx.stroke();
        ctx.font = 'bold ' + (radius * 0.7) + "px Georgia, serif";
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('N', x, y - radius - 8);
        ctx.fillText('S', x, y + radius + 8);
        ctx.fillText('W', x - radius - 8, y);
        ctx.fillText('E', x + radius + 8, y);
    },

        /**
     * Запрашивает актуальные данные карты у нативного движка через IPC.
     * @returns {Promise<void>}
     */
    fetchMapData: async function() {
        if (window.electronAPI && window.electronAPI.nexusGetWorldMap) {
            try {
                const response = await window.electronAPI.nexusGetWorldMap();
                if (response && response.status === 'ok' && response.map) {
                    if (typeof World !== 'undefined' && World) {
                        World.map = response.map;
                    }
                }
            } catch (e) {
                console.error("[Nexus Cartographer] Ошибка получения карты:", e);
            }
        }
    },


    /**
     * Обновляет кэш фонового слоя (тайлы и дороги) на OffscreenCanvas.
     * @param {Object} map - Объект глобальной карты (World.map)
     */
    updateBackgroundCache: function(map) {
        console.log("[Nexus Cartographer] Обновление кэша карты...");
        if (!this.bgCacheCanvas) {
            if (typeof OffscreenCanvas !== 'undefined') {
                this.bgCacheCanvas = new OffscreenCanvas(map.width * this.TILE_SIZE, map.height * this.TILE_SIZE);
            } else {
                this.bgCacheCanvas = document.createElement('canvas');
                this.bgCacheCanvas.width = map.width * this.TILE_SIZE;
                this.bgCacheCanvas.height = map.height * this.TILE_SIZE;
            }
        } else {
            this.bgCacheCanvas.width = map.width * this.TILE_SIZE;
            this.bgCacheCanvas.height = map.height * this.TILE_SIZE;
        }

        const ctx = this.bgCacheCanvas.getContext('2d', { alpha: false });
        ctx.fillStyle = '#0a0a0a';
        ctx.fillRect(0, 0, this.bgCacheCanvas.width, this.bgCacheCanvas.height);

        const colors = [
            '#1a3b5c', '#2980b9', '#2ecc71', '#27ae60', '#7f8c8d', 
            '#f39c12', '#e67e22', '#8e44ad', '#ecf0f1', '#34495e', 
            '#9b59b6', '#3498db', '#c0392b',
            '#3cb043', '#1f618d', '#58d68d', '#d35400', '#555555'
        ];

        // Pass 1: Base terrain
        for (let y = 0; y < map.height; y++) {
            for (let x = 0; x < map.width; x++) {
                let cell = map.grid ? map.grid[y * map.width + x] : null;
                let tileType = cell ? cell[0] : (map.tiles ? map.tiles[y * map.width + x] : 0);
                let isFlooded = cell ? cell[4] : false;

                let color = colors[tileType] || '#000';
                // Убрана подмена цвета реки на равнину, теперь река рисуется своим цветом

                ctx.fillStyle = color;
                ctx.fillRect(x * this.TILE_SIZE, y * this.TILE_SIZE, this.TILE_SIZE, this.TILE_SIZE);
                
                if (isFlooded) {
                    ctx.fillStyle = 'rgba(41, 128, 185, 0.6)';
                    ctx.fillRect(x * this.TILE_SIZE, y * this.TILE_SIZE, this.TILE_SIZE, this.TILE_SIZE);
                }
            }
        }

        // Векторные реки удалены, так как они создавали визуальный мусор.
        // Теперь реки отрисовываются как обычные тайлы в Pass 1.

        if (map.roads) {
            map.roads.forEach(road => {
                ctx.beginPath();
                let isRuined = road.condition === 'ruined' || road.integrity < 30;
                
                if (road.type === 'bridge') {
                    ctx.lineWidth = 4;
                    ctx.strokeStyle = isRuined ? '#c0392b' : '#8b4513';
                    if (isRuined) ctx.setLineDash([4, 4]);
                    else ctx.setLineDash([]);
                } else if (road.type === 'tunnel') {
                    ctx.lineWidth = 3;
                    ctx.strokeStyle = isRuined ? '#e74c3c' : '#555555';
                    ctx.setLineDash([6, 4]);
                } else if (road.type === 'ferry') {
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = isRuined ? '#c0392b' : '#3498db';
                    ctx.setLineDash([4, 4]);
                } else if (road.type === 'highway') {
                    ctx.lineWidth = 3;
                    ctx.strokeStyle = isRuined ? '#e74c3c' : '#7f8c8d';
                    if (isRuined) ctx.setLineDash([5, 5]);
                    else ctx.setLineDash([]);
                } else if (road.type === 'sea_route') {
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = '#3498db';
                    ctx.setLineDash([6, 6]);
                } else if (road.type === 'paved' || road.condition === 'paved') {
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = isRuined ? '#e74c3c' : '#95a5a6';
                    if (isRuined) ctx.setLineDash([5, 5]);
                    else ctx.setLineDash([]);
                } else {
                    ctx.lineWidth = 2;
                    ctx.strokeStyle = isRuined ? '#a93226' : '#8b4513';
                    if (isRuined) ctx.setLineDash([3, 6]);
                    else ctx.setLineDash([8, 4]);
                }

                road.waypoints.forEach((wp, idx) => {
                    const px = wp[0] * this.TILE_SIZE + this.TILE_SIZE / 2;
                    const py = wp[1] * this.TILE_SIZE + this.TILE_SIZE / 2;
                    if (idx === 0) ctx.moveTo(px, py);
                    else ctx.lineTo(px, py);
                });
                ctx.stroke();
                ctx.setLineDash([]);

                if (road.type === 'bridge' && !isRuined) {
                    ctx.lineWidth = 1;
                    ctx.strokeStyle = '#d35400';
                    road.waypoints.forEach((wp, idx) => {
                        if (idx % 2 === 0) {
                            const px = wp[0] * this.TILE_SIZE + this.TILE_SIZE / 2;
                            const py = wp[1] * this.TILE_SIZE + this.TILE_SIZE / 2;
                            ctx.beginPath();
                            ctx.moveTo(px - 3, py - 3);
                            ctx.lineTo(px + 3, py + 3);
                            ctx.stroke();
                        }
                    });
                }
            });
        }

        this.lastGenerationTick = map.generation_tick;
    },

    /**
     * Отрисовывает полупрозрачные слои фильтров (политика, экономика, угрозы) поверх карты.
     * @param {CanvasRenderingContext2D} ctx - Контекст отрисовки
     * @param {Object} map - Объект глобальной карты
     * @param {Function} transform - Функция преобразования мировых координат в экранные
     */
        /**
     * Обновляет кэш тайловых фильтров (диаграмма Вороного) на OffscreenCanvas.
     * @param {Object} map - Объект глобальной карты
     */
        updateFilterCache: function(map) {
        if (this.currentFilter === 'none') return;
        
        if (!this.filterCacheCanvas) {
            if (typeof OffscreenCanvas !== 'undefined') {
                this.filterCacheCanvas = new OffscreenCanvas(map.width * this.TILE_SIZE, map.height * this.TILE_SIZE);
            } else {
                this.filterCacheCanvas = document.createElement('canvas');
                this.filterCacheCanvas.width = map.width * this.TILE_SIZE;
                this.filterCacheCanvas.height = map.height * this.TILE_SIZE;
            }
        } else {
            this.filterCacheCanvas.width = map.width * this.TILE_SIZE;
            this.filterCacheCanvas.height = map.height * this.TILE_SIZE;
        }

        const ctx = this.filterCacheCanvas.getContext('2d', { alpha: true });
        ctx.clearRect(0, 0, this.filterCacheCanvas.width, this.filterCacheCanvas.height);

        const locs = Object.values(map.locations);
        if (locs.length === 0) return;

        if (this.currentFilter === 'political') {
            // --- ПОЛИТИЧЕСКАЯ КАРТА (CIV 5 STYLE) ---
            const ownership = new Array(map.width * map.height).fill(null);
            const MAX_TERRITORY_RADIUS = 25; // Максимальный радиус владений от города

            // 1. Определяем владельца каждого тайла
            for (let y = 0; y < map.height; y++) {
                for (let x = 0; x < map.width; x++) {
                    const tileType = map.grid ? map.grid[y * map.width + x][0] : map.tiles[y * map.width + x];
                    if (tileType === 0) continue; // Океаны (0) никому не принадлежат

                    let nearestLoc = null;
                    let minDist = Infinity;
                    for (let i = 0; i < locs.length; i++) {
                        const dx = locs[i].x - x;
                        const dy = locs[i].y - y;
                        const dist = Math.sqrt(dx*dx + dy*dy);
                        if (dist < minDist) {
                            minDist = dist;
                            nearestLoc = locs[i];
                        }
                    }

                    if (nearestLoc && minDist <= MAX_TERRITORY_RADIUS) {
                        ownership[y * map.width + x] = nearestLoc.faction;
                    }
                }
            }

            // Функция для получения цвета фракции
            const getFactionColor = (factionId) => {
                let hash = 0;
                for (let i = 0; i < factionId.length; i++) {
                    hash = factionId.charCodeAt(i) + ((hash << 5) - hash);
                }
                return Math.abs(hash) % 360;
            };

            // 2. Отрисовка заливки и четких границ
            for (let y = 0; y < map.height; y++) {
                for (let x = 0; x < map.width; x++) {
                    const faction = ownership[y * map.width + x];
                    if (!faction) continue;

                    const hue = getFactionColor(faction);
                    const px = x * this.TILE_SIZE;
                    const py = y * this.TILE_SIZE;

                    // Легкая полупрозрачная заливка территории
                    ctx.fillStyle = `hsla(${hue}, 70%, 50%, 0.25)`;
                    ctx.fillRect(px, py, this.TILE_SIZE, this.TILE_SIZE);

                    // Отрисовка границ (проверяем соседей)
                    ctx.strokeStyle = `hsla(${hue}, 80%, 60%, 0.9)`;
                    ctx.lineWidth = 2;
                    ctx.beginPath();

                    // Верхний сосед
                    if (y === 0 || ownership[(y - 1) * map.width + x] !== faction) {
                        ctx.moveTo(px, py); ctx.lineTo(px + this.TILE_SIZE, py);
                    }
                    // Нижний сосед
                    if (y === map.height - 1 || ownership[(y + 1) * map.width + x] !== faction) {
                        ctx.moveTo(px, py + this.TILE_SIZE); ctx.lineTo(px + this.TILE_SIZE, py + this.TILE_SIZE);
                    }
                    // Левый сосед
                    if (x === 0 || ownership[y * map.width + (x - 1)] !== faction) {
                        ctx.moveTo(px, py); ctx.lineTo(px, py + this.TILE_SIZE);
                    }
                    // Правый сосед
                    if (x === map.width - 1 || ownership[y * map.width + (x + 1)] !== faction) {
                        ctx.moveTo(px + this.TILE_SIZE, py); ctx.lineTo(px + this.TILE_SIZE, py + this.TILE_SIZE);
                    }
                    ctx.stroke();
                }
            }
        } else if (this.currentFilter === 'economic' || this.currentFilter === 'threat') {
            // --- ЭКОНОМИКА И УГРОЗА (HEATMAP STYLE) ---
            // Используем screen blend mode для красивого смешивания градиентов
            ctx.globalCompositeOperation = 'screen';
            
            for (let i = 0; i < locs.length; i++) {
                const loc = locs[i];
                const region = typeof World !== 'undefined' && World.regions ? World.regions[loc.id] : null;
                if (!region) continue;

                const px = (loc.x + 0.5) * this.TILE_SIZE;
                const py = (loc.y + 0.5) * this.TILE_SIZE;
                
                let radius = 0;
                let colorStops = [];

                if (this.currentFilter === 'economic') {
                    const wealth = Math.min(1, region.moneySupply / 30000);
                    radius = 40 + (wealth * 120); // Радиус свечения зависит от богатства
                    colorStops = [
                        { stop: 0, color: `rgba(241, 196, 15, ${0.4 + wealth * 0.5})` }, // Золотой центр
                        { stop: 0.5, color: `rgba(46, 204, 113, ${0.1 + wealth * 0.3})` }, // Зеленоватый край
                        { stop: 1, color: 'rgba(0, 0, 0, 0)' }
                    ];
                } else if (this.currentFilter === 'threat') {
                    const threat = Math.min(1, region.threat_level / 100);
                    radius = 60 + (threat * 100);
                    // От зеленого (безопасно) к красному (опасно)
                    const r = Math.floor(threat * 255);
                    const g = Math.floor((1 - threat) * 255);
                    colorStops = [
                        { stop: 0, color: `rgba(${r}, ${g}, 0, ${0.5 + threat * 0.4})` },
                        { stop: 0.6, color: `rgba(${r}, ${g}, 0, ${0.1 + threat * 0.2})` },
                        { stop: 1, color: 'rgba(0, 0, 0, 0)' }
                    ];
                }

                const grad = ctx.createRadialGradient(px, py, 0, px, py, radius);
                colorStops.forEach(cs => grad.addColorStop(cs.stop, cs.color));
                
                ctx.fillStyle = grad;
                ctx.beginPath();
                ctx.arc(px, py, radius, 0, Math.PI * 2);
                ctx.fill();
            }
            ctx.globalCompositeOperation = 'source-over'; // Возвращаем стандартный режим
        }
    },

    /**
     * Главный цикл отрисовки карты. Комбинирует кэшированный фон, фильтры и динамические маркеры.
     */
    render: function() {
        if (this.animationFrameId) cancelAnimationFrame(this.animationFrameId);

        if (!this.mapContext || !this.mapCanvas || typeof player === 'undefined' || !player) return;

        const ctx = this.mapContext;
        const width = this.mapCanvas.width;
        const height = this.mapCanvas.height;
        if (width === 0) return;

        ctx.fillStyle = '#0a0a0a';
        ctx.fillRect(0, 0, width, height);

        if (typeof World !== 'undefined' && World && World.map) {
            const map = World.map;
            
            if (!this.bgCacheCanvas || this.lastGenerationTick !== map.generation_tick) {
                this.updateBackgroundCache(map);
                if (this.currentFilter !== 'none') this.updateFilterCache(map);
            }

            ctx.imageSmoothingEnabled = false;
            const scaledWidth = map.width * this.TILE_SIZE * this.mapState.zoom;
            const scaledHeight = map.height * this.TILE_SIZE * this.mapState.zoom;
            
            const intOffsetX = Math.floor(this.mapState.offsetX);
            const intOffsetY = Math.floor(this.mapState.offsetY);
            ctx.drawImage(this.bgCacheCanvas, intOffsetX, intOffsetY, Math.floor(scaledWidth), Math.floor(scaledHeight));

            if (this.currentFilter !== 'none' && this.filterCacheCanvas) {
                ctx.drawImage(this.filterCacheCanvas, intOffsetX, intOffsetY, Math.floor(scaledWidth), Math.floor(scaledHeight));
            }

            const transform = (worldX, worldY) => ({
                x: Math.floor((Number(worldX) * this.TILE_SIZE * this.mapState.zoom) + this.mapState.offsetX),
                y: Math.floor((Number(worldY) * this.TILE_SIZE * this.mapState.zoom) + this.mapState.offsetY)
            });

            if (typeof player !== 'undefined' && player) {
                let currX = 0, currY = 0;
                let isTraveling = player.travel && player.travel.active;
                
                if (isTraveling && player.travel.currentX !== undefined) {
                    currX = player.travel.currentX;
                    currY = player.travel.currentY;
                } else if (map.locations) {
                    let targetId = null;
                    // Т3 ФИКС: Если мы в подлокации, берем координаты родительского региона
                    if (player.currentSublocation) {
                        const sub = (player.subLocations && player.subLocations[player.currentSublocation]) || 
                                    (World && World.subLocations && World.subLocations[player.currentSublocation]);
                        if (sub && sub.parentId) targetId = sub.parentId;
                    }

                    if (targetId && map.locations[targetId]) {
                        currX = map.locations[targetId].x;
                        currY = map.locations[targetId].y;
                    } else {
                        // Старый фолбэк по имени
                        const currentLocName = (player.location || "").toLowerCase().trim();
                        let playerPoint = Object.values(map.locations).find(p => p.name.toLowerCase().trim().includes(currentLocName) || currentLocName.includes(p.name.toLowerCase().trim()));
                        if (playerPoint) {
                            currX = playerPoint.x;
                            currY = playerPoint.y;
                        }
                    }
                }

                const currPos = transform(currX + 0.5, currY + 0.5);

                if (isTraveling && World && World.player_trek && World.player_trek.path) {
                    ctx.save();
                    ctx.setLineDash([8 * this.mapState.zoom, 8 * this.mapState.zoom]);
                    ctx.strokeStyle = 'rgba(241, 196, 15, 0.9)';
                    ctx.lineWidth = Math.max(2, 3 * this.mapState.zoom);
                    ctx.beginPath();
                    for (let i = 0; i < World.player_trek.path.length; i++) {
                        const pt = transform(World.player_trek.path[i][0] + 0.5, World.player_trek.path[i][1] + 0.5);
                        if (i === 0) ctx.moveTo(pt.x, pt.y);
                        else ctx.lineTo(pt.x, pt.y);
                    }
                    ctx.stroke();
                    ctx.restore();
                }

                this.drawMapMarker(ctx, currPos, 'default', true);

                if (this.mapState.isFollowingPlayer) {
                    const targetOffsetX = (width / 2) - ((currX + 0.5) * this.TILE_SIZE * this.mapState.zoom);
                    const targetOffsetY = (height / 2) - ((currY + 0.5) * this.TILE_SIZE * this.mapState.zoom);
                    this.mapState.offsetX += (targetOffsetX - this.mapState.offsetX) * 0.1;
                    this.mapState.offsetY += (targetOffsetY - this.mapState.offsetY) * 0.1;
                }
            }

            if (map.disasters && map.disasters.length > 0) {
                const time = Date.now();
                map.disasters.forEach(dis => {
                    const px = transform(dis.epicenter_x + 0.5, dis.epicenter_y + 0.5);
                    const radiusPx = dis.radius * this.TILE_SIZE * this.mapState.zoom;

                    ctx.save();
                    ctx.globalCompositeOperation = 'screen';
                    
                    let pulse = (Math.sin(time / 300) + 1) / 2;
                    let color1, color2;
                    
                    if (dis.type === 'flood') {
                        color1 = `rgba(41, 128, 185, ${0.3 + pulse * 0.2})`;
                        color2 = `rgba(41, 128, 185, 0)`;
                    } else if (dis.type === 'wildfire' || dis.type === 'volcano') {
                        color1 = `rgba(231, 76, 60, ${0.4 + pulse * 0.3})`;
                        color2 = `rgba(211, 84, 0, 0)`;
                    } else if (dis.type === 'earthquake') {
                        color1 = `rgba(139, 69, 19, ${0.3 + pulse * 0.2})`;
                        color2 = `rgba(139, 69, 19, 0)`;
                        px.x += (Math.random() - 0.5) * 4;
                        px.y += (Math.random() - 0.5) * 4;
                    } else if (dis.type === 'plague') {
                        color1 = `rgba(142, 68, 173, ${0.3 + pulse * 0.2})`;
                        color2 = `rgba(46, 204, 113, 0)`;
                    } else if (dis.type === 'aether_storm') {
                        color1 = `rgba(155, 89, 182, ${0.4 + pulse * 0.3})`;
                        color2 = `rgba(142, 68, 173, 0)`;
                    } else if (dis.type === 'drought') {
                        color1 = `rgba(243, 156, 18, ${0.3 + pulse * 0.2})`;
                        color2 = `rgba(230, 126, 34, 0)`;
                    } else if (dis.type === 'monster_invasion') {
                        color1 = `rgba(192, 57, 43, ${0.3 + pulse * 0.2})`;
                        color2 = `rgba(0, 0, 0, 0)`;
                    } else {
                        color1 = `rgba(255, 255, 255, ${0.2 + pulse * 0.2})`;
                        color2 = `rgba(255, 255, 255, 0)`;
                    }

                    const grad = ctx.createRadialGradient(px.x, px.y, 0, px.x, px.y, radiusPx);
                    grad.addColorStop(0, color1);
                    grad.addColorStop(1, color2);
                    
                    ctx.fillStyle = grad;
                    ctx.beginPath();
                    ctx.arc(px.x, px.y, radiusPx, 0, Math.PI * 2);
                    ctx.fill();
                    
                    ctx.globalCompositeOperation = 'source-over';
                    ctx.font = `${16 * this.mapState.zoom}px Arial`;
                    ctx.textAlign = 'center';
                    ctx.textBaseline = 'middle';
                    let icon = '⚠️';
                    if (dis.type === 'flood') icon = '🌊';
                    if (dis.type === 'wildfire') icon = '🔥';
                    if (dis.type === 'earthquake') icon = '💥';
                    if (dis.type === 'volcano') icon = '🌋';
                    if (dis.type === 'plague') icon = '☣️';
                    if (dis.type === 'aether_storm') icon = '🌀';
                    if (dis.type === 'drought') icon = '☀️';
                    if (dis.type === 'monster_invasion') icon = '👹';
                    
                    ctx.fillText(icon, px.x, px.y);
                    ctx.restore();
                });
            }


            if (map.locations) {
                Object.values(map.locations).forEach(loc => {
                    const pos = transform(loc.x + 0.5, loc.y + 0.5);
                    if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;

                    const isPlayerLocation = player.location && player.location.toLowerCase().trim().includes(loc.name.toLowerCase().trim());
                    this.drawMapMarker(ctx, pos, loc.type, isPlayerLocation, loc.faction);
                    const fontSize = Math.max(8, Math.min(14, 10 * this.mapState.zoom));
                    ctx.font = (isPlayerLocation ? 'bold ' : '') + fontSize + "px 'MedievalSharp', cursive";
                    ctx.fillStyle = '#ecf0f1';
                    ctx.strokeStyle = '#000';
                    ctx.lineWidth = 2;
                    ctx.textAlign = 'center';
                    ctx.strokeText(loc.name.split('(')[0].trim(), pos.x, pos.y + 15 * this.mapState.zoom);
                    ctx.fillText(loc.name.split('(')[0].trim(), pos.x, pos.y + 15 * this.mapState.zoom);
                });
            }

                        if (typeof World !== 'undefined' && World) {
                // Отрисовка караванов (Желтые маркеры)
                if (World.regions) {
                    Object.values(World.regions).forEach(r => {
                        if (r.caravans) {
                            r.caravans.forEach(c => {
                                if (c.x !== undefined && c.y !== undefined) {
                                    const pos = transform(c.x + 0.5, c.y + 0.5);
                                    if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                                    ctx.fillStyle = '#f1c40f';
                                    ctx.beginPath();
                                    ctx.arc(pos.x, pos.y, 3 * this.mapState.zoom, 0, 2 * Math.PI);
                                    ctx.fill();
                                    ctx.strokeStyle = '#000';
                                    ctx.lineWidth = 1;
                                    ctx.stroke();
                                }
                            });
                        }
                    });
                }
                // Отрисовка армий (Красные маркеры)
                if (World.factions) {
                    Object.values(World.factions).forEach(f => {
                        if (f.armies) {
                            f.armies.forEach(a => {
                                if (a.siegeDays > 0 || (a.current_phase && a.current_phase !== "march")) {
                                    // Армия ведет бой или осаду - рисуем статичный маркер над городом
                                    const end = map.locations[a.destination];
                                    if (end) {
                                        const pos = transform(end.x + 0.5, end.y + 0.5);
                                        if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                                        ctx.fillStyle = '#e74c3c';
                                        ctx.beginPath();
                                        ctx.arc(pos.x + 8 * this.mapState.zoom, pos.y - 8 * this.mapState.zoom, 4 * this.mapState.zoom, 0, 2 * Math.PI);
                                        ctx.fill();
                                        ctx.strokeStyle = '#fff';
                                        ctx.lineWidth = 1;
                                        ctx.stroke();
                                    }
                                } else if (a.x !== undefined && a.y !== undefined) {
                                    // Армия в пути (реальные координаты)
                                    const pos = transform(a.x + 0.5, a.y + 0.5);
                                    if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                                    ctx.fillStyle = '#e74c3c';
                                    ctx.beginPath();
                                    ctx.arc(pos.x, pos.y, 3 * this.mapState.zoom, 0, 2 * Math.PI);
                                    ctx.fill();
                                    ctx.strokeStyle = '#000';
                                    ctx.lineWidth = 1;
                                    ctx.stroke();
                                }
                            });
                        }
                    });
                }
            };



            if (typeof World !== 'undefined' && World) {
                if (World.port_facilities) {
                    Object.keys(World.port_facilities).forEach(rid => {
                        if (map.locations[rid]) {
                            const loc = map.locations[rid];
                            const port = World.port_facilities[rid];
                            const pos = transform(loc.x + 0.5, loc.y + 0.5);
                            if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                            ctx.font = `${12 * this.mapState.zoom}px Arial`;
                            let icons = "⚓";
                            if (port.has_shipyard) icons += " 🏗️";
                            if (port.level > 1) icons += " 🗼";
                            if (port.is_blockaded) icons += " 🚫";
                            ctx.fillText(icons, pos.x + 12 * this.mapState.zoom, pos.y - 12 * this.mapState.zoom);
                        }
                    });
                }
                if (World.ships) {
                    World.ships.forEach(ship => {
                        const pos = transform(ship.x + 0.5, ship.y + 0.5);
                        if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                        ctx.font = `${14 * this.mapState.zoom}px Arial`;
                        let icon = "⛵";
                        if (ship.type === "WAR_GALLEY" || ship.type === "WAR_FRIGATE") icon = "⛴️";
                        if (ship.type === "PIRATE") icon = "🏴‍☠️";
                        if (ship.type === "SEA_MONSTER") icon = "🦑";
                        if (ship.type === "TRANSPORT") icon = "🛶";
                        ctx.fillText(icon, pos.x, pos.y);
                    });
                }

                if (World.fleets) {
                    World.fleets.forEach(fleet => {
                        const pos = transform(fleet.x + 0.5, fleet.y + 0.5);
                        if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;
                        ctx.font = `${18 * this.mapState.zoom}px Arial`;
                        ctx.fillText("⚓🛡️", pos.x, pos.y - 10 * this.mapState.zoom);
                    });
                }

                if (World.monsters) {
                    World.monsters.forEach(m => {
                        if (m.health <= 0 || !m.is_visible_on_map) return;
                        const pos = transform(m.lair_x + 0.5, m.lair_y + 0.5);
                        if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;

                        ctx.save();
                        ctx.globalCompositeOperation = 'screen';
                        const radiusPx = 5 * this.TILE_SIZE * this.mapState.zoom;
                        const pulse = (Math.sin(Date.now() / 300) + 1) / 2;
                        let color1 = `rgba(142, 68, 173, ${0.4 + pulse * 0.2})`;
                        if (m.type === 'DRAGON' || m.type === 'FIRE_ELEMENTAL' || m.type === 'BALOR') color1 = `rgba(231, 76, 60, ${0.4 + pulse * 0.2})`;
                        if (m.type === 'LICH_KING' || m.type === 'VAMPIRE_LORD') color1 = `rgba(44, 62, 80, ${0.6 + pulse * 0.2})`;
                        if (m.type === 'KRAKEN' || m.type === 'LEVIATHAN') color1 = `rgba(41, 128, 185, ${0.4 + pulse * 0.2})`;

                        const grad = ctx.createRadialGradient(pos.x, pos.y, 0, pos.x, pos.y, radiusPx);
                        grad.addColorStop(0, color1);
                        grad.addColorStop(1, 'rgba(0,0,0,0)');
                        ctx.fillStyle = grad;
                        ctx.beginPath();
                        ctx.arc(pos.x, pos.y, radiusPx, 0, Math.PI * 2);
                        ctx.fill();
                        ctx.restore();

                        ctx.font = `${24 * this.mapState.zoom}px Arial`;
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'middle';
                        let icon = '👹';
                        if (m.type === 'DRAGON') icon = '🐉';
                        if (m.type === 'KRAKEN' || m.type === 'LEVIATHAN') icon = '🦑';
                        if (m.type === 'LICH_KING' || m.type === 'VAMPIRE_LORD') icon = '💀';
                        if (m.type === 'FIRE_ELEMENTAL') icon = '🔥';
                        if (m.type === 'BEHOLDER') icon = '👁️';
                        
                        ctx.shadowColor = 'black';
                        ctx.shadowBlur = 5;
                        ctx.fillText(icon, pos.x, pos.y);
                        ctx.shadowBlur = 0;
                        
                        ctx.font = `bold ${11 * this.mapState.zoom}px Arial`;
                        ctx.fillStyle = '#e74c3c';
                        ctx.strokeStyle = '#000';
                        ctx.lineWidth = 2;
                        ctx.strokeText(m.name, pos.x, pos.y + 20 * this.mapState.zoom);
                        ctx.fillText(m.name, pos.x, pos.y + 20 * this.mapState.zoom);
                    });
                }
            }


            if (!this.isMapInitialized && map.locations) {
                let targetId = null;
                if (player.currentSublocation) {
                    const sub = (player.subLocations && player.subLocations[player.currentSublocation]) || 
                                (World && World.subLocations && World.subLocations[player.currentSublocation]);
                    if (sub && sub.parentId) targetId = sub.parentId;
                }

                let playerPoint = (targetId && map.locations[targetId]) ? map.locations[targetId] : null;
                
                if (!playerPoint) {
                    const currentLocName = (player.location || "").toLowerCase().trim();
                    playerPoint = Object.values(map.locations).find(p => p.name.toLowerCase().trim().includes(currentLocName) || currentLocName.includes(p.name.toLowerCase().trim()));
                }

                if (!playerPoint && Object.values(map.locations).length > 0) playerPoint = Object.values(map.locations)[0];

                if (playerPoint) {
                    this.mapState.offsetX = (width / 2) - ((playerPoint.x + 0.5) * this.TILE_SIZE * this.mapState.zoom);
                    this.mapState.offsetY = (height / 2) - ((playerPoint.y + 0.5) * this.TILE_SIZE * this.mapState.zoom);
                    this.isMapInitialized = true;
                    this.animationFrameId = requestAnimationFrame(() => this.render());
                    return;
                }
            }
        }

        if (player.mapMarkers) {
            const transform = (worldX, worldY) => ({
                x: Math.floor((Number(worldX) * this.TILE_SIZE * this.mapState.zoom) + this.mapState.offsetX),
                y: Math.floor((Number(worldY) * this.TILE_SIZE * this.mapState.zoom) + this.mapState.offsetY)
            });
            Object.values(player.mapMarkers).forEach(marker => {
                const pos = transform(marker.x + 0.5, marker.y + 0.5);
                if (pos.x < -50 || pos.x > width + 50 || pos.y < -50 || pos.y > height + 50) return;

                this.drawMapMarker(ctx, pos, 'default', false);
                const fontSize = Math.max(8, Math.min(14, 10 * this.mapState.zoom));
                ctx.font = fontSize + "px 'MedievalSharp', cursive";
                ctx.fillStyle = '#f1c40f';
                ctx.strokeStyle = '#000';
                ctx.lineWidth = 2;
                ctx.textAlign = 'center';
                ctx.strokeText(marker.name, pos.x, pos.y + 15 * this.mapState.zoom);
                ctx.fillText(marker.name, pos.x, pos.y + 15 * this.mapState.zoom);
            });
        }

        this.drawCompassRose(ctx, width - 30, 30, 15);
        this.animationFrameId = requestAnimationFrame(() => this.render());
    },

    /**
     * Обновляет HTML-списки сайдбара (Известные регионы и Открытые места).
     */
    updateSidebar: function() {
        const globalLocationsList = document.getElementById('global-locations-list');
        const customLocationsList = document.getElementById('custom-locations-list');
        if (!globalLocationsList || !customLocationsList) return;

        const mapPanelTitle = document.querySelector('.map-panel .panel-toggle > span:first-child');
        if (mapPanelTitle) mapPanelTitle.textContent = typeof t === 'function' ? t('gameInterface.mapPanel.title') : 'Карта';
        const globalTitle = document.querySelector('.map-panel h3[data-i18n="gameInterface.mapPanel.globalTitle"]');
        const customTitle = document.querySelector('.map-panel h3[data-i18n="gameInterface.mapPanel.customTitle"]');
        if (globalTitle) globalTitle.textContent = typeof t === 'function' ? t('gameInterface.mapPanel.globalTitle') : 'Известные регионы:';
        if (customTitle) customTitle.textContent = typeof t === 'function' ? t('gameInterface.mapPanel.customTitle') : 'Открытые места:';

        globalLocationsList.innerHTML = '';
        const locationsData = (typeof globalLocations === 'object' && globalLocations !== null) ? globalLocations : {};
        const globalKeys = Object.keys(locationsData);
        const displayableGlobalKeys = globalKeys.filter(key => key !== 'startLocation' && locationsData[key]?.name);

        const renderSubLocations = (parentId) => {
            let subs = [];
            if (typeof player !== 'undefined' && player && player.subLocations) {
                subs = subs.concat(Object.values(player.subLocations).filter(sub => sub.parentId === parentId));
            }
            if (typeof World !== 'undefined' && World && World.subLocations) {
                const worldSubs = Object.values(World.subLocations).filter(sub => sub.parentId === parentId && sub.type !== 'house');
                subs = subs.concat(worldSubs);
            }
            if (subs.length === 0) return '';
            let html = '<ul class="location-poi-list">';
            subs.forEach(sub => {
                html += `<li class="location-poi-item" title="${sub.description || ''}">${sub.name}</li>`;
            });
            html += '</ul>';
            return html;
        };

        if (displayableGlobalKeys.length > 0) {
            displayableGlobalKeys.sort((a, b) => (locationsData[a]?.name || '').localeCompare(locationsData[b]?.name || ''));
            displayableGlobalKeys.forEach(key => {
                const loc = locationsData[key];
                const li = document.createElement('li');
                li.innerHTML = `<span class="location-name">${loc.name}</span>`;
                
                let resourcesHtml = '';
                if (typeof World !== 'undefined' && World && World.regions && World.regions[key]) {
                    const reg = World.regions[key];
                    if (reg.available_raw_resources && reg.available_raw_resources.length > 0) {
                        const icons = reg.available_raw_resources.map(r => typeof getResourceIcon === 'function' ? getResourceIcon(r) : "📦").join(' ');
                        resourcesHtml = `<div class="location-resources" style="font-size: 1.0em; margin-top: 4px; margin-left: 10px;" title="Доступное сырье">${icons}</div>`;
                    }
                }
                
                if (loc.description) {
                    li.innerHTML += `<span class="location-desc" title="${loc.description}">${loc.description}</span>`;
                }
                li.innerHTML += resourcesHtml;
                li.innerHTML += renderSubLocations(key);
                globalLocationsList.appendChild(li);
            });
        } else {
            globalLocationsList.innerHTML = `<li>Нет известных регионов</li>`;
        }

        customLocationsList.innerHTML = '';
        if (typeof player !== 'undefined' && player && player.mapMarkers) {
            const markerEntries = Object.entries(player.mapMarkers);
            if (markerEntries.length > 0) {
                markerEntries.sort(([, a], [, b]) => (a.name || '').localeCompare(b.name || ''));
                markerEntries.forEach(([, marker]) => {
                    const li = document.createElement('li');
                    li.innerHTML = `<span class="location-name">${marker.name}</span>`;
                    if (marker.description) {
                        li.innerHTML += `<span class="location-desc" title="${marker.description}">${marker.description}</span>`;
                    }
                    li.innerHTML += renderSubLocations(marker.id);
                    customLocationsList.appendChild(li);
                });
            } else {
                customLocationsList.innerHTML = `<li>Пока ничего не открыто</li>`;
            }
        } else {
            customLocationsList.innerHTML = `<li>Пока ничего не открыто</li>`;
        }
    }


};
