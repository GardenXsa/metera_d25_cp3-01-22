// World Structure Engine (WSE) - Main Engine Module
// Combines all generators to create complete world structures

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include "world_structures.h"
#include "settlement_generator.h"
#include "building_calculator.h"
#include "building_generator.h"
#include "catalog_loader.h"
#include "room_generator.h"
#include "structure_element_generator.h"
#include "furniture_placer.h"

// Структура региона с facilities для использования в WSE
struct RegionWithFacilities {
    std::string id;
    std::string name;
    int population;
    double moneySupply;
    std::string faction_id;
    std::string era;
    int grid_x_start;
    int grid_y_start;
    int grid_width;
    int grid_height;
    bool is_nomadic;
    bool is_ruin;
    std::unordered_map<std::string, int> facilities;  // facility_type -> level
};

/**
 * Основной класс World Structure Engine.
 * Реализует двухэтапную генерацию:
 * - Этап 1: buildWorldStructures() - архитектурная генерация (пустые контейнеры)
 * - Этап 2: populateContainers() - наполнение контейнеров по состоянию экономики
 */
class WorldStructureEngine {
public:
    WorldStructureEngine(unsigned int seed = std::random_device{}()) 
        : rng(seed), 
          settlementGen(seed),
          buildingGen(seed),
          roomGen(seed),
          elementGen(seed) {}

    /**
     * ЭТАП 1: Архитектурная генерация мира.
     * Создаёт поселения, здания, комнаты, стены, двери, окна, мебель.
     * Контейнеры создаются пустыми.
     * 
     * @param regions Вектор регионов с facilities
     * @return Заполненная структура WorldStructures
     */
    WorldStructures buildWorldStructures(const std::vector<RegionWithFacilities>& regions) {
        WorldStructures world;
        
        for (const auto& region : regions) {
            if (region.population <= 0) {
                continue;
            }

            // 1. Генерация поселений
            Region regionForGen = createRegionFromData(region);
            std::vector<Settlement> settlements = settlementGen.generateSettlements(regionForGen);
            
            for (const auto& settlement : settlements) {
                world.settlements[settlement.id] = settlement;
                
                // 2. Расчёт непроизводственных зданий
                BuildingCountMap buildingCounts = buildingCalc.calculateNonProductionBuildings(settlement);
                
                // 3. Генерация всех зданий
                std::vector<Building> buildings = buildingGen.generateAllBuildings(
                    settlement,
                    region.facilities,
                    buildingCounts
                );
                
                // Добавляем здания в реестр
                for (const auto& building : buildings) {
                    world.buildings[building.id] = building;
                    
                    // 4. Генерация комнат для здания
                    roomGen.generateRoomsForBuilding(building, world);
                }
                
                // 5. Генерация стен, дверей, окон, лестниц
                elementGen.generateElementsForWorld(world);
                
                // 6. Размещение мебели (требуется загруженный каталог)
                // Загружаем каталог если еще не загружен
                if (furnitureCatalog.items.empty()) {
                    furnitureCatalog = CatalogLoader::loadFurnitureCatalog("furniture_catalog.json");
                }
                FurniturePlacer furnPlacer(furnitureCatalog, rng());
                furnPlacer.placeFurnitureInWorld(world);
            }
        }
        
        return world;
    }

    /**
     * ЭТАП 2: Наполнение контейнеров предметами.
     * Наполняет контейнеры ТОЛЬКО теми предметами, которые есть в экономике после preSimulate.
     * 
     * @param world Структура мира из Этапа 1
     * @param economyState Состояние экономики после пресимуляции
     *                     (facility_id -> resource_id -> quantity)
     */
    void populateContainers(
        WorldStructures& world,
        const std::unordered_map<std::string, std::unordered_map<std::string, int>>& economyState
    ) {
        // Проходим по всем контейнерам (мебель с is_container = true)
        for (auto& [furnitureId, furniture] : world.furniture) {
            if (!furniture.is_container) {
                continue;
            }
            
            // Определяем тип контейнера и соответствующий ресурс из экономики
            std::string resourceId = getResourceTypeForContainer(furniture.type);
            
            if (resourceId.empty()) {
                continue;  // Неизвестный тип контейнера
            }
            
            // Ищем facility, к которому относится контейнер
            // (это требует связи furniture -> room -> building -> facility)
            std::string facilityId = getFacilityIdForFurniture(world, furnitureId);
            
            if (facilityId.empty()) {
                continue;  // Контейнер не принадлежит производственному зданию
            }
            
            // Проверяем наличие ресурса в экономике
            auto facilityIt = economyState.find(facilityId);
            if (facilityIt == economyState.end()) {
                continue;  // Facility нет в экономике
            }
            
            auto resourceIt = facilityIt->second.find(resourceId);
            if (resourceIt == facilityIt->second.end() || resourceIt->second <= 0) {
                continue;  // Ресурса нет или количество = 0
            }
            
            // Наполняем контейнер в пределах его вместимости
            int availableQuantity = resourceIt->second;
            int containerCapacity = furniture.max_weight;  // или max_slots
            
            // Здесь должна быть логика добавления предметов в контейнер
            // furniture.items.add(resourceId, std::min(availableQuantity, containerCapacity));
        }
        
        // Также наполняем личные хранилища в жилых домах
        distributePersonalItems(world, economyState);
    }

