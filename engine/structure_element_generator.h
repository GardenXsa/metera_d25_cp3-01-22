#pragma once

#include "world_structures.h"
#include <vector>
#include <string>
#include <random>
#include <set>

/**
 * Генератор статических элементов зданий: стен, дверей, окон, лестниц.
 */
class StructureElementGenerator {
public:
    StructureElementGenerator(unsigned int seed = std::random_device{}()) 
        : rng_(seed) {}

    /**
     * Генерирует стены, двери, окна и лестницы для всех комнат в реестре.
     * Предполагается, что комнаты уже сгенерированы и привязаны к зданиям.
     */
    void generateElementsForWorld(WorldStructures& world_structures) {
        // Группируем комнаты по зданиям для удобства
        // В реальном коде можно оптимизировать, но для ясности сделаем перебор
        
        // 1. Генерация стен (WallSegment)
        // Стены определяются периметром комнат. Смежные стены объединяются или считаются отдельно?
        // Для простоты: каждая комната имеет 4 стены. Если стены совпадают координатами с соседом - это общая стена.
        // Здесь реализуем упрощенную модель: стены по периметру комнаты.
        
        for (auto& [id, room] : world_structures.rooms) {
            generateWallsForRoom(room, world_structures);
            generateDoorsForRoom(room, world_structures);
            generateWindowsForRoom(room, world_structures);
        }

        // 2. Генерация лестниц (между этажами одного здания)
        generateStairsForWorld(world_structures);
    }

private:
    std::mt19937 rng_;

    int generateInt(int min_val, int max_val) {
        if (min_val >= max_val) return min_val;
        std::uniform_int_distribution<> dist(min_val, max_val);
        return dist(rng_);
    }

    float generateFloat() {
        std::uniform_real_distribution<> dist(0.0f, 1.0f);
        return dist(rng_);
    }

    std::string generateId(const std::string& prefix) {
        static uint64_t counter = 0;
        return prefix + "_" + std::to_string(++counter);
    }

    void generateWallsForRoom(const Room& room, WorldStructures& ws) {
        // Создаем 4 сегмента стен для каждой комнаты
        // North, East, South, West
        
        // North Wall (y)
        createWallSegment(room, "north", room.width, 0, ws);
        // East Wall (x + width)
        createWallSegment(room, "east", 0, room.height, ws);
        // South Wall (y + height)
        createWallSegment(room, "south", room.width, 0, ws);
        // West Wall (x)
        createWallSegment(room, "west", 0, room.height, ws);
    }

    void createWallSegment(const Room& room, const std::string& side, int len_x, int len_y, WorldStructures& ws) {
        WallSegment wall;
        wall.id = generateId("wall");
        wall.room_id = room.id;
        wall.building_id = room.building_id;
        wall.material_id = "stone"; // По умолчанию, можно брать из конфига здания
        
        // Расчет координат и длины в зависимости от стороны
        if (side == "north") {
            wall.x = room.x;
            wall.y = room.y;
            wall.z = room.floor_level; // Уровень этажа
            wall.length = len_x;
            wall.axis = "x";
            wall.thickness = 1;
            wall.height = 3; // Стандартная высота стены
        } else if (side == "south") {
            wall.x = room.x;
            wall.y = room.y + room.height;
            wall.z = room.floor_level;
            wall.length = len_x;
            wall.axis = "x";
            wall.thickness = 1;
            wall.height = 3;
        } else if (side == "east") {
            wall.x = room.x + room.width;
            wall.y = room.y;
            wall.z = room.floor_level;
            wall.length = len_y;
            wall.axis = "y";
            wall.thickness = 1;
            wall.height = 3;
        } else if (side == "west") {
            wall.x = room.x;
            wall.y = room.y;
            wall.z = room.floor_level;
            wall.length = len_y;
            wall.axis = "y";
            wall.thickness = 1;
            wall.height = 3;
        }

        wall.health = 100;
        wall.is_destroyed = false;

        ws.wallSegments[wall.id] = wall;
    }

