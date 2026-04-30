// Catalog Loader Module for World Structure Engine
// Loads furniture_catalog.json and material_catalog.json

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <fstream>
#include <sstream>

// Simple JSON parsing helpers (for production, use nlohmann/json or similar)
namespace json_helper {
    inline std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    inline bool parse_bool(const std::string& s) {
        return trim(s) == "true";
    }

    inline double parse_double(const std::string& s) {
        return std::stod(trim(s));
    }

    inline int parse_int(const std::string& s) {
        return std::stoi(trim(s));
    }
}

// Furniture catalog entry structures
struct ContainerProps {
    int max_weight;
    int max_slots;
    int lock_difficulty = 0;
    double trap_chance = 0.0;
};

struct FurnitureCatalogEntry {
    std::string id;
    std::string category;
    std::string name;
    double health;
    double weight;
    bool flammable;
    std::optional<ContainerProps> container_props;  // null if not a container
    std::vector<std::string> interactions;
    std::unordered_map<std::string, int> crafting_cost;
    
    // Helper to check if container
    bool is_container() const { return container_props.has_value(); }
    int get_max_weight() const { return container_props.value_or(ContainerProps{0,0}).max_weight; }
    int get_max_slots() const { return container_props.value_or(ContainerProps{0,0}).max_slots; }
    int get_lock_difficulty() const { return container_props.value_or(ContainerProps{0,0}).lock_difficulty; }
    double get_trap_chance() const { return container_props.value_or(ContainerProps{0,0}).trap_chance; }
};

// Material catalog entry structure
struct MaterialCatalogEntry {
    std::string id;
    std::string name;
    std::string type;  // "wall", "floor", "roof"
    double health;
    bool flammable;
    double thermal_insulation;
};

// Catalog data containers
struct FurnitureCatalog {
    std::unordered_map<std::string, FurnitureCatalogEntry> entries;
    std::vector<FurnitureCatalogEntry> items;  // Alias for backward compatibility
};

struct MaterialCatalog {
    std::unordered_map<std::string, MaterialCatalogEntry> entries;
};

// Parser class for loading catalogs from JSON files
class CatalogLoader {
public:
    static FurnitureCatalog loadFurnitureCatalog(const std::string& filepath);
    static MaterialCatalog loadMaterialCatalog(const std::string& filepath);

private:
    // Internal JSON parsing methods
    static std::string readFile(const std::string& filepath);
    static std::vector<std::string> splitArrayElements(const std::string& content);
    static std::string extractStringValue(const std::string& json, const std::string& key);
    static double extractDoubleValue(const std::string& json, const std::string& key);
    static bool extractBoolValue(const std::string& json, const std::string& key);
    static std::optional<ContainerProps> extractContainerProps(const std::string& json);
    static std::vector<std::string> extractStringArray(const std::string& json, const std::string& key);
    static std::unordered_map<std::string, int> extractCraftingCost(const std::string& json);
};

// Implementation
inline std::string CatalogLoader::readFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

inline std::vector<std::string> CatalogLoader::splitArrayElements(const std::string& content) {
    std::vector<std::string> elements;
    int braceDepth = 0;
    int bracketDepth = 0;
    size_t start = 0;
    
    // Find the array start
    size_t arrayStart = content.find('[');
    if (arrayStart == std::string::npos) return elements;
    
    for (size_t i = arrayStart; i < content.size(); ++i) {
        char c = content[i];
        if (c == '{') braceDepth++;
        else if (c == '}') braceDepth--;
        else if (c == '[') bracketDepth++;
        else if (c == ']') {
            bracketDepth--;
            if (bracketDepth == 0 && braceDepth == 0 && i > start) {
                std::string elem = content.substr(start, i - start);
                if (!json_helper::trim(elem).empty()) {
                    elements.push_back(elem);
                }
            }
        } else if (c == ',' && braceDepth == 0 && bracketDepth == 1) {
            std::string elem = content.substr(start, i - start);
            if (!json_helper::trim(elem).empty()) {
                elements.push_back(elem);
            }
            start = i + 1;
        }
    }
    return elements;
}

inline std::string CatalogLoader::extractStringValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    
    size_t endPos = json.find('"', pos + 1);
    if (endPos == std::string::npos) return "";
    
    return json.substr(pos + 1, endPos - pos - 1);
}

inline double CatalogLoader::extractDoubleValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return 0.0;
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0.0;
    
    size_t start = pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;
    
    size_t end = start;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-')) end++;
    
    if (end == start) return 0.0;
    return json_helper::parse_double(json.substr(start, end - start));
}

inline bool CatalogLoader::extractBoolValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return false;
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    
    size_t start = pos + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;
    
    if (json.substr(start, 4) == "true") return true;
    return false;
}

