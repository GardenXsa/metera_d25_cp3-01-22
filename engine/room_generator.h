#pragma once

#include "world_structures.h"
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <cmath>

// Конфигурация генерации комнат
struct RoomConfig {
    int min_width = 3;
    int min_height = 3;
    int max_aspect_ratio = 3; // Чтобы комнаты не были слишком узкими
};

/**
 * Генератор комнат для зданий.
 * Отвечает за разбиение габаритов здания на отдельные помещения (Room).
 */
class RoomGenerator {
public:
    RoomGenerator(unsigned int seed = std::random_device{}()) 
        : rng_(seed) {}

    /**
     * Генерирует список комнат для заданного здания.
     * @param building Здание, для которого генерируются комнаты.
     * @param world_structures Реестр для сохранения созданных комнат.
     * @return Вектор ID созданных комнат.
     */
    std::vector<std::string> generateRoomsForBuilding(
        const Building& building, 
        WorldStructures& world_structures,
        const RoomConfig& config = RoomConfig()
    ) {
        std::vector<std::string> room_ids;
        
        int b_width = building.width;
        int b_height = building.height;
        int b_floors = building.floors;

        // Минимальный размер здания
        if (b_width < config.min_width || b_height < config.min_height) {
            // Если здание слишком маленькое, создаем одну комнату-студию
            Room room;
            room.id = generateId("room");
            room.building_id = building.id;
            room.floor_level = 1;
            room.x = 0;
            room.y = 0;
            room.width = b_width;
            room.height = b_height;
            room.purpose = determineRoomPurpose(building.type, 0);
            
            world_structures.rooms[room.id] = room;
            room_ids.push_back(room.id);
            return room_ids;
        }

        // Генерация по этажам
        for (int floor = 1; floor <= b_floors; ++floor) {
            // Список областей для деления (начинаем с одного большого пространства)
            struct Area { int x, y, w, h; };
            std::vector<Area> areas;
            areas.push_back({0, 0, b_width, b_height});

            // Итеративное деление пространства
            // Делим до тех пор, пока комнаты не станут слишком маленькими
            int iterationCount = 0;
            bool changed = true;
            while (changed && iterationCount < 15) {  // Ограничено 15 итерациями для безопасности
                changed = false;
                iterationCount++;
                std::vector<Area> next_areas;
                
                for (const auto& area : areas) {
                    // Пытаемся разделить область, если она достаточно большая
                    bool split = false;
                    
                    // Шанс деления зависит от размера. Большие делим чаще.
                    float split_chance = 0.4f; 
                    if (area.w > config.min_width * 3 && area.h > config.min_height * 3) {
                        if (generateFloat() < split_chance) split = true;
                    } else if (area.w > config.min_width * 5 || area.h > config.min_height * 5) {
                         // Если очень длинная/широкая, делим обязательно
                         split = true;
                    }

                    if (split) {
                        changed = true;
                        // Выбираем направление: 0 - вертикально, 1 - горизонтально
                        // Предпочитаем деление перпендикулярно большей стороне
                        int direction = (area.w > area.h) ? 0 : 1;
                        if (area.w == area.h) direction = (generateInt(0, 1));

                        int split_pos = 0;
                        if (direction == 0) { // Вертикальный разрез (делим ширину)
                            // Оставляем минимум с обеих сторон
                            int min_w = config.min_width;
                            if (area.w >= min_w * 2 + 1) {
                                split_pos = min_w + generateInt(0, area.w - min_w * 2);
                                
                                // Проверка аспекта
                                int h1 = area.h;
                                int w1 = split_pos;
                                int w2 = area.w - split_pos;
                                
                                if (w1 > h1 * config.max_aspect_ratio) split_pos = h1 * config.max_aspect_ratio;
                                if (w2 > h1 * config.max_aspect_ratio) split_pos = area.w - (h1 * config.max_aspect_ratio);
                                
                                if (split_pos < min_w) split_pos = min_w;
                                if (split_pos > area.w - min_w) split_pos = area.w - min_w;

                                if (split_pos >= min_w && split_pos <= area.w - min_w) {
                                    next_areas.push_back({area.x, area.y, split_pos, area.h});
                                    next_areas.push_back({area.x + split_pos, area.y, area.w - split_pos, area.h});
                                } else {
                                    next_areas.push_back(area);
                                }
                            } else {
                                next_areas.push_back(area);
                            }
                        } else { // Горизонтальный разрез (делим высоту)
                            int min_h = config.min_height;
                            if (area.h >= min_h * 2 + 1) {
                                split_pos = min_h + generateInt(0, area.h - min_h * 2);
                                
                                int w = area.w;
                                int h1 = split_pos;
                                int h2 = area.h - split_pos;

                                if (h1 > w * config.max_aspect_ratio) split_pos = w * config.max_aspect_ratio;
                                if (h2 > w * config.max_aspect_ratio) split_pos = area.h - (w * config.max_aspect_ratio);

                                if (split_pos < min_h) split_pos = min_h;
                                if (split_pos > area.h - min_h) split_pos = area.h - min_h;
                                
                                if (split_pos >= min_h && split_pos <= area.h - min_h) {
                                    next_areas.push_back({area.x, area.y, area.w, split_pos});
                                    next_areas.push_back({area.x, area.y + split_pos, area.w, area.h - split_pos});
                                } else {
                                    next_areas.push_back(area);
                                }
                            } else {
                                next_areas.push_back(area);
                            }
                        }
                    } else {
                        next_areas.push_back(area);
                    }
                }
                areas = next_areas;
            }

            // Преобразуем области в комнаты
            for (const auto& area : areas) {
                // Пропускаем области с некорректными размерами
                if (area.w < config.min_width || area.h < config.min_height) {
                    continue;
                }
                
                Room room;
                room.id = generateId("room");
                room.building_id = building.id;
                room.floor_level = floor;
                room.x = area.x;
                room.y = area.y;
                room.width = area.w;
                room.height = area.h;
                
                // Назначаем назначение комнаты на основе типа здания и случайности/размера
                room.purpose = determineRoomPurpose(building.type, generateFloat());
                
                // Дополнительные свойства (заглушки для будущего наполнения)
                room.light_level = 5; // Стандартное освещение
                room.is_outdoor = false;

                world_structures.rooms[room.id] = room;
                room_ids.push_back(room.id);
            }
        }

        return room_ids;
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
        return prefix + "_" + std::to_string(++counter); // В реальном проекте нужен UUID или хэш
    }

