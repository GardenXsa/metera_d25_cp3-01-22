#pragma once

#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <set>
#include <cmath>
#include "world_structures.h"

// Структура региона (входные данные)
struct Region {
    std::string id;
    std::string name;
    int population;
    double moneySupply; // Для расчета wealth_level
    std::string faction_id;
    std::string era;
    int grid_x_start;
    int grid_y_start;
    int grid_width;
    int grid_height;
    bool is_nomadic;
    bool is_ruin;
};

class SettlementGenerator {
public:
    SettlementGenerator(unsigned int seed = std::random_device{}()) 
        : rng(seed) {}

    std::vector<Settlement> generateSettlements(const Region& region) {
        std::vector<Settlement> settlements;

        if (region.population <= 0) {
            return settlements;
        }

        // 1. Расчет количества поселений
        int count = std::max(1, region.population / 2000);

        // 2. Генерация уникальных координат
        std::set<std::pair<int, int>> usedCoords;
        std::uniform_int_distribution<int> distX(region.grid_x_start, region.grid_x_start + region.grid_width - 1);
        std::uniform_int_distribution<int> distY(region.grid_y_start, region.grid_y_start + region.grid_height - 1);

        auto getUniqueCoord = [&]() -> std::pair<int, int> {
            while (true) {
                int x = distX(rng);
                int y = distY(rng);
                if (usedCoords.find({x, y}) == usedCoords.end()) {
                    usedCoords.insert({x, y});
                    return {x, y};
                }
            }
        };

        // 3. Определение типа поселения и общих параметров
        std::string baseType = determineBaseType(region);
        
        // Расчет wealth_level (0..10) на основе moneySupply
        // Нормализуем: предположим, что 1000.0 это максимум для уровня 10
        int wealth = static_cast<int>(std::clamp(region.moneySupply / 100.0, 0.0, 10.0));

        for (int i = 0; i < count; ++i) {
            Settlement s;
            s.id = "sett_" + region.id + "_" + std::to_string(i);
            s.name = region.name + (count > 1 ? " #" + std::to_string(i+1) : "");
            s.population = region.population / count; // Распределяем население равномерно
            s.wealth_level = wealth;
            s.faction_id = region.faction_id;
            s.era = region.era;
            
            auto coord = getUniqueCoord();
            s.coordinates_x = coord.first;
            s.coordinates_y = coord.second;

            // Уточнение типа для конкретного поселения (если нужно разнообразие внутри региона)
            // В базовой версии все поселения региона одного типа, определенного по региону
            s.settlement_type = baseType;

            // Defensive wall
            s.defensive_wall = (s.settlement_type == "town" || s.settlement_type == "city" || s.settlement_type == "fort");

            // Districts
            if (s.settlement_type == "town" || s.settlement_type == "city") {
                s.districts = {"residential", "commercial", "industrial", "religious", "military"};
            }

            settlements.push_back(s);
        }

        return settlements;
    }

private:
    std::mt19937 rng;

    std::string determineBaseType(const Region& region) {
        std::string lowerName = region.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        // Проверка по ключевым словам
        if (lowerName.find("город") != std::string::npos || lowerName.find("city") != std::string::npos) {
            return "city";
        }
        if (lowerName.find("форт") != std::string::npos || lowerName.find("fort") != std::string::npos) {
            return "fort";
        }
        if (region.is_nomadic) {
            return "nomad_camp";
        }
        if (region.is_ruin) {
            return "ruin_outpost";
        }

        // Проверка по численности населения
        int pop = region.population;
        if (pop <= 500) {
            return "hamlet";
        } else if (pop <= 2000) {
            return "village";
        } else if (pop <= 8000) {
            return "town";
        } else {
            return "city";
        }
    }
};
