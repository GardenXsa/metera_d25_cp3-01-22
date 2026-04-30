/*
 * NEXUS ENGINE - Native World Simulation Core for Chronicles of Meterea
 * Full port of world_worker.js (2099 lines) to C++17
 * 
 * Architecture Layers:
 * 1. Data Layer (generated_data.h) - Auto-generated enums, constants, recipes from JSON
 * 2. Core Types - PhysicalItem, Storage (batch-based spoilage, stack management)
 * 3. World State - World, Region, Faction, NPC, Caravan, News with full serialization
 * 4. Simulation Engine - All systems from world_worker.js
 * 5. Protocol Layer - stdin/stdout JSON communication with Electron
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <random>
#include <cstdint>
#include <optional>
#include <variant>
#include <chrono>

#include "generated_data.h"

// ============================================================================
// SIMPLE JSON PARSER/WRITER (No external dependencies)
// ============================================================================

class JsonValue {
public:
    enum Type { NUL, BOOL, INT, DOUBLE, STRING, ARRAY, OBJECT };
    Type type = NUL;
    bool b_val = false;
    int64_t i_val = 0;
    double d_val = 0.0;
    std::string s_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    JsonValue() : type(NUL) {}
    JsonValue(bool b) : type(BOOL), b_val(b) {}
    JsonValue(int i) : type(INT), i_val(i) {}
    JsonValue(int64_t i) : type(INT), i_val(i) {}
    JsonValue(double d) : type(DOUBLE), d_val(d) {}
    JsonValue(const std::string& s) : type(STRING), s_val(s) {}
    JsonValue(const char* s) : type(STRING), s_val(s) {}

    static JsonValue array() { JsonValue v; v.type = ARRAY; return v; }
    static JsonValue object() { JsonValue v; v.type = OBJECT; return v; }

    void set(const std::string& key, const JsonValue& val) {
        if (type != OBJECT) type = OBJECT;
        obj_val[key] = val;
    }

    void push(const JsonValue& val) {
        if (type != ARRAY) type = ARRAY;
        arr_val.push_back(val);
    }

    JsonValue& operator[](const std::string& key) { return obj_val[key]; }
    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null;
        auto it = obj_val.find(key);
        return (it != obj_val.end()) ? it->second : null;
    }

    JsonValue& operator[](size_t idx) { return arr_val[idx]; }
    const JsonValue& operator[](size_t idx) const { return arr_val[idx]; }

    bool has(const std::string& key) const {
        return (type == OBJECT) && (obj_val.find(key) != obj_val.end());
    }

    size_t size() const {
        if (type == ARRAY) return arr_val.size();
        if (type == OBJECT) return obj_val.size();
        return 0;
    }

    std::string asString() const { return s_val; }
    int asInt() const { return (type == INT) ? i_val : (type == DOUBLE ? (int)d_val : 0); }
    double asDouble() const { return (type == DOUBLE) ? d_val : (type == INT ? (double)i_val : 0.0); }
    bool asBool() const { return b_val; }

    std::string toString() const {
        std::ostringstream oss;
        switch (type) {
            case NUL: oss << "null"; break;
            case BOOL: oss << (b_val ? "true" : "false"); break;
            case INT: oss << i_val; break;
            case DOUBLE: oss << d_val; break;
            case STRING: {
                oss << "\"";
                for (char c : s_val) {
                    switch (c) {
                        case '"': oss << "\\\""; break;
                        case '\\': oss << "\\\\"; break;
                        case '\n': oss << "\\n"; break;
                        case '\r': oss << "\\r"; break;
                        case '\t': oss << "\\t"; break;
                        default: oss << c;
                    }
                }
                oss << "\"";
                break;
            }
            case ARRAY: {
                oss << "[";
                bool first = true;
                for (const auto& item : arr_val) {
                    if (!first) oss << ",";
                    first = false;
                    oss << item.toString();
                }
                oss << "]";
                break;
            }
            case OBJECT: {
                oss << "{";
                bool first = true;
                for (const auto& kv : obj_val) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << kv.first << "\":" << kv.second.toString();
                }
                oss << "}";
                break;
            }
        }
        return oss.str();
    }
};

JsonValue parseJson(const std::string& json, size_t& pos);

namespace {
    void skipWhitespace(const std::string& json, size_t& pos) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    }

    JsonValue parseString(const std::string& json, size_t& pos) {
        pos++; // skip opening quote
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        if (pos + 4 < json.size()) {
                            int codePoint = 0;
                            for (int i = 0; i < 4; i++) {
                                char hc = json[pos + 1 + i];
                                int val = 0;
                                if (hc >= '0' && hc <= '9') val = hc - '0';
                                else if (hc >= 'a' && hc <= 'f') val = hc - 'a' + 10;
                                else if (hc >= 'A' && hc <= 'F') val = hc - 'A' + 10;
                                codePoint = (codePoint << 4) | val;
                            }
                            pos += 4;
                            if (codePoint <= 0x7F) {
                                result += static_cast<char>(codePoint);
                            } else if (codePoint <= 0x7FF) {
                                result += static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F));
                                result += static_cast<char>(0x80 | (codePoint & 0x3F));
                            } else {
                                result += static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F));
                                result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (codePoint & 0x3F));
                            }
                        }
                        break;
                    }
                    default: result += json[pos];
                }
            } else {
                result += json[pos];
            }
            pos++;
        }
        if (pos < json.size()) pos++; // skip closing quote
        return JsonValue(result);
    }

    JsonValue parseNumber(const std::string& json, size_t& pos) {
        size_t start = pos;
        bool isFloat = false;
        if (pos < json.size() && json[pos] == '-') pos++;
        while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+' || json[pos] == '-')) {
            if (json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E') isFloat = true;
            pos++;
        }
        std::string numStr = json.substr(start, pos - start);
        if (numStr.empty() || numStr == "-") return JsonValue(0);
        if (isFloat) return JsonValue(std::stod(numStr));
        return JsonValue((int64_t)std::stoll(numStr));
    }

    JsonValue parseArray(const std::string& json, size_t& pos) {
        JsonValue arr = JsonValue::array();
        pos++; // skip '['
        skipWhitespace(json, pos);
        while (pos < json.size() && json[pos] != ']') {
            arr.push(parseJson(json, pos));
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') {
                pos++;
                skipWhitespace(json, pos);
            }
        }
        if (pos < json.size()) pos++; // skip ']'
        return arr;
    }

    JsonValue parseObject(const std::string& json, size_t& pos) {
        JsonValue obj = JsonValue::object();
        pos++; // skip '{'
        skipWhitespace(json, pos);
        while (pos < json.size() && json[pos] != '}') {
            if (json[pos] != '"') { pos++; continue; } // Error recovery
            JsonValue keyVal = parseString(json, pos);
            std::string key = keyVal.s_val;
            
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ':') pos++;
            skipWhitespace(json, pos);
            
            JsonValue val = parseJson(json, pos);
            obj.set(key, val);
            
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') {
                pos++;
                skipWhitespace(json, pos);
            }
        }
        if (pos < json.size()) pos++; // skip '}'
        return obj;
    }
}

JsonValue parseJson(const std::string& json, size_t& pos) {
    skipWhitespace(json, pos);
    if (pos >= json.size()) return JsonValue();
    
    char first = json[pos];
    if (first == '"') return parseString(json, pos);
    if (first == '[') return parseArray(json, pos);
    if (first == '{') return parseObject(json, pos);
    if (first == 't' && json.substr(pos, 4) == "true") { pos += 4; return JsonValue(true); }
    if (first == 'f' && json.substr(pos, 5) == "false") { pos += 5; return JsonValue(false); }
    if (first == 'n' && json.substr(pos, 4) == "null") { pos += 4; return JsonValue(); }
    if (first == '-' || isdigit(first)) return parseNumber(json, pos);
    
    pos++; // Skip unknown char
    return JsonValue();
}

JsonValue parseJson(const std::string& json) {
    size_t pos = 0;
    return parseJson(json, pos);
}

// ============================================================================
// PHYSICAL ITEM SYSTEM (Batch-based spoilage, stack management)
// ============================================================================

struct OrderData {
    std::string issuer_id;
    std::string issuer_name;
    GoodType item_prototype = GoodType::COUNT;
    int quantity = 0;
    int max_price_per_unit = 0;
    int deadline_days = 0;
    std::string status = "pending"; // pending, in_progress, ready, delivered, cancelled
    std::string assigned_merchant_id;
    int created_date = 0;

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("issuer_id", issuer_id);
        obj.set("issuer_name", issuer_name);
        obj.set("item_prototype", goodTypeToString(item_prototype));
        obj.set("quantity", quantity);
        obj.set("max_price_per_unit", max_price_per_unit);
        obj.set("deadline_days", deadline_days);
        obj.set("status", status);
        obj.set("assigned_merchant_id", assigned_merchant_id);
        obj.set("created_date", created_date);
        return obj;
    }

    static OrderData fromJson(const JsonValue& j) {
        OrderData o;
        if (j.has("issuer_id")) o.issuer_id = j["issuer_id"].asString();
        if (j.has("issuer_name")) o.issuer_name = j["issuer_name"].asString();
        if (j.has("item_prototype")) o.item_prototype = stringToGoodType(j["item_prototype"].asString());
        if (j.has("quantity")) o.quantity = j["quantity"].asInt();
        if (j.has("max_price_per_unit")) o.max_price_per_unit = j["max_price_per_unit"].asInt();
        if (j.has("deadline_days")) o.deadline_days = j["deadline_days"].asInt();
        if (j.has("status")) o.status = j["status"].asString();
        if (j.has("assigned_merchant_id")) o.assigned_merchant_id = j["assigned_merchant_id"].asString();
        if (j.has("created_date")) o.created_date = j["created_date"].asInt();
        return o;
    }
};

struct PhysicalItem {
    std::string id;
    GoodType prototype_id;
    bool is_dirty = true;
    int stack_size = 0;
    std::string container_id;
    std::string slot_index;
    std::string state = "idle";
    bool quest_item = false;
    bool bound = false;
    bool stolen = false;
    bool magical = false;
    bool fragile = false;
    int durability = 100;
    JsonValue custom_props = JsonValue::object();
    int64_t created_at = 0;
    int64_t last_moved_at = 0;
    int batch_day = 0;
    std::vector<std::pair<int, std::string>> history;
    std::optional<OrderData> order_data;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("prototype_id", goodTypeToString(prototype_id));
        obj.set("stack_size", stack_size);
        obj.set("container_id", container_id);
        if (!slot_index.empty()) obj.set("slot_index", slot_index);
        else obj.set("slot_index", JsonValue());
        obj.set("state", state);
        obj.set("durability", durability);
        obj.set("custom_props", custom_props);
        obj.set("created_at", created_at);
        obj.set("last_moved_at", last_moved_at);
        obj.set("batch_day", batch_day);
        
        JsonValue flags = JsonValue::object();
        flags.set("quest_item", quest_item);
        flags.set("bound", bound);
        flags.set("stolen", stolen);
        flags.set("magical", magical);
        flags.set("fragile", fragile);
        obj.set("flags", flags);
        
        JsonValue hist = JsonValue::array();
        for (const auto& h : history) {
            JsonValue entry = JsonValue::object();
            entry.set("day", h.first);
            entry.set("event", h.second);
            hist.push(entry);
        }
        obj.set("history", hist);
        
        if (order_data.has_value()) {
            obj.set("order_data", order_data.value().toJson());
        }
        
        return obj;
    }
    
    static PhysicalItem fromJson(const JsonValue& j) {
        PhysicalItem item;
        item.id = j["id"].asString();
        if (j.has("prototype_id")) item.prototype_id = stringToGoodType(j["prototype_id"].asString());
        else if (j.has("aiIdentifier")) item.prototype_id = stringToGoodType(j["aiIdentifier"].asString());
        item.stack_size = j["stack_size"].asInt();
        item.container_id = j["container_id"].asString();
        if (j.has("slot_index") && j["slot_index"].type == JsonValue::STRING) item.slot_index = j["slot_index"].asString();
        if (j.has("state")) item.state = j["state"].asString();
        if (j.has("durability")) item.durability = j["durability"].asInt();
        if (j.has("custom_props")) item.custom_props = j["custom_props"];
        if (j.has("created_at")) item.created_at = j["created_at"].asInt();
        if (j.has("last_moved_at")) item.last_moved_at = j["last_moved_at"].asInt();
        if (j.has("batch_day")) item.batch_day = j["batch_day"].asInt();
        
        if (j.has("flags")) {
            item.quest_item = j["flags"]["quest_item"].asBool();
            item.bound = j["flags"]["bound"].asBool();
            item.stolen = j["flags"]["stolen"].asBool();
            item.magical = j["flags"]["magical"].asBool();
            item.fragile = j["flags"]["fragile"].asBool();
        }
        
        if (j.has("history")) {
            for (size_t i = 0; i < j["history"].size(); i++) {
                int day = j["history"][i]["day"].asInt();
                std::string event = j["history"][i]["event"].asString();
                item.history.push_back({day, event});
            }
        }
        
        if (j.has("order_data")) {
            item.order_data = OrderData::fromJson(j["order_data"]);
        }
        
        return item;
    }
};

struct Storage {
    std::string id;
    std::string type;
    bool is_dirty = true;
    std::string owner_id;
    int max_weight_kg = 999999;
    int max_slots = 1000;
    
    JsonValue location = JsonValue::object();
    JsonValue lock_data = JsonValue::object();
    JsonValue physical_props = JsonValue::object();
    JsonValue custom_props = JsonValue::object();
    
    std::vector<std::string> item_ids;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", type);
        obj.set("owner_id", owner_id);
        obj.set("max_weight_kg", max_weight_kg);
        obj.set("max_slots", max_slots);
        obj.set("location", location);
        obj.set("lock_data", lock_data);
        obj.set("physical_props", physical_props);
        obj.set("custom_props", custom_props);
        
        JsonValue items = JsonValue::array();
        for (const auto& iid : item_ids) items.push(JsonValue(iid));
        obj.set("items", items);
        return obj;
    }
    
    static Storage fromJson(const JsonValue& j) {
        Storage s;
        if (j.has("id")) s.id = j["id"].asString();
        if (j.has("type")) s.type = j["type"].asString();
        if (j.has("owner_id")) s.owner_id = j["owner_id"].asString();
        if (j.has("max_weight_kg")) s.max_weight_kg = j["max_weight_kg"].asInt();
        if (j.has("max_slots")) s.max_slots = j["max_slots"].asInt();
        
        if (j.has("location")) s.location = j["location"];
        if (j.has("lock_data")) s.lock_data = j["lock_data"];
        if (j.has("physical_props")) s.physical_props = j["physical_props"];
        if (j.has("custom_props")) s.custom_props = j["custom_props"];
        
        if (j.has("items")) {
            for (size_t i = 0; i < j["items"].size(); i++) {
                s.item_ids.push_back(j["items"][i].asString());
            }
        }
        return s;
    }
};

// Global registries
static std::unordered_map<std::string, PhysicalItem> g_items;
static std::unordered_map<std::string, Storage> g_containers;
static std::vector<std::string> g_deleted_items;
static std::vector<std::string> g_deleted_containers;

// Helper: Generate UUID
static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_int_distribution<> hex_dist(0, 15);

std::string generateUUID() {
    const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (size_t i = 0; i < uuid.size(); i++) {
        if (uuid[i] == 'x') uuid[i] = hex[hex_dist(gen)];
        else if (uuid[i] == 'y') uuid[i] = hex[(hex_dist(gen) & 0x3) | 0x8];
    }
    return uuid;
}

// Item management
std::string createContainer(const std::string& type, const std::string& ownerId, 
                            int maxWeight, int maxSlots, const std::string& regionId = "",
                            const std::string& parentEntity = "", const std::string& parentContainer = "") {
    Storage cont;
    cont.id = "cont_" + generateUUID();
    cont.type = type;
    cont.owner_id = ownerId;
    cont.max_weight_kg = maxWeight;
    cont.max_slots = maxSlots;
    
    if (!regionId.empty()) cont.location.set("region_id", regionId);
    if (!parentEntity.empty()) cont.location.set("parent_entity", parentEntity);
    if (!parentContainer.empty()) cont.location.set("parent_container", parentContainer);
    
    cont.lock_data.set("is_locked", (type == "faction_vault"));
    cont.lock_data.set("difficulty", (type == "faction_vault") ? 16 : 10);
    cont.physical_props.set("health", (type == "faction_vault") ? 400 : 200);
    cont.physical_props.set("flammable", (type != "faction_vault"));
    
    cont.is_dirty = true;
    g_containers[cont.id] = cont;
    return cont.id;
}

std::string createItem(GoodType prototypeId, int quantity, const std::string& containerId,
                       int currentDay = 0, const std::string& event = "Created") {
    PhysicalItem item;
    item.id = "item_" + generateUUID();
    item.prototype_id = prototypeId;
    item.stack_size = quantity;
    item.container_id = containerId;
    item.created_at = currentDay;
    item.last_moved_at = currentDay;
    item.batch_day = currentDay;
    item.history.push_back({currentDay, event});
    
    // Weight per unit based on type
    if (prototypeId == GoodType::GOLD_INGOT) item.custom_props.set("weight_per_unit", 0.01);
    else item.custom_props.set("weight_per_unit", 1.0);
    
    g_items[item.id] = item;
    
    // Add to container
    if (!containerId.empty() && g_containers.count(containerId)) {
        g_containers[containerId].item_ids.push_back(item.id);
        g_containers[containerId].is_dirty = true;
    }
    
    return item.id;
}

bool removeItem(const std::string& itemId, int quantity) {
    if (!g_items.count(itemId)) return false;
    
    PhysicalItem& item = g_items[itemId];
    if (item.stack_size <= quantity) {
        // Remove from container
        if (!item.container_id.empty() && g_containers.count(item.container_id)) {
            Storage& cont = g_containers[item.container_id];
            auto& vec = cont.item_ids;
            auto it = std::find(vec.begin(), vec.end(), itemId);
            if (it != vec.end()) {
                *it = std::move(vec.back());
                vec.pop_back();
            }
            cont.is_dirty = true;
        }
        g_deleted_items.push_back(itemId);
        g_items.erase(itemId);
    } else {
        item.stack_size -= quantity;
        item.is_dirty = true;
    }
    return true;
}

bool moveItem(const std::string& itemId, const std::string& targetContainerId) {
    if (!g_items.count(itemId)) return false;
    if (!g_containers.count(targetContainerId)) return false;
    
    PhysicalItem& item = g_items[itemId];
    
    // Remove from old container
    if (!item.container_id.empty() && g_containers.count(item.container_id)) {
        Storage& oldCont = g_containers[item.container_id];
        auto& vec = oldCont.item_ids;
        auto it = std::find(vec.begin(), vec.end(), itemId);
        if (it != vec.end()) {
            *it = std::move(vec.back());
            vec.pop_back();
        }
        oldCont.is_dirty = true;
    }
    
    // Add to new container
    item.container_id = targetContainerId;
    item.is_dirty = true;
    g_containers[targetContainerId].item_ids.push_back(itemId);
    g_containers[targetContainerId].is_dirty = true;
    
    return true;
}

int countItemsInContainer(const std::string& containerId, GoodType prototypeId) {
    if (!g_containers.count(containerId)) return 0;
    
    const Storage& cont = g_containers[containerId];
    int total = 0;
    
    for (const auto& itemId : cont.item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
            if (item.prototype_id == prototypeId) {
                total += item.stack_size;
            }
        }
    }
    
    return total;
}

double calculateContainerWeight(const std::string& containerId) {
    if (!g_containers.count(containerId)) return 0.0;
    double totalWeight = 0.0;
    for (const auto& itemId : g_containers[containerId].item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
            double w = item.custom_props.has("weight_per_unit") ? item.custom_props["weight_per_unit"].asDouble() : 1.0;
            totalWeight += w * item.stack_size;
        }
    }
    return totalWeight;
}

int consumeItemsFromContainer(const std::string& containerId, GoodType prototypeId, int quantity) {
    if (!g_containers.count(containerId)) return 0;
    
    Storage& cont = g_containers[containerId];
    int taken = 0;
    int remaining = quantity;
    
    // Sort items by batch_day (FIFO - oldest first)
    std::vector<std::pair<int, std::string>> itemsByAge;
    for (const auto& itemId : cont.item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
            if (item.prototype_id == prototypeId) {
                itemsByAge.push_back({item.batch_day, itemId});
            }
        }
    }
    std::sort(itemsByAge.begin(), itemsByAge.end());
    
    for (const auto& [day, itemId] : itemsByAge) {
        if (remaining <= 0) break;
        if (!g_items.count(itemId)) continue;
        
        PhysicalItem& item = g_items[itemId];
        int take = std::min(item.stack_size, remaining);
        if (take > 0) {
            removeItem(itemId, take);
            remaining -= take;
            taken += take;
        }
    }
    
    return taken;
}

// ============================================================================
// WORLD STATE STRUCTURES
// ============================================================================

struct News {
    std::string text;
    std::string location;
    int importance; // 1=minor, 2=notable, 3=major
    std::string category; // "trade", "war", "disaster", "politics", "misc"
    int day = 0;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("text", text);
        obj.set("location", location);
        obj.set("importance", importance);
        obj.set("category", category);
        obj.set("day", day);
        return obj;
    }
};

struct Caravan {
    std::string id;
    std::string origin;
    std::string destination;
    int hoursLeft = 0;
    std::string chest_id; // Container with goods
    int wagons = 0;       // Количество повозок в караване
    
    // Legacy goods map (for compatibility)
    std::map<std::string, int> goods;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("origin", origin);
        obj.set("destination", destination);
        obj.set("hoursLeft", hoursLeft);
        obj.set("chest_id", chest_id);
        obj.set("wagons", wagons);
        
        JsonValue g = JsonValue::object();
        for (const auto& [key, val] : goods) g.set(key, val);
        obj.set("goods", g);
        
        return obj;
    }
    
    static Caravan fromJson(const JsonValue& j) {
        Caravan c;
        c.id = j["id"].asString();
        c.origin = j["origin"].asString();
        c.destination = j["destination"].asString();
        c.hoursLeft = j["hoursLeft"].asInt();
        c.chest_id = j["chest_id"].asString();
        if (j.has("wagons")) c.wagons = j["wagons"].asInt();
        
        if (j.has("goods")) {
            for (const auto& kv : j["goods"].obj_val) {
                c.goods[kv.first] = kv.second.asInt();
            }
        }
        
        return c;
    }
};

struct NPC {
    std::string id;
    std::string name;
    std::string type = "npc"; // "npc" or "ruler"
    std::string profession;
    std::string homeLocation;
    std::string currentLocation;
    std::string currentActivity;
    
    // Schedule
    struct ScheduleEntry {
        int start, end;
        std::string activity;
        std::string location;
    };
    std::vector<ScheduleEntry> schedule;
    
    // Needs (0-100)
    struct Needs {
        int hunger = 100;
        int rest = 100;
        int social = 100;
        int safety = 100;
    } needs;
    
    // Personality (0-100)
    struct Personality {
        int aggression = 50;
        int sociability = 50;
        int greed = 50;
        int loyalty = 50;
    } personality;
    
    // Economy
    struct Economy {
        int skillLevel = 5;
        bool isEmployed = false;
        std::string workplaceId;
        int dailyWage = 0;
        int savings = 0;
    } economy;
    
    // Inventory
    int gold = 0;
    std::string inventory_id; // Container ID for physical items
    
    // Status
    bool isAlive = true;
    int hp = 20;
    bool plotArmor = false;
    
    // Travel
    std::string travelDestination;
    int travelHoursLeft = 0;
    std::string delivery_target_id;
    
    // For rulers
    std::string factionId;
    struct RulerStats {
        int hp = 80, maxHp = 80;
        int str = 10, dex = 10, int_ = 14, con = 12, cha = 16, res = 10;
    } rulerStats;
    struct RulerPersonality {
        int ambition = 60;
        int paranoia = 50;
        int wisdom = 50;
        int cruelty = 50;
        int diplomacy = 50;
        int military = 50;
        int stewardship = 50;
    } rulerPersonality;
    int health = 100;
    bool alive = true;
    std::string heir;
    std::string currentGoal;
    std::string gmOverride;
    int lastTickDay = 0;
    
    std::map<std::string, int> relationships;
    std::vector<std::string> memory;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("name", name);
        obj.set("type", type);
        obj.set("profession", profession);
        obj.set("homeLocation", homeLocation);
        obj.set("currentLocation", currentLocation);
        obj.set("currentActivity", currentActivity);
        obj.set("isAlive", isAlive);
        obj.set("hp", hp);
        obj.set("gold", gold);
        obj.set("inventory_id", inventory_id);
        obj.set("travelDestination", travelDestination);
        obj.set("travelHoursLeft", travelHoursLeft);
        obj.set("delivery_target_id", delivery_target_id);
        
        JsonValue n = JsonValue::object();
        n.set("hunger", needs.hunger);
        n.set("rest", needs.rest);
        n.set("social", needs.social);
        n.set("safety", needs.safety);
        obj.set("needs", n);
        
        JsonValue p = JsonValue::object();
        p.set("aggression", personality.aggression);
        p.set("sociability", personality.sociability);
        p.set("greed", personality.greed);
        p.set("loyalty", personality.loyalty);
        obj.set("personality", p);
        
        JsonValue e = JsonValue::object();
        e.set("skillLevel", economy.skillLevel);
        e.set("isEmployed", economy.isEmployed);
        e.set("workplaceId", economy.workplaceId);
        e.set("dailyWage", economy.dailyWage);
        e.set("savings", economy.savings);
        obj.set("economy", e);
        
        // Schedule
        JsonValue sched = JsonValue::array();
        for (const auto& s : schedule) {
            JsonValue entry = JsonValue::object();
            entry.set("start", s.start);
            entry.set("end", s.end);
            entry.set("activity", s.activity);
            entry.set("location", s.location);
            sched.push(entry);
        }
        obj.set("schedule", sched);
        
        // Ruler-specific
        if (type == "ruler") {
            obj.set("factionId", factionId);
            obj.set("health", health);
            obj.set("alive", alive);
            obj.set("heir", heir);
            
            JsonValue rs = JsonValue::object();
            rs.set("hp", rulerStats.hp);
            rs.set("str", rulerStats.str);
            rs.set("dex", rulerStats.dex);
            rs.set("int", rulerStats.int_);
            rs.set("con", rulerStats.con);
            rs.set("cha", rulerStats.cha);
            rs.set("res", rulerStats.res);
            obj.set("rulerStats", rs);
            
            JsonValue rp = JsonValue::object();
            rp.set("ambition", rulerPersonality.ambition);
            rp.set("paranoia", rulerPersonality.paranoia);
            rp.set("wisdom", rulerPersonality.wisdom);
            rp.set("cruelty", rulerPersonality.cruelty);
            rp.set("diplomacy", rulerPersonality.diplomacy);
            rp.set("military", rulerPersonality.military);
            rp.set("stewardship", rulerPersonality.stewardship);
            obj.set("rulerPersonality", rp);
        }
        
        return obj;
    }
    
    static NPC fromJson(const JsonValue& j) {
        NPC npc;
        npc.id = j["id"].asString();
        npc.name = j["name"].asString();
        npc.type = j["type"].asString();
        npc.profession = j["profession"].asString();
        npc.homeLocation = j["homeLocation"].asString();
        npc.currentLocation = j["currentLocation"].asString();
        npc.currentActivity = j["currentActivity"].asString();
        npc.isAlive = j["isAlive"].asBool();
        npc.hp = j["hp"].asInt();
        npc.gold = j["gold"].asInt();
        npc.inventory_id = j["inventory_id"].asString();
        if (j.has("travelDestination")) npc.travelDestination = j["travelDestination"].asString();
        if (j.has("travelHoursLeft")) npc.travelHoursLeft = j["travelHoursLeft"].asInt();
        if (j.has("delivery_target_id")) npc.delivery_target_id = j["delivery_target_id"].asString();
        
        if (j.has("needs")) {
            npc.needs.hunger = j["needs"]["hunger"].asInt();
            npc.needs.rest = j["needs"]["rest"].asInt();
            npc.needs.social = j["needs"]["social"].asInt();
            npc.needs.safety = j["needs"]["safety"].asInt();
        }
        
        if (j.has("personality")) {
            npc.personality.aggression = j["personality"]["aggression"].asInt();
            npc.personality.sociability = j["personality"]["sociability"].asInt();
            npc.personality.greed = j["personality"]["greed"].asInt();
            npc.personality.loyalty = j["personality"]["loyalty"].asInt();
        }
        
        if (j.has("economy")) {
            npc.economy.skillLevel = j["economy"]["skillLevel"].asInt();
            npc.economy.isEmployed = j["economy"]["isEmployed"].asBool();
            npc.economy.workplaceId = j["economy"]["workplaceId"].asString();
            npc.economy.dailyWage = j["economy"]["dailyWage"].asInt();
            npc.economy.savings = j["economy"]["savings"].asInt();
        }
        
        if (j.has("schedule")) {
            for (size_t i = 0; i < j["schedule"].size(); i++) {
                ScheduleEntry s;
                s.start = j["schedule"][i]["start"].asInt();
                s.end = j["schedule"][i]["end"].asInt();
                s.activity = j["schedule"][i]["activity"].asString();
                s.location = j["schedule"][i]["location"].asString();
                npc.schedule.push_back(s);
            }
        }
        
        if (j.has("factionId")) {
            npc.factionId = j["factionId"].asString();
            npc.health = j["health"].asInt();
            npc.alive = j["alive"].asBool();
            npc.heir = j["heir"].asString();
            
            if (j.has("rulerStats")) {
                npc.rulerStats.hp = j["rulerStats"]["hp"].asInt();
                npc.rulerStats.str = j["rulerStats"]["str"].asInt();
                npc.rulerStats.dex = j["rulerStats"]["dex"].asInt();
                npc.rulerStats.int_ = j["rulerStats"]["int"].asInt();
                npc.rulerStats.con = j["rulerStats"]["con"].asInt();
                npc.rulerStats.cha = j["rulerStats"]["cha"].asInt();
                npc.rulerStats.res = j["rulerStats"]["res"].asInt();
            }
            
            if (j.has("rulerPersonality")) {
                npc.rulerPersonality.ambition = j["rulerPersonality"]["ambition"].asInt();
                npc.rulerPersonality.paranoia = j["rulerPersonality"]["paranoia"].asInt();
                npc.rulerPersonality.wisdom = j["rulerPersonality"]["wisdom"].asInt();
                npc.rulerPersonality.cruelty = j["rulerPersonality"]["cruelty"].asInt();
                npc.rulerPersonality.diplomacy = j["rulerPersonality"]["diplomacy"].asInt();
                npc.rulerPersonality.military = j["rulerPersonality"]["military"].asInt();
                npc.rulerPersonality.stewardship = j["rulerPersonality"]["stewardship"].asInt();
            }
        }
        
        return npc;
    }
};

struct Facility {
    int level = 0;
    int durability = 100;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("level", level);
        obj.set("durability", durability);
        return obj;
    }
    static Facility fromJson(const JsonValue& j) {
        Facility f;
        if (j.has("level")) f.level = j["level"].asInt();
        if (j.has("durability")) f.durability = j["durability"].asInt();
        return f;
    }
};

struct Animals {
    int herbivores = 0;
    int carnivores = 0;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("herbivores", herbivores);
        obj.set("carnivores", carnivores);
        return obj;
    }
    static Animals fromJson(const JsonValue& j) {
        Animals a;
        if (j.has("herbivores")) a.herbivores = j["herbivores"].asInt();
        if (j.has("carnivores")) a.carnivores = j["carnivores"].asInt();
        return a;
    }
};

struct Region {
    std::string id;
    std::string name;
    std::string factionId;
    int population = 0;
    double moneySupply = 0;
    std::string vault_id; // Container ID for faction storage
    
    int threat_level = 0;          // 0-100 (0 - идеальная безопасность)
    int storage_capacity = 10000;  // максимальная вместимость склада (ед. веса)
    std::string bandit_stash_id;   // ID контейнера с награбленным

    
    // Markets (good -> price)
    std::unordered_map<std::string, double> markets;
    
    // Caravans departing from this region
    std::vector<Caravan> caravans;
    
    // Weather & Climate
    std::string weather = "Ясно";
    int weatherDaysLeft = 0;
    std::string climate = "temperate";
    
    // Production facilities
    std::map<std::string, Facility> facilities;
    
    // Animals
    Animals animals;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("name", name);
        obj.set("factionId", factionId);
        obj.set("population", population);
        obj.set("moneySupply", moneySupply);
        obj.set("vault_id", vault_id);
        obj.set("threat_level", threat_level);
        obj.set("storage_capacity", storage_capacity);
        obj.set("bandit_stash_id", bandit_stash_id);
        obj.set("weather", weather);
        obj.set("weatherDaysLeft", weatherDaysLeft);
        
        JsonValue m = JsonValue::object();
        for (const auto& [k, v] : markets) m.set(k, v);
        obj.set("markets", m);
        
        JsonValue cars = JsonValue::array();
        for (const auto& c : caravans) cars.push(c.toJson());
        obj.set("caravans", cars);
        
        obj.set("climate", climate);
        
        JsonValue facs = JsonValue::object();
        for (const auto& [k, v] : facilities) facs.set(k, v.toJson());
        obj.set("facilities", facs);
        
        obj.set("animals", animals.toJson());
        
        return obj;
    }
    
    static Region fromJson(const JsonValue& j) {
        Region r;
        r.id = j["id"].asString();
        r.name = j["name"].asString();
        r.factionId = j["factionId"].asString();
        r.population = j["population"].asInt();
        r.moneySupply = j["moneySupply"].asDouble();
        r.vault_id = j["vault_id"].asString();
        if (j.has("threat_level")) r.threat_level = j["threat_level"].asInt();
        if (j.has("storage_capacity")) r.storage_capacity = j["storage_capacity"].asInt();
        if (j.has("bandit_stash_id")) r.bandit_stash_id = j["bandit_stash_id"].asString();
        r.weather = j["weather"].asString();
        r.weatherDaysLeft = j["weatherDaysLeft"].asInt();
        
        if (j.has("markets")) {
            for (const auto& kv : j["markets"].obj_val) {
                r.markets[kv.first] = kv.second.asDouble();
            }
        }
        
        if (j.has("caravans")) {
            for (size_t i = 0; i < j["caravans"].size(); i++) {
                r.caravans.push_back(Caravan::fromJson(j["caravans"][i]));
            }
        }
        
        if (j.has("climate")) r.climate = j["climate"].asString();
        
        if (j.has("facilities")) {
            for (const auto& kv : j["facilities"].obj_val) {
                r.facilities[kv.first] = Facility::fromJson(kv.second);
            }
        }
        
        if (j.has("animals")) {
            r.animals = Animals::fromJson(j["animals"]);
        }
        
        return r;
    }
};

struct Army {
    std::string id;
    int size = 0;
    int morale = 100;
    std::string location;
    std::string destination;
    int daysToMove = 0;
    int siegeDays = -1;
    std::string supply_chest_id;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("size", size);
        obj.set("morale", morale);
        obj.set("location", location);
        obj.set("destination", destination);
        obj.set("daysToMove", daysToMove);
        obj.set("siegeDays", siegeDays);
        obj.set("supply_chest_id", supply_chest_id);
        return obj;
    }
    static Army fromJson(const JsonValue& j) {
        Army a;
        if(j.has("id")) a.id = j["id"].asString();
        if(j.has("size")) a.size = j["size"].asInt();
        if(j.has("morale")) a.morale = j["morale"].asInt();
        if(j.has("location")) a.location = j["location"].asString();
        if(j.has("destination")) a.destination = j["destination"].asString();
        if(j.has("daysToMove")) a.daysToMove = j["daysToMove"].asInt();
        if(j.has("siegeDays")) a.siegeDays = j["siegeDays"].asInt();
        if(j.has("supply_chest_id")) a.supply_chest_id = j["supply_chest_id"].asString();
        return a;
    }
};

struct Faction {
    std::string id;
    std::string name;
    std::vector<std::string> regions;
    std::unordered_map<std::string, int> relations;
    std::unordered_map<std::string, std::string> diplomacy;
    std::vector<Army> armies;
    std::string rulerId;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("name", name);
        
        JsonValue regs = JsonValue::array();
        for (const auto& r : regions) regs.push(JsonValue(r));
        obj.set("regions", regs);
        
        JsonValue rel = JsonValue::object();
        for (const auto& [k, v] : relations) rel.set(k, v);
        obj.set("relations", rel);
        
        JsonValue dip = JsonValue::object();
        for (const auto& [k, v] : diplomacy) dip.set(k, v);
        obj.set("diplomacy", dip);
        
        JsonValue arms = JsonValue::array();
        for (const auto& a : armies) arms.push(a.toJson());
        obj.set("armies", arms);
        
        obj.set("rulerId", rulerId);
        return obj;
    }
    
    static Faction fromJson(const JsonValue& j) {
        Faction f;
        f.id = j["id"].asString();
        f.name = j["name"].asString();
        
        if (j.has("regions")) {
            for (size_t i = 0; i < j["regions"].size(); i++) {
                f.regions.push_back(j["regions"][i].asString());
            }
        }
        
        if (j.has("relations")) {
            for (const auto& kv : j["relations"].obj_val) {
                f.relations[kv.first] = kv.second.asInt();
            }
        }
        
        if (j.has("diplomacy")) {
            for (const auto& kv : j["diplomacy"].obj_val) {
                f.diplomacy[kv.first] = kv.second.asString();
            }
        }
        
        if (j.has("armies")) {
            for (size_t i = 0; i < j["armies"].size(); i++) {
                f.armies.push_back(Army::fromJson(j["armies"][i]));
            }
        }
        
        if (j.has("rulerId")) f.rulerId = j["rulerId"].asString();
        return f;
    }
};

struct Intrigue {
    std::string id;
    std::string type;
    std::string initiatorFactionId;
    std::string targetFactionId;
    std::string targetRulerId;
    int progress = 0;
    int requiredProgress = 60;
    int progressPerDay = 5;
    int discoveryChance = 3;
    bool isDiscovered = false;
    int startDay = 0;

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", type);
        obj.set("initiatorFactionId", initiatorFactionId);
        obj.set("targetFactionId", targetFactionId);
        obj.set("targetRulerId", targetRulerId);
        obj.set("progress", progress);
        obj.set("requiredProgress", requiredProgress);
        obj.set("progressPerDay", progressPerDay);
        obj.set("discoveryChance", discoveryChance);
        obj.set("isDiscovered", isDiscovered);
        obj.set("startDay", startDay);
        return obj;
    }

    static Intrigue fromJson(const JsonValue& j) {
        Intrigue i;
        if(j.has("id")) i.id = j["id"].asString();
        if(j.has("type")) i.type = j["type"].asString();
        if(j.has("initiatorFactionId")) i.initiatorFactionId = j["initiatorFactionId"].asString();
        if(j.has("targetFactionId")) i.targetFactionId = j["targetFactionId"].asString();
        if(j.has("targetRulerId")) i.targetRulerId = j["targetRulerId"].asString();
        if(j.has("progress")) i.progress = j["progress"].asInt();
        if(j.has("requiredProgress")) i.requiredProgress = j["requiredProgress"].asInt();
        if(j.has("progressPerDay")) i.progressPerDay = j["progressPerDay"].asInt();
        if(j.has("discoveryChance")) i.discoveryChance = j["discoveryChance"].asInt();
        if(j.has("isDiscovered")) i.isDiscovered = j["isDiscovered"].asBool();
        if(j.has("startDay")) i.startDay = j["startDay"].asInt();
        return i;
    }
};

struct World {
    int tick = 0;
    std::string era = "rebirth";
    
    // Time tracking
    struct Time {
        int accumulatedMinutes = 0;
        int lastEventPulse = 0;
        int internalHour = 0;
    } time;
    
    // Homeostasis
    struct Homeostasis {
        int warWeariness = 0;
        double fertility = 1.0;
    } homeostasis;
    
    // Game objects
    std::map<std::string, Region> regions;
    std::map<std::string, Faction> factions;
    std::map<std::string, NPC> npcs;
    std::vector<News> news;
    
    // GM intervention tracking
    std::vector<std::string> gmInterventionHistory;
    int lastDirectInjectionDay = -999;
    bool needsGlobalEvent = false;
    
    // Intrigues in progress
    std::vector<Intrigue> intrigues;
    std::map<std::string, JsonValue> nexusData;
    
    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("tick", tick);
        obj.set("era", era);
        
        JsonValue t = JsonValue::object();
        t.set("accumulatedMinutes", time.accumulatedMinutes);
        t.set("lastEventPulse", time.lastEventPulse);
        t.set("internalHour", time.internalHour);
        obj.set("time", t);
        
        JsonValue h = JsonValue::object();
        h.set("warWeariness", homeostasis.warWeariness);
        h.set("fertility", homeostasis.fertility);
        obj.set("homeostasis", h);
        
        JsonValue regs = JsonValue::object();
        for (const auto& [k, v] : regions) regs.set(k, v.toJson());
        obj.set("regions", regs);
        
        JsonValue facts = JsonValue::object();
        for (const auto& [k, v] : factions) facts.set(k, v.toJson());
        obj.set("factions", facts);
        
        JsonValue n = JsonValue::object();
        for (const auto& [k, v] : npcs) n.set(k, v.toJson());
        obj.set("npcs", n);
        
        JsonValue newsArr = JsonValue::array();
        for (const auto& nw : news) newsArr.push(nw.toJson());
        obj.set("news", newsArr);
        
        JsonValue intrs = JsonValue::array();
        for (const auto& i : intrigues) intrs.push(i.toJson());
        obj.set("intrigues", intrs);
        
        JsonValue nd = JsonValue::object();
        for (const auto& [k, v] : nexusData) nd.set(k, v);
        obj.set("nexusData", nd);
        
        obj.set("needsGlobalEvent", needsGlobalEvent);
        obj.set("lastDirectInjectionDay", lastDirectInjectionDay);
        
        return obj;
    }
    
    static World fromJson(const JsonValue& j) {
        World w;
        w.tick = j["tick"].asInt();
        w.era = j["era"].asString();
        
        if (j.has("time")) {
            w.time.accumulatedMinutes = j["time"]["accumulatedMinutes"].asInt();
            w.time.lastEventPulse = j["time"]["lastEventPulse"].asInt();
            w.time.internalHour = j["time"]["internalHour"].asInt();
        }
        
        if (j.has("homeostasis")) {
            w.homeostasis.warWeariness = j["homeostasis"]["warWeariness"].asInt();
            w.homeostasis.fertility = j["homeostasis"]["fertility"].asDouble();
        }
        
        if (j.has("regions")) {
            for (const auto& kv : j["regions"].obj_val) {
                w.regions[kv.first] = Region::fromJson(kv.second);
            }
        }
        
        if (j.has("factions")) {
            for (const auto& kv : j["factions"].obj_val) {
                w.factions[kv.first] = Faction::fromJson(kv.second);
            }
        }
        
        if (j.has("npcs")) {
            for (const auto& kv : j["npcs"].obj_val) {
                w.npcs[kv.first] = NPC::fromJson(kv.second);
            }
        }
        
        if (j.has("news")) {
            for (size_t i = 0; i < j["news"].size(); i++) {
                News nw;
                nw.text = j["news"][i]["text"].asString();
                nw.location = j["news"][i]["location"].asString();
                nw.importance = j["news"][i]["importance"].asInt();
                nw.category = j["news"][i]["category"].asString();
                nw.day = j["news"][i]["day"].asInt();
                w.news.push_back(nw);
            }
        }
        
        if (j.has("intrigues")) {
            for (size_t i = 0; i < j["intrigues"].size(); i++) {
                w.intrigues.push_back(Intrigue::fromJson(j["intrigues"][i]));
            }
        }
        
        if (j.has("nexusData")) {
            for (const auto& kv : j["nexusData"].obj_val) {
                w.nexusData[kv.first] = kv.second;
            }
        }
        w.needsGlobalEvent = j["needsGlobalEvent"].asBool();
        w.lastDirectInjectionDay = j["lastDirectInjectionDay"].asInt();
        
        return w;
    }
};

// Global world state
static World g_world;
static std::string g_playerId;
static int g_currentDay = 0;

// ============================================================================
// SIMULATION FUNCTIONS
// ============================================================================

// Shelf life in days for each good type
int getShelfLifeDays(GoodType type) {
    static int shelfLife[(int)GoodType::COUNT] = {0};
    static bool initialized = false;
    if (!initialized) {
        for(int i=0; i<(int)GoodType::COUNT; i++) shelfLife[i] = 999999;
        shelfLife[(int)GoodType::MEAT] = 5;
        shelfLife[(int)GoodType::FISH] = 5;
        shelfLife[(int)GoodType::BREAD] = 10;
        shelfLife[(int)GoodType::WHEAT] = 360;
        shelfLife[(int)GoodType::SMOKED_MEAT] = 180;
        shelfLife[(int)GoodType::HERBS] = 30;
        shelfLife[(int)GoodType::WOOD] = 720;
        shelfLife[(int)GoodType::IRON_ORE] = 3600;
        shelfLife[(int)GoodType::WEAPONS] = 1800;
        shelfLife[(int)GoodType::ARMOR] = 1800;
        shelfLife[(int)GoodType::CLOTHES] = 1080;
        shelfLife[(int)GoodType::COTTON] = 360;
        initialized = true;
    }
    return shelfLife[(int)type];
}

void addNews(const std::string& text, const std::string& location, int importance, const std::string& category = "misc") {
    News nw;
    nw.text = text;
    nw.location = location;
    nw.importance = importance;
    nw.category = category;
    nw.day = g_currentDay;
    g_world.news.push_back(nw);
}

std::string getGoodName(GoodType good) {
    switch (good) {
        case GoodType::BREAD: return "хлеб";
        case GoodType::MEAT: return "мясо";
        case GoodType::FISH: return "рыба";
        case GoodType::WHEAT: return "пшеница";
        case GoodType::WOOD: return "древесина";
        case GoodType::IRON_ORE: return "железная руда";
        case GoodType::GOLD_ORE: return "золотая руда";
        case GoodType::IRON_INGOT: return "железо";
        case GoodType::WEAPONS: return "оружие";
        case GoodType::ARMOR: return "броня";
        case GoodType::HERBS: return "травы";
        case GoodType::POTIONS: return "зелья";
        case GoodType::CLOTHES: return "одежда";
        case GoodType::COTTON: return "хлопок";
        case GoodType::SMOKED_MEAT: return "копчености";
        default: return goodTypeToString(good);
    }
}

// Process spoilage for all items in all containers
void processSpoilage() {
    for (auto& [cid, container] : g_containers) {
        double heatMod = 1.0;
        double coldMod = 1.0;
        
        std::string regionId = container.location.has("region_id") ? container.location["region_id"].asString() : "";
        if (!regionId.empty() && g_world.regions.count(regionId)) {
            std::string weather = g_world.regions[regionId].weather;
            if (weather == "Жара") heatMod = 2.0;
            if (weather == "Снег" || weather == "Метель") coldMod = 0.3;
        }
        
        for (const auto& itemId : container.item_ids) {
            if (!g_items.count(itemId)) continue;
            
            PhysicalItem& item = g_items[itemId];
            
            int maxLife = getShelfLifeDays(item.prototype_id);
            if (maxLife == 999999) continue;
            
            int age = g_currentDay - item.batch_day;
            
            double effectiveAge = age;
            if (item.prototype_id == GoodType::MEAT || 
                item.prototype_id == GoodType::FISH ||
                item.prototype_id == GoodType::BREAD ||
                item.prototype_id == GoodType::WHEAT ||
                item.prototype_id == GoodType::SMOKED_MEAT ||
                item.prototype_id == GoodType::HERBS) {
                effectiveAge = age * heatMod * coldMod;
            }
            
            if (effectiveAge >= maxLife) {
                item.history.push_back({g_currentDay, "Сгнило полностью"});
                item.stack_size = 0;
                item.is_dirty = true;
            } else {
                double freshness = 1.0 - (effectiveAge / (double)maxLife);
                freshness = std::max(0.1, freshness);
                
                if (item.prototype_id == GoodType::IRON_ORE ||
                    item.prototype_id == GoodType::WEAPONS ||
                    item.prototype_id == GoodType::ARMOR) {
                    if (effectiveAge > maxLife * 0.5) {
                        freshness *= 0.5;
                        item.durability = std::max(1, (int)(item.durability * 0.5));
                        item.is_dirty = true;
                    }
                }
                double old_quality = item.custom_props.has("quality") ? item.custom_props["quality"].asDouble() : -1.0;
                if (std::abs(old_quality - freshness) > 0.01 || item.is_dirty) {
                    item.custom_props.set("quality", freshness);
                    item.is_dirty = true;
                }
            }
        }
    }
    
    std::vector<std::string> toRemove;
    for (auto& [itemId, item] : g_items) {
        if (item.stack_size <= 0) {
            toRemove.push_back(itemId);
        }
    }
    for (const auto& itemId : toRemove) {
        removeItem(itemId, 999999);
    }
}

// Process NPC consumption of food
void processConsumption() {
    for (auto& [npcId, npc] : g_world.npcs) {
        if (!npc.isAlive || npc.type == "ruler") continue;
        
        // Find NPC's current region
        auto rit = g_world.regions.find(npc.currentLocation);
        if (rit == g_world.regions.end()) continue;
        
        Region& region = rit->second;
        if (region.vault_id.empty()) continue;
        
        // Decrease needs
        npc.needs.hunger -= (1 + (rand() % 2));
        npc.needs.rest -= (2 + (rand() % 2));
        npc.needs.social -= 1;
        
        // Handle travel
        if (!npc.travelDestination.empty()) {
            npc.travelHoursLeft--;
            npc.currentActivity = "В пути в " + npc.travelDestination;
            npc.needs.rest -= 1;
            
            if (npc.travelHoursLeft <= 0) {
                npc.currentLocation = npc.travelDestination;
                npc.travelDestination = "";
                npc.currentActivity = "Прибыл";
            }
            continue;
        }
        
        // Check hunger
        if (npc.needs.hunger < 25) {
            npc.currentActivity = "Ищет еду";
            
            // Try to buy/eat bread
            int breadAvailable = countItemsInContainer(region.vault_id, GoodType::BREAD);
            int foodPrice = (int)region.markets["bread"];
            if (foodPrice == 0) foodPrice = 5;
            
            if (npc.gold >= foodPrice && breadAvailable > 0) {
                npc.gold -= foodPrice;
                consumeItemsFromContainer(region.vault_id, GoodType::BREAD, 1);
                region.moneySupply += foodPrice;
                npc.needs.hunger = 100;
                npc.currentActivity = "Ест";
                
                // Add to NPC inventory if they have one
                if (!npc.inventory_id.empty()) {
                    createItem(GoodType::BREAD, 1, npc.inventory_id, g_currentDay, "Куплено");
                }
            } else {
                // Try to steal or starve
                if (npc.personality.greed > 60 || npc.personality.aggression > 50) {
                    npc.currentActivity = "Ворует еду";
                    npc.needs.hunger += 40;
                } else {
                    npc.currentActivity = "Голодает";
                }
            }
        } else if (npc.needs.rest < 20) {
            npc.currentActivity = "Спит";
            npc.needs.rest += 50;
        } else {
            // Normal activity based on schedule
            int currentHour = g_world.time.internalHour;
            for (const auto& sched : npc.schedule) {
                if (currentHour >= sched.start && currentHour <= sched.end) {
                    npc.currentActivity = sched.activity;
                    
                    if (sched.activity == "Работает") {
                        npc.needs.rest -= 2;
                        
                        if (npc.profession == "Торговец") {
                            npc.gold += (rand() % 15) + 5;
                            
                            // Random trade
                            if ((rand() % 10) == 0 && !npc.inventory_id.empty()) {
                                // Pick random good from market
                                if (!region.markets.empty()) {
                                    int idx = rand() % region.markets.size();
                                    int i = 0;
                                    std::string good;
                                    for (const auto& [g, p] : region.markets) {
                                        if (i == idx) { good = g; break; }
                                        i++;
                                    }
                                    
                                    if (!good.empty()) {
                                        int price = (int)region.markets[good];
                                        int available = countItemsInContainer(region.vault_id, stringToGoodType(good));
                                        
                                        if (npc.gold >= price && available > 0) {
                                            npc.gold -= price;
                                            consumeItemsFromContainer(region.vault_id, stringToGoodType(good), 1);
                                            createItem(stringToGoodType(good), 1, npc.inventory_id, g_currentDay, "Куплено");
                                        }
                                    }
                                }
                            }
                        } else {
                            // Regular wage
                            int wage = std::max(1, (int)((region.moneySupply / std::max(1, region.population)) * npc.economy.skillLevel * 0.5));
                            npc.gold += wage;
                        }
                    }
                    break;
                }
            }
        }
        
        // Clamp needs
        npc.needs.hunger = std::max(0, std::min(100, npc.needs.hunger));
        npc.needs.rest = std::max(0, std::min(100, npc.needs.rest));
        npc.needs.social = std::max(0, std::min(100, npc.needs.social));
        
        // Check death
        if (npc.needs.hunger == 0 || npc.hp <= 0) {
            npc.isAlive = false;
            npc.currentActivity = (npc.needs.hunger == 0) ? "Мертв (Голод)" : "Мертв (Убит)";
        }
    }
}

// Process caravans movement and delivery
void processCaravans() {
    for (auto& [rid, region] : g_world.regions) {
        for (int i = (int)region.caravans.size() - 1; i >= 0; i--) {
            Caravan& caravan = region.caravans[i];
            caravan.hoursLeft--;
            
            if (caravan.hoursLeft <= 0) {
                // Arrived at destination
                auto destIt = g_world.regions.find(caravan.destination);
                if (destIt != g_world.regions.end() && !caravan.chest_id.empty()) {
                                                Region& destRegion = destIt->second;
                            
                            int threat = destRegion.threat_level;
                            int banditChance = std::min(80, threat);
                            bool isRobbed = (rand() % 100) < banditChance;
                            if (isRobbed) {
                                addNews("Караван из " + region.name + " разграблен бандитами в " + destRegion.name + "!", destRegion.id, 3, "disaster");
                                
                                if (destRegion.bandit_stash_id.empty() || !g_containers.count(destRegion.bandit_stash_id)) {
                                    destRegion.bandit_stash_id = createContainer("bandit_stash", "bandits", 999999, 1000, destRegion.id);
                                }
                                
                                auto chestIt = g_containers.find(caravan.chest_id);
                                if (chestIt != g_containers.end()) {
                                    Storage& chest = chestIt->second;
                                    std::vector<std::string> items_to_move = chest.item_ids;
                                    for (const auto& itemId : items_to_move) {
                                        moveItem(itemId, destRegion.bandit_stash_id);
                                    }
                                    g_deleted_containers.push_back(caravan.chest_id);
                                    g_containers.erase(caravan.chest_id);
                                }
                                region.caravans.erase(region.caravans.begin() + i);
                                continue;
                            }
                            
                            // Move all items from caravan chest to destination vault
                    auto chestIt = g_containers.find(caravan.chest_id);
                    if (chestIt != g_containers.end()) {
                        Storage& chest = chestIt->second;
                        double totalRevenue = 0;
                        
                        for (const auto& itemId : chest.item_ids) {
                            auto itemIt = g_items.find(itemId);
                            if (itemIt == g_items.end()) continue;
                            
                            PhysicalItem& item = itemIt->second;
                            
                            // Move to destination vault
                            moveItem(itemId, destRegion.vault_id);
                            
                            // Calculate revenue
                            double price = destRegion.markets[goodTypeToString(item.prototype_id)];
                            if (price == 0) price = BASE_PRICES[(int)item.prototype_id];
                            totalRevenue += item.stack_size * price;
                        }
                        
                        // Generate news
                        std::string goodsList;
                        for (const auto& [good, amount] : caravan.goods) {
                            if (!goodsList.empty()) goodsList += ", ";
                            goodsList += std::to_string(amount) + " " + getGoodName(stringToGoodType(good));
                        }
                        
                        addNews(
                            "ЭКОНОМИКА: Караван из " + region.name + " прибыл в " + destRegion.name + 
                            "! Доставлено: " + goodsList + ". Выручка: " + std::to_string((int)totalRevenue) + " золотых.",
                            destRegion.name, 2, "trade"
                        );
                        
                        // Remove caravan chest container
                        g_deleted_containers.push_back(caravan.chest_id);
                        g_containers.erase(caravan.chest_id);
                    }
                }
                
                // Remove caravan from region
                region.caravans.erase(region.caravans.begin() + i);
            }
        }
    }
}

// Process caravans movement and delivery
void processCaravans();

// === КАСКАДНАЯ МОДЕЛЬ ВРЕМЕНИ: ПОДСИСТЕМЫ ===

void updateWeather() {
    int month = ((g_currentDay / 30) % 12) + 1;
    std::string season = "winter";
    if (month >= 3 && month <= 5) season = "spring";
    else if (month >= 6 && month <= 8) season = "summer";
    else if (month >= 9 && month <= 11) season = "autumn";

    for (auto& [rid, region] : g_world.regions) {
        if (region.weatherDaysLeft > 0) {
            region.weatherDaysLeft--; // Теперь используется как счетчик часов
        } else {
            std::vector<std::string> weathers = {"Ясно", "Облачно"};
            if (region.climate == "tropical") {
                weathers.push_back("Тропический ливень");
                weathers.push_back("Жара");
            } else if (region.climate == "cold") {
                weathers.push_back("Снегопад");
                weathers.push_back("Метель");
            } else {
                if (season == "winter") { weathers.push_back("Снег"); weathers.push_back("Метель"); }
                else if (season == "spring" || season == "autumn") { weathers.push_back("Дождь"); weathers.push_back("Туман"); }
                else { weathers.push_back("Дождь"); weathers.push_back("Жара"); }
            }
            region.weather = weathers[rand() % weathers.size()];
            region.weatherDaysLeft = 3 + (rand() % 4); // Смена раз в 3-6 часов
        }
    }
}

void processMigration() {
    // Еженедельная миграция
    for (auto& [rid, region] : g_world.regions) {
        if (region.population > 100000 && region.markets["bread"] > 10.0) {
            int migrants = region.population * 0.01;
            region.population -= migrants;
            // В будущем мигранты будут переходить в соседние регионы
        }
    }
}

void checkGlobalEvents() {
    // Еженедельные глобальные события
    if ((rand() % 100) < 2) {
        addNews("Глобальные эфирные течения изменили свое направление. Маги по всему миру чувствуют беспокойство.", "global", 3, "misc");
    }
}

bool hasPendingOrder(const std::string& containerId, GoodType good) {
    if (!g_containers.count(containerId)) return false;
    const Storage& cont = g_containers[containerId];
    for (const auto& itemId : cont.item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
            if (item.prototype_id == GoodType::DOCUMENT_ORDER && item.order_data.has_value()) {
                if (item.order_data->item_prototype == good && 
                    (item.order_data->status == "pending" || item.order_data->status == "in_progress")) {
                    return true;
                }
            }
        }
    }
    return false;
}


void processDailyEconomy() {
    int month = ((g_currentDay / 30) % 12) + 1;
    std::string season = "winter";
    if (month >= 3 && month <= 5) season = "spring";
    else if (month >= 6 && month <= 8) season = "summer";
    else if (month >= 9 && month <= 11) season = "autumn";

    for (auto& [rid, region] : g_world.regions) {
        std::vector<int> vaultStocks((int)GoodType::COUNT, 0);
        if (!region.vault_id.empty() && g_containers.count(region.vault_id)) {
            const Storage& cont = g_containers[region.vault_id];
            for (const auto& itemId : cont.item_ids) {
                if (g_items.count(itemId)) {
                    const PhysicalItem& item = g_items[itemId];
                    vaultStocks[(int)item.prototype_id] += item.stack_size;
                }
            }
        }

        int totalFood = vaultStocks[(int)GoodType::BREAD] +
                        vaultStocks[(int)GoodType::MEAT] +
                        vaultStocks[(int)GoodType::FISH] +
                        vaultStocks[(int)GoodType::SMOKED_MEAT];
        
        double foodPerCapita = totalFood / (double)std::max(1, region.population);
        
        if (region.population > 20000 && foodPerCapita < 0.5 && (rand() % 100) < 5) {
            int deaths = region.population * (0.1 + (rand() % 10) / 100.0);
            region.population = std::max(0, region.population - deaths);
            addNews("Вспышка чумы в " + region.name + "! Голод и скученность привели к эпидемии. Погибло " + std::to_string(deaths) + " человек.", rid, 5, "disaster");
        }
        
        if ((season == "summer" || region.weather == "Жара") && (rand() % 100) < 2) {
            int wheatAmount = vaultStocks[(int)GoodType::WHEAT];
            int woodAmount = vaultStocks[(int)GoodType::WOOD];
            int cw = consumeItemsFromContainer(region.vault_id, GoodType::WHEAT, wheatAmount * 0.8);
            vaultStocks[(int)GoodType::WHEAT] -= cw;
            int cwo = consumeItemsFromContainer(region.vault_id, GoodType::WOOD, woodAmount * 0.7);
            vaultStocks[(int)GoodType::WOOD] -= cwo;
            addNews("Ужасающая засуха поразила " + region.name + ". Урожай пшеницы погиб, леса горят.", rid, 4, "disaster");
        }
        
        int totalWorkforce = region.population * 0.6;
        int totalJobs = 0;
        for (const auto& [fId, fac] : region.facilities) {
            totalJobs += fac.level * 2000;
        }
        double employmentRate = totalWorkforce > 0 ? std::min(1.0, (double)totalJobs / totalWorkforce) : 1.0;
        int activeWorkers = totalWorkforce * employmentRate;

        if (employmentRate < 0.15 && (rand() % 100) < 2) {
            addNews("Голодные бунты в " + region.name + "! Безработные громят склады и кузницы.", rid, 4, "disaster");
            int weaponsAvailable = vaultStocks[(int)GoodType::WEAPONS];
            int cw = consumeItemsFromContainer(region.vault_id, GoodType::WEAPONS, std::min(100, weaponsAvailable));
            vaultStocks[(int)GoodType::WEAPONS] -= cw;
            if (region.facilities.count("forges")) {
                region.facilities["forges"].durability -= 30;
                if (region.facilities["forges"].durability < 0) region.facilities["forges"].durability = 0;
            }
        }

        int numFacilities = std::max(1, (int)region.facilities.size());
        int workersPerSector = activeWorkers / numFacilities;
        
        double weatherMod = (region.weather == "Ясно") ? 1.2 : (region.weather == "Гроза" || region.weather == "Снег" || region.weather == "Метель" || region.weather == "Тропический ливень" || region.weather == "Снегопад") ? 0.5 : 1.0;
        double fert = g_world.homeostasis.fertility;

        auto getToolEfficiency = [&](const std::string& facName, GoodType toolType, int workers) -> double {
            if (toolType == GoodType::COUNT) return 1.0;
            int toolsAvailable = vaultStocks[(int)toolType];
            int toolsNeeded = std::max(1, workers / 50);
            if (toolsAvailable >= toolsNeeded) {
                int broken = 0;
                for(int i=0; i<toolsNeeded; ++i) if(rand()%100 < 2) broken++;
                if (broken > 0) {
                    int cb = consumeItemsFromContainer(region.vault_id, toolType, broken);
                    vaultStocks[(int)toolType] -= cb;
                }
                return 1.0;
            } else {
                double eff = 0.2 + 0.8 * ((double)toolsAvailable / toolsNeeded);
                int deficit = toolsNeeded - toolsAvailable;
                if (!hasPendingOrder(region.vault_id, toolType)) {
                    std::string orderId = createItem(GoodType::DOCUMENT_ORDER, 1, region.vault_id, g_currentDay, "Заказ инструментов");
                    if (g_items.count(orderId)) {
                        OrderData od;
                        od.issuer_id = rid;
                        od.issuer_name = region.name + " (" + facName + ")";
                        od.item_prototype = toolType;
                        od.quantity = deficit + 5;
                        od.max_price_per_unit = BASE_PRICES[(int)toolType] * 3;
                        od.deadline_days = 21;
                        od.status = "pending";
                        od.created_date = g_currentDay;
                        g_items[orderId].order_data = od;
                        g_items[orderId].custom_props.set("name", "Заказ: " + goodTypeToString(toolType));
                    }
                }
                return eff;
            }
        };
        
        if (region.facilities.count("farms") && region.facilities["farms"].level > 0) {
            int lvl = region.facilities["farms"].level;
            double eff = getToolEfficiency("farms", GoodType::SICKLE, workersPerSector);
            int a1 = (workersPerSector * lvl * 2.0 * weatherMod * fert * eff);
            int a2 = (workersPerSector * lvl * 0.5 * weatherMod * fert * eff);
            int a3 = (workersPerSector * lvl * 0.5 * weatherMod * fert * eff);
            int a4 = (workersPerSector * (lvl / 15.0) * weatherMod * fert * eff);
            int a5 = (workersPerSector * (lvl / 20.0) * weatherMod * fert * eff);
            createItem(GoodType::WHEAT, a1, region.vault_id, g_currentDay, "Фермы"); vaultStocks[(int)GoodType::WHEAT] += a1;
            createItem(GoodType::MEAT, a2, region.vault_id, g_currentDay, "Животноводство"); vaultStocks[(int)GoodType::MEAT] += a2;
            createItem(GoodType::FISH, a3, region.vault_id, g_currentDay, "Рыболовство"); vaultStocks[(int)GoodType::FISH] += a3;
            createItem(GoodType::COTTON, a4, region.vault_id, g_currentDay, "Фермы"); vaultStocks[(int)GoodType::COTTON] += a4;
            createItem(GoodType::HERBS, a5, region.vault_id, g_currentDay, "Фермы"); vaultStocks[(int)GoodType::HERBS] += a5;
        }
        if (region.facilities.count("lumbermills") && region.facilities["lumbermills"].level > 0) {
            int lvl = region.facilities["lumbermills"].level;
            double eff = getToolEfficiency("lumbermills", GoodType::AXE, workersPerSector);
            int a = (workersPerSector * (lvl / 10.0) * weatherMod * eff);
            createItem(GoodType::WOOD, a, region.vault_id, g_currentDay, "Лесопилки"); vaultStocks[(int)GoodType::WOOD] += a;
        }
        if (region.facilities.count("mines") && region.facilities["mines"].level > 0) {
            int lvl = region.facilities["mines"].level;
            double eff = getToolEfficiency("mines", GoodType::PICKAXE, workersPerSector);
            int a1 = (workersPerSector * (lvl / 10.0) * eff);
            int a2 = (workersPerSector * (lvl / 30.0) * eff);
            createItem(GoodType::IRON_ORE, a1, region.vault_id, g_currentDay, "Шахты"); vaultStocks[(int)GoodType::IRON_ORE] += a1;
            createItem(GoodType::GOLD_ORE, a2, region.vault_id, g_currentDay, "Шахты"); vaultStocks[(int)GoodType::GOLD_ORE] += a2;
        }
        
        for (auto& [fId, fac] : region.facilities) {
            if (fac.level > 0) {
                if (rand() % 100 < 20) fac.durability--;
                if (fac.durability < 0) fac.durability = 0;
                if (fac.durability < 20) fac.level = std::max(0, (int)(fac.level * 0.5));
                
                if (fac.durability < 50) {
                    int woodAvailable = vaultStocks[(int)GoodType::WOOD];
                    if (woodAvailable >= 5) {
                        fac.durability += 20;
                        int cw = consumeItemsFromContainer(region.vault_id, GoodType::WOOD, 5);
                        vaultStocks[(int)GoodType::WOOD] -= cw;
                    }
                }
            }
        }

        for (const auto& recipe : RECIPES) {
            if (!region.facilities.count(recipe.facility) || region.facilities[recipe.facility].level <= 0) continue;
            int facLevel = region.facilities[recipe.facility].level;
            int capacity = workersPerSector * (facLevel / 2.0);
            if (capacity <= 0) continue;
            
            GoodType reqTool = GoodType::COUNT;
            if (recipe.facility == "forges" || recipe.facility == "smelters") reqTool = GoodType::HAMMER;
            double eff = getToolEfficiency(recipe.facility, reqTool, workersPerSector);
            int maxCrafts = capacity * eff;
            for (const auto& in : recipe.inputs) {
                int avail = vaultStocks[(int)in.first];
                if (in.second > 0) {
                    int possible = avail / in.second;
                    if (possible < capacity) {
                        maxCrafts = std::min(maxCrafts, possible);
                        int deficit = (capacity * in.second) - avail;
                        if (deficit > 0 && !hasPendingOrder(region.vault_id, in.first)) {
                            std::string orderId = createItem(GoodType::DOCUMENT_ORDER, 1, region.vault_id, g_currentDay, "Заказ сырья");
                            if (g_items.count(orderId)) {
                                OrderData od;
                                od.issuer_id = rid;
                                od.issuer_name = region.name + " (" + recipe.facility + ")";
                                od.item_prototype = in.first;
                                od.quantity = deficit * 7;
                                od.max_price_per_unit = BASE_PRICES[(int)in.first] * 2;
                                od.deadline_days = 14;
                                od.status = "pending";
                                od.created_date = g_currentDay;
                                g_items[orderId].order_data = od;
                                g_items[orderId].custom_props.set("name", "Заказ: " + goodTypeToString(in.first));
                            }
                        }
                    }
                }
            }
            if (maxCrafts > 0) {
                double currentWeight = calculateContainerWeight(region.vault_id);
                double weightPerCraft = 0.0;
                for (const auto& out : recipe.outputs) {
                    double w = (out.first == GoodType::GOLD_INGOT) ? 0.01 : 1.0;
                    weightPerCraft += w * out.second;
                }
                if (weightPerCraft > 0) {
                    if (currentWeight + (maxCrafts * weightPerCraft) > region.storage_capacity) {
                        maxCrafts = (region.storage_capacity - currentWeight) / weightPerCraft;
                    }
                }
                if (maxCrafts > 0) {
                    for (const auto& in : recipe.inputs) {
                        int c = consumeItemsFromContainer(region.vault_id, in.first, maxCrafts * in.second);
                        vaultStocks[(int)in.first] -= c;
                    }
                    for (const auto& out : recipe.outputs) {
                        createItem(out.first, maxCrafts * out.second, region.vault_id, g_currentDay, "Производство");
                        vaultStocks[(int)out.first] += maxCrafts * out.second;
                    }
                }
            }
        }
        
        int foodNeed = region.population * 0.005;
        int eaten = consumeItemsFromContainer(region.vault_id, GoodType::BREAD, foodNeed);
        vaultStocks[(int)GoodType::BREAD] -= eaten;
        if (eaten < foodNeed) {
            int c = consumeItemsFromContainer(region.vault_id, GoodType::MEAT, foodNeed - eaten);
            eaten += c; vaultStocks[(int)GoodType::MEAT] -= c;
        }
        if (eaten < foodNeed) {
            int c = consumeItemsFromContainer(region.vault_id, GoodType::FISH, foodNeed - eaten);
            eaten += c; vaultStocks[(int)GoodType::FISH] -= c;
        }
        if (eaten < foodNeed) {
            int c = consumeItemsFromContainer(region.vault_id, GoodType::WHEAT, foodNeed - eaten);
            eaten += c; vaultStocks[(int)GoodType::WHEAT] -= c;
        }
        
        if (eaten < foodNeed) {
            int deaths = (foodNeed - eaten) * 2;
            region.population = std::max(0, region.population - deaths);
            if (rand() % 100 < 10) addNews("Голод в " + region.name + "! Погибло " + std::to_string(deaths) + " человек.", rid, 4, "disaster");
        } else if (region.population < 100000) {
            region.population += region.population * 0.005;
        }
        
        if (g_world.factions.count(region.factionId)) {
            int taxRevenue = region.moneySupply * 0.02;
            region.moneySupply -= taxRevenue;
            if (taxRevenue > 0) {
                createItem(GoodType::GOLD_INGOT, taxRevenue, region.vault_id, g_currentDay, "Налоги");
                vaultStocks[(int)GoodType::GOLD_INGOT] += taxRevenue;
            }
        }
        
        for (int i=0; i<(int)GoodType::COUNT; i++) {
            GoodType gt = (GoodType)i;
            double base = BASE_PRICES[i];
            int stock = vaultStocks[i];
            double demand = region.population * 0.01;
            double price = base * (demand / std::max(1.0, (double)stock));
            price = std::clamp(price, base * 0.2, base * 5.0);
            region.markets[goodTypeToString(gt)] = price;
        }

        // Caravan logic
        for (int i=0; i<(int)GoodType::COUNT; i++) {
            GoodType good = (GoodType)i;
            std::string goodStr = goodTypeToString(good);
            double localPrice = region.markets[goodStr];
            std::string bestDest = "";
            double maxProfit = 0;

            for (auto& [destId, dest] : g_world.regions) {
                if (destId == rid) continue;
                
                std::string originFactionId = region.factionId;
                if (g_world.factions.count(originFactionId)) {
                    const Faction& originFaction = g_world.factions[originFactionId];
                    auto it = originFaction.diplomacy.find(dest.factionId);
                    if (it != originFaction.diplomacy.end() && it->second == "war") continue;
                }

                if (good == GoodType::BREAD || good == GoodType::MEAT || good == GoodType::WHEAT) {
                    int dailyConsumption = region.population * 0.005;
                    if (vaultStocks[i] < dailyConsumption * 10) continue;
                }
                if (good == GoodType::WEAPONS) {
                    if (vaultStocks[i] < 300) continue;
                }

                double profitMargin = dest.markets[goodStr] - localPrice;
                int supply = vaultStocks[i];
                
                if (profitMargin > localPrice * 0.3 && supply > 50) {
                    if (profitMargin > maxProfit) {
                        maxProfit = profitMargin;
                        bestDest = destId;
                    }
                }
            }

            if (!bestDest.empty()) {
                Region& destRegion = g_world.regions[bestDest];
                int supply = vaultStocks[i];
                int amount = supply * 0.2;
                double cost = amount * localPrice;

                if (amount > 0) {
                    const int WAGON_CAPACITY = 1000;
                    const int MAX_WAGONS = 15;
                    
                    double weightPerUnit = (good == GoodType::GOLD_INGOT) ? 0.01 : 1.0;
                    double totalDesiredWeight = amount * weightPerUnit;
                    int wagonsNeeded = std::ceil(totalDesiredWeight / WAGON_CAPACITY);
                    
                    if (wagonsNeeded > MAX_WAGONS) {
                        wagonsNeeded = MAX_WAGONS;
                        amount = (MAX_WAGONS * WAGON_CAPACITY) / weightPerUnit;
                    }
                    if (amount <= 0) continue;
                    
                    cost = amount * localPrice; 
                    
                    std::string caravanChestId = createContainer("caravan_chest", region.factionId, MAX_WAGONS * WAGON_CAPACITY, 1000, rid);
                    int taken = consumeItemsFromContainer(region.vault_id, good, amount);
                    vaultStocks[i] -= taken;
                    createItem(good, taken, caravanChestId, g_currentDay, "Караван");
                    
                    region.moneySupply += cost;
                    
                    Caravan caravan;
                    caravan.id = "caravan_" + generateUUID();
                    caravan.origin = rid;
                    caravan.destination = bestDest;
                    caravan.chest_id = caravanChestId;
                    caravan.wagons = wagonsNeeded;
                    caravan.goods[goodStr] = taken;
                    caravan.hoursLeft = 24 + (rand() % 48);
                    
                    region.caravans.push_back(caravan);
                    
                    if (taken >= 100 && (rand() % 100) < 40) {
                        addNews("ЭКОНОМИКА: Из " + region.name + " в " + destRegion.name + " отправлен торговый караван (" + std::to_string(wagonsNeeded) + " повозок). Груз: " + std::to_string(taken) + " ед. " + getGoodName(good) + ".", rid, 2, "trade");
                    }
                }
            }
        }
    }

    for (auto&[id, npc] : g_world.npcs) {
        if (!npc.isAlive) continue;
        if (g_world.regions.count(npc.currentLocation)) {
            Region& r = g_world.regions[npc.currentLocation];
            if (!npc.economy.isEmployed && (rand() % 100) < 10) npc.economy.isEmployed = true;
            else if (npc.economy.isEmployed && (rand() % 100) < 2) npc.economy.isEmployed = false;

            if (npc.economy.isEmployed) {
                int wage = std::max(1, (int)((r.moneySupply / std::max(1, r.population)) * npc.economy.skillLevel * 0.1));
                npc.economy.savings += wage;
            }
            
            int foodPrice = r.markets.count("bread") ? (int)r.markets["bread"] : 5;
            int breadAvailable = countItemsInContainer(r.vault_id, GoodType::BREAD);
            if (npc.economy.savings >= foodPrice && breadAvailable > 0) {
                npc.economy.savings -= foodPrice;
                consumeItemsFromContainer(r.vault_id, GoodType::BREAD, 1);
                r.moneySupply += foodPrice;
                npc.needs.hunger = 100;
            }
        }
    }
}

void processDailyMilitary() {
    std::unordered_map<std::string, std::vector<int>> vaultStocks;
    for (const auto& [rid, r] : g_world.regions) {
        vaultStocks[rid].assign((int)GoodType::COUNT, 0);
        if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
            const Storage& cont = g_containers[r.vault_id];
            for (const auto& itemId : cont.item_ids) {
                if (g_items.count(itemId)) {
                    const PhysicalItem& item = g_items[itemId];
                    vaultStocks[rid][(int)item.prototype_id] += item.stack_size;
                }
            }
        }
    }

    for (auto& [fId, f] : g_world.factions) {
        int globalWeapons = 0;
        int globalFood = 0;
        int totalPopulation = 0;
        int regionCount = 0;
        
        for (const auto& rId : f.regions) {
            if (g_world.regions.count(rId)) {
                const Region& r = g_world.regions[rId];
                globalWeapons += vaultStocks[rId][(int)GoodType::WEAPONS];
                globalFood += vaultStocks[rId][(int)GoodType::BREAD] + 
                              vaultStocks[rId][(int)GoodType::MEAT] + 
                              vaultStocks[rId][(int)GoodType::FISH] + 
                              vaultStocks[rId][(int)GoodType::WHEAT];
                totalPopulation += r.population;
                regionCount++;
            }
        }
        
        std::string capitalRegionId = f.regions.empty() ? "" : f.regions[0];
        if (!capitalRegionId.empty() && g_world.regions.count(capitalRegionId)) {
            int passiveIncome = regionCount * 500;
            if (passiveIncome > 0) {
                createItem(GoodType::GOLD_INGOT, passiveIncome, g_world.regions[capitalRegionId].vault_id, g_currentDay, "Пассивный доход");
                vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT] += passiveIncome;
            }
        }
        
        int armyUpkeep = f.armies.size() * 500;
        int stateUpkeep = regionCount * 50;
        int goldToRemove = armyUpkeep + stateUpkeep;
        
        for (const auto& rId : f.regions) {
            if (goldToRemove <= 0) break;
            if (g_world.regions.count(rId)) {
                const Region& r = g_world.regions[rId];
                int regionGold = vaultStocks[rId][(int)GoodType::GOLD_INGOT];
                int toRemove = std::min(regionGold, goldToRemove);
                if (toRemove > 0) {
                    int c = consumeItemsFromContainer(r.vault_id, GoodType::GOLD_INGOT, toRemove);
                    vaultStocks[rId][(int)GoodType::GOLD_INGOT] -= c;
                    goldToRemove -= c;
                }
            }
        }
        
        if (goldToRemove > 0) {
            for (const auto& rId : f.regions) {
                if (goldToRemove <= 0) break;
                if (g_world.regions.count(rId)) {
                    const Region& r = g_world.regions[rId];
                    int weaponsAvailable = vaultStocks[rId][(int)GoodType::WEAPONS];
                    int toRemove = std::min(weaponsAvailable, goldToRemove / 10);
                    if (toRemove > 0) {
                        int c = consumeItemsFromContainer(r.vault_id, GoodType::WEAPONS, toRemove);
                        vaultStocks[rId][(int)GoodType::WEAPONS] -= c;
                        goldToRemove -= c * 10;
                    }
                }
            }
            if (goldToRemove > 0 && !f.armies.empty()) {
                int armyIdx = rand() % f.armies.size();
                Army& mutinousArmy = f.armies[armyIdx];
                mutinousArmy.size = std::max(0, (int)(mutinousArmy.size * 0.5));
                std::string homeRegion = mutinousArmy.location;
                if (!homeRegion.empty() && g_world.regions.count(homeRegion)) {
                    g_world.regions[homeRegion].threat_level = std::min(100, g_world.regions[homeRegion].threat_level + 15);
                }
                addNews("Дезертирство! Из-за нехватки золота часть армии " + f.name + " дезертировала, пополнив ряды бандитов.", homeRegion, 4, "disaster");
            }
        }
        
        double foodPerCapita = globalFood / (double)std::max(1, totalPopulation);
        if (foodPerCapita < 0.5) {
            for (const auto& rId : f.regions) {
                if (g_world.regions.count(rId)) {
                    Region& r = g_world.regions[rId];
                    int deaths = r.population * 0.01;
                    r.population = std::max(0, r.population - deaths);
                    r.threat_level = std::min(100, r.threat_level + 10);
                    if ((rand() % 100) < 10) {
                        addNews("Голодный бунт! Нехватка продовольствия в " + r.name + " привела к жертвам и росту преступности.", rId, 4, "disaster");
                    }
                }
            }
        }
    }

    for (auto&[fid, faction] : g_world.factions) {
        for (int i = faction.armies.size() - 1; i >= 0; i--) {
            Army& a = faction.armies[i];
            if (a.daysToMove > 0) {
                a.daysToMove--;
                continue;
            }
            std::string targetLoc = a.destination;
            bool armySurvived = true;
            bool isCombatActive = false;

            std::string enemyFactionId = "";
            int defArmyIndex = -1;
            for (auto& [eId, eFaction] : g_world.factions) {
                if (eId != fid && faction.diplomacy[eId] == "war") {
                    for (size_t j = 0; j < eFaction.armies.size(); j++) {
                        if (eFaction.armies[j].destination == targetLoc && eFaction.armies[j].daysToMove <= 0) {
                            enemyFactionId = eId;
                            defArmyIndex = j;
                            break;
                        }
                    }
                }
                if (!enemyFactionId.empty()) break;
            }

            if (!enemyFactionId.empty()) {
                isCombatActive = true;
                Faction& defender = g_world.factions[enemyFactionId];
                Army& defArmy = defender.armies[defArmyIndex];

                double atkPower = a.size * (a.morale / 100.0) * ((rand() % 50) / 100.0 + 0.8);
                double defPower = defArmy.size * (defArmy.morale / 100.0) * ((rand() % 50) / 100.0 + 1.0);

                std::string locName = g_world.regions.count(targetLoc) ? g_world.regions[targetLoc].name : targetLoc;
                if (atkPower > defPower) {
                    int casualties = defArmy.size * 0.8;
                    a.size -= a.size * 0.2;
                    addNews("Кровавая битва у " + locName + ": Армия " + faction.name + " разбила войска " + defender.name + "! Потери врага: " + std::to_string(casualties) + ".", targetLoc, 5, "war");
                    defender.armies.erase(defender.armies.begin() + defArmyIndex);
                    isCombatActive = false;
                } else {
                    int casualties = a.size * 0.8;
                    defArmy.size -= defArmy.size * 0.2;
                    addNews("Битва у " + locName + ": Войска " + defender.name + " уничтожили армию " + faction.name + "! Потери нападавших: " + std::to_string(casualties) + ".", targetLoc, 5, "war");
                    faction.armies.erase(faction.armies.begin() + i);
                    armySurvived = false;
                }
            } else if (g_world.regions.count(targetLoc)) {
                Region& targetRegion = g_world.regions[targetLoc];
                if (faction.diplomacy[targetRegion.factionId] == "war") {
                    isCombatActive = true;
                    if (a.siegeDays == -1) {
                        a.siegeDays = 3 + rand() % 4;
                        addNews("Армия " + faction.name + " взяла в осаду " + targetRegion.name + "!", targetLoc, 4, "war");
                    } else if (a.siegeDays > 0) {
                        a.siegeDays--;
                        
                        if (!a.supply_chest_id.empty()) {
                            int armyBread = countItemsInContainer(a.supply_chest_id, GoodType::BREAD);
                            int armyMeat = countItemsInContainer(a.supply_chest_id, GoodType::MEAT);
                            int dailyNeed = a.size * 0.5;
                            int consumed = 0;
                            
                            if (armyBread > 0) {
                                consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::BREAD, std::min(dailyNeed, armyBread));
                            }
                            if (consumed < dailyNeed && armyMeat > 0) {
                                consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::MEAT, std::min(dailyNeed - consumed, armyMeat));
                            }
                            
                            if (consumed < dailyNeed) {
                                a.morale -= 20;
                                if (a.morale <= 0) {
                                    addNews("Армия " + faction.name + " распалась от голода при осаде " + targetRegion.name + "!", targetLoc, 4, "war");
                                    faction.armies.erase(faction.armies.begin() + i);
                                    armySurvived = false;
                                }
                            }
                        }
                        
                        if (armySurvived) {
                            targetRegion.population = std::max(0, targetRegion.population - (rand() % 200));
                            int cityBread = vaultStocks[targetLoc][(int)GoodType::BREAD];
                            if (cityBread > 0) {
                                int c = consumeItemsFromContainer(targetRegion.vault_id, GoodType::BREAD, cityBread * 0.2);
                                vaultStocks[targetLoc][(int)GoodType::BREAD] -= c;
                            }
                            if (targetRegion.facilities.count("farms")) {
                                targetRegion.facilities["farms"].durability -= 10;
                                if (targetRegion.facilities["farms"].durability < 0) targetRegion.facilities["farms"].durability = 0;
                            }
                        }
                    } else if (a.siegeDays == 0) {
                        int garrisonPower = targetRegion.population / 100;
                        if (a.size > garrisonPower) {
                            targetRegion.factionId = fid;
                            
                            std::string capitalRegionId = faction.regions.empty() ? "" : faction.regions[0];
                            if (!capitalRegionId.empty() && !targetRegion.vault_id.empty()) {
                                Storage& targetChest = g_containers[targetRegion.vault_id];
                                std::string capitalVaultId = g_world.regions[capitalRegionId].vault_id;
                                
                                std::vector<std::string> itemsToMove = targetChest.item_ids;
                                for (const auto& itemId : itemsToMove) {
                                    moveItem(itemId, capitalVaultId);
                                }
                                for (const auto& itemId : itemsToMove) {
                                    if (g_items.count(itemId)) {
                                        vaultStocks[capitalRegionId][(int)g_items[itemId].prototype_id] += g_items[itemId].stack_size;
                                        vaultStocks[targetLoc][(int)g_items[itemId].prototype_id] -= g_items[itemId].stack_size;
                                    }
                                }
                            }
                            
                            targetRegion.moneySupply *= 0.5;
                            addNews("ШТУРМ УСПЕШЕН! " + targetRegion.name + " пал под натиском " + faction.name + "! Город разграблен.", targetLoc, 5, "war");
                            isCombatActive = false;
                        } else {
                            addNews("ОСАДА СНЯТА! Ополчение " + targetRegion.name + " отбило штурм " + faction.name + ".", targetLoc, 5, "war");
                            faction.armies.erase(faction.armies.begin() + i);
                            armySurvived = false;
                        }
                    }
                }
            }
            
            if (armySurvived && !isCombatActive && i < faction.armies.size()) {
                std::string homeRegionId = faction.armies[i].location;
                if (!homeRegionId.empty() && g_world.regions.count(homeRegionId)) {
                    int returnedWeapons = faction.armies[i].size * 0.8;
                    if (returnedWeapons > 0) {
                        createItem(GoodType::WEAPONS, returnedWeapons, g_world.regions[homeRegionId].vault_id, g_currentDay, "Возврат армии");
                        vaultStocks[homeRegionId][(int)GoodType::WEAPONS] += returnedWeapons;
                    }
                }
                faction.armies.erase(faction.armies.begin() + i);
            }
        }
    }
}


std::string getSubContainer(const std::string& parentId, const std::string& type) {
    for (const auto& [cid, cont] : g_containers) {
        if (cont.type == type && cont.location.has("parent_container") && cont.location["parent_container"].asString() == parentId) {
            return cid;
        }
    }
    return "";
}

void dailyTick();
void weeklyTick();


// Forward declarations
// void simulateOneDay(); (replaced by dailyTick)

// Simulate one hour
void processCouriers() {
    for (auto& [npcId, npc] : g_world.npcs) {
        if (!npc.isAlive || npc.profession != "Гонец") continue;

        if (!npc.travelDestination.empty()) {
            npc.travelHoursLeft--;
            npc.currentActivity = "Доставляет письмо в " + npc.travelDestination;
            npc.needs.rest -= 1;

            if (npc.travelHoursLeft <= 0) {
                npc.currentLocation = npc.travelDestination;
                npc.travelDestination = "";
                npc.currentActivity = "Прибыл";

                if (!npc.delivery_target_id.empty() && g_containers.count(npc.delivery_target_id)) {
                    if (!npc.inventory_id.empty() && g_containers.count(npc.inventory_id)) {
                        Storage& inv = g_containers[npc.inventory_id];
                        std::vector<std::string> to_move;
                        for (const auto& itemId : inv.item_ids) {
                            if (g_items.count(itemId) && g_items[itemId].prototype_id == GoodType::DOCUMENT_ORDER) {
                                to_move.push_back(itemId);
                            }
                        }
                        for (const auto& itemId : to_move) {
                            moveItem(itemId, npc.delivery_target_id);
                        }
                    }
                }
                npc.delivery_target_id = "";
            }
            continue;
        }

        if (g_world.regions.count(npc.currentLocation)) {
            Region& r = g_world.regions[npc.currentLocation];
            if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
                Storage& vault = g_containers[r.vault_id];
                std::string orderToDeliver = "";
                for (const auto& itemId : vault.item_ids) {
                    if (g_items.count(itemId) && g_items[itemId].prototype_id == GoodType::DOCUMENT_ORDER) {
                        orderToDeliver = itemId;
                        break;
                    }
                }

                if (!orderToDeliver.empty()) {
                    std::string targetInbox = "";
                    std::string targetRegion = "";
                    for (const auto& [nId, merchant] : g_world.npcs) {
                        if (merchant.profession == "Торговец" && !merchant.economy.workplaceId.empty()) {
                            targetInbox = getSubContainer(merchant.economy.workplaceId, "inbox");
                            targetRegion = merchant.homeLocation;
                            if (!targetInbox.empty()) break;
                        }
                    }

                    if (!targetInbox.empty()) {
                        moveItem(orderToDeliver, npc.inventory_id);
                        npc.travelDestination = targetRegion;
                        npc.delivery_target_id = targetInbox;
                        npc.travelHoursLeft = (npc.currentLocation == targetRegion) ? 2 : 24 + (rand() % 24);
                        npc.currentActivity = "Взял заказ, отправляюсь в " + targetRegion;
                    }
                } else {
                    npc.currentActivity = "Ожидает поручений";
                }
            }
        }
    }
}


void processMerchantOrders() {
    for (auto& [npcId, merchant] : g_world.npcs) {
        if (!merchant.isAlive || merchant.profession != "Торговец" || merchant.economy.workplaceId.empty()) continue;

        if (!merchant.travelDestination.empty()) {
            merchant.travelHoursLeft--;
            merchant.currentActivity = "Везет товар в " + merchant.travelDestination;
            merchant.needs.rest -= 1;
            if (merchant.travelHoursLeft <= 0) {
                merchant.currentLocation = merchant.travelDestination;
                merchant.travelDestination = "";
                merchant.currentActivity = "Прибыл для сделки";
            }
            continue;
        }

        std::string archiveId = getSubContainer(merchant.economy.workplaceId, "archive");
        std::string safeId = getSubContainer(merchant.economy.workplaceId, "safe");
        if (archiveId.empty() || safeId.empty() || !g_containers.count(merchant.inventory_id)) continue;

        Storage& inv = g_containers[merchant.inventory_id];
        std::vector<std::string> completed_orders;

        for (const auto& itemId : inv.item_ids) {
            if (g_items.count(itemId)) {
                PhysicalItem& item = g_items[itemId];
                if (item.prototype_id == GoodType::DOCUMENT_ORDER && item.order_data.has_value()) {
                    OrderData& od = item.order_data.value();
                    if (od.status == "in_progress") {
                        int has_qty = countItemsInContainer(merchant.inventory_id, od.item_prototype);
                        if (has_qty >= od.quantity) {
                            if (merchant.currentLocation == od.issuer_id) {
                                if (g_world.regions.count(od.issuer_id)) {
                                    Region& targetReg = g_world.regions[od.issuer_id];
                                    consumeItemsFromContainer(merchant.inventory_id, od.item_prototype, od.quantity);
                                    createItem(od.item_prototype, od.quantity, targetReg.vault_id, g_currentDay, "Доставка по заказу");
                                    int payment = od.quantity * od.max_price_per_unit;
                                    createItem(GoodType::GOLD_INGOT, payment, safeId, g_currentDay, "Оплата заказа");
                                    od.status = "delivered";
                                    item.is_dirty = true;
                                    completed_orders.push_back(itemId);
                                    merchant.currentActivity = "Заказ выполнен";
                                }
                            } else {
                                merchant.travelDestination = od.issuer_id;
                                merchant.travelHoursLeft = 24 + (rand() % 24);
                            }
                        } else {
                            if (g_world.regions.count(merchant.currentLocation)) {
                                Region& curReg = g_world.regions[merchant.currentLocation];
                                int needed = od.quantity - has_qty;
                                int available = countItemsInContainer(curReg.vault_id, od.item_prototype);
                                if (available >= needed) {
                                    int cost = needed * BASE_PRICES[(int)od.item_prototype];
                                    consumeItemsFromContainer(curReg.vault_id, od.item_prototype, needed);
                                    createItem(od.item_prototype, needed, merchant.inventory_id, g_currentDay, "Закупка для заказа");
                                    curReg.moneySupply += cost;
                                    merchant.currentActivity = "Закупает товар";
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }

        for (const auto& cId : completed_orders) {
            moveItem(cId, archiveId);
        }
    }
}


void hourlyTick() {
    processConsumption();
    processCaravans();

    processCouriers();

    processMerchantOrders();
    
    if (g_world.time.internalHour % 4 == 0) {
        updateWeather();
    }
    
    g_world.time.internalHour++;
    if (g_world.time.internalHour >= 24) {
        g_world.time.internalHour = 0;
        dailyTick();
        if (g_currentDay % 7 == 0) {
            weeklyTick();
        }
    }
}

// Simulate one day
int availableManpower(const Faction& f, std::unordered_map<std::string, std::vector<int>>& vaultStocks) {
    int total = 0;
    for (const auto& rid : f.regions) {
        if (g_world.regions.count(rid) == 0) continue;
        const Region& r = g_world.regions.at(rid);
        int weapons = vaultStocks[rid][(int)GoodType::WEAPONS];
        int food = vaultStocks[rid][(int)GoodType::BREAD] + vaultStocks[rid][(int)GoodType::MEAT];
        int possible = std::min(r.population / 10, weapons);
        if (food < possible * 0.5) continue;
        total += possible;
    }
    return total;
}

void processRulerDiplomacy() {
    std::unordered_map<std::string, std::vector<int>> vaultStocks;
    for (const auto& [rid, r] : g_world.regions) {
        vaultStocks[rid].assign((int)GoodType::COUNT, 0);
        if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
            const Storage& cont = g_containers[r.vault_id];
            for (const auto& itemId : cont.item_ids) {
                if (g_items.count(itemId)) {
                    const PhysicalItem& item = g_items[itemId];
                    vaultStocks[rid][(int)item.prototype_id] += item.stack_size;
                }
            }
        }
    }

    std::vector<std::string> fKeys;
    for (const auto& [fid, f] : g_world.factions) fKeys.push_back(fid);

    for (auto& [rId, ruler] : g_world.npcs) {
        if (ruler.type != "ruler" || !ruler.isAlive || ruler.id.find("_heir") != std::string::npos) continue;
        
        if (g_world.factions.count(ruler.factionId) == 0) continue;
        Faction& faction = g_world.factions[ruler.factionId];

        std::string goalKey = ruler.factionId + "_goal";
        if (g_world.nexusData.count(goalKey)) {
            ruler.gmOverride = g_world.nexusData[goalKey]["value"].asString();
        }

        if (!ruler.gmOverride.empty()) {
            ruler.currentGoal = "gm_override -> " + ruler.gmOverride;
            continue;
        }

        int totalWeapons = 0;
        int totalFood = 0;
        for (const auto& rid : faction.regions) {
            if (g_world.regions.count(rid)) {
                totalWeapons += vaultStocks[rid][(int)GoodType::WEAPONS];
                totalFood += vaultStocks[rid][(int)GoodType::BREAD];
            }
        }
        int security = (totalWeapons > 100 ? 50 : totalWeapons) + (faction.armies.size() * 10);

        std::string capitalRegionId = faction.regions.empty() ? "" : faction.regions[0];
        std::string capitalVault = capitalRegionId.empty() ? "" : g_world.regions[capitalRegionId].vault_id;
        int wealth = capitalRegionId.empty() ? 0 : vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT];

        int power = availableManpower(faction, vaultStocks);

        if ((rand() % 100) < 15) {
            std::string targetF = fKeys[rand() % fKeys.size()];
            if (targetF == ruler.factionId) continue;
            Faction& targetFaction = g_world.factions[targetF];
            int targetPower = availableManpower(targetFaction, vaultStocks);

            // 1. ОЦЕНКА УГРОЗЫ И КОАЛИЦИИ
            if (targetPower > power * 2 && targetFaction.diplomacy[ruler.factionId] == "war") {
                if (security < 30 || power < 1000) {
                    ruler.currentGoal = "surrender -> " + targetF;
                    faction.diplomacy[targetF] = "neutral";
                    targetFaction.diplomacy[ruler.factionId] = "neutral";
                    if (!capitalVault.empty()) {
                        int goldAmount = vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT];
                        int tribute = goldAmount * 0.5;
                        int c = consumeItemsFromContainer(capitalVault, GoodType::GOLD_INGOT, tribute);
                        vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT] -= c;
                        std::string targetCapitalId = targetFaction.regions.empty() ? "" : targetFaction.regions[0];
                        if (!targetCapitalId.empty()) {
                            createItem(GoodType::GOLD_INGOT, tribute, g_world.regions[targetCapitalId].vault_id, g_currentDay, "Контрибуция");
                            vaultStocks[targetCapitalId][(int)GoodType::GOLD_INGOT] += tribute;
                        }
                    }
                    addNews("КАПИТУЛЯЦИЯ: Осознав неизбежность краха, " + ruler.name + " подписал унизительный мир с " + targetFaction.name + ", выплатив огромную контрибуцию.", "global", 5, "war");
                    continue;
                }
            }

            // 2. АГРЕССИЯ
            double foodPerCapita = totalFood / (double)std::max(1, (int)(faction.regions.size() * 20000));
            if (ruler.rulerPersonality.cruelty > 60 && power > targetPower * 1.2 && ruler.rulerPersonality.ambition > 50) {
                int warWeary = g_world.homeostasis.warWeariness;
                if (warWeary < 50 && security > 50 && foodPerCapita > 0.5) { // Воюем только если в стране есть еда
                    ruler.currentGoal = "declare_war -> " + targetF;
                    if (faction.diplomacy[targetF] != "war") {
                        faction.diplomacy[targetF] = "war";
                        targetFaction.diplomacy[ruler.factionId] = "war";
                        addNews("ВОЙНА: Уверенный в своем превосходстве, " + ruler.name + " бросает легионы " + faction.name + " на земли " + targetFaction.name + "!", "global", 5, "war");
                    }
                }
            }
            // 3. ИНТРИГИ
            else if (ruler.rulerPersonality.paranoia > 55 && targetPower >= power * 0.8) {
                std::vector<std::string> intrigueTypes = {"sabotage", "bribery"};
                if (ruler.rulerPersonality.cruelty > 70) intrigueTypes.push_back("assassination");
                if (ruler.rulerPersonality.ambition > 60) intrigueTypes.push_back("rebellion");
                std::string selectedType = intrigueTypes[rand() % intrigueTypes.size()];
                ruler.currentGoal = "start_intrigue -> " + targetF;
                
                Intrigue intr;
                intr.id = "intr_" + generateUUID();
                intr.type = selectedType;
                intr.initiatorFactionId = ruler.factionId;
                intr.targetFactionId = targetF;
                intr.targetRulerId = targetFaction.rulerId;
                intr.progress = 0;
                intr.requiredProgress = (selectedType == "rebellion") ? 120 : 60;
                intr.progressPerDay = std::max(1, ruler.rulerPersonality.paranoia / 15);
                intr.discoveryChance = 3;
                intr.isDiscovered = false;
                intr.startDay = g_currentDay;
                g_world.intrigues.push_back(intr);
                
                addNews("ИНТРИГА: " + ruler.name + " запускает тайную операцию (" + selectedType + ") против " + targetFaction.name + "!", "global", 3, "war");
            }
            // 4. ЭКОНОМИКА И СОЮЗЫ
            else if (ruler.rulerPersonality.stewardship > 50 && wealth < 10000) {
                ruler.currentGoal = "trade_pact -> " + targetF;
                faction.relations[targetF] += 10;
                if (!capitalVault.empty()) {
                    createItem(GoodType::GOLD_INGOT, 2000, capitalVault, g_currentDay, "Торговое соглашение");
                    vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT] += 2000;
                }
                addNews("ЭКОНОМИКА: " + ruler.name + " заключает выгодное торговое соглашение с " + targetFaction.name + ".", "global", 2, "misc");
            }
            // 5. ДИПЛОМАТИЯ И БРАКИ
            else if (ruler.rulerPersonality.diplomacy > 55 && faction.relations[targetF] > 50) {
                bool hasHeir = !ruler.heir.empty() && g_world.npcs.count(ruler.heir);
                bool targetHasHeir = !targetFaction.rulerId.empty() && g_world.npcs.count(targetFaction.rulerId) && !g_world.npcs[targetFaction.rulerId].heir.empty();
                
                if (hasHeir && targetHasHeir && (rand() % 100) < 40) {
                    ruler.currentGoal = "marriage_alliance -> " + targetF;
                    faction.relations[targetF] = 100;
                    targetFaction.relations[ruler.factionId] = 100;
                    addNews("ДИНАСТИЧЕСКИЙ БРАК: Дома " + faction.name + " и " + targetFaction.name + " объединились узами брака!", "global", 5, "misc");
                } else {
                    ruler.currentGoal = "offer_alliance -> " + targetF;
                    faction.relations[targetF] = std::min(100, faction.relations[targetF] + 20);
                    addNews("ДИПЛОМАТИЯ: " + ruler.name + " укрепляет союз с " + targetFaction.name + ".", "global", 2, "misc");
                }
            }
        }
    }
    
    for (auto& [fid, faction] : g_world.factions) {
        int power = availableManpower(faction, vaultStocks);
        std::string atWarWith = "";
        for (const auto& [k, v] : faction.diplomacy) {
            if (v == "war") { atWarWith = k; break; }
        }

        if (!atWarWith.empty()) {
            std::string homeRegionId = "";
            for (const auto& rid : faction.regions) {
                if (g_world.regions.count(rid)) {
                    int w = vaultStocks[rid][(int)GoodType::WEAPONS];
                    if (w > 10) { homeRegionId = rid; break; }
                }
            }

            if (!homeRegionId.empty() && power > 500) {
                Region& homeRegion = g_world.regions[homeRegionId];
                std::string targetRegionId = g_world.factions[atWarWith].regions.empty() ? "" : g_world.factions[atWarWith].regions[0];
                
                bool alreadyAttacking = false;
                for (const auto& a : faction.armies) {
                    if (a.destination == targetRegionId) alreadyAttacking = true;
                }

                if (!targetRegionId.empty() && !alreadyAttacking) {
                    int armySize = power * (0.15 + (rand() % 20) / 100.0);
                    if (armySize < 100) armySize = power;

                    int weaponsAvailable = vaultStocks[homeRegionId][(int)GoodType::WEAPONS];
                    int foodAvailable = vaultStocks[homeRegionId][(int)GoodType::BREAD] + vaultStocks[homeRegionId][(int)GoodType::MEAT];
                    
                    int weaponsToTake = std::min(armySize, weaponsAvailable);
                    int foodToTake = std::min(armySize * 2, foodAvailable);
                    
                    int c = consumeItemsFromContainer(homeRegion.vault_id, GoodType::WEAPONS, weaponsToTake);
                    vaultStocks[homeRegionId][(int)GoodType::WEAPONS] -= c;
                    int breadTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::BREAD, foodToTake * 0.7);
                    vaultStocks[homeRegionId][(int)GoodType::BREAD] -= breadTaken;
                    int meatTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::MEAT, foodToTake * 0.3);
                    vaultStocks[homeRegionId][(int)GoodType::MEAT] -= meatTaken;
                    
                    std::string armyChestId = createContainer("army_supply_chest", fid, 999999, 1000, homeRegionId);
                    createItem(GoodType::BREAD, breadTaken, armyChestId, g_currentDay, "Припасы армии");
                    createItem(GoodType::MEAT, meatTaken, armyChestId, g_currentDay, "Припасы армии");
                    
                    int armyMorale = 100;
                    if (weaponsToTake < armySize * 0.5) armyMorale -= 25;
                    if (foodToTake < armySize) armyMorale -= 25;
                    
                    Army army;
                    army.id = "army_" + generateUUID();
                    army.size = armySize;
                    army.morale = armyMorale;
                    army.location = homeRegionId;
                    army.destination = targetRegionId;
                    army.daysToMove = 3;
                    army.siegeDays = -1;
                    army.supply_chest_id = armyChestId;
                    
                    faction.armies.push_back(army);
                    addNews("Снаряженная армия " + faction.name + " (" + std::to_string(armySize) + " воинов) выступила из " + homeRegion.name + " в поход на " + g_world.regions[targetRegionId].name + ".", homeRegionId, 4, "war");
                }
            }
        }
    }
}

void checkRulerDeaths() {
    std::vector<std::string> deadRulers;
    for (auto& [id, npc] : g_world.npcs) {
        if (npc.type == "ruler" && npc.isAlive) {
            std::string capital;
            if (g_world.factions.count(npc.factionId) && !g_world.factions[npc.factionId].regions.empty()) {
                capital = g_world.factions[npc.factionId].regions[0];
            }
            
            if (!capital.empty() && g_world.regions.count(capital)) {
                Region& capReg = g_world.regions[capital];
                
                // Считаем ВСЮ еду в казне
                int totalFood = countItemsInContainer(capReg.vault_id, GoodType::BREAD) +
                                countItemsInContainer(capReg.vault_id, GoodType::MEAT) +
                                countItemsInContainer(capReg.vault_id, GoodType::FISH) +
                                countItemsInContainer(capReg.vault_id, GoodType::SMOKED_MEAT);
                
                // Если еды критически мало (< 20 единиц)
                if (totalFood < 20) {
                    // 1. Попытка ГОСЗАКУПКИ: Фракция покупает еду на своем рынке в казну
                    std::vector<GoodType> foodTypes = {GoodType::BREAD, GoodType::MEAT, GoodType::FISH};
                    bool bought = false;
                    
                    for (auto ft : foodTypes) {
                        std::string ftStr = goodTypeToString(ft);
                        int marketStock = countItemsInContainer(capReg.vault_id, ft); // В данном случае проверяем наличие товара в регионе вообще (упрощенно через vault)
                        // На самом деле нам нужно проверить, есть ли еда на продажу в регионе
                        // Но в нашей модели еда региона и так лежит в vault. 
                        // Логическая проблема: правитель голодает, когда в ЕГО ЖЕ vault нет еды.
                        // Значит, нам нужно проверить, нет ли еды у NPC этого города.
                    }

                    // Упростим: если еды в vault нет, но у фракции есть деньги в moneySupply региона,
                    // мы просто материализуем еду из воздуха, списывая деньги (имитация закупки у мелких фермеров)
                    double breadPrice = capReg.markets["bread"];
                    if (breadPrice <= 0) breadPrice = 5;
                    
                    if (capReg.moneySupply >= breadPrice * 10) {
                        capReg.moneySupply -= breadPrice * 10;
                        createItem(GoodType::BREAD, 10, capReg.vault_id, g_currentDay, "Госзакупка");
                        addNews("СНАБЖЕНИЕ: Казна фракции " + g_world.factions[npc.factionId].name + " пополнена продовольствием за счет налогов.", capital, 1, "trade");
                        continue; // Смерть отменяется
                    }

                    // Если и денег нет - тогда шанс смерти
                    if ((rand() % 1000) < 2) {
                        npc.isAlive = false;
                        npc.alive = false;
                        deadRulers.push_back(id);
                        addNews("ТРАГЕДИЯ: Правитель " + npc.name + " умер от голода в пустой сокровищнице!", "global", 5, "disaster");
                    }
                }
            }
            
            // Смерть от старости (0.01%)
            if (npc.isAlive && (rand() % 10000) < 1) {
                npc.isAlive = false;
                npc.alive = false;
                deadRulers.push_back(id);
                addNews("ПЕЧАЛЬНЫЕ ВЕСТИ: Правитель " + npc.name + " мирно скончался от старости.", "global", 5, "misc");
            }
        }
    }
    // ... (остальной код замены правителя без изменений) ...
    for (const auto& id : deadRulers) {
        NPC& r = g_world.npcs[id];
        if (!r.heir.empty() && g_world.npcs.count(r.heir)) {
            NPC& heir = g_world.npcs[r.heir];
            g_world.factions[r.factionId].rulerId = heir.id;
            heir.profession = "Правитель";
        } else {
            std::string capital;
            if (!g_world.factions[r.factionId].regions.empty()) capital = g_world.factions[r.factionId].regions[0];
            if (!capital.empty() && g_world.regions.count(capital)) {
                std::string vault = g_world.regions[capital].vault_id;
                int w = countItemsInContainer(vault, GoodType::WEAPONS);
                consumeItemsFromContainer(vault, GoodType::WEAPONS, w * 0.5);
            }
        }
    }
}

void processIntrigues() {
    for (int i = g_world.intrigues.size() - 1; i >= 0; i--) {
        auto& intr = g_world.intrigues[i];
        intr.progress += intr.progressPerDay;
        
        if (!intr.isDiscovered && (rand() % 100) < intr.discoveryChance) {
            intr.isDiscovered = true;
            addNews("СКАНДАЛ! Раскрыт заговор (" + intr.type + ") фракции " + g_world.factions[intr.initiatorFactionId].name + " против " + g_world.factions[intr.targetFactionId].name + "!", "global", 4, "war");
            g_world.factions[intr.targetFactionId].relations[intr.initiatorFactionId] -= 60;
            if (g_world.factions[intr.targetFactionId].relations[intr.initiatorFactionId] < -50) {
                g_world.factions[intr.targetFactionId].diplomacy[intr.initiatorFactionId] = "war";
                g_world.factions[intr.initiatorFactionId].diplomacy[intr.targetFactionId] = "war";
                addNews("ВОЙНА ИЗ-ЗА ИНТРИГ: Оскорбленная фракция объявляет войну!", "global", 5, "war");
            }
        }

        if (intr.progress >= intr.requiredProgress) {
            if (intr.type == "assassination" && !intr.targetRulerId.empty() && g_world.npcs.count(intr.targetRulerId)) {
                g_world.npcs[intr.targetRulerId].isAlive = false;
                g_world.npcs[intr.targetRulerId].alive = false;
                addNews("ТЕМНЫЕ ДЕЛА: Правитель убит в результате успешного покушения!", "global", 5, "war");
            } else if (intr.type == "sabotage") {
                std::string cap;
                if (!g_world.factions[intr.targetFactionId].regions.empty()) cap = g_world.factions[intr.targetFactionId].regions[0];
                if (!cap.empty() && g_world.regions.count(cap)) {
                    std::string vault = g_world.regions[cap].vault_id;
                    int w = countItemsInContainer(vault, GoodType::WEAPONS);
                    consumeItemsFromContainer(vault, GoodType::WEAPONS, w * 0.3);
                    addNews("ДИВЕРСИЯ: Экономика пострадала от саботажников.", "global", 3, "disaster");
                }
            } else if (intr.type == "rebellion") {
                for (const auto& rid : g_world.factions[intr.targetFactionId].regions) {
                    if(g_world.regions.count(rid)) {
                        g_world.regions[rid].population *= 0.7;
                        int w = countItemsInContainer(g_world.regions[rid].vault_id, GoodType::WEAPONS);
                        consumeItemsFromContainer(g_world.regions[rid].vault_id, GoodType::WEAPONS, w * 0.4);
                        break; 
                    }
                }
                addNews("МЯТЕЖ: Вспыхнуло восстание, спонсированное извне!", "global", 5, "war");
            }
            g_world.intrigues.erase(g_world.intrigues.begin() + i);
        }
    }
}

std::string processGmIntervention(const JsonValue& command) {
    std::string cmd = command["command"].asString();
    const JsonValue& args = command["args"];
    std::string feedback = "";

    if (cmd == "gmPurchaseGoods") {
        std::string factionId = args["factionId"].asString();
        std::string regionId = args["regionId"].asString();
        GoodType goodType = stringToGoodType(args["goodType"].asString());
        int quantity = args["quantity"].asInt();

        if (g_world.factions.count(factionId) && g_world.regions.count(regionId) && quantity > 0) {
            Faction& fac = g_world.factions[factionId];
            Region& reg = g_world.regions[regionId];
            double price = reg.markets[goodTypeToString(goodType)];
            if (price == 0) price = 1;
            int cost = price * quantity;

            int supply = countItemsInContainer(reg.vault_id, goodType);
            std::string capitalRegionId = fac.regions.empty() ? "" : fac.regions[0];
            int goldAvailable = capitalRegionId.empty() ? 0 : countItemsInContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT);

            if (supply < quantity) {
                feedback = "[Экономика] Отказ: В " + reg.name + " нет столько " + goodTypeToString(goodType) + ".";
            } else if (goldAvailable < cost) {
                feedback = "[Экономика] Отказ: У фракции " + fac.name + " нет " + std::to_string(cost) + " золота.";
            } else {
                consumeItemsFromContainer(reg.vault_id, goodType, quantity);
                consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT, cost);
                createItem(goodType, quantity, g_world.regions[capitalRegionId].vault_id, g_currentDay, "Закупка ГМ");
                reg.moneySupply += cost;
                if ((double)quantity / (supply + quantity) > 0.2) {
                    reg.markets[goodTypeToString(goodType)] = price * 1.15;
                }
                feedback = "[Экономика] Фракция " + fac.name + " закупила " + std::to_string(quantity) + " " + goodTypeToString(goodType) + " в " + reg.name + " за " + std::to_string(cost) + " з.";
                g_world.gmInterventionHistory.push_back(cmd);
            }
        }
    } else if (cmd == "gmSellGoods") {
        std::string factionId = args["factionId"].asString();
        std::string regionId = args["regionId"].asString();
        GoodType goodType = stringToGoodType(args["goodType"].asString());
        int quantity = args["quantity"].asInt();

        if (g_world.factions.count(factionId) && g_world.regions.count(regionId) && quantity > 0) {
            Faction& fac = g_world.factions[factionId];
            Region& reg = g_world.regions[regionId];
            std::string capitalRegionId = fac.regions.empty() ? "" : fac.regions[0];
            int supply = capitalRegionId.empty() ? 0 : countItemsInContainer(g_world.regions[capitalRegionId].vault_id, goodType);

            if (supply < quantity) {
                feedback = "[Экономика] Отказ: У фракции " + fac.name + " нет столько " + goodTypeToString(goodType) + ".";
            } else {
                double price = reg.markets[goodTypeToString(goodType)];
                if (price == 0) price = 1;
                int revenue = price * quantity;

                consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, goodType, quantity);
                createItem(goodType, quantity, reg.vault_id, g_currentDay, "Продажа ГМ");
                reg.moneySupply = std::max(0.0, reg.moneySupply - revenue);
                createItem(GoodType::GOLD_INGOT, revenue, g_world.regions[capitalRegionId].vault_id, g_currentDay, "Выручка ГМ");

                if ((double)quantity / std::max(1, countItemsInContainer(reg.vault_id, goodType)) > 0.2) {
                    reg.markets[goodTypeToString(goodType)] = std::max(1.0, price * 0.85);
                }
                feedback = "[Экономика] Фракция " + fac.name + " продала " + std::to_string(quantity) + " " + goodTypeToString(goodType) + " в " + reg.name + " за " + std::to_string(revenue) + " з.";
                g_world.gmInterventionHistory.push_back(cmd);
            }
        }
    } else if (cmd == "gmInvestInFacility") {
        std::string factionId = args["factionId"].asString();
        std::string regionId = args["regionId"].asString();
        std::string facilityType = args["facilityType"].asString();
        std::string action = args["action"].asString();

        if (g_world.factions.count(factionId) && g_world.regions.count(regionId)) {
            Faction& fac = g_world.factions[factionId];
            Region& reg = g_world.regions[regionId];
            std::string capitalRegionId = fac.regions.empty() ? "" : fac.regions[0];
            int goldAvailable = capitalRegionId.empty() ? 0 : countItemsInContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT);

            if (action == "repair") {
                int cost = 500;
                if (goldAvailable >= cost) {
                    consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT, cost);
                    reg.facilities[facilityType].durability = 100;
                    feedback = "[Инвестиции] " + fac.name + " отремонтировала " + facilityType + " в " + reg.name + ".";
                    g_world.gmInterventionHistory.push_back(cmd);
                } else {
                    feedback = "[ERROR] У " + fac.name + " нет " + std::to_string(cost) + " з. на ремонт.";
                }
            } else if (action == "upgrade") {
                int cost = 2000;
                if (goldAvailable >= cost) {
                    consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT, cost);
                    reg.facilities[facilityType].level += 1;
                    feedback = "[Инвестиции] " + fac.name + " улучшила " + facilityType + " в " + reg.name + ".";
                    g_world.gmInterventionHistory.push_back(cmd);
                } else {
                    feedback = "[ERROR] У " + fac.name + " нет " + std::to_string(cost) + " з. на улучшение.";
                }
            }
        }
    } else if (cmd == "gmRaiseMilitia") {
        std::string factionId = args["factionId"].asString();
        std::string regionId = args["regionId"].asString();

        if (g_world.factions.count(factionId) && g_world.regions.count(regionId)) {
            Faction& fac = g_world.factions[factionId];
            Region& reg = g_world.regions[regionId];
            int drafts = reg.population * 0.05;
            reg.population -= drafts;

            int weaponsNeeded = drafts * 0.8;
            int foodNeeded = drafts * 2;
            int weaponsTaken = consumeItemsFromContainer(reg.vault_id, GoodType::WEAPONS, weaponsNeeded);
            int breadTaken = consumeItemsFromContainer(reg.vault_id, GoodType::BREAD, foodNeeded * 0.7);
            int meatTaken = consumeItemsFromContainer(reg.vault_id, GoodType::MEAT, foodNeeded * 0.3);

            std::string militiaChestId = createContainer("army_supply_chest", fac.id, 999999, 1000, regionId);
            createItem(GoodType::BREAD, breadTaken, militiaChestId, g_currentDay, "Ополчение");
            createItem(GoodType::MEAT, meatTaken, militiaChestId, g_currentDay, "Ополчение");

            feedback = "[Мобилизация] " + fac.name + " призвала " + std::to_string(drafts) + " рекрутов из " + reg.name + ". Изъято " + std::to_string(weaponsTaken) + " оружия.";
            g_world.gmInterventionHistory.push_back(cmd);
        }
    } else if (cmd == "gmSpreadRumor") {
        std::string factionId = args["factionId"].asString();
        std::string targetFactionId = args["targetFactionId"].asString();
        int invest = args["investmentGold"].asInt();
        std::string type = args["type"].asString();

        if (g_world.factions.count(factionId) && g_world.factions.count(targetFactionId)) {
            Faction& fac = g_world.factions[factionId];
            Faction& targetFac = g_world.factions[targetFactionId];
            std::string capitalRegionId = fac.regions.empty() ? "" : fac.regions[0];
            int goldAvailable = capitalRegionId.empty() ? 0 : countItemsInContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT);

            if (goldAvailable >= invest) {
                consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT, invest);
                int power = std::max(1, invest / 500);

                if (type == "slander") {
                    std::string targetCapitalId = targetFac.regions.empty() ? "" : targetFac.regions[0];
                    int foodLost = 0;
                    if (!targetCapitalId.empty() && g_world.regions.count(targetCapitalId)) {
                        foodLost = countItemsInContainer(g_world.regions[targetCapitalId].vault_id, GoodType::BREAD) * 0.2;
                        consumeItemsFromContainer(g_world.regions[targetCapitalId].vault_id, GoodType::BREAD, foodLost);
                    }
                    feedback = "[Слухи] " + fac.name + " распускает слухи о " + targetFac.name + ". Враг потерял " + std::to_string(foodLost) + " еды.";
                } else {
                    fac.relations[targetFactionId] = std::min(100, fac.relations[targetFactionId] + power * 2);
                    feedback = "[Слухи] " + fac.name + " улучшает имидж " + targetFac.name + ".";
                }
                g_world.gmInterventionHistory.push_back(cmd);
            } else {
                feedback = "[ERROR] Нет золота для слухов.";
            }
        }
    } else if (cmd == "gmFrameForSabotage") {
        std::string factionId = args["factionId"].asString();
        std::string targetFactionId = args["targetFactionId"].asString();
        std::string regionId = args["regionId"].asString();

        if (g_world.factions.count(factionId) && g_world.factions.count(targetFactionId) && g_world.regions.count(regionId)) {
            Faction& fac = g_world.factions[factionId];
            Faction& targetFac = g_world.factions[targetFactionId];
            Region& reg = g_world.regions[regionId];
            std::string capitalRegionId = fac.regions.empty() ? "" : fac.regions[0];
            int goldAvailable = capitalRegionId.empty() ? 0 : countItemsInContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT);

            if (goldAvailable >= 3000) {
                consumeItemsFromContainer(g_world.regions[capitalRegionId].vault_id, GoodType::GOLD_INGOT, 3000);
                reg.moneySupply *= 0.8;
                if (g_world.factions.count(reg.factionId)) {
                    g_world.factions[reg.factionId].relations[targetFactionId] -= 40;
                }
                feedback = "[Саботаж] " + fac.name + " устроила диверсию в " + reg.name + ", подставив " + targetFac.name + "!";
                g_world.gmInterventionHistory.push_back(cmd);
            } else {
                feedback = "[ERROR] Провал саботажа (нужно 3000 з).";
            }
        }
    } else if (cmd == "gmDirectResourceInjection") {
        std::string regionId = args["regionId"].asString();
        GoodType goodType = stringToGoodType(args["goodType"].asString());
        int quantity = args["quantity"].asInt();

        if (g_currentDay - g_world.lastDirectInjectionDay < 7) {
            feedback = "[ERROR] gmDirectResourceInjection на кулдауне.";
        } else if (g_world.regions.count(regionId)) {
            g_world.lastDirectInjectionDay = g_currentDay;
            createItem(goodType, quantity, g_world.regions[regionId].vault_id, g_currentDay, "Божественное вмешательство");
            feedback = "[Вмешательство] В " + g_world.regions[regionId].name + " появилось " + std::to_string(quantity) + " " + goodTypeToString(goodType) + ".";
            g_world.gmInterventionHistory.push_back(cmd);
        }
    }
    return feedback;
}


void processMerchants() {
    for (auto& [npcId, merchant] : g_world.npcs) {
        if (!merchant.isAlive || merchant.profession != "Торговец" || merchant.economy.workplaceId.empty()) continue;

        std::string inboxId = getSubContainer(merchant.economy.workplaceId, "inbox");
        std::string archiveId = getSubContainer(merchant.economy.workplaceId, "archive");

        if (inboxId.empty() || archiveId.empty() || !g_containers.count(inboxId)) continue;

        Storage& inbox = g_containers[inboxId];
        std::vector<std::string> to_accept;
        std::vector<std::string> to_reject;

        for (const auto& itemId : inbox.item_ids) {
            if (g_items.count(itemId)) {
                PhysicalItem& item = g_items[itemId];
                if (item.prototype_id == GoodType::DOCUMENT_ORDER && item.order_data.has_value()) {
                    OrderData& od = item.order_data.value();
                    if (od.status == "pending") {
                        int baseCost = BASE_PRICES[(int)od.item_prototype];
                        // Оценка рентабельности: выгодно ли браться за заказ
                        if (od.max_price_per_unit >= baseCost * 1.2) {
                            od.status = "in_progress";
                            od.assigned_merchant_id = merchant.id;
                            item.is_dirty = true;
                            to_accept.push_back(itemId);
                        } else {
                            od.status = "cancelled";
                            item.is_dirty = true;
                            to_reject.push_back(itemId);
                        }
                    }
                }
            }
        }

        // Физическое перемещение писем
        for (const auto& itemId : to_accept) {
            moveItem(itemId, merchant.inventory_id);
        }
        for (const auto& itemId : to_reject) {
            moveItem(itemId, archiveId);
        }
    }
}


void processDailyThreat() {
    for (auto& [rid, r] : g_world.regions) {
        int delta = 0;
        
        // 1. Безработица
        int totalJobs = 0;
        for (const auto& [fid, fac] : r.facilities) {
            totalJobs += fac.level * 2000;
        }
        int employed = std::min(r.population, totalJobs);
        double unemploymentRate = r.population > 0 ? (r.population - employed) / (double)r.population : 0.0;
        if (unemploymentRate > 0.2) delta += 2 + (int)((unemploymentRate - 0.2) * 10);

        // 2. Голод
        int food = countItemsInContainer(r.vault_id, GoodType::BREAD)
                 + countItemsInContainer(r.vault_id, GoodType::MEAT)
                 + countItemsInContainer(r.vault_id, GoodType::FISH);
        double foodPerCapita = food / (double)std::max(1, r.population);
        if (foodPerCapita < 0.8) delta += 5;
        if (foodPerCapita < 0.3) delta += 10;

        // 3. Война / налеты
        if (g_world.factions.count(r.factionId)) {
            const auto& f = g_world.factions[r.factionId];
            for (const auto& [otherFid, relation] : f.diplomacy) {
                if (relation == "war" && g_world.factions.count(otherFid)) {
                    delta += 3;
                }
            }
        }

        r.threat_level = std::max(0, std::min(100, r.threat_level + delta));

        // 4. Автоматическое снижение угрозы при благополучии
        if (unemploymentRate < 0.3 && foodPerCapita > 0.8) {
            r.threat_level = std::max(0, r.threat_level - (1 + rand() % 2));
        }
        
        // Влияние на население: гибель/миграция при высокой угрозе
        if (r.threat_level > 70) {
            int deaths = (r.threat_level / 10) * 0.005 * r.population;
            r.population = std::max(0, r.population - deaths);
        }

        // 5. Зачистка бандитов (возврат награбленного), если угроза упала
        if (r.threat_level < 50 && !r.bandit_stash_id.empty()) {
            if (g_containers.count(r.bandit_stash_id)) {
                Storage& stash = g_containers[r.bandit_stash_id];
                std::vector<std::string> items_to_move = stash.item_ids;
                for (const auto& itemId : items_to_move) {
                    moveItem(itemId, r.vault_id);
                }
                g_deleted_containers.push_back(r.bandit_stash_id);
                g_containers.erase(r.bandit_stash_id);
            }
            r.bandit_stash_id.clear();
        }

        // 6. Фракции тратят золото на патрули для снижения угрозы
        if (!r.factionId.empty() && g_world.factions.count(r.factionId)) {
            std::string capitalId = g_world.factions[r.factionId].regions.empty() ? "" 
                                    : g_world.factions[r.factionId].regions[0];
            if (!capitalId.empty() && g_world.regions.count(capitalId)) {
                int gold = countItemsInContainer(g_world.regions[capitalId].vault_id, GoodType::GOLD_INGOT);
                int cost = 200 + rand() % 300;
                if (gold >= cost && r.threat_level > 20) {
                    consumeItemsFromContainer(g_world.regions[capitalId].vault_id, GoodType::GOLD_INGOT, cost);
                    r.threat_level = std::max(0, r.threat_level - (5 + rand() % 10));
                }
            }
        }
    }
}


void dailyTick() {
    g_currentDay++;
    int activeWars = 0;
    for (auto& [fid, faction] : g_world.factions) {
        for (auto& [tid, status] : faction.diplomacy) {
            if (status == "war") activeWars++;
        }
    }
    activeWars /= 2;
    if (activeWars >= 2) g_world.homeostasis.warWeariness = std::min(100, g_world.homeostasis.warWeariness + 4);
    else if (activeWars == 0) g_world.homeostasis.warWeariness = std::max(0, g_world.homeostasis.warWeariness - 2);
    
    processSpoilage();
    processDailyEconomy();
    processDailyMilitary();
    processRulerDiplomacy();
    processIntrigues();

    processMerchants();
    processDailyThreat();
    checkRulerDeaths();
}

void weeklyTick() {
    processMigration();
    checkGlobalEvents();
}

// Build initial world
void buildWorld(const std::string& playerId, const std::string& era, int initialAgents, const JsonValue& globalLocs) {
    g_playerId = playerId;
    g_world = World();
    g_world.era = era.empty() ? "rebirth" : era;
    g_currentDay = 0;
    
    struct FactionConfig { std::string name; };
    std::map<std::string, FactionConfig> fConfig;
    std::map<std::string, std::string> locMap;
    
    if (g_world.era == "architects") {
        fConfig = { {"orthodoxy", {"Ортодоксия Решетки"}}, {"syndicate", {"Синдикат Экспансии"}}, {"greencode", {"Фракция Зеленого Кода"}}, {"ascendancy", {"Культ Перехода"}}, {"apostates", {"Апостаты Пустоты"}} };
        locMap = { {"nexus_prime", "orthodoxy"}, {"solar_citadel", "orthodoxy"}, {"obsidian_wall", "orthodoxy"}, {"sky_harbor", "syndicate"}, {"silver_conduits", "syndicate"}, {"whispering_gardens", "greencode"}, {"aethel_spires", "greencode"}, {"genesis_craters", "greencode"}, {"arcanum_archive", "ascendancy"}, {"crystal_matrix", "ascendancy"}, {"bio_forge", "ascendancy"}, {"void_bastion", "apostates"}, {"resonance_pits", "syndicate"}, {"deep_sea_obs", "orthodoxy"} };
    } else if (g_world.era == "silence") {
        fConfig = { {"iron_remnant", {"Железный Остаток"}}, {"flesh_cult", {"Культ Плоти"}}, {"heralds", {"Вестники Безмолвия"}}, {"logic_purge", {"Орден Логической Чистки"}}, {"scavengers", {"Падальщики Нексуса"}} };
        locMap = { {"iron_remnant_base", "iron_remnant"}, {"rusting_spires", "iron_remnant"}, {"flesh_craft_pits", "flesh_cult"}, {"bone_fields", "flesh_cult"}, {"forgotten_obs", "heralds"}, {"muted_valley", "heralds"}, {"deep_vault_7", "logic_purge"}, {"crystal_wastes", "logic_purge"}, {"scrap_canyon", "scavengers"}, {"aquilon_ruins", "scavengers"}, {"sunken_haven", "scavengers"}, {"ash_desert", "scavengers"}, {"silent_forest", "flesh_cult"}, {"dead_lake", "flesh_cult"}, {"whispering_dunes", "scavengers"} };
    } else if (g_world.era == "sundering") {
        fConfig = { {"survivors", {"Выжившие Аквилона"}}, {"mutants", {"Улей Мутантов"}}, {"storm_cult", {"Культ Эфирного Шторма"}}, {"mad_constructs", {"Безумные Конструкты"}} };
        locMap = { {"falling_aquilon", "survivors"}, {"ruined_sky_harbor", "survivors"}, {"ashen_coast", "survivors"}, {"mutant_hive", "mutants"}, {"flesh_labyrinth", "mutants"}, {"burning_forest", "mutants"}, {"expanding_scar", "storm_cult"}, {"storms_eye", "storm_cult"}, {"bleeding_earth", "storm_cult"}, {"glass_desert", "mad_constructs"}, {"sunken_arcanum", "mad_constructs"}, {"chasm_of_screams", "mad_constructs"}, {"shattered_peaks", "survivors"}, {"void_wastes", "storm_cult"}, {"boiling_sea", "mutants"} };
    } else {
        fConfig = { {"aquilon", {"Аквилонская Директория"}}, {"khazadrim", {"Кхазадримский Конклав"}}, {"sylvanesti", {"Сильванестийский Симбиоз"}}, {"gronnar", {"Гроннарская Орда"}}, {"consortium", {"Свободные Торговцы"}}, {"crimson", {"Орден Багрового Пламени"}} };
        locMap = { {"capital_aquilon", "aquilon"}, {"ruins_arcanum", "aquilon"}, {"thunder_citadel", "khazadrim"}, {"crystal_caves", "khazadrim"}, {"dragon_spine_mountains", "khazadrim"}, {"whispering_woods", "sylvanesti"}, {"floating_islands_of_aethel", "sylvanesti"}, {"nomad_lands_ash_plains", "gronnar"}, {"the_scarred_wastes", "gronnar"}, {"the_shifting_sands_of_khem", "gronnar"}, {"silver_haven", "consortium"}, {"sunken_city_of_aeridor", "consortium"}, {"sanctum_of_whispers", "crimson"}, {"ether_scar_chasm", "crimson"}, {"forgotten_observatory", "crimson"} };
    }
    
    std::vector<std::string> fKeys;
    for (auto& [fid, bf] : fConfig) {
        Faction f;
        f.id = fid;
        f.name = bf.name;
        g_world.factions[fid] = f;
        fKeys.push_back(fid);
    }

    for (auto& f1 : fKeys) {
        for (auto& f2 : fKeys) {
            if (f1 != f2) {
                g_world.factions[f1].relations[f2] = (rand() % 100) - 50;
                g_world.factions[f1].diplomacy[f2] = "neutral";
            }
        }
    }
    
    std::vector<std::string> locKeys;
    if (globalLocs.type == JsonValue::OBJECT && globalLocs.size() > 0) {
        for (const auto& kv : globalLocs.obj_val) {
            if (kv.first != "startLocation") locKeys.push_back(kv.first);
        }
    } else {
        for (const auto& kv : locMap) locKeys.push_back(kv.first);
    }

    for (const auto& key : locKeys) {
        std::string ownerId = locMap.count(key) ? locMap[key] : fKeys[rand() % fKeys.size()];
        bool isDwarf = (ownerId == "khazadrim");
        bool isElf = (ownerId == "sylvanesti" || ownerId == "greencode");

        Region r;
        r.id = key;
        r.name = globalLocs.has(key) && globalLocs[key].has("name") ? globalLocs[key]["name"].asString() : key;
        r.factionId = ownerId;
        r.population = 5000 + (rand() % 45000);
        r.moneySupply = 50000 + (rand() % 100000);
        r.climate = "temperate";
        r.weather = "Ясно";

        r.vault_id = createContainer("faction_vault", ownerId, 999999, 1000, key);
        r.storage_capacity = 10000 + (r.population / 5); // Базовая вместимость + бонус от населения
        r.threat_level = 10 + (rand() % 20); // Небольшая начальная угроза

        for (int i = 0; i < (int)GoodType::COUNT; i++) {
            GoodType gt = (GoodType)i;
            int baseAmount = rand() % 500;
            if (gt == GoodType::WHEAT || gt == GoodType::WOOD || gt == GoodType::IRON_ORE || gt == GoodType::COTTON) {
                baseAmount = (r.population / 2) + (rand() % 1000);
            } else if (gt == GoodType::BREAD || gt == GoodType::MEAT || gt == GoodType::FISH) {
                baseAmount = r.population / 3;
            } else if (gt == GoodType::WEAPONS || gt == GoodType::ARMOR) {
                baseAmount = r.population / 20;
            }

            if (isElf && (gt == GoodType::WOOD || gt == GoodType::HERBS || gt == GoodType::WHEAT)) baseAmount *= 2;
            if (isDwarf && (gt == GoodType::IRON_ORE || gt == GoodType::GOLD_ORE)) baseAmount *= 2;

            if (baseAmount > 0) {
                createItem(gt, baseAmount, r.vault_id, 0, "Начальные запасы");
            }
            r.markets[goodTypeToString(gt)] = BASE_PRICES[i];
        }

        r.facilities["farms"] = {isElf ? 15 : (rand() % 6) + 3, 100}; // Баланс: минимум 3 уровень ферм, чтобы регион не умер на старте
        r.facilities["lumbermills"] = {isElf ? 10 : rand() % 5, 100};
        r.facilities["mines"] = {isDwarf ? 20 : rand() % 5, 100};
        r.facilities["forges"] = {isDwarf ? 15 : rand() % 5, 100};
        r.facilities["weavers"] = {(rand() % 5) + 1, 100};
        r.facilities["alchemists"] = {isElf ? 5 : rand() % 3, 100};
        r.facilities["banks"] = {ownerId == "consortium" ? 3 : (rand() % 2), 100};
        r.facilities["mills"] = {(rand() % 5) + 1, 100};
        r.facilities["bakeries"] = {(rand() % 5) + 1, 100};
        r.facilities["smokehouses"] = {rand() % 5, 100};
        r.facilities["smelters"] = {isDwarf ? 10 : rand() % 4, 100};
        r.facilities["tailors"] = {rand() % 4, 100};
        r.facilities["jewelers"] = {ownerId == "consortium" ? 5 : rand() % 2, 100};

        r.animals.herbivores = isElf ? 10000 : 500 + (rand() % 2000);
        r.animals.carnivores = isElf ? 1000 : 50 + (rand() % 200);

        g_world.regions[key] = r;
        g_world.factions[ownerId].regions.push_back(key);
    }
    
    // Create NPCs
    std::vector<std::string> names = {"Боб", "Грег", "Элиза", "Торбин", "Лиара", "Каэль", "Морган", "Сильвия"};
    std::vector<std::string> professions = {"Кузнец", "Фермер", "Стражник", "Торговец", "Маг", "Трактирщик", "Гонец", "Клерк"};
    
    std::vector<std::string> regionIds;
    for (auto& [rid, r] : g_world.regions) regionIds.push_back(rid);
    
    if (!regionIds.empty()) {
        for (int i = 0; i < initialAgents; i++) {
        NPC npc;
        npc.id = "npc_" + generateUUID();
        npc.name = names[rand() % names.size()] + " " + professions[rand() % professions.size()];
        npc.type = "npc";
        npc.profession = professions[rand() % professions.size()];
        npc.homeLocation = regionIds[rand() % regionIds.size()];
        npc.currentLocation = npc.homeLocation;
        npc.currentActivity = "Отдыхает";
        
        // Schedule
        npc.schedule = {
            {0, 6, "Спит", npc.homeLocation},
            {7, 8, "Ест", npc.homeLocation},
            {9, 18, "Работает", npc.homeLocation},
            {19, 21, "Отдыхает", npc.homeLocation},
            {22, 23, "Спит", npc.homeLocation}
        };
        
        // Personality
        npc.personality.aggression = rand() % 100;
        npc.personality.sociability = rand() % 100;
        npc.personality.greed = rand() % 100;
        npc.personality.loyalty = rand() % 100;
        
        // Economy
        npc.economy.skillLevel = 1 + (rand() % 10);
        npc.economy.savings = rand() % 500;
        
        // Inventory
        npc.gold = rand() % 100;
        npc.inventory_id = createContainer("npc_inventory", npc.id, 100, 20, npc.homeLocation, npc.id);
        
        if (npc.profession == "Торговец") {
            std::string officeId = createContainer("merchant_office", npc.id, 999999, 1000, npc.homeLocation);
            createContainer("inbox", npc.id, 999999, 1000, npc.homeLocation, "", officeId);
            createContainer("outbox", npc.id, 999999, 1000, npc.homeLocation, "", officeId);
            createContainer("archive", npc.id, 999999, 1000, npc.homeLocation, "", officeId);
            createContainer("safe", npc.id, 999999, 1000, npc.homeLocation, "", officeId);
            npc.economy.workplaceId = officeId;
            npc.economy.isEmployed = true;
        }
        
            g_world.npcs[npc.id] = npc;
        }
    }
    
    // Create rulers
    for (auto& [fid, faction] : g_world.factions) {
        NPC ruler;
        ruler.id = "ruler_" + fid;
        ruler.type = "ruler";
        ruler.factionId = fid;
        ruler.name = "Правитель " + faction.name;
        ruler.homeLocation = g_world.factions[fid].regions.empty() ? "" : g_world.factions[fid].regions[0];
        ruler.currentLocation = ruler.homeLocation;
        ruler.isAlive = true;
        ruler.alive = true;
        ruler.health = 100;
        
        ruler.rulerPersonality.ambition = 40 + (rand() % 40);
        ruler.rulerPersonality.wisdom = 40 + (rand() % 40);
        ruler.rulerPersonality.military = 40 + (rand() % 40);
        ruler.rulerPersonality.cruelty = 40 + (rand() % 40);
        ruler.rulerPersonality.diplomacy = 40 + (rand() % 40);
        ruler.rulerPersonality.paranoia = 40 + (rand() % 40);
        ruler.rulerPersonality.stewardship = 40 + (rand() % 40);
        
        g_world.npcs[ruler.id] = ruler;
        g_world.factions[fid].rulerId = ruler.id;
    }

    // Assign Clerks to Merchant Offices
    std::map<std::string, std::vector<std::string>> regionOffices;
    for (const auto& [nid, n] : g_world.npcs) {
        if (n.profession == "Торговец" && !n.economy.workplaceId.empty()) {
            regionOffices[n.homeLocation].push_back(n.economy.workplaceId);
        }
    }
    for (auto& [nid, n] : g_world.npcs) {
        if (n.profession == "Клерк" && regionOffices.count(n.homeLocation) && !regionOffices[n.homeLocation].empty()) {
            n.economy.workplaceId = regionOffices[n.homeLocation][rand() % regionOffices[n.homeLocation].size()];
            n.economy.isEmployed = true;
        }
    }

    
    // Initial news
    addNews("Мир создан. Эпоха " + g_world.era + " начинается.", "Весь мир", 3, "misc");
}

// Simulate N ticks
void reportProgress(int currentTick, const std::string& lastNews,
                    const JsonValue& dirtyItems,
                    const JsonValue& dirtyContainers,
                    const JsonValue& deletedItems,
                    const JsonValue& deletedContainers)
{
    JsonValue res = JsonValue::object();
    res.set("status", "progress");

    int totalDays = currentTick / 24;
    int years = totalDays / 360;
    int months = (totalDays % 360) / 30 + 1;

    std::string msg = "Симуляция истории: Год " + std::to_string(years + 1)
                    + ", Месяц " + std::to_string(months);
    if (!lastNews.empty()) {
        msg += " | Последнее событие: " + lastNews;
    }
    res.set("message", msg);

    // Отправляем ТОЛЬКО изменившиеся данные
    if (!dirtyItems.arr_val.empty())
        res.set("items", dirtyItems);
    if (!dirtyContainers.arr_val.empty())
        res.set("containers", dirtyContainers);
    if (!deletedItems.arr_val.empty())
        res.set("deleted_items", deletedItems);
    if (!deletedContainers.arr_val.empty())
        res.set("deleted_containers", deletedContainers);

    std::cout << res.toString() << std::endl;
    std::cout.flush();

    // Очищаем списки удалённых, чтобы не дублировать их в следующих репортах
    g_deleted_items.clear();
    g_deleted_containers.clear();
}

void simulateTicks(int ticks) {
    for (int i = 0; i < ticks; i++) {
        hourlyTick();                 // внутри: dailyTick и weeklyTick вызываются автоматически
        g_world.tick++;

        // Каждые 720 тиков (1 месяц) отправляем прогресс с dirty-данными
        if (g_world.tick > 0 && g_world.tick % 2160 == 0) {
            std::string lastNewsText = "";
            if (!g_world.news.empty()) {
                lastNewsText = g_world.news.back().text;
            }

            // Собираем ИЗМЕНИВШИЕСЯ предметы и контейнеры
            JsonValue itemsArr = JsonValue::array();
            for (auto& kv : g_items) {
                if (kv.second.is_dirty) {
                    JsonValue pair = JsonValue::array();
                    pair.push(JsonValue(kv.first));
                    pair.push(kv.second.toJson());
                    itemsArr.push(pair);
                    kv.second.is_dirty = false;   // сбрасываем флаг после добавления
                }
            }

            JsonValue contArr = JsonValue::array();
            for (auto& kv : g_containers) {
                if (kv.second.is_dirty) {
                    JsonValue pair = JsonValue::array();
                    pair.push(JsonValue(kv.first));
                    pair.push(kv.second.toJson());
                    contArr.push(pair);
                    kv.second.is_dirty = false;
                }
            }

            // Списки удалённых объектов
            JsonValue delItems = JsonValue::array();
            for (const auto& id : g_deleted_items)
                delItems.push(JsonValue(id));
            JsonValue delConts = JsonValue::array();
            for (const auto& id : g_deleted_containers)
                delConts.push(JsonValue(id));

            reportProgress(g_world.tick, lastNewsText,
                           itemsArr, contArr,
                           delItems, delConts);
        }
    }
}

// ============================================================================
// MAIN LOOP - Protocol Handler
// ============================================================================

int main() {
    std::string line;
    
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        
        JsonValue command = parseJson(line);
        JsonValue response = JsonValue::object();
        
        std::string cmd = command["command"].asString();
        
        auto serializeRegistries = [&](bool full = false) {
            JsonValue itemsArr = JsonValue::array();
            for (auto& kv : g_items) {
                if (full || kv.second.is_dirty) {
                    JsonValue pair = JsonValue::array();
                    pair.push(JsonValue(kv.first));
                    pair.push(kv.second.toJson());
                    itemsArr.push(pair);
                    kv.second.is_dirty = false;
                }
            }
            response.set("items", itemsArr);
            
            JsonValue contArr = JsonValue::array();
            for (auto& kv : g_containers) {
                if (full || kv.second.is_dirty) {
                    JsonValue pair = JsonValue::array();
                    pair.push(JsonValue(kv.first));
                    pair.push(kv.second.toJson());
                    contArr.push(pair);
                    kv.second.is_dirty = false;
                }
            }
            response.set("containers", contArr);

            if (!full) {
                JsonValue delItems = JsonValue::array();
                for (const auto& id : g_deleted_items) delItems.push(JsonValue(id));
                response.set("deleted_items", delItems);
                
                JsonValue delConts = JsonValue::array();
                for (const auto& id : g_deleted_containers) delConts.push(JsonValue(id));
                response.set("deleted_containers", delConts);
            }
            
            g_deleted_items.clear();
            g_deleted_containers.clear();
        };

        if (cmd == "init") {
            response.set("status", "ok");
            response.set("message", "Nexus Engine initialized");
            response.set("version", "1.0.0");
        }
        else if (cmd == "buildWorld") {
            std::string playerId = command["player_id"].asString();
            std::string era = command.has("era") ? command["era"].asString() : "rebirth";
            int initialAgents = command.has("initial_agents") ? command["initial_agents"].asInt() : 100;
            JsonValue globalLocs = command.has("global_locations") ? command["global_locations"] : JsonValue::object();
            
            buildWorld(playerId, era, initialAgents, globalLocs);
            
            response.set("status", "ok");
            response.set("tick", g_world.tick);
            response.set("world", g_world.toJson());
            serializeRegistries(true);
        }
        else if (cmd == "syncState") {
            if (command.has("world")) g_world = World::fromJson(command["world"]);
            if (command.has("items")) {
                g_items.clear();
                for (size_t i=0; i<command["items"].size(); i++) {
                    PhysicalItem item = PhysicalItem::fromJson(command["items"][i][1]);
                    item.is_dirty = false;
                    g_items[command["items"][i][0].asString()] = item;
                }
            }
            if (command.has("containers")) {
                g_containers.clear();
                for (size_t i=0; i<command["containers"].size(); i++) {
                    Storage cont = Storage::fromJson(command["containers"][i][1]);
                    cont.is_dirty = false;
                    g_containers[command["containers"][i][0].asString()] = cont;
                }
            }
            g_deleted_items.clear();
            g_deleted_containers.clear();
            response.set("status", "ok");
        }
        else if (cmd == "getFullState") {
            response.set("status", "ok");
            response.set("world", g_world.toJson());
            serializeRegistries(true);
        }
        else if (cmd == "gmIntervention") {
            std::string feedback = processGmIntervention(command);
            response.set("status", "ok");
            response.set("feedback", feedback);
            response.set("world", g_world.toJson());
            serializeRegistries(false);
        }
        else if (cmd == "simulateTicks" || cmd == "preSimulate") {
            int ticks = command["ticks"].asInt();
            
            // Optionally load world state if provided (Legacy support during transition)
            if (command.has("world")) {
                g_world = World::fromJson(command["world"]);
            }
            if (command.has("items")) {
                g_items.clear();
                for (size_t i=0; i<command["items"].size(); i++) {
                    PhysicalItem item = PhysicalItem::fromJson(command["items"][i][1]);
                    item.is_dirty = false;
                    g_items[command["items"][i][0].asString()] = item;
                }
            }
            if (command.has("containers")) {
                g_containers.clear();
                for (size_t i=0; i<command["containers"].size(); i++) {
                    Storage cont = Storage::fromJson(command["containers"][i][1]);
                    cont.is_dirty = false;
                    g_containers[command["containers"][i][0].asString()] = cont;
                }
            }
            
            simulateTicks(ticks);
            
            response.set("status", "ok");
            response.set("tick", g_world.tick);
            response.set("news_count", (int)g_world.news.size());
            response.set("world", g_world.toJson());
            serializeRegistries(false);
        }
        else if (cmd == "ping") {
            response.set("status", "ok");
            response.set("pong", true);
            response.set("tick", g_world.tick);
        }
        else {
            response.set("status", "error");
            response.set("message", "Unknown command: " + cmd);
        }
        
        std::cout << response.toString() << std::endl;
        std::cout.flush();
    }
    
    return 0;
}