    /**
     * Определяет назначение комнаты на основе типа здания.
     * @param building_type Тип здания (например, "tavern", "house")
     * @param rand Случайное число 0..1 для вариативности внутри типа
     */
    std::string determineRoomPurpose(const std::string& building_type, float rand) {
        // Простая эвристика. В будущем можно использовать веса.
        
        if (building_type.find("house") != std::string::npos || 
            building_type.find("residential") != std::string::npos) {
            if (rand < 0.4) return "bedroom";
            if (rand < 0.7) return "living_room";
            if (rand < 0.85) return "kitchen";
            return "storage";
        }
        
        if (building_type.find("tavern") != std::string::npos) {
            if (rand < 0.5) return "common_hall";
            if (rand < 0.7) return "guest_room";
            if (rand < 0.85) return "kitchen";
            return "cellar";
        }

        if (building_type.find("temple") != std::string::npos) {
            if (rand < 0.6) return "prayer_hall";
            if (rand < 0.8) return "shrine";
            return "priest_quarters";
        }

        if (building_type.find("barracks") != std::string::npos) {
            if (rand < 0.7) return "sleeping_quarters";
            if (rand < 0.9) return "armory";
            return "mess_hall";
        }

        if (building_type.find("shop") != std::string::npos || 
            building_type.find("market") != std::string::npos) {
            if (rand < 0.6) return "sales_floor";
            return "back_storage";
        }

        if (building_type.find("library") != std::string::npos) {
            if (rand < 0.8) return "reading_room";
            return "archive";
        }

        // По умолчанию
        if (rand < 0.5) return "generic_room";
        return "corridor";
    }
};
