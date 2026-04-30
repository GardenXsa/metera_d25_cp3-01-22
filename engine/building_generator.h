// Building Generator Module for World Structure Engine
// Generates production and non-production buildings with rooms and containers

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <random>
#include "world_structures.h"
#include "settlement_generator.h"
#include "building_calculator.h"

// Структура для описания комнаты в здании
struct RoomTemplate {
    std::string name;
    std::string type;
    double size_x;
    double size_y;
    std::string floor_material;
    double ceiling_height;
    bool has_windows;
    bool is_outdoor;  // для дворов, улиц
    std::vector<std::string> container_types;  // какие контейнеры создать
    std::vector<std::string> furniture_types;  // какая мебель нужна
};

// Структура здания с полным описанием
struct BuildingTemplate {
    std::string type;
    std::vector<RoomTemplate> rooms;
    int floors;
    std::string facility_id;  // пустой если не производственное
};

class BuildingGenerator {
public:
    BuildingGenerator(unsigned int seed = std::random_device{}()) 
        : rng(seed) {}

    /**
     * Генерирует все здания для поселения.
     * 
     * @param settlement Поселение, для которого создаются здания
     * @param facilities Карта Facilities региона (ключ - тип facility, значение - уровень)
     * @param buildingCounts Результат calculateNonProductionBuildings
     * @return Вектор сгенерированных зданий
     */
    std::vector<Building> generateAllBuildings(
        const Settlement& settlement,
        const std::unordered_map<std::string, int>& facilities,
        const BuildingCountMap& buildingCounts
    ) {
        std::vector<Building> buildings;

        // 1. Генерация производственных зданий (по одному на каждый Facility)
        for (const auto& [facilityType, level] : facilities) {
            Building prodBuilding = createProductionBuilding(facilityType, level, settlement);
            buildings.push_back(prodBuilding);
        }

        // 2. Генерация непроизводственных зданий по расчётам
        // Жилые дома
        if (buildingCounts.count("house_poor")) {
            for (int i = 0; i < buildingCounts.at("house_poor"); ++i) {
                buildings.push_back(createHouse("poor", settlement));
            }
        }
        if (buildingCounts.count("house_mid")) {
            for (int i = 0; i < buildingCounts.at("house_mid"); ++i) {
                buildings.push_back(createHouse("mid", settlement));
            }
        }
        if (buildingCounts.count("house_rich")) {
            for (int i = 0; i < buildingCounts.at("house_rich"); ++i) {
                buildings.push_back(createHouse("rich", settlement));
            }
        }

        // Таверны
        if (buildingCounts.count("tavern")) {
            for (int i = 0; i < buildingCounts.at("tavern"); ++i) {
                buildings.push_back(createTavern(settlement));
            }
        }

        // Храмы
        if (buildingCounts.count("temple") && buildingCounts.at("temple") > 0) {
            for (int i = 0; i < buildingCounts.at("temple"); ++i) {
                buildings.push_back(createTemple(settlement));
            }
        }

        // Ратуша
        if (buildingCounts.count("town_hall") && buildingCounts.at("town_hall") > 0) {
            for (int i = 0; i < buildingCounts.at("town_hall"); ++i) {
                buildings.push_back(createTownHall(settlement));
            }
        }

        // Рынки
        if (buildingCounts.count("market")) {
            for (int i = 0; i < buildingCounts.at("market"); ++i) {
                buildings.push_back(createMarket(settlement));
            }
        }

        // Казармы
        if (buildingCounts.count("barracks") && buildingCounts.at("barracks") > 0) {
            for (int i = 0; i < buildingCounts.at("barracks"); ++i) {
                buildings.push_back(createBarracks(settlement));
            }
        }

        // Школы/библиотеки
        if (buildingCounts.count("school")) {
            for (int i = 0; i < buildingCounts.at("school"); ++i) {
                buildings.push_back(createSchool(settlement));
            }
        }
        if (buildingCounts.count("library")) {
            for (int i = 0; i < buildingCounts.at("library"); ++i) {
                buildings.push_back(createLibrary(settlement));
            }
        }

        // Лечебницы
        if (buildingCounts.count("clinic")) {
            for (int i = 0; i < buildingCounts.at("clinic"); ++i) {
                buildings.push_back(createClinic(settlement));
            }
        }

        // Склады
        if (buildingCounts.count("warehouse")) {
            for (int i = 0; i < buildingCounts.at("warehouse"); ++i) {
                buildings.push_back(createWarehouse(settlement));
            }
        }

        // Тюрьмы
        if (buildingCounts.count("prison") && buildingCounts.at("prison") > 0) {
            for (int i = 0; i < buildingCounts.at("prison"); ++i) {
                buildings.push_back(createPrison(settlement));
            }
        }

        // Гостиницы
        if (buildingCounts.count("inn")) {
            for (int i = 0; i < buildingCounts.at("inn"); ++i) {
                buildings.push_back(createInn(settlement));
            }
        }

        return buildings;
    }

private:
    std::mt19937 rng;
    int buildingCounter = 0;

