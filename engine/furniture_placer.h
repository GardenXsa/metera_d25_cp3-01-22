#pragma once

#include "world_structures.h"
#include "catalog_loader.h" // Для доступа к каталогу мебели
#include <vector>
#include <string>
#include <random>
#include <map>

/**
 * Разместитель мебели в комнатах.
 * Использует furniture_catalog.json для выбора типов мебели.
 */
class FurniturePlacer {
public:
    FurniturePlacer(const FurnitureCatalog& catalog, unsigned int seed = std::random_device{}()) 
        : catalog_(catalog), rng_(seed) {}

    /**
     * Расставляет мебель во всех комнатах мира.
     * Логика: для каждого типа комнаты (purpose) есть предпочтительный набор мебели.
     */
    void placeFurnitureInWorld(WorldStructures& world_structures) {
        for (auto& [id, room] : world_structures.rooms) {
            placeFurnitureInRoom(room, world_structures);
        }
    }

private:
    const FurnitureCatalog& catalog_;
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

    /**
     * Возвращает список ID мебели, подходящей для данной цели комнаты.
     */
    std::vector<std::string> getFurnitureForPurpose(const std::string& purpose) {
        // Простая маппинг-логика. В идеале это должно быть в конфиге мебели (теги).
        if (purpose == "bedroom") {
            return {"bed", "chest", "table"};
        }
        if (purpose == "kitchen") {
            return {"table", "chair", "cupboard"};
        }
        if (purpose == "living_room" || purpose == "common_hall") {
            return {"table", "chair", "bench", "fireplace"};
        }
        if (purpose == "dining_room" || purpose == "mess_hall") {
            return {"table", "chair", "bench"};
        }
        if (purpose == "storage" || purpose == "cellar" || purpose == "back_storage") {
            return {"chest", "barrel", "shelf"};
        }
        if (purpose == "prayer_hall" || purpose == "shrine") {
            return {"altar", "bench"};
        }
        if (purpose == "sales_floor") {
            return {"counter", "shelf", "table"};
        }
        if (purpose == "reading_room" || purpose == "archive") {
            return {"table", "chair", "bookshelf"};
        }
        
        // Дефолтный набор
        return {"table", "chair"};
    }

    void placeFurnitureInRoom(const Room& room, WorldStructures& ws) {
        std::vector<std::string> candidates = getFurnitureForPurpose(room.purpose);
        
        // Количество предметов мебели зависит от размера комнаты
        int area = room.width * room.height;
        int items_count = area / 5; // 1 предмет на 5 кв. ед.
        if (items_count < 1) items_count = 1;
        if (items_count > 6) items_count = 6; // Лимит, чтобы не заставлять всё

        // Перемешиваем кандидатов и берем нужное количество
        std::shuffle(candidates.begin(), candidates.end(), rng_);
        
        int placed_count = 0;
        for (const auto& type : candidates) {
            if (placed_count >= items_count) break;

            // Ищем в каталоге объект с таким id или category
            const FurnitureCatalogEntry* item_template = nullptr;
            for (const auto& item : catalog_.items) {
                if (item.id == type || item.category == type) {
                    item_template = &item;
                    break;
                }
            }

            if (!item_template) continue;

            // Создаем экземпляр мебели
            Furniture furn;
            furn.id = generateId("furn");
            furn.room_id = room.id;
            furn.building_id = room.building_id;
            furn.template_id = item_template->id;
            furn.name = item_template->name;
            furn.type = item_template->category;
            
            // Пытаемся разместить в свободном месте комнаты
            // Упрощенная логика: случайные координаты внутри комнаты
            // В полной версии нужен check коллизий
            if (tryPlaceFurniture(furn, room, *item_template, ws)) {
                ws.furniture[furn.id] = furn;
                placed_count++;
            }
        }
    }

    bool tryPlaceFurniture(Furniture& furn, const Room& room, const FurnitureCatalogEntry& tmpl, WorldStructures& ws) {
        // Простая попытка размещения: центр комнаты + случайное смещение
        // Проверка границ комнаты
        int fx = room.x + 1 + generateInt(0, std::max(0, room.width - 2));
        int fy = room.y + 1 + generateInt(0, std::max(0, room.height - 2));
        
        furn.x = fx;
        furn.y = fy;
        furn.z = room.floor_level;
        
        furn.health = tmpl.health;
        furn.weight = tmpl.weight;
        furn.flammable = tmpl.flammable;
        
        // Копируем свойства контейнера если есть
        if (tmpl.is_container()) {
            furn.is_container = true;
            furn.max_weight = tmpl.get_max_weight();
            furn.max_slots = tmpl.get_max_slots();
            furn.lock_difficulty = tmpl.get_lock_difficulty();
            furn.trap_chance = tmpl.get_trap_chance();
            furn.key_id = ""; // Пока пустой, генерируется при луте
        } else {
            furn.is_container = false;
        }

        // Здесь можно добавить проверку: не стоит ли уже мебель в этих координатах
        // Для простоты пропускаем сложную проверку коллизий
        
        return true;
    }
};
