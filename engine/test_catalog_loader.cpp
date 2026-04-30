// Test program for Catalog Loader Module
#include <iostream>
#include "catalog_loader.h"

int main() {
    try {
        std::cout << "=== Testing Furniture Catalog Loader ===" << std::endl;
        
        FurnitureCatalog furnitureCatalog = CatalogLoader::loadFurnitureCatalog("../data/furniture_catalog.json");
        
        std::cout << "Loaded " << furnitureCatalog.entries.size() << " furniture entries:" << std::endl;
        for (const auto& [id, entry] : furnitureCatalog.entries) {
            std::cout << "  - " << entry.name << " (" << id << ")" << std::endl;
            std::cout << "    Category: " << entry.category << std::endl;
            std::cout << "    Health: " << entry.health << ", Weight: " << entry.weight << std::endl;
            std::cout << "    Flammable: " << (entry.flammable ? "yes" : "no") << std::endl;
            
            if (entry.container_props.has_value()) {
                const auto& props = entry.container_props.value();
                std::cout << "    Container: max_weight=" << props.max_weight 
                          << ", max_slots=" << props.max_slots;
                if (props.lock_difficulty > 0) {
                    std::cout << ", lock_difficulty=" << props.lock_difficulty;
                }
                if (props.trap_chance > 0.0) {
                    std::cout << ", trap_chance=" << props.trap_chance;
                }
                std::cout << std::endl;
            } else {
                std::cout << "    Not a container" << std::endl;
            }
            
            std::cout << "    Interactions: ";
            for (size_t i = 0; i < entry.interactions.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << entry.interactions[i];
            }
            std::cout << std::endl;
            
            std::cout << "    Crafting cost: ";
            bool first = true;
            for (const auto& [material, qty] : entry.crafting_cost) {
                if (!first) std::cout << ", ";
                std::cout << material << ":" << qty;
                first = false;
            }
            std::cout << std::endl;
            std::cout << std::endl;
        }
        
        std::cout << "\n=== Testing Material Catalog Loader ===" << std::endl;
        
        MaterialCatalog materialCatalog = CatalogLoader::loadMaterialCatalog("../data/material_catalog.json");
        
        std::cout << "Loaded " << materialCatalog.entries.size() << " material entries:" << std::endl;
        for (const auto& [id, entry] : materialCatalog.entries) {
            std::cout << "  - " << entry.name << " (" << id << ")" << std::endl;
            std::cout << "    Type: " << entry.type << std::endl;
            std::cout << "    Health: " << entry.health << std::endl;
            std::cout << "    Flammable: " << (entry.flammable ? "yes" : "no") << std::endl;
            std::cout << "    Thermal insulation: " << entry.thermal_insulation << std::endl;
            std::cout << std::endl;
        }
        
        std::cout << "=== All tests passed! ===" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