    std::string generateBuildingId(const std::string& type) {
        return "bld_" + type + "_" + std::to_string(++buildingCounter);
    }

    /**
     * Создаёт производственное здание по типу Facility.
     * Согласно Т3 раздел 5.1 - полный перечень производственных зданий.
     */
    Building createProductionBuilding(
        const std::string& facilityType, 
        int level,
        const Settlement& settlement
    ) {
        Building building;
        building.id = generateBuildingId("prod");
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.floors = 1;  // Большинство производственных одноэтажные

        std::vector<RoomTemplate> rooms;

        // Определение типа здания и комнат по типу facility
        if (facilityType == "farms") {
            building.type = "farmstead";
            building.facility_id = "facility_farms_" + settlement.id;
            rooms = {
                {"Жилой дом фермера", "residential", 8.0, 6.0, "floor_wood_planks", 2.5, true, false, {"personal_storage_poor"}, {"wooden_bed"}},
                {"Амбар", "storage", 12.0, 8.0, "floor_dirt", 3.0, false, false, {"chest_raw_materials", "chest_finished_goods"}, {}},
                {"Хлев", "livestock", 10.0, 6.0, "floor_dirt", 2.5, false, true, {}, {}},
                {"Птичник", "livestock", 4.0, 3.0, "floor_dirt", 2.0, false, true, {}, {}},
                {"Двор", "outdoor", 15.0, 12.0, "floor_dirt", 0.0, false, true, {}, {"stone_well"}}
            };
        }
        else if (facilityType == "mills") {
            building.type = "mill";
            building.facility_id = "facility_mills_" + settlement.id;
            rooms = {
                {"Жерновой зал", "production", 10.0, 8.0, "floor_stone_slabs", 3.5, true, false, {}, {}},
                {"Склад муки", "storage", 8.0, 6.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "bakeries") {
            building.type = "bakery";
            building.facility_id = "facility_bakeries_" + settlement.id;
            rooms = {
                {"Пекарня", "production", 10.0, 7.0, "floor_stone_slabs", 3.0, true, false, {}, {}},
                {"Комната расстойки", "production", 6.0, 5.0, "floor_wood_planks", 2.5, false, false, {}, {}}
            };
        }
        else if (facilityType == "lumbermills") {
            building.type = "lumber_mill";
            building.facility_id = "facility_lumbermills_" + settlement.id;
            rooms = {
                {"Площадка с пилорамой", "production", 15.0, 10.0, "floor_dirt", 4.0, false, true, {}, {}},
                {"Склад брёвен", "storage", 12.0, 8.0, "floor_dirt", 3.0, false, true, {"chest_raw_materials"}, {}},
                {"Склад досок", "storage", 10.0, 6.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "mines") {
            building.type = "mine";
            building.facility_id = "facility_mines_" + settlement.id;
            rooms = {
                {"Надшахтное здание", "entrance", 8.0, 6.0, "floor_stone_slabs", 2.5, true, false, {}, {}},
                {"Штольня 1", "tunnel", 20.0, 3.0, "floor_dirt", 2.0, false, false, {}, {}},
                {"Штольня 2", "tunnel", 15.0, 3.0, "floor_dirt", 2.0, false, false, {}, {}},
                {"Склад руды", "storage", 8.0, 6.0, "floor_dirt", 2.5, false, false, {"chest_raw_materials"}, {}},
                {"Склад породы", "storage", 6.0, 5.0, "floor_dirt", 2.0, false, true, {}, {}}
            };
        }
        else if (facilityType == "smelters") {
            building.type = "smelter";
            building.facility_id = "facility_smelters_" + settlement.id;
            rooms = {
                {"Плавильный цех", "production", 12.0, 10.0, "floor_stone_slabs", 4.0, true, false, {}, {}},
                {"Разливочная", "production", 8.0, 6.0, "floor_stone_slabs", 3.0, false, false, {}, {}},
                {"Склад руды и угля", "storage", 10.0, 8.0, "floor_dirt", 2.5, false, false, {"chest_raw_materials"}, {}},
                {"Склад слитков", "storage", 8.0, 6.0, "floor_stone_slabs", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "forges") {
            building.type = "smithy";
            building.facility_id = "facility_forges_" + settlement.id;
            rooms = {
                {"Мастерская", "production", 10.0, 8.0, "floor_stone_slabs", 3.5, true, false, {}, {}},
                {"Склад сырья", "storage", 6.0, 5.0, "floor_wood_planks", 2.5, false, false, {"chest_raw_materials"}, {}},
                {"Склад готовой продукции", "storage", 6.0, 5.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}},
                {"Комната мастера", "residential", 5.0, 4.0, "floor_wood_planks", 2.5, true, false, {"personal_storage_rich"}, {"wooden_bed"}}
            };
        }
        else if (facilityType == "weavers") {
            building.type = "weavery";
            building.facility_id = "facility_weavers_" + settlement.id;
            rooms = {
                {"Прядильный цех", "production", 12.0, 8.0, "floor_wood_planks", 3.0, true, false, {}, {}},
                {"Склад ткани", "storage", 8.0, 6.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "tailors") {
            building.type = "tailor_shop";
            building.facility_id = "facility_tailors_" + settlement.id;
            rooms = {
                {"Швейный цех", "production", 10.0, 7.0, "floor_wood_planks", 3.0, true, false, {}, {}},
                {"Примерочная", "service", 4.0, 3.0, "floor_wood_planks", 2.5, true, false, {}, {}},
                {"Склад одежды", "storage", 6.0, 5.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "smokehouses") {
            building.type = "smokehouse";
            building.facility_id = "facility_smokehouses_" + settlement.id;
            rooms = {
                {"Разделочная", "production", 8.0, 6.0, "floor_stone_slabs", 3.0, false, false, {}, {}},
                {"Коптильня", "production", 5.0, 4.0, "floor_stone_slabs", 2.5, false, false, {}, {}},
                {"Склад мяса", "storage", 6.0, 5.0, "floor_stone_slabs", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "alchemists") {
            building.type = "alchemy_lab";
            building.facility_id = "facility_alchemists_" + settlement.id;
            rooms = {
                {"Лаборатория", "production", 10.0, 8.0, "floor_stone_slabs", 3.0, true, false, {}, {}},
                {"Склад ингредиентов", "storage", 6.0, 5.0, "floor_wood_planks", 2.5, false, false, {"chest_raw_materials"}, {}},
                {"Склад зелий", "storage", 5.0, 4.0, "floor_wood_planks", 2.5, false, false, {"chest_finished_goods"}, {}}
            };
        }
        else if (facilityType == "jewelers") {
            building.type = "jewelry_workshop";
            building.facility_id = "facility_jewelers_" + settlement.id;
            rooms = {
                {"Мастерская", "production", 8.0, 6.0, "floor_wood_planks", 3.0, true, false, {}, {}},
                {"Сейф", "secure_storage", 3.0, 3.0, "floor_stone_slabs", 2.5, false, false, {"chest_finished_goods"}, {}}  // сейф будет помечен как запертый
            };
        }
        else if (facilityType == "banks") {
            building.type = "bank";
            building.facility_id = "facility_banks_" + settlement.id;
            rooms = {
                {"Операционный зал", "service", 12.0, 10.0, "floor_marble", 3.5, true, false, {}, {}},
                {"Хранилище", "secure_storage", 8.0, 6.0, "floor_stone_slabs", 3.0, false, false, {"chest_finished_goods"}, {}},  // запертое
                {"Кабинет управляющего", "office", 6.0, 5.0, "floor_wood_planks", 2.8, true, false, {"personal_storage_rich"}, {}},
                {"Комната охраны", "guard", 5.0, 4.0, "floor_stone_slabs", 2.5, false, false, {}, {}}
            };
        }
        else {
            // Неизвестный тип facility - создаём универсальное здание
            building.type = "generic_production";
            building.facility_id = "facility_" + facilityType + "_" + settlement.id;
            rooms = {
                {"Производственный цех", "production", 12.0, 8.0, "floor_dirt", 3.0, true, false, {"chest_raw_materials", "chest_finished_goods"}, {}},
                {"Склад", "storage", 8.0, 6.0, "floor_dirt", 2.5, false, false, {}, {}}
            };
        }

        // Масштабирование размеров комнат от уровня Facility
        float sizeMultiplier = 1.0f + (level - 1) * 0.2f;
        for (auto& room : rooms) {
            room.size_x *= sizeMultiplier;
            room.size_y *= sizeMultiplier;
        }

        building.floors = 1;
        
        // Здесь должна быть логика создания комнат, стен и т.д.
        // Пока возвращаем базовую структуру
        
        return building;
    }

    /**
     * Создаёт жилой дом указанного уровня богатства.
     */
    Building createHouse(const std::string& wealthClass, const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("house");
        building.type = "house_" + wealthClass;
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        std::vector<RoomTemplate> rooms;

        if (wealthClass == "poor") {
            rooms = {
                {"Бедная хижина", "residential", 5.0, 4.0, "floor_dirt", 2.2, false, false, {"personal_storage_poor"}, {"wooden_bed"}}
            };
        }
        else if (wealthClass == "mid") {
            rooms = {
                {"Жилая комната", "residential", 6.0, 5.0, "floor_wood_planks", 2.5, true, false, {"personal_storage_poor"}, {"wooden_bed"}},
                {"Кухня", "kitchen", 4.0, 3.0, "floor_dirt", 2.3, false, false, {}, {}}
            };
        }
        else {  // rich
            rooms = {
                {"Гостиная", "residential", 8.0, 6.0, "floor_wood_planks", 2.8, true, false, {"personal_storage_rich"}, {}},
                {"Спальня", "bedroom", 6.0, 5.0, "floor_wood_planks", 2.6, true, false, {"personal_storage_rich"}, {"wooden_bed"}},
                {"Кухня", "kitchen", 5.0, 4.0, "floor_stone_slabs", 2.5, true, false, {}, {}},
                {"Кладовая", "storage", 3.0, 3.0, "floor_wood_planks", 2.3, false, false, {"personal_storage_rich"}, {}}
            };
        }

        return building;
    }

    Building createTavern(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("tavern");
        building.type = "tavern";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 2;

        //Rooms будут созданы room_generator'ом
        
        return building;
    }

    Building createTemple(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("temple");
        building.type = "temple";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createTownHall(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("townhall");
        building.type = "town_hall";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 2;

        return building;
    }

    Building createMarket(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("market");
        building.type = "market";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createBarracks(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("barracks");
        building.type = "barracks";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createSchool(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("school");
        building.type = "school";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createLibrary(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("library");
        building.type = "library";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createClinic(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("clinic");
        building.type = "clinic";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createWarehouse(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("warehouse");
        building.type = "warehouse";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createPrison(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("prison");
        building.type = "prison";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 1;

        return building;
    }

    Building createInn(const Settlement& settlement) {
        Building building;
        building.id = generateBuildingId("inn");
        building.type = "inn";
        building.settlement_id = settlement.id;
        building.wealth_level = settlement.wealth_level;
        building.era = settlement.era;
        building.facility_id = "";
        building.floors = 2;

        return building;
    }
};
