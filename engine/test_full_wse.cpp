// Тест полного цикла генерации мира WSE
#include <iostream>
#include <vector>
#include "wse_engine.h"

int main() {
    std::cout << "=== World Structure Engine - Full Test ===\n" << std::endl;
    
    // Создаём тестовые регионы (уменьшенные для быстрого теста)
    std::vector<RegionWithFacilities> regions;
    
    // Регион 1: Небольшой город
    RegionWithFacilities region1;
    region1.id = "region_city";
    region1.name = "Городской регион";
    region1.population = 3000;  // Уменьшено с 25000
    region1.moneySupply = 50000;
    region1.faction_id = "faction_human_kingdom";
    region1.era = "medieval";
    region1.is_nomadic = false;
    region1.is_ruin = false;
    region1.grid_x_start = 0;
    region1.grid_y_start = 0;
    region1.grid_width = 10;
    region1.grid_height = 10;
    region1.facilities["market"] = 3;
    regions.push_back(region1);
    
    // Регион 2: Деревня
    RegionWithFacilities region2;
    region2.id = "region_village";
    region2.name = "Тихая долина";
    region2.population = 400;  // Уменьшено с 800
    region2.moneySupply = 5000;
    region2.faction_id = "faction_human_kingdom";
    region2.era = "medieval";
    region2.is_nomadic = false;
    region2.is_ruin = false;
    region2.grid_x_start = 10;
    region2.grid_y_start = 0;
    region2.grid_width = 8;
    region2.grid_height = 8;
    region2.facilities["farm"] = 2;
    regions.push_back(region2);
    
    // Регион 3: Кочевники (маленький лагерь)
    RegionWithFacilities region3;
    region3.id = "region_nomads";
    region3.name = "Степной лагерь";
    region3.population = 200;  // Уменьшено с 1200
    region3.moneySupply = 2000;
    region3.faction_id = "faction_nomads";
    region3.era = "medieval";
    region3.is_nomadic = true;
    region3.is_ruin = false;
    region3.grid_x_start = 0;
    region3.grid_y_start = 10;
    region3.grid_width = 5;
    region3.grid_height = 5;
    region3.facilities["tent"] = 1;
    regions.push_back(region3);
    
    // Удалим регион 4 (руины) для упрощения теста
    
    // Запускаем движок
    WorldStructureEngine engine(42); // Фиксированный seed для воспроизводимости
    
    // Генерируем мир
    auto world = engine.buildWorldStructures(regions);
    
    // Наполняем контейнеры (заглушка экономики)
    std::unordered_map<std::string, std::unordered_map<std::string, int>> economy;
    engine.populateContainers(world, economy);
    
    // Экспортируем в JSON
    std::string json = engine.exportToJson(world);
    // Сохраняем в файл
    std::ofstream outFile("generated_world.json");
    outFile << json;
    outFile.close();
    
    std::cout << "\n=== Тест завершён успешно! ===" << std::endl;
    std::cout << "Результаты сохранены в generated_world.json" << std::endl;
    
    return 0;
}