inline std::optional<ContainerProps> CatalogLoader::extractContainerProps(const std::string& json) {
    size_t pos = json.find("\"container_props\"");
    if (pos == std::string::npos) return std::nullopt;
    
    size_t braceStart = json.find('{', pos);
    if (braceStart == std::string::npos) return std::nullopt;
    
    int depth = 1;
    size_t braceEnd = braceStart + 1;
    while (braceEnd < json.size() && depth > 0) {
        if (json[braceEnd] == '{') depth++;
        else if (json[braceEnd] == '}') depth--;
        braceEnd++;
    }
    
    std::string propsJson = json.substr(braceStart, braceEnd - braceStart);
    
    ContainerProps props;
    props.max_weight = static_cast<int>(extractDoubleValue(propsJson, "max_weight"));
    props.max_slots = static_cast<int>(extractDoubleValue(propsJson, "max_slots"));
    
    // Check for optional fields
    if (propsJson.find("\"lock_difficulty\"") != std::string::npos) {
        props.lock_difficulty = static_cast<int>(extractDoubleValue(propsJson, "lock_difficulty"));
    }
    if (propsJson.find("\"trap_chance\"") != std::string::npos) {
        props.trap_chance = extractDoubleValue(propsJson, "trap_chance");
    }
    
    return props;
}

inline std::vector<std::string> CatalogLoader::extractStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return result;
    
    size_t bracketStart = json.find('[', pos);
    if (bracketStart == std::string::npos) return result;
    
    size_t bracketEnd = json.find(']', bracketStart);
    if (bracketEnd == std::string::npos) return result;
    
    std::string arrayContent = json.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
    
    size_t pos2 = 0;
    while ((pos2 = arrayContent.find('"', pos2)) != std::string::npos) {
        size_t endPos = arrayContent.find('"', pos2 + 1);
        if (endPos == std::string::npos) break;
        result.push_back(arrayContent.substr(pos2 + 1, endPos - pos2 - 1));
        pos2 = endPos + 1;
    }
    
    return result;
}

inline std::unordered_map<std::string, int> CatalogLoader::extractCraftingCost(const std::string& json) {
    std::unordered_map<std::string, int> result;
    size_t pos = json.find("\"crafting_cost\"");
    if (pos == std::string::npos) return result;
    
    size_t braceStart = json.find('{', pos);
    if (braceStart == std::string::npos) return result;
    
    size_t braceEnd = json.find('}', braceStart);
    if (braceEnd == std::string::npos) return result;
    
    std::string costContent = json.substr(braceStart + 1, braceEnd - braceStart - 1);
    
    size_t pos2 = 0;
    while ((pos2 = costContent.find('"', pos2)) != std::string::npos) {
        size_t keyEnd = costContent.find('"', pos2 + 1);
        if (keyEnd == std::string::npos) break;
        
        std::string material = costContent.substr(pos2 + 1, keyEnd - pos2 - 1);
        
        size_t colonPos = costContent.find(':', keyEnd);
        if (colonPos == std::string::npos) break;
        
        size_t numStart = colonPos + 1;
        while (numStart < costContent.size() && (costContent[numStart] == ' ' || costContent[numStart] == '\t')) numStart++;
        
        size_t numEnd = numStart;
        while (numEnd < costContent.size() && isdigit(costContent[numEnd])) numEnd++;
        
        if (numEnd > numStart) {
            int quantity = std::stoi(costContent.substr(numStart, numEnd - numStart));
            result[material] = quantity;
        }
        
        pos2 = numEnd;
    }
    
    return result;
}

inline FurnitureCatalog CatalogLoader::loadFurnitureCatalog(const std::string& filepath) {
    FurnitureCatalog catalog;
    std::string content = readFile(filepath);
    std::vector<std::string> elements = splitArrayElements(content);
    
    for (const auto& elem : elements) {
        FurnitureCatalogEntry entry;
        entry.id = extractStringValue(elem, "id");
        entry.category = extractStringValue(elem, "category");
        entry.name = extractStringValue(elem, "name");
        entry.health = extractDoubleValue(elem, "health");
        entry.weight = extractDoubleValue(elem, "weight");
        entry.flammable = extractBoolValue(elem, "flammable");
        entry.container_props = extractContainerProps(elem);
        entry.interactions = extractStringArray(elem, "interactions");
        entry.crafting_cost = extractCraftingCost(elem);
        
        catalog.entries[entry.id] = entry;
        catalog.items.push_back(entry);  // Also add to vector for iteration
    }
    
    return catalog;
}

inline MaterialCatalog CatalogLoader::loadMaterialCatalog(const std::string& filepath) {
    MaterialCatalog catalog;
    std::string content = readFile(filepath);
    std::vector<std::string> elements = splitArrayElements(content);
    
    for (const auto& elem : elements) {
        MaterialCatalogEntry entry;
        entry.id = extractStringValue(elem, "id");
        entry.name = extractStringValue(elem, "name");
        entry.type = extractStringValue(elem, "type");
        entry.health = extractDoubleValue(elem, "health");
        entry.flammable = extractBoolValue(elem, "flammable");
        entry.thermal_insulation = extractDoubleValue(elem, "thermal_insulation");
        
        catalog.entries[entry.id] = entry;
    }
    
    return catalog;
}