    /**
     * Экспорт структуры мира в JSON для синхронизации с JS-частью.
     */
    std::string exportToJson(const WorldStructures& world) {
        // Простая реализация без внешней JSON-библиотеки
        // Для продакшена использовать nlohmann/json
        
        std::string json = "{\n";
        
        // Settlements
        json += "  \"settlements\": {\n";
        bool first = true;
        for (const auto& [id, settlement] : world.settlements) {
            if (!first) json += ",\n";
            first = false;
            json += "    \"" + id + "\": {\n";
            json += "      \"name\": \"" + settlement.name + "\",\n";
            json += "      \"type\": \"" + settlement.settlement_type + "\",\n";
            json += "      \"population\": " + std::to_string(settlement.population) + ",\n";
            json += "      \"wealth_level\": " + std::to_string(settlement.wealth_level) + ",\n";
            json += "      \"coordinates\": {\"x\": " + std::to_string(static_cast<int>(settlement.coordinates_x)) 
                         + ", \"y\": " + std::to_string(static_cast<int>(settlement.coordinates_y)) + "}\n";
            json += "    }";
        }
        json += "\n  },\n";
        
        // Buildings (упрощённо)
        json += "  \"buildings\": {\n";
        first = true;
        for (const auto& [id, building] : world.buildings) {
            if (!first) json += ",\n";
            first = false;
            json += "    \"" + id + "\": {\n";
            json += "      \"type\": \"" + building.type + "\",\n";
            json += "      \"settlement_id\": \"" + building.settlement_id + "\"\n";
            json += "    }";
        }
        json += "\n  },\n";
        
        // Rooms
        json += "  \"rooms\": {\n";
        first = true;
        for (const auto& [id, room] : world.rooms) {
            if (!first) json += ",\n";
            first = false;
            json += "    \"" + id + "\": {\n";
            json += "      \"building_id\": \"" + room.building_id + "\",\n";
            json += "      \"floor\": " + std::to_string(room.floor_level) + ",\n";
            json += "      \"purpose\": \"" + room.purpose + "\",\n";
            json += "      \"x\": " + std::to_string(room.x) + ",\n";
            json += "      \"y\": " + std::to_string(room.y) + ",\n";
            json += "      \"width\": " + std::to_string(room.width) + ",\n";
            json += "      \"height\": " + std::to_string(room.height) + "\n";
            json += "    }";
        }
        json += "\n  },\n";
        
        // Furniture
        json += "  \"furniture\": {\n";
        first = true;
        for (const auto& [id, furn] : world.furniture) {
            if (!first) json += ",\n";
            first = false;
            json += "    \"" + id + "\": {\n";
            json += "      \"room_id\": \"" + furn.room_id + "\",\n";
            json += "      \"type\": \"" + furn.type + "\",\n";
            json += "      \"name\": \"" + furn.name + "\"\n";
            json += "    }";
        }
        json += "\n  }\n";
        
        json += "}";
        
        return json;
    }

private:
    std::mt19937 rng;
    SettlementGenerator settlementGen;
    BuildingCalculator buildingCalc;
    BuildingGenerator buildingGen;
    RoomGenerator roomGen;
    StructureElementGenerator elementGen;
    
    FurnitureCatalog furnitureCatalog;
    MaterialCatalog materialCatalog;

    /**
     * Преобразует RegionWithFacilities в Region для settlement generator.
     */
    Region createRegionFromData(const RegionWithFacilities& region) {
        Region r;
        r.id = region.id;
        r.name = region.name;
        r.population = region.population;
        r.moneySupply = region.moneySupply;
        r.faction_id = region.faction_id;
        r.era = region.era;
        r.grid_x_start = region.grid_x_start;
        r.grid_y_start = region.grid_y_start;
        r.grid_width = region.grid_width;
        r.grid_height = region.grid_height;
        r.is_nomadic = region.is_nomadic;
        r.is_ruin = region.is_ruin;
        return r;
    }

    /**
     * Определяет тип ресурса для контейнера по его типу.
     */
    std::string getResourceTypeForContainer(const std::string& containerType) {
        if (containerType == "chest_raw_materials") {
            return "raw_materials";
        }
        if (containerType == "chest_finished_goods") {
            return "finished_goods";
        }
        if (containerType == "personal_storage_poor" || 
            containerType == "personal_storage_rich") {
            return "personal_items";
        }
        return "";
    }

    /**
     * Находит facility_id для мебели, проходя по цепочке связей.
     */
    std::string getFacilityIdForFurniture(
        const WorldStructures& world,
        const std::string& furnitureId
    ) {
        // 1. Найти комнату, которой принадлежит мебель
        std::string roomId;
        for (const auto& [id, furniture] : world.furniture) {
            if (id == furnitureId) {
                roomId = furniture.room_id;
                break;
            }
        }
        
        if (roomId.empty()) {
            return "";
        }
        
        // 2. Найти здание, которому принадлежит комната
        std::string buildingId;
        for (const auto& [id, room] : world.rooms) {
            if (id == roomId) {
                buildingId = room.building_id;
                break;
            }
        }
        
        if (buildingId.empty()) {
            return "";
        }
        
        // 3. Получить facility_id из здания
        auto buildingIt = world.buildings.find(buildingId);
        if (buildingIt == world.buildings.end()) {
            return "";
        }
        
        return buildingIt->second.facility_id;
    }

    /**
     * Распределяет личные вещи по жилым домам.
     */
    void distributePersonalItems(
        WorldStructures& world,
        const std::unordered_map<std::string, std::unordered_map<std::string, int>>& economyState
    ) {
        // Логика распределения личных вещей по персональным хранилищам
        // на основе общего количества населения и wealth_level
        
        // Упрощённая версия - заглушка для будущей реализации
    }
};