    void generateDoorsForRoom(const Room& room, WorldStructures& ws) {
        // Логика размещения дверей:
        // 1. Если комната имеет выход наружу (граничит с границей здания или считается внешней) - нужна дверь.
        // 2. Если комната граничит с другой комнатой - нужна внутренняя дверь.
        // Упрощение: ставим 1 дверь случайно на любой стене, если это не склад/подвал.
        
        if (room.purpose == "cellar" || room.purpose == "storage") {
            // В подвалах и складах дверей может не быть или они специфичны, пропускаем для простоты
            // или ставим одну скрытую.
            if (generateFloat() > 0.5) return; 
        }

        // Выбираем случайную стену для двери
        std::vector<std::string> sides = {"north", "south", "east", "west"};
        std::string door_side = sides[generateInt(0, 3)];
        
        Door door;
        door.id = generateId("door");
        door.room_id = room.id;
        door.building_id = room.building_id;
        door.is_open = false;
        door.is_locked = false;
        door.lock_difficulty = 0;
        
        // Позиционирование двери в центре выбранной стены (со смещением)
        int offset = 1; // Отступ от угла
        if (door_side == "north" || door_side == "south") {
            door.x = room.x + (room.width / 2);
            door.y = (door_side == "north") ? room.y : room.y + room.height;
            door.z = room.floor_level;
            door.axis = "x"; // Дверь в стене вдоль X
        } else {
            door.x = (door_side == "east") ? room.x + room.width : room.x;
            door.y = room.y + (room.height / 2);
            door.z = room.floor_level;
            door.axis = "y";
        }

        ws.doors[door.id] = door;
    }

    void generateWindowsForRoom(const Room& room, WorldStructures& ws) {
        // Окна только во внешних стенах. 
        // Считаем, что внешние стены - те, что совпадают с габаритами здания (эту инфу надо бы передавать).
        // Упрощение: ставим окно с вероятностью 30% на каждую стену, если это не подвал.
        
        if (room.floor_level <= 0) return; // Нет окон в подвале
        if (room.purpose == "cellar") return;

        std::vector<std::string> sides = {"north", "south", "east", "west"};
        
        for (const auto& side : sides) {
            if (generateFloat() < 0.4f) { // 40% шанс окна на стену
                Window win;
                win.id = generateId("window");
                win.room_id = room.id;
                win.building_id = room.building_id;
                win.is_open = false;
                win.material_id = "glass";
                
                if (side == "north" || side == "south") {
                    win.x = room.x + (room.width / 2);
                    win.y = (side == "north") ? room.y : room.y + room.height;
                    win.z = room.floor_level + 1; // Чуть выше пола
                    win.axis = "x";
                } else {
                    win.x = (side == "east") ? room.x + room.width : room.x;
                    win.y = room.y + (room.height / 2);
                    win.z = room.floor_level + 1;
                    win.axis = "y";
                }
                
                ws.windows[win.id] = win;
            }
        }
    }

    void generateStairsForWorld(WorldStructures& ws) {
        // Находим здания с несколькими этажами
        // Группируем комнаты по building_id
        std::map<std::string, std::vector<Room*>> building_rooms;
        for (auto& [id, room] : ws.rooms) {
            building_rooms[room.building_id].push_back(&room);
        }

        for (auto& [b_id, rooms] : building_rooms) {
            if (rooms.empty()) continue;
            
            // Находим макс этаж
            int max_floor = 0;
            for (const auto* r : rooms) {
                if (r->floor_level > max_floor) max_floor = r->floor_level;
            }

            if (max_floor < 2) continue; // Лестницы не нужны для одноэтажек

            // Для каждого перехода между этажами ставим лестницу
            // Пытаемся найти комнату на этаже N и поставить лестницу, ведущую на N+1
            // Упрощение: ставим одну лестницу в здании, привязанную к первой найденной комнате на каждом этаже
            
            // Сортируем комнаты по этажу
            std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* b){
                return a->floor_level < b->floor_level;
            });

            // Берем первую комнату как точку привязки для лестницы (холл)
            // В идеале нужно искать комнату с тегом "stairwell" или "corridor"
            const Room* base_room = rooms[0]; 
            
            for (int f = 1; f < max_floor; ++f) {
                Stairs stairs;
                stairs.id = generateId("stairs");
                stairs.building_id = b_id;
                // Лестница соединяет этаж f и f+1
                // Размещаем её в координатах базовой комнаты (или рядом)
                stairs.x = base_room->x + 1; 
                stairs.y = base_room->y + 1;
                stairs.z = f; // Начинается на этаже f
                stairs.direction = "up";
                stairs.steps_count = 10; // Примерно
                
                ws.stairs[stairs.id] = stairs;
            }
        }
    }
};
