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
#include <numeric>
#include <queue>
#include <future>
#include <thread>
#include <functional>
#include <memory>
#include <type_traits>
#include <stdexcept>

#include <thread>
#include <mutex>
#include <atomic>

#include "generated_data.h"

// ============================================================================
// THREAD POOL (For high-performance parallel simulation)
// ============================================================================
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back([this] {
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
    }
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared< std::packaged_task<return_type()> >(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

inline ThreadPool* getThreadPool() {
    static ThreadPool pool(std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4);
    return &pool;
}

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
    std::string target_container_id;

    

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
        obj.set("target_container_id", target_container_id);
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
        if (j.has("target_container_id")) o.target_container_id = j["target_container_id"].asString();
        return o;
    }
};

struct PhysicalItem {
    std::string id;
    GoodType prototype_id;
    std::string raw_prototype_id;
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
    bool is_legendary = false;
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
        obj.set("prototype_id", raw_prototype_id.empty() ? goodTypeToString(prototype_id) : raw_prototype_id);
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
        flags.set("is_legendary", is_legendary);
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
        if (j.has("prototype_id")) {
            item.raw_prototype_id = j["prototype_id"].asString();
            item.prototype_id = stringToGoodType(item.raw_prototype_id);
        }
        else if (j.has("aiIdentifier")) {
            item.raw_prototype_id = j["aiIdentifier"].asString();
            item.prototype_id = stringToGoodType(item.raw_prototype_id);
        }
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
            if (j["flags"].has("is_legendary")) item.is_legendary = j["flags"]["is_legendary"].asBool();
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
        std::unordered_map<GoodType, std::vector<std::string>> items_by_type;
        std::vector<int> cached_stocks = std::vector<int>((int)GoodType::COUNT, 0);
        
        

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

#include <mutex>
#include <shared_mutex>

template <typename T>
class ObjectPool {
public:
    std::vector<T> data;
    std::vector<bool> active;
    std::unordered_map<std::string, uint32_t> id_map;
    std::vector<uint32_t> free_slots;

    T& operator[](const std::string& id) {
        auto it = id_map.find(id);
        if (it != id_map.end()) return data[it->second];
        uint32_t idx;
        if (!free_slots.empty()) {
            idx = free_slots.back();
            free_slots.pop_back();
            active[idx] = true;
        } else {
            idx = data.size();
            data.emplace_back();
            active.push_back(true);
        }
        id_map[id] = idx;
        return data[idx];
    }

    void erase(const std::string& id) {
        auto it = id_map.find(id);
        if (it != id_map.end()) {
            active[it->second] = false;
            free_slots.push_back(it->second);
            id_map.erase(it);
        }
    }

    bool count(const std::string& id) const { return id_map.count(id) > 0; }
    void clear() { data.clear(); active.clear(); id_map.clear(); free_slots.clear(); }
    bool empty() const { return id_map.empty(); }
    size_t size() const { return id_map.size(); }
};

// Global registries
static ObjectPool<PhysicalItem> g_items;
static ObjectPool<Storage> g_containers;
static std::vector<std::string> g_deleted_items;
static std::vector<std::string> g_deleted_containers;
static bool g_bootstrap = false;

// Thread Safety Primitives

static std::recursive_mutex g_registry_mutex;
static std::mutex g_news_mutex;
static std::mutex g_sublocations_mutex;
static std::mutex g_npc_state_mutex;
    static std::mutex g_faction_state_mutex;
    static std::map<std::pair<std::string, std::string>, std::vector<std::pair<int,int>>> g_path_cache;
    static bool g_path_cache_dirty = true;

    void rebuildContainerIndices() {
        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
        for (size_t i=0; i<g_containers.data.size(); ++i) {
            if (!g_containers.active[i]) continue;
            Storage& cont = g_containers.data[i];
            cont.items_by_type.clear();
            cont.cached_stocks.assign((int)GoodType::COUNT, 0);
            for (const auto& itemId : cont.item_ids) {
                if (g_items.count(itemId)) {
                    cont.items_by_type[g_items[itemId].prototype_id].push_back(itemId);
                    int type_idx = (int)g_items[itemId].prototype_id;
                    if (type_idx >= 0 && type_idx < (int)GoodType::COUNT) {
                        cont.cached_stocks[type_idx] += g_items[itemId].stack_size;
                    }
                }
            }
        }
    }

// Thread-safe RNG (No std::random_device to prevent MinGW crashes)
inline int thread_safe_rand() {
    thread_local std::mt19937 gen(static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()) ^ static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    thread_local std::uniform_int_distribution<> dist(0, 32767);
    return dist(gen);
}

// Helper: Generate UUID (Thread-safe)
std::string generateUUID() {
    thread_local std::mt19937 gen(static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()) + static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())) + 1);
    std::uniform_int_distribution<> hex_dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (size_t i = 0; i < uuid.size(); i++) {
        if (uuid[i] == 'x') uuid[i] = hex[hex_dist(gen)];
        else if (uuid[i] == 'y') uuid[i] = hex[(hex_dist(gen) & 0x3) | 0x8];
    }
    return uuid;
}

// Item management
int getShelfLifeDays(GoodType type); // Forward declaration for item stacking

struct WorldMap;
enum class MovementType : uint8_t {
    LAND,
    WATER,
    AIR,
    ANY
};

// Forward declaration for A* pathfinding
std::vector<std::pair<int,int>> findPath(const WorldMap& map, int startX, int startY, int goalX, int goalY, const std::vector<bool>& has_road, const std::vector<int>& path_status, MovementType moveType, int entity_size = 0);

std::string createContainer(const std::string& type, const std::string& ownerId, 
                            int maxWeight, int maxSlots, const std::string& regionId = "",
                            const std::string& parentEntity = "", const std::string& parentContainer = "") {
    std::string new_id = "cont_" + generateUUID();
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    Storage cont;
    cont.id = new_id;
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
    std::string new_id = "item_" + generateUUID();
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    
    if (!containerId.empty() && g_containers.count(containerId)) {
        Storage& cont = g_containers[containerId];
        auto it = cont.items_by_type.find(prototypeId);
        if (it != cont.items_by_type.end()) {
            for (const std::string& existingId : it->second) {
                if (g_items.count(existingId)) {
                    PhysicalItem& exItem = g_items[existingId];
                    bool canStack = false;
                    if (!exItem.order_data.has_value() && !exItem.quest_item && exItem.durability == 100) {
                        if (getShelfLifeDays(prototypeId) == 999999) canStack = true; // Бессрочные товары стакаются всегда
                        else if (exItem.batch_day == currentDay) canStack = true; // Скоропортящиеся - только в рамках одного дня
                    }
                    if (canStack) {
                        exItem.stack_size += quantity;
                        exItem.is_dirty = true;
                        cont.cached_stocks[(int)prototypeId] += quantity;
                        cont.is_dirty = true;
                        return existingId;
                    }
                }
            }
        }
    }
    
    PhysicalItem item;
    item.id = new_id;
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
        g_containers[containerId].items_by_type[prototypeId].push_back(item.id);
        g_containers[containerId].cached_stocks[(int)prototypeId] += quantity;
        g_containers[containerId].is_dirty = true;
    }
    
    return item.id;
}

bool removeItem(const std::string& itemId, int quantity) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    if (!g_items.count(itemId)) return false;
    
    PhysicalItem& item = g_items[itemId];
    std::string oldOwner = "";
    if (item.stack_size <= quantity) {
        // Remove from container
        if (!item.container_id.empty() && g_containers.count(item.container_id)) {
            Storage& cont = g_containers[item.container_id];
            cont.cached_stocks[(int)item.prototype_id] -= item.stack_size;
            auto& vec = cont.item_ids;
            auto it = std::find(vec.begin(), vec.end(), itemId);
            if (it != vec.end()) {
                *it = std::move(vec.back());
                vec.pop_back();
            }
            auto& type_vec = cont.items_by_type[item.prototype_id];
            auto it2 = std::find(type_vec.begin(), type_vec.end(), itemId);
            if (it2 != type_vec.end()) {
                *it2 = std::move(type_vec.back());
                type_vec.pop_back();
            }
            cont.is_dirty = true;
        }
        g_deleted_items.push_back(itemId);
        g_items.erase(itemId);
    } else {
        item.stack_size -= quantity;
        item.is_dirty = true;
        if (!item.container_id.empty() && g_containers.count(item.container_id)) {
            g_containers[item.container_id].cached_stocks[(int)item.prototype_id] -= quantity;
        }
    }
    return true;
}

bool moveItem(const std::string& itemId, const std::string& targetContainerId);

    int countItemsInContainer(const std::string& containerId, GoodType prototypeId) {
        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
        if (!g_containers.count(containerId)) return 0;
        int type_idx = (int)prototypeId;
        if (type_idx < 0 || type_idx >= (int)GoodType::COUNT) return 0;
        return g_containers[containerId].cached_stocks[type_idx];
    }

double calculateContainerWeight(const std::string& containerId) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    if (!g_containers.count(containerId)) return 0.0;
    double totalWeight = 0.0;
    for (const auto& itemId : g_containers[containerId].item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
    std::string oldOwner = "";
            double w = item.custom_props.has("weight_per_unit") ? item.custom_props["weight_per_unit"].asDouble() : 1.0;
            totalWeight += w * item.stack_size;
        }
    }
    return totalWeight;
}

int consumeItemsFromContainer(const std::string& containerId, GoodType prototypeId, int quantity) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    if (!g_containers.count(containerId)) return 0;
    if (prototypeId == GoodType::COUNT) return 0;
    
    Storage& cont = g_containers[containerId];
    int taken = 0;
    int remaining = quantity;
    
    // Sort items by batch_day (FIFO - oldest first)
    std::vector<std::pair<int, std::string>> itemsByAge;
    auto it = cont.items_by_type.find(prototypeId);
    if (it != cont.items_by_type.end()) {
        for (const auto& itemId : it->second) {
            if (g_items.count(itemId)) {
                itemsByAge.push_back({g_items[itemId].batch_day, itemId});
            }
        }
    }
    std::sort(itemsByAge.begin(), itemsByAge.end());
    
    for (const auto& [day, itemId] : itemsByAge) {
        if (remaining <= 0) break;
        if (!g_items.count(itemId)) continue;
        
        PhysicalItem& item = g_items[itemId];
    std::string oldOwner = "";
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
    std::string id;
    std::string text;
    std::string location;
    int importance; // 1=minor, 2=notable, 3=major
    std::string category; // "trade", "war", "disaster", "politics", "misc"
    int day = 0;
    std::string causal_link; // ID of the event that caused this news
    
    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("text", text);
        obj.set("location", location);
        obj.set("importance", importance);
        obj.set("category", category);
        obj.set("day", day);
        obj.set("causal_link", causal_link);
        return obj;
    }
};

struct Caravan {
    std::string id;
    std::string merchant_id; // Владелец каравана (NPC)
    std::string origin;
    std::string destination;
    int hoursLeft = 0;
    double x = 0.0;
    double y = 0.0;
    std::vector<std::pair<int, int>> path;
    int path_index = 0;
    std::string chest_id; // Container with goods
    int wagons = 0;       // Количество повозок в караване
    int guards = 0;       // Количество нанятой охраны
    int guard_cost = 0;   // Затраты на охрану
    int transport_cost = 0; // Затраты на транспорт
    
    // Legacy goods map (for compatibility)
    std::map<std::string, int> goods;
    
            

    JsonValue toJson() const {
            JsonValue obj = JsonValue::object();
            obj.set("id", id);
            obj.set("merchant_id", merchant_id);
            obj.set("origin", origin);
            obj.set("destination", destination);
            obj.set("hoursLeft", hoursLeft);
            obj.set("x", x);
            obj.set("y", y);
            obj.set("path_index", path_index);
            JsonValue pArr = JsonValue::array();
            for (const auto& pt : path) {
                JsonValue ptArr = JsonValue::array();
                ptArr.push(pt.first); ptArr.push(pt.second);
                pArr.push(ptArr);
            }
            obj.set("path", pArr);
            obj.set("chest_id", chest_id);
            obj.set("wagons", wagons);
            obj.set("guards", guards);
            obj.set("guard_cost", guard_cost);
            obj.set("transport_cost", transport_cost);
        
        JsonValue g = JsonValue::object();
        for (const auto& [key, val] : goods) g.set(key, val);
        obj.set("goods", g);
        
        return obj;
    }
    
            static Caravan fromJson(const JsonValue& j) {
            Caravan c;
            c.id = j["id"].asString();
            if (j.has("merchant_id")) c.merchant_id = j["merchant_id"].asString();
            c.origin = j["origin"].asString();
            c.destination = j["destination"].asString();
            c.hoursLeft = j["hoursLeft"].asInt();
            if (j.has("x")) c.x = j["x"].asDouble();
            if (j.has("y")) c.y = j["y"].asDouble();
            if (j.has("path_index")) c.path_index = j["path_index"].asInt();
            if (j.has("path")) {
                for (size_t i = 0; i < j["path"].size(); i++) {
                    if (j["path"][i].size() >= 2) {
                        c.path.push_back({j["path"][i][0].asInt(), j["path"][i][1].asInt()});
                    }
                }
            }
            c.chest_id = j["chest_id"].asString();
            if (j.has("wagons")) c.wagons = j["wagons"].asInt();
            if (j.has("guards")) c.guards = j["guards"].asInt();
            if (j.has("guard_cost")) c.guard_cost = j["guard_cost"].asInt();
            if (j.has("transport_cost")) c.transport_cost = j["transport_cost"].asInt();
        
        if (j.has("goods")) {
            for (const auto& kv : j["goods"].obj_val) {
                c.goods[kv.first] = kv.second.asInt();
            }
        }
        
        return c;
    }
};

struct Wound {
    std::string type; // e.g., "deep_wound", "broken_arm", "scar"
    int severity;     // 1-10
    int day_received;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("type", type);
        obj.set("severity", severity);
        obj.set("day_received", day_received);
        return obj;
    }

    static Wound fromJson(const JsonValue& j) {
        Wound w;
        if (j.has("type")) w.type = j["type"].asString();
        if (j.has("severity")) w.severity = j["severity"].asInt();
        if (j.has("day_received")) w.day_received = j["day_received"].asInt();
        return w;
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
        std::string profession_type = "none"; // farmer, artisan, merchant, innkeeper, ruler, cleric, mage, mercenary
        std::string personal_inventory_id;
        std::string storage_id;

        int reserve_gold = 100;
        int reserve_food = 5;
    } economy;
    
    // Inventory
    int gold = 0;
    std::string inventory_id; // Container ID for physical items
    
    // Demographics & Life Cycle
    std::string race = "human"; // human, elf, dwarf, orc
    int age_days = 0;
    bool is_male = true;
    std::string father_id;
    std::string mother_id;
    std::vector<std::string> children_ids;
    std::string spouse_id;
    std::vector<std::string> diseases;
    std::vector<Wound> wounds;
    int immunity = 100;
    std::vector<std::string> owned_businesses;
    int death_day = -1;
    std::string death_cause;

    int professionChangeTimestamp = 0;
    int currentWealthLevel = 0;

    // Status
    bool isAlive = true;
    int hp = 20;
    int maxHp = 20;
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

        int supportLevel = 100;
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
        obj.set("maxHp", maxHp);
        obj.set("race", race);
        obj.set("age_days", age_days);
        obj.set("is_male", is_male);
        obj.set("father_id", father_id);
        obj.set("mother_id", mother_id);
        obj.set("spouse_id", spouse_id);
        obj.set("immunity", immunity);
        obj.set("death_day", death_day);
        obj.set("death_cause", death_cause);

        obj.set("professionChangeTimestamp", professionChangeTimestamp);
        obj.set("currentWealthLevel", currentWealthLevel);
        
        JsonValue childs = JsonValue::array();
        for (const auto& c : children_ids) childs.push(JsonValue(c));
        obj.set("children_ids", childs);

        JsonValue bus = JsonValue::array();
        for (const auto& b : owned_businesses) bus.push(JsonValue(b));
        obj.set("owned_businesses", bus);

        JsonValue dis = JsonValue::array();
        for (const auto& d : diseases) dis.push(JsonValue(d));
        obj.set("diseases", dis);

        JsonValue wnds = JsonValue::array();
        for (const auto& w : wounds) wnds.push(w.toJson());
        obj.set("wounds", wnds);

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
        e.set("profession_type", economy.profession_type);
        e.set("personal_inventory_id", economy.personal_inventory_id);
        e.set("storage_id", economy.storage_id);

        e.set("reserve_gold", economy.reserve_gold);
        e.set("reserve_food", economy.reserve_food);
        obj.set("economy", e);
        
        JsonValue rels = JsonValue::object();
        for (const auto& [k, v] : relationships) rels.set(k, v);
        obj.set("relationships", rels);

        JsonValue mems = JsonValue::array();
        for (const auto& m : memory) mems.push(JsonValue(m));
        obj.set("memory", mems);


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

            rp.set("supportLevel", rulerPersonality.supportLevel);
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
        if (j.has("maxHp")) npc.maxHp = j["maxHp"].asInt(); else npc.maxHp = npc.hp;
        if (j.has("race")) npc.race = j["race"].asString();
        if (j.has("age_days")) npc.age_days = j["age_days"].asInt();
        if (j.has("is_male")) npc.is_male = j["is_male"].asBool();
        if (j.has("father_id")) npc.father_id = j["father_id"].asString();
        if (j.has("mother_id")) npc.mother_id = j["mother_id"].asString();
        if (j.has("spouse_id")) npc.spouse_id = j["spouse_id"].asString();
        if (j.has("immunity")) npc.immunity = j["immunity"].asInt(); else npc.immunity = 100;
        if (j.has("death_day")) npc.death_day = j["death_day"].asInt();
        if (j.has("death_cause")) npc.death_cause = j["death_cause"].asString();

        if (j.has("professionChangeTimestamp")) npc.professionChangeTimestamp = j["professionChangeTimestamp"].asInt();
        if (j.has("currentWealthLevel")) npc.currentWealthLevel = j["currentWealthLevel"].asInt();

        if (j.has("children_ids")) {
            for (size_t i = 0; i < j["children_ids"].size(); i++) npc.children_ids.push_back(j["children_ids"][i].asString());
        }
        if (j.has("owned_businesses")) {
            for (size_t i = 0; i < j["owned_businesses"].size(); i++) npc.owned_businesses.push_back(j["owned_businesses"][i].asString());
        }
        if (j.has("diseases")) {
            for (size_t i = 0; i < j["diseases"].size(); i++) npc.diseases.push_back(j["diseases"][i].asString());
        }

        if (j.has("wounds")) {
            for (size_t i = 0; i < j["wounds"].size(); i++) npc.wounds.push_back(Wound::fromJson(j["wounds"][i]));
        }

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
            if (j["economy"].has("profession_type")) npc.economy.profession_type = j["economy"]["profession_type"].asString();
            if (j["economy"].has("personal_inventory_id")) npc.economy.personal_inventory_id = j["economy"]["personal_inventory_id"].asString();
            if (j["economy"].has("storage_id")) npc.economy.storage_id = j["economy"]["storage_id"].asString();

            if (j["economy"].has("reserve_gold")) npc.economy.reserve_gold = j["economy"]["reserve_gold"].asInt();
            if (j["economy"].has("reserve_food")) npc.economy.reserve_food = j["economy"]["reserve_food"].asInt();
        }
        
        if (j.has("relationships")) {
            for (const auto& kv : j["relationships"].obj_val) {
                npc.relationships[kv.first] = kv.second.asInt();
            }
        }
        if (j.has("memory")) {
            for (size_t i = 0; i < j["memory"].size(); i++) {
                npc.memory.push_back(j["memory"][i].asString());
            }
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

                if (j["rulerPersonality"].has("supportLevel")) npc.rulerPersonality.supportLevel = j["rulerPersonality"]["supportLevel"].asInt();
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

struct CityBlock {
    int x = 0, y = 0;
    std::string type;
    std::string name;
    std::string linked_id;
    std::string sublocation_id;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("x", x);
        obj.set("y", y);
        obj.set("type", type);
        obj.set("name", name);
        obj.set("linked_id", linked_id);
        obj.set("sublocation_id", sublocation_id);
        return obj;
    }

    static CityBlock fromJson(const JsonValue& j) {
        CityBlock b;
        if (j.has("x")) b.x = j["x"].asInt();
        if (j.has("y")) b.y = j["y"].asInt();
        if (j.has("type")) b.type = j["type"].asString();
        if (j.has("name")) b.name = j["name"].asString();
        if (j.has("linked_id")) b.linked_id = j["linked_id"].asString();
        if (j.has("sublocation_id")) b.sublocation_id = j["sublocation_id"].asString();
        return b;
    }
};


struct LogisticRule {
    std::string id;
    std::string type; // "transfer" (send to facility/warehouse), "order" (buy via merchant), "pull" (take from)
    GoodType resource = GoodType::COUNT;
    std::string target_id; // business_id or region_id (for orders)
    int amount = 0;
    int frequency_days = 1;
    int days_since_last = 0;
    int max_price = 0; // only for "order"
    int keep_reserve = 0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", type);
        obj.set("resource", goodTypeToString(resource));
        obj.set("target_id", target_id);
        obj.set("amount", amount);
        obj.set("frequency_days", frequency_days);
        obj.set("days_since_last", days_since_last);
        obj.set("max_price", max_price);
        obj.set("keep_reserve", keep_reserve);
        return obj;
    }

    static LogisticRule fromJson(const JsonValue& j) {
        LogisticRule r;
        if (j.has("id")) r.id = j["id"].asString();
        if (j.has("type")) r.type = j["type"].asString();
        if (j.has("resource")) r.resource = stringToGoodType(j["resource"].asString());
        if (j.has("target_id")) r.target_id = j["target_id"].asString();
        if (j.has("amount")) r.amount = j["amount"].asInt();
        if (j.has("frequency_days")) r.frequency_days = j["frequency_days"].asInt();
        if (j.has("days_since_last")) r.days_since_last = j["days_since_last"].asInt();
        if (j.has("max_price")) r.max_price = j["max_price"].asInt();
        if (j.has("keep_reserve")) r.keep_reserve = j["keep_reserve"].asInt();
        return r;
    }
};


struct Business {
    std::string id;
    std::vector<std::string> owner_ids;
    std::string region_id;
    std::string facility_type;
    int level = 1;
    int durability = 100;
    int cash_balance = 0;
    int reinvestment_pool = 0;
    std::string manager_id;
    int employee_count = 0;
    int target_employee_count = 100;
    int target_efficiency = 100;
    bool is_active = true;
    int months_loss_streak = 0;
    std::string production_focus; // Specific GoodType string, e.g., "wheat"
    std::string local_storage_id; // Container ID for inputs/outputs
    std::vector<LogisticRule> logistics;
    std::vector<std::string> activity_logs;

    void addLog(int day, const std::string& msg) {
        int year = day / 360 + 1;
        int month = (day % 360) / 30 + 1;
        int d = (day % 30) + 1;
        std::string entry = "[Год " + std::to_string(year) + ", Месяц " + std::to_string(month) + ", День " + std::to_string(d) + "] " + msg;
        activity_logs.insert(activity_logs.begin(), entry);
        if (activity_logs.size() > 30) activity_logs.pop_back();
    }

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        JsonValue owners = JsonValue::array();
        for (const auto& o : owner_ids) owners.push(JsonValue(o));
        obj.set("owner_ids", owners);
        obj.set("region_id", region_id);
        obj.set("facility_type", facility_type);
        obj.set("level", level);
        obj.set("durability", durability);
        obj.set("cash_balance", cash_balance);
        obj.set("reinvestment_pool", reinvestment_pool);
        obj.set("manager_id", manager_id);
        obj.set("employee_count", employee_count);
        obj.set("target_employee_count", target_employee_count);
        obj.set("target_efficiency", target_efficiency);
        obj.set("is_active", is_active);
        obj.set("months_loss_streak", months_loss_streak);
        obj.set("production_focus", production_focus);
        obj.set("local_storage_id", local_storage_id);
        JsonValue logs = JsonValue::array();
        for (const auto& l : logistics) logs.push(l.toJson());
        obj.set("logistics", logs);
        JsonValue alogs = JsonValue::array();
        for (const auto& l : activity_logs) alogs.push(JsonValue(l));
        obj.set("activity_logs", alogs);
        return obj;
    }

    static Business fromJson(const JsonValue& j) {
        Business b;
        if (j.has("id")) b.id = j["id"].asString();
        if (j.has("owner_ids")) {
            for (size_t i = 0; i < j["owner_ids"].size(); i++) b.owner_ids.push_back(j["owner_ids"][i].asString());
        }
        if (j.has("region_id")) b.region_id = j["region_id"].asString();
        if (j.has("facility_type")) b.facility_type = j["facility_type"].asString();
        if (j.has("level")) b.level = j["level"].asInt();
        if (j.has("durability")) b.durability = j["durability"].asInt();
        if (j.has("cash_balance")) b.cash_balance = j["cash_balance"].asInt();
        if (j.has("reinvestment_pool")) b.reinvestment_pool = j["reinvestment_pool"].asInt();
        if (j.has("manager_id")) b.manager_id = j["manager_id"].asString();
        if (j.has("employee_count")) b.employee_count = j["employee_count"].asInt();
        if (j.has("target_employee_count")) b.target_employee_count = j["target_employee_count"].asInt(); else b.target_employee_count = b.employee_count;
        if (j.has("target_efficiency")) b.target_efficiency = j["target_efficiency"].asInt(); else b.target_efficiency = 100;
        if (j.has("is_active")) b.is_active = j["is_active"].asBool();
        if (j.has("months_loss_streak")) b.months_loss_streak = j["months_loss_streak"].asInt();
        if (j.has("production_focus")) b.production_focus = j["production_focus"].asString();
        if (j.has("local_storage_id")) b.local_storage_id = j["local_storage_id"].asString();
        if (j.has("logistics")) {
            for (size_t i = 0; i < j["logistics"].size(); i++) {
                b.logistics.push_back(LogisticRule::fromJson(j["logistics"][i]));
            }
        }
        if (j.has("activity_logs")) {
            for (size_t i = 0; i < j["activity_logs"].size(); i++) {
                b.activity_logs.push_back(j["activity_logs"][i].asString());
            }
        }
        return b;
    }
};


struct MarketOffer {
    std::string id;
    std::string seller_id;
    GoodType good = GoodType::COUNT;
    int quantity = 0;
    double price = 0.0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("seller_id", seller_id);
        obj.set("good", goodTypeToString(good));
        obj.set("quantity", quantity);
        obj.set("price", price);
        return obj;
    }

    static MarketOffer fromJson(const JsonValue& j) {
        MarketOffer o;
        if (j.has("id")) o.id = j["id"].asString();
        if (j.has("seller_id")) o.seller_id = j["seller_id"].asString();
        if (j.has("good")) o.good = stringToGoodType(j["good"].asString());
        if (j.has("quantity")) o.quantity = j["quantity"].asInt();
        if (j.has("price")) o.price = j["price"].asDouble();
        return o;
    }
};

struct PriceHistory {
    std::vector<double> history;
    int index = 0;

    void add(double price) {
        if (history.size() < 30) {
            history.push_back(price);
        } else {
            history[index] = price;
            index = (index + 1) % 30;
        }
    }

    double getAvg(int days) const {
        if (history.empty()) return 0.0;
        int count = std::min((int)history.size(), days);
        double sum = 0;
        int curr = index - 1;
        for (int i = 0; i < count; i++) {
            if (curr < 0) curr = history.size() - 1;
            sum += history[curr];
            curr--;
        }
        return sum / count;
    }

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        JsonValue arr = JsonValue::array();
        for (double p : history) arr.push(JsonValue(p));
        obj.set("history", arr);
        obj.set("index", index);
        return obj;
    }

    static PriceHistory fromJson(const JsonValue& j) {
        PriceHistory ph;
        if (j.has("history")) {
            for (size_t i = 0; i < j["history"].size(); i++) {
                ph.history.push_back(j["history"][i].asDouble());
            }
        }
        if (j.has("index")) ph.index = j["index"].asInt();
        return ph;
    }
};


enum class DiplomaticState : uint8_t {
    PEACE, NON_AGGRESSION_PACT, DEFENSIVE_ALLIANCE, FULL_ALLIANCE,
    COLD_WAR, BORDER_CONFLICT, LIMITED_WAR, TOTAL_WAR
};

inline std::string diploStateToString(DiplomaticState s) {
    switch(s) {
        case DiplomaticState::PEACE: return "PEACE";
        case DiplomaticState::NON_AGGRESSION_PACT: return "NON_AGGRESSION_PACT";
        case DiplomaticState::DEFENSIVE_ALLIANCE: return "DEFENSIVE_ALLIANCE";
        case DiplomaticState::FULL_ALLIANCE: return "FULL_ALLIANCE";
        case DiplomaticState::COLD_WAR: return "COLD_WAR";
        case DiplomaticState::BORDER_CONFLICT: return "BORDER_CONFLICT";
        case DiplomaticState::LIMITED_WAR: return "LIMITED_WAR";
        case DiplomaticState::TOTAL_WAR: return "TOTAL_WAR";
        default: return "PEACE";
    }
}

inline DiplomaticState stringToDiploState(const std::string& s) {
    if (s == "NON_AGGRESSION_PACT") return DiplomaticState::NON_AGGRESSION_PACT;
    if (s == "DEFENSIVE_ALLIANCE") return DiplomaticState::DEFENSIVE_ALLIANCE;
    if (s == "FULL_ALLIANCE") return DiplomaticState::FULL_ALLIANCE;
    if (s == "COLD_WAR") return DiplomaticState::COLD_WAR;
    if (s == "BORDER_CONFLICT") return DiplomaticState::BORDER_CONFLICT;
    if (s == "LIMITED_WAR") return DiplomaticState::LIMITED_WAR;
    if (s == "TOTAL_WAR") return DiplomaticState::TOTAL_WAR;
    return DiplomaticState::PEACE;
}

enum class CasusBelli : uint8_t {
    NONE, BORDER_INCIDENT, RECLAIM_CORES, HUMANITARIAN, PREEMPTIVE, IMPERIALISM
};

inline std::string cbToString(CasusBelli cb) {
    switch(cb) {
        case CasusBelli::BORDER_INCIDENT: return "BORDER_INCIDENT";
        case CasusBelli::RECLAIM_CORES: return "RECLAIM_CORES";
        case CasusBelli::HUMANITARIAN: return "HUMANITARIAN";
        case CasusBelli::PREEMPTIVE: return "PREEMPTIVE";
        case CasusBelli::IMPERIALISM: return "IMPERIALISM";
        default: return "NONE";
    }
}

inline CasusBelli stringToCb(const std::string& s) {
    if (s == "BORDER_INCIDENT") return CasusBelli::BORDER_INCIDENT;
    if (s == "RECLAIM_CORES") return CasusBelli::RECLAIM_CORES;
    if (s == "HUMANITARIAN") return CasusBelli::HUMANITARIAN;
    if (s == "PREEMPTIVE") return CasusBelli::PREEMPTIVE;
    if (s == "IMPERIALISM") return CasusBelli::IMPERIALISM;
    return CasusBelli::NONE;
}

struct WarGoal {
    std::string targetRegionId;
    bool achieved = false;
    int deadlineDays = 0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("targetRegionId", targetRegionId);
        obj.set("achieved", achieved);
        obj.set("deadlineDays", deadlineDays);
        return obj;
    }

    static WarGoal fromJson(const JsonValue& j) {
        WarGoal w;
        if (j.has("targetRegionId")) w.targetRegionId = j["targetRegionId"].asString();
        if (j.has("achieved")) w.achieved = j["achieved"].asBool();
        if (j.has("deadlineDays")) w.deadlineDays = j["deadlineDays"].asInt();
        return w;
    }
};

struct Ultimatum {
    std::string fromFactionId;
    std::string toFactionId;
    std::string demand;
    int expiresDay = 0;
    bool accepted = false;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("fromFactionId", fromFactionId);
        obj.set("toFactionId", toFactionId);
        obj.set("demand", demand);
        obj.set("expiresDay", expiresDay);
        obj.set("accepted", accepted);
        return obj;
    }

    static Ultimatum fromJson(const JsonValue& j) {
        Ultimatum u;
        if (j.has("fromFactionId")) u.fromFactionId = j["fromFactionId"].asString();
        if (j.has("toFactionId")) u.toFactionId = j["toFactionId"].asString();
        if (j.has("demand")) u.demand = j["demand"].asString();
        if (j.has("expiresDay")) u.expiresDay = j["expiresDay"].asInt();
        if (j.has("accepted")) u.accepted = j["accepted"].asBool();
        return u;
    }
};

struct Coalition {
    std::string leaderFactionId;
    std::string targetFactionId;
    std::vector<std::string> members;
    int formedOnDay = 0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("leaderFactionId", leaderFactionId);
        obj.set("targetFactionId", targetFactionId);
        obj.set("formedOnDay", formedOnDay);
        JsonValue mems = JsonValue::array();
        for (const auto& m : members) mems.push(JsonValue(m));
        obj.set("members", mems);
        return obj;
    }

    static Coalition fromJson(const JsonValue& j) {
        Coalition c;
        if (j.has("leaderFactionId")) c.leaderFactionId = j["leaderFactionId"].asString();
        if (j.has("targetFactionId")) c.targetFactionId = j["targetFactionId"].asString();
        if (j.has("formedOnDay")) c.formedOnDay = j["formedOnDay"].asInt();
        if (j.has("members")) {
            for (size_t i = 0; i < j["members"].size(); i++) {
                c.members.push_back(j["members"][i].asString());
            }
        }
        return c;
    }
};


struct PlannedHarvest {
    int days_left = 0;
    GoodType good = GoodType::COUNT;
    int amount = 0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("days_left", days_left);
        obj.set("good", goodTypeToString(good));
        obj.set("amount", amount);
        return obj;
    }

    static PlannedHarvest fromJson(const JsonValue& j) {
        PlannedHarvest p;
        if (j.has("days_left")) p.days_left = j["days_left"].asInt();
        if (j.has("good")) p.good = stringToGoodType(j["good"].asString());
        if (j.has("amount")) p.amount = j["amount"].asInt();
        return p;
    }
};


enum class ShipType { MERCHANT, TRANSPORT, WAR_GALLEY, WAR_FRIGATE, EXPLORER, PIRATE, SEA_MONSTER };

inline std::string shipTypeToString(ShipType t) {
    switch(t) {
        case ShipType::MERCHANT: return "MERCHANT";
        case ShipType::TRANSPORT: return "TRANSPORT";
        case ShipType::WAR_GALLEY: return "WAR_GALLEY";
        case ShipType::WAR_FRIGATE: return "WAR_FRIGATE";
        case ShipType::EXPLORER: return "EXPLORER";
        case ShipType::PIRATE: return "PIRATE";
        case ShipType::SEA_MONSTER: return "SEA_MONSTER";

        default: return "MERCHANT";
    }
}

inline ShipType stringToShipType(const std::string& s) {
    if (s == "TRANSPORT") return ShipType::TRANSPORT;
    if (s == "WAR_GALLEY") return ShipType::WAR_GALLEY;
    if (s == "WAR_FRIGATE") return ShipType::WAR_FRIGATE;
    if (s == "EXPLORER") return ShipType::EXPLORER;
    if (s == "PIRATE") return ShipType::PIRATE;
    if (s == "SEA_MONSTER") return ShipType::SEA_MONSTER;

    return ShipType::MERCHANT;
}

struct Ship {
    std::string id;
    std::string owner_id;
    ShipType type = ShipType::MERCHANT;
    int hull = 100;
    int sailors = 10;
    int cargo_capacity = 100;
    std::string chest_id;
    double speed = 1.0;
    double x = 0.0;
    double y = 0.0;
    std::string destination;
    std::vector<std::pair<int,int>> path;
    int path_index = 0;
    int cannons = 0;
    int marines = 0;

    std::string fleet_id;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("owner_id", owner_id);
        obj.set("type", shipTypeToString(type));
        obj.set("hull", hull);
        obj.set("sailors", sailors);
        obj.set("cargo_capacity", cargo_capacity);
        obj.set("chest_id", chest_id);
        obj.set("speed", speed);
        obj.set("x", x);
        obj.set("y", y);
        obj.set("destination", destination);
        obj.set("path_index", path_index);
        JsonValue pArr = JsonValue::array();
        for (const auto& pt : path) {
            JsonValue ptArr = JsonValue::array();
            ptArr.push(pt.first); ptArr.push(pt.second);
            pArr.push(ptArr);
        }
        obj.set("path", pArr);
        obj.set("cannons", cannons);
        obj.set("marines", marines);

        obj.set("fleet_id", fleet_id);
        return obj;
    }

    static Ship fromJson(const JsonValue& j) {
        Ship s;
        if (j.has("id")) s.id = j["id"].asString();
        if (j.has("owner_id")) s.owner_id = j["owner_id"].asString();
        if (j.has("type")) s.type = stringToShipType(j["type"].asString());
        if (j.has("hull")) s.hull = j["hull"].asInt();
        if (j.has("sailors")) s.sailors = j["sailors"].asInt();
        if (j.has("cargo_capacity")) s.cargo_capacity = j["cargo_capacity"].asInt();
        if (j.has("chest_id")) s.chest_id = j["chest_id"].asString();
        if (j.has("speed")) s.speed = j["speed"].asDouble();
        if (j.has("x")) s.x = j["x"].asDouble();
        if (j.has("y")) s.y = j["y"].asDouble();
        if (j.has("destination")) s.destination = j["destination"].asString();
        if (j.has("path_index")) s.path_index = j["path_index"].asInt();
        if (j.has("path")) {
            for (size_t i = 0; i < j["path"].size(); i++) {
                if (j["path"][i].size() >= 2) {
                    s.path.push_back({j["path"][i][0].asInt(), j["path"][i][1].asInt()});
                }
            }
        }
        if (j.has("cannons")) s.cannons = j["cannons"].asInt();
        if (j.has("marines")) s.marines = j["marines"].asInt();

        if (j.has("fleet_id")) s.fleet_id = j["fleet_id"].asString();
        return s;
    }
};

struct Fleet {
    std::string id;
    std::string owner_id;
    std::vector<std::string> ship_ids;
    std::string admiral_id;
    double x = 0.0;
    double y = 0.0;
    std::string destination;
    std::vector<std::pair<int,int>> path;
    int path_index = 0;
    std::string mission = "patrol";

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("owner_id", owner_id);
        JsonValue sArr = JsonValue::array();
        for (const auto& sid : ship_ids) sArr.push(JsonValue(sid));
        obj.set("ship_ids", sArr);
        obj.set("admiral_id", admiral_id);
        obj.set("x", x);
        obj.set("y", y);
        obj.set("destination", destination);
        obj.set("path_index", path_index);
        JsonValue pArr = JsonValue::array();
        for (const auto& pt : path) {
            JsonValue ptArr = JsonValue::array();
            ptArr.push(pt.first); ptArr.push(pt.second);
            pArr.push(ptArr);
        }
        obj.set("path", pArr);
        obj.set("mission", mission);
        return obj;
    }

    static Fleet fromJson(const JsonValue& j) {
        Fleet f;
        if (j.has("id")) f.id = j["id"].asString();
        if (j.has("owner_id")) f.owner_id = j["owner_id"].asString();
        if (j.has("ship_ids")) {
            for (size_t i = 0; i < j["ship_ids"].size(); i++) f.ship_ids.push_back(j["ship_ids"][i].asString());
        }
        if (j.has("admiral_id")) f.admiral_id = j["admiral_id"].asString();
        if (j.has("x")) f.x = j["x"].asDouble();
        if (j.has("y")) f.y = j["y"].asDouble();
        if (j.has("destination")) f.destination = j["destination"].asString();
        if (j.has("path_index")) f.path_index = j["path_index"].asInt();
        if (j.has("path")) {
            for (size_t i = 0; i < j["path"].size(); i++) {
                if (j["path"][i].size() >= 2) f.path.push_back({j["path"][i][0].asInt(), j["path"][i][1].asInt()});
            }
        }
        if (j.has("mission")) f.mission = j["mission"].asString();
        return f;
    }
};


enum class PortType { NONE, FISHING, TRADE, MILITARY };

inline std::string portTypeToString(PortType t) {
    switch(t) {
        case PortType::FISHING: return "FISHING";
        case PortType::TRADE: return "TRADE";
        case PortType::MILITARY: return "MILITARY";
        default: return "NONE";
    }
}

inline PortType stringToPortType(const std::string& s) {
    if (s == "FISHING") return PortType::FISHING;
    if (s == "TRADE") return PortType::TRADE;
    if (s == "MILITARY") return PortType::MILITARY;
    return PortType::NONE;
}

struct ShipBuildOrder {
    std::string id;
    ShipType type;
    int days_left;
    std::string owner_id;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", shipTypeToString(type));
        obj.set("days_left", days_left);
        obj.set("owner_id", owner_id);
        return obj;
    }

    static ShipBuildOrder fromJson(const JsonValue& j) {
        ShipBuildOrder o;
        if (j.has("id")) o.id = j["id"].asString();
        if (j.has("type")) o.type = stringToShipType(j["type"].asString());
        if (j.has("days_left")) o.days_left = j["days_left"].asInt();
        if (j.has("owner_id")) o.owner_id = j["owner_id"].asString();
        return o;
    }
};

struct PortFacility {
    int level = 1;
    int durability = 100;
    PortType type = PortType::NONE;
    std::string dock_container_id;
    std::vector<std::string> docked_ship_ids;
    bool is_blockaded = false;

    bool has_shipyard = false;
    std::vector<ShipBuildOrder> build_queue;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("level", level);
        obj.set("durability", durability);
        obj.set("type", portTypeToString(type));
        obj.set("dock_container_id", dock_container_id);
        JsonValue shipsArr = JsonValue::array();
        for (const auto& sid : docked_ship_ids) shipsArr.push(JsonValue(sid));
        obj.set("docked_ship_ids", shipsArr);
        obj.set("has_shipyard", has_shipyard);

        obj.set("is_blockaded", is_blockaded);
        JsonValue bqArr = JsonValue::array();
        for (const auto& bq : build_queue) bqArr.push(bq.toJson());
        obj.set("build_queue", bqArr);
        return obj;
    }

    static PortFacility fromJson(const JsonValue& j) {
        PortFacility p;
        if (j.has("level")) p.level = j["level"].asInt();
        if (j.has("durability")) p.durability = j["durability"].asInt();
        if (j.has("type")) p.type = stringToPortType(j["type"].asString());
        if (j.has("dock_container_id")) p.dock_container_id = j["dock_container_id"].asString();
        if (j.has("docked_ship_ids")) {
            for (size_t i = 0; i < j["docked_ship_ids"].size(); i++) {
                p.docked_ship_ids.push_back(j["docked_ship_ids"][i].asString());
            }
        }
        if (j.has("has_shipyard")) p.has_shipyard = j["has_shipyard"].asBool();

        if (j.has("is_blockaded")) p.is_blockaded = j["is_blockaded"].asBool();
        if (j.has("build_queue")) {
            for (size_t i = 0; i < j["build_queue"].size(); i++) {
                p.build_queue.push_back(ShipBuildOrder::fromJson(j["build_queue"][i]));
            }
        }
        return p;
    }
};


enum class MonsterType {
    DRAGON, HYDRA, PHOENIX, LICH_KING, VAMPIRE_LORD, DEATH_KNIGHT,
    ARCHDEMON, BALOR, EREDAR, STORM_GIANT, FIRE_ELEMENTAL,
    KRAKEN, LEVIATHAN, SEA_SERPENT, BEHOLDER, ILLITHID_MIND, ABOLETH, COUNT
};

inline std::string monsterTypeToString(MonsterType t) {
    switch(t) {
        case MonsterType::DRAGON: return "DRAGON";
        case MonsterType::HYDRA: return "HYDRA";
        case MonsterType::PHOENIX: return "PHOENIX";
        case MonsterType::LICH_KING: return "LICH_KING";
        case MonsterType::VAMPIRE_LORD: return "VAMPIRE_LORD";
        case MonsterType::DEATH_KNIGHT: return "DEATH_KNIGHT";
        case MonsterType::ARCHDEMON: return "ARCHDEMON";
        case MonsterType::BALOR: return "BALOR";
        case MonsterType::EREDAR: return "EREDAR";
        case MonsterType::STORM_GIANT: return "STORM_GIANT";
        case MonsterType::FIRE_ELEMENTAL: return "FIRE_ELEMENTAL";
        case MonsterType::KRAKEN: return "KRAKEN";
        case MonsterType::LEVIATHAN: return "LEVIATHAN";
        case MonsterType::SEA_SERPENT: return "SEA_SERPENT";
        case MonsterType::BEHOLDER: return "BEHOLDER";
        case MonsterType::ILLITHID_MIND: return "ILLITHID_MIND";
        case MonsterType::ABOLETH: return "ABOLETH";
        default: return "DRAGON";
    }
}

inline MonsterType stringToMonsterType(const std::string& s) {
    if (s == "HYDRA") return MonsterType::HYDRA;
    if (s == "PHOENIX") return MonsterType::PHOENIX;
    if (s == "LICH_KING") return MonsterType::LICH_KING;
    if (s == "VAMPIRE_LORD") return MonsterType::VAMPIRE_LORD;
    if (s == "DEATH_KNIGHT") return MonsterType::DEATH_KNIGHT;
    if (s == "ARCHDEMON") return MonsterType::ARCHDEMON;
    if (s == "BALOR") return MonsterType::BALOR;
    if (s == "EREDAR") return MonsterType::EREDAR;
    if (s == "STORM_GIANT") return MonsterType::STORM_GIANT;
    if (s == "FIRE_ELEMENTAL") return MonsterType::FIRE_ELEMENTAL;
    if (s == "KRAKEN") return MonsterType::KRAKEN;
    if (s == "LEVIATHAN") return MonsterType::LEVIATHAN;
    if (s == "SEA_SERPENT") return MonsterType::SEA_SERPENT;
    if (s == "BEHOLDER") return MonsterType::BEHOLDER;
    if (s == "ILLITHID_MIND") return MonsterType::ILLITHID_MIND;
    if (s == "ABOLETH") return MonsterType::ABOLETH;
    return MonsterType::DRAGON;
}

struct EpicMonster {
    std::string id;
    MonsterType type = MonsterType::DRAGON;
    std::string name;
    std::string state = "ACTIVE"; // DORMANT, RISING, ACTIVE, WEAKENED, DEFEATED
    int level = 1;
    int health = 1000;
    int maxHealth = 1000;
    int attack = 50;
    int defense = 30;
    std::string region_id;
    int lair_x = 0;
    int lair_y = 0;
    int dread_contribution = 20;
    bool is_visible_on_map = true;
    std::string treasure_chest_id;
    int days_active = 0;
    std::vector<std::string> special_abilities;

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", monsterTypeToString(type));
        obj.set("name", name);
        obj.set("state", state);
        obj.set("level", level);
        obj.set("health", health);
        obj.set("maxHealth", maxHealth);
        obj.set("attack", attack);
        obj.set("defense", defense);
        obj.set("region_id", region_id);
        obj.set("lair_x", lair_x);
        obj.set("lair_y", lair_y);
        obj.set("dread_contribution", dread_contribution);
        obj.set("is_visible_on_map", is_visible_on_map);
        obj.set("treasure_chest_id", treasure_chest_id);
        obj.set("days_active", days_active);
        JsonValue abils = JsonValue::array();
        for (const auto& a : special_abilities) abils.push(JsonValue(a));
        obj.set("special_abilities", abils);
        return obj;
    }

    static EpicMonster fromJson(const JsonValue& j) {
        EpicMonster m;
        if (j.has("id")) m.id = j["id"].asString();
        if (j.has("type")) m.type = stringToMonsterType(j["type"].asString());
        if (j.has("name")) m.name = j["name"].asString();
        if (j.has("state")) m.state = j["state"].asString();
        if (j.has("level")) m.level = j["level"].asInt();
        if (j.has("health")) m.health = j["health"].asInt();
        if (j.has("maxHealth")) m.maxHealth = j["maxHealth"].asInt();
        if (j.has("attack")) m.attack = j["attack"].asInt();
        if (j.has("defense")) m.defense = j["defense"].asInt();
        if (j.has("region_id")) m.region_id = j["region_id"].asString();
        if (j.has("lair_x")) m.lair_x = j["lair_x"].asInt();
        if (j.has("lair_y")) m.lair_y = j["lair_y"].asInt();
        if (j.has("dread_contribution")) m.dread_contribution = j["dread_contribution"].asInt();
        if (j.has("is_visible_on_map")) m.is_visible_on_map = j["is_visible_on_map"].asBool();
        if (j.has("treasure_chest_id")) m.treasure_chest_id = j["treasure_chest_id"].asString();
        if (j.has("days_active")) m.days_active = j["days_active"].asInt();
        if (j.has("special_abilities")) {
            for (size_t i = 0; i < j["special_abilities"].size(); i++) {
                m.special_abilities.push_back(j["special_abilities"][i].asString());
            }
        }
        return m;
    }
};


struct Region {
    std::string id;
    std::string name;
    std::string factionId;
    int population = 0;
    std::vector<double> age_pyramid; // 121 elements (0..120 years)
    int labor_force = 0;
    double unemployment_rate = 0.0;
    int average_wage = 60;
    bool no_road = false;

    double moneySupply = 0;
    std::string vault_id; // Container ID for faction storage
    
    int threat_level = 0;
    int dread = 0;                 // 0-100 Ужас региона (для призыва эпических монстров)          // 0-100 (0 - идеальная безопасность)
    int storage_capacity = 10000;  // максимальная вместимость склада (ед. веса)
    std::string bandit_stash_id;   // ID контейнера с награбленным

    
    // Markets (good -> price)
    std::unordered_map<std::string, double> markets;
    std::vector<MarketOffer> market_square; // T3: Physical market offers
    std::string current_season = "spring"; // T3: Seasonality
    
    // Caravans departing from this region
    std::vector<Caravan> caravans;
    
    // Weather & Climate
    double fertility = 1.0;
    double mineral_wealth = 1.0;
    std::string weather = "Ясно";
    int weatherDaysLeft = 0;
    std::string climate = "temperate";
    std::string placement_type;
    std::string base_type; // Explicit location type (city, fort, anomaly, etc.)
    
    // Production facilities
    std::map<std::string, Facility> facilities;
    
    // Animals
    Animals animals;

    // Available raw resources based on geography/climate
    std::set<GoodType> available_raw_resources;

    // City Layout (CityGen)
    std::vector<CityBlock> cityLayout;
    std::unordered_map<std::string, PriceHistory> priceHistory;
    int starvation_days = 0;

    double attractivenessIndex = 0.0;
    int migrationCooldown = 0;

    std::vector<PlannedHarvest> planned_harvests;
    std::unordered_map<std::string, int> reserveTargets;

    std::unordered_map<std::string, double> prodModifiers;
    JsonValue custom_props = JsonValue::object();
    int layoutWidth = 0;
    int layoutHeight = 0;
    
    // T3: War & Stability mechanics
    int stability = 70;
    int unrest = 0;
    bool isOccupied = false;
    std::string occupierFactionId;
    int daysUnderOccupation = 0;
    int productionBlockedDays = 0;
    
    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("name", name);
        obj.set("factionId", factionId);
        obj.set("population", population);
        obj.set("fertility", fertility);
        obj.set("mineral_wealth", mineral_wealth);
        JsonValue pyr = JsonValue::array();
        for (double val : age_pyramid) pyr.push(JsonValue(val));
        obj.set("age_pyramid", pyr);
        obj.set("labor_force", labor_force);
        obj.set("unemployment_rate", unemployment_rate);
        obj.set("average_wage", average_wage);
        obj.set("no_road", no_road);
        obj.set("moneySupply", moneySupply);
        obj.set("vault_id", vault_id);
        obj.set("threat_level", threat_level);
        obj.set("dread", dread);
        obj.set("storage_capacity", storage_capacity);
        obj.set("bandit_stash_id", bandit_stash_id);
        obj.set("weather", weather);
        obj.set("weatherDaysLeft", weatherDaysLeft);
        obj.set("placement_type", placement_type);
        obj.set("base_type", base_type);
        
        JsonValue m = JsonValue::object();
        for (const auto& [k, v] : markets) m.set(k, v);
        obj.set("markets", m);
        
        JsonValue msq = JsonValue::array();
        for (const auto& offer : market_square) msq.push(offer.toJson());
        obj.set("market_square", msq);
        
        obj.set("current_season", current_season);
        
        JsonValue cars = JsonValue::array();
        for (const auto& c : caravans) cars.push(c.toJson());
        obj.set("caravans", cars);
        
        obj.set("climate", climate);
        
        JsonValue facs = JsonValue::object();
        for (const auto& [k, v] : facilities) facs.set(k, v.toJson());
        obj.set("facilities", facs);
        
        obj.set("animals", animals.toJson());
        
        obj.set("layoutWidth", layoutWidth);
        obj.set("layoutHeight", layoutHeight);
        
        JsonValue resArr = JsonValue::array();
        for (auto g : available_raw_resources) {
            resArr.push(JsonValue(goodTypeToString(g)));
        }
        obj.set("available_raw_resources", resArr);
        JsonValue layoutArr = JsonValue::array();
        for (const auto& block : cityLayout) layoutArr.push(block.toJson());
        obj.set("cityLayout", layoutArr);
        JsonValue phObj = JsonValue::object();
        for (const auto& [k, v] : priceHistory) phObj.set(k, v.toJson());
        obj.set("priceHistory", phObj);
        obj.set("starvation_days", starvation_days);

        obj.set("attractivenessIndex", attractivenessIndex);
        obj.set("migrationCooldown", migrationCooldown);

        JsonValue phArr = JsonValue::array();
        for (const auto& ph : planned_harvests) phArr.push(ph.toJson());
        obj.set("planned_harvests", phArr);

        JsonValue rtObj = JsonValue::object();
        for (const auto& [k, v] : reserveTargets) rtObj.set(k, v);
        obj.set("reserveTargets", rtObj);


        JsonValue pmObj = JsonValue::object();
        for (const auto& [k, v] : prodModifiers) pmObj.set(k, v);
        obj.set("prodModifiers", pmObj);
        obj.set("custom_props", custom_props);
        
        // T3
        obj.set("stability", stability);
        obj.set("unrest", unrest);
        obj.set("isOccupied", isOccupied);
        obj.set("occupierFactionId", occupierFactionId);
        obj.set("daysUnderOccupation", daysUnderOccupation);
        obj.set("productionBlockedDays", productionBlockedDays);

        

        return obj;
    }
    
    static Region fromJson(const JsonValue& j) {
        Region r;
        r.id = j["id"].asString();
        r.name = j["name"].asString();
        r.factionId = j["factionId"].asString();
        r.population = j["population"].asInt();
        if (j.has("fertility")) r.fertility = j["fertility"].asDouble();
        if (j.has("mineral_wealth")) r.mineral_wealth = j["mineral_wealth"].asDouble();
        if (j.has("age_pyramid")) {
            for (size_t i = 0; i < j["age_pyramid"].size(); i++) r.age_pyramid.push_back(j["age_pyramid"][i].asDouble());
        }
        if (j.has("labor_force")) r.labor_force = j["labor_force"].asInt();
        if (j.has("unemployment_rate")) r.unemployment_rate = j["unemployment_rate"].asDouble();
        if (j.has("average_wage")) r.average_wage = j["average_wage"].asInt();
        else r.average_wage = 60;
        if (j.has("no_road")) r.no_road = j["no_road"].asBool();
        r.moneySupply = j["moneySupply"].asDouble();
        r.vault_id = j["vault_id"].asString();
        if (j.has("threat_level")) r.threat_level = j["threat_level"].asInt();
        if (j.has("dread")) r.dread = j["dread"].asInt();
        if (j.has("storage_capacity")) r.storage_capacity = j["storage_capacity"].asInt();
        if (j.has("bandit_stash_id")) r.bandit_stash_id = j["bandit_stash_id"].asString();
        r.weather = j["weather"].asString();
        r.weatherDaysLeft = j["weatherDaysLeft"].asInt();
        if (j.has("placement_type")) r.placement_type = j["placement_type"].asString();
        if (j.has("base_type")) r.base_type = j["base_type"].asString();
        
        if (j.has("markets")) {
            for (const auto& kv : j["markets"].obj_val) {
                r.markets[kv.first] = kv.second.asDouble();
            }
        }
        
        if (j.has("market_square")) {
            for (size_t i = 0; i < j["market_square"].size(); i++) {
                r.market_square.push_back(MarketOffer::fromJson(j["market_square"][i]));
            }
        }
        
        if (j.has("current_season")) r.current_season = j["current_season"].asString();
        
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
        
        if (j.has("layoutWidth")) r.layoutWidth = j["layoutWidth"].asInt();
        if (j.has("layoutHeight")) r.layoutHeight = j["layoutHeight"].asInt();
        if (j.has("cityLayout")) {
            for (size_t i = 0; i < j["cityLayout"].size(); i++) {
                r.cityLayout.push_back(CityBlock::fromJson(j["cityLayout"][i]));
            }
        }
        
                if (j.has("priceHistory")) {
            for (const auto& kv : j["priceHistory"].obj_val) {
                r.priceHistory[kv.first] = PriceHistory::fromJson(kv.second);
            }
        }
        if (j.has("starvation_days")) r.starvation_days = j["starvation_days"].asInt();

        if (j.has("attractivenessIndex")) r.attractivenessIndex = j["attractivenessIndex"].asDouble();
        if (j.has("migrationCooldown")) r.migrationCooldown = j["migrationCooldown"].asInt();

        if (j.has("planned_harvests")) {
            for (size_t i = 0; i < j["planned_harvests"].size(); i++) {
                r.planned_harvests.push_back(PlannedHarvest::fromJson(j["planned_harvests"][i]));
            }
        }
        if (j.has("reserveTargets")) {
            for (const auto& kv : j["reserveTargets"].obj_val) {
                r.reserveTargets[kv.first] = kv.second.asInt();
            }
        }

        if (j.has("prodModifiers")) {
            for (const auto& kv : j["prodModifiers"].obj_val) {
                r.prodModifiers[kv.first] = kv.second.asDouble();
            }
        }
        if (j.has("custom_props")) r.custom_props = j["custom_props"];

if (j.has("available_raw_resources")) {
            for (size_t i = 0; i < j["available_raw_resources"].size(); i++) {
                r.available_raw_resources.insert(stringToGoodType(j["available_raw_resources"][i].asString()));
            }
        }
        
                // T3
        if (j.has("stability")) r.stability = j["stability"].asInt();
        if (j.has("unrest")) r.unrest = j["unrest"].asInt();
        if (j.has("isOccupied")) r.isOccupied = j["isOccupied"].asBool();
        if (j.has("occupierFactionId")) r.occupierFactionId = j["occupierFactionId"].asString();
        if (j.has("daysUnderOccupation")) r.daysUnderOccupation = j["daysUnderOccupation"].asInt();
        if (j.has("productionBlockedDays")) r.productionBlockedDays = j["productionBlockedDays"].asInt();

        
// Backward compatibility for old saves
        if (r.available_raw_resources.empty()) {
            std::string lowerName = r.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c){ return std::tolower(c); });
            if (lowerName.find("гор") != std::string::npos || lowerName.find("пик") != std::string::npos || lowerName.find("шахт") != std::string::npos || lowerName.find("mountain") != std::string::npos || lowerName.find("citadel") != std::string::npos || lowerName.find("цитадель") != std::string::npos) {
                r.available_raw_resources = {GoodType::IRON_ORE, GoodType::GOLD_ORE, GoodType::MEAT};
            } else if (lowerName.find("лес") != std::string::npos || lowerName.find("рощ") != std::string::npos || lowerName.find("forest") != std::string::npos || lowerName.find("wood") != std::string::npos) {
                r.available_raw_resources = {GoodType::WOOD, GoodType::HERBS, GoodType::COTTON, GoodType::MEAT};
            } else if (lowerName.find("пустын") != std::string::npos || lowerName.find("песк") != std::string::npos || lowerName.find("desert") != std::string::npos || lowerName.find("sand") != std::string::npos || lowerName.find("пепел") != std::string::npos || lowerName.find("ash") != std::string::npos) {
                r.available_raw_resources = {GoodType::IRON_ORE, GoodType::HERBS};
            } else if (lowerName.find("море") != std::string::npos || lowerName.find("озер") != std::string::npos || lowerName.find("гавань") != std::string::npos || lowerName.find("порт") != std::string::npos || lowerName.find("sea") != std::string::npos || lowerName.find("haven") != std::string::npos) {
                r.available_raw_resources = {GoodType::FISH, GoodType::WOOD, GoodType::HERBS};
            } else {
                r.available_raw_resources = {GoodType::WHEAT, GoodType::COTTON, GoodType::WOOD, GoodType::MEAT};
            }
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
    double x = 0.0;
    double y = 0.0;
    std::vector<std::pair<int, int>> path;
    int path_index = 0;
    std::string supply_chest_id;
    std::string general_id;
    std::string current_phase = "march"; // march, vanguard_clash, main_battle, rout, victory

    std::string embarked_ship_id;
    std::string target_monster_id;
    
    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("size", size);
        obj.set("morale", morale);
        obj.set("location", location);
        obj.set("destination", destination);
        obj.set("daysToMove", daysToMove);
        obj.set("siegeDays", siegeDays);
        obj.set("x", x);
        obj.set("y", y);
        obj.set("path_index", path_index);
        JsonValue pArr = JsonValue::array();
        for (const auto& pt : path) {
            JsonValue ptArr = JsonValue::array();
            ptArr.push(pt.first); ptArr.push(pt.second);
            pArr.push(ptArr);
        }
        obj.set("path", pArr);
        obj.set("supply_chest_id", supply_chest_id);
        obj.set("general_id", general_id);
        obj.set("current_phase", current_phase);

        obj.set("embarked_ship_id", embarked_ship_id);
        obj.set("target_monster_id", target_monster_id);
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
        if(j.has("x")) a.x = j["x"].asDouble();
        if(j.has("y")) a.y = j["y"].asDouble();
        if(j.has("path_index")) a.path_index = j["path_index"].asInt();
        if (j.has("path")) {
            for (size_t i = 0; i < j["path"].size(); i++) {
                if (j["path"][i].size() >= 2) {
                    a.path.push_back({j["path"][i][0].asInt(), j["path"][i][1].asInt()});
                }
            }
        }
        if(j.has("supply_chest_id")) a.supply_chest_id = j["supply_chest_id"].asString();
        if(j.has("general_id")) a.general_id = j["general_id"].asString();
        if(j.has("current_phase")) a.current_phase = j["current_phase"].asString();

        if(j.has("embarked_ship_id")) a.embarked_ship_id = j["embarked_ship_id"].asString();
        if(j.has("target_monster_id")) a.target_monster_id = j["target_monster_id"].asString();
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

    int warExhaustion = 0;
    
    // T3: Advanced Diplomacy & War
    DiplomaticState warType = DiplomaticState::PEACE;
    int stability = 70;
    int legitimacy = 100;
    WarGoal activeWarGoal;
    std::vector<Ultimatum> ultimatums;
    std::vector<Coalition> coalitions;
    int daysInCurrentWar = 0;
    CasusBelli currentCasusBelli = CasusBelli::NONE;
    std::unordered_map<std::string, int> truceUntil;
    
    

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

        obj.set("warExhaustion", warExhaustion);
        
        // T3
        obj.set("warType", diploStateToString(warType));
        obj.set("stability", stability);
        obj.set("legitimacy", legitimacy);
        obj.set("activeWarGoal", activeWarGoal.toJson());
        
        JsonValue ults = JsonValue::array();
        for (const auto& u : ultimatums) ults.push(u.toJson());
        obj.set("ultimatums", ults);
        
        JsonValue coals = JsonValue::array();
        for (const auto& c : coalitions) coals.push(c.toJson());
        obj.set("coalitions", coals);
        
        obj.set("daysInCurrentWar", daysInCurrentWar);
        obj.set("currentCasusBelli", cbToString(currentCasusBelli));
        
        JsonValue truces = JsonValue::object();
        for (const auto& [k, v] : truceUntil) truces.set(k, v);
        obj.set("truceUntil", truces);

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

        if (j.has("warExhaustion")) f.warExhaustion = j["warExhaustion"].asInt();
        
        // T3
        if (j.has("warType")) f.warType = stringToDiploState(j["warType"].asString());
        if (j.has("stability")) f.stability = j["stability"].asInt();
        if (j.has("legitimacy")) f.legitimacy = j["legitimacy"].asInt();
        if (j.has("activeWarGoal")) f.activeWarGoal = WarGoal::fromJson(j["activeWarGoal"]);
        
        if (j.has("ultimatums")) {
            for (size_t i = 0; i < j["ultimatums"].size(); i++) {
                f.ultimatums.push_back(Ultimatum::fromJson(j["ultimatums"][i]));
            }
        }
        if (j.has("coalitions")) {
            for (size_t i = 0; i < j["coalitions"].size(); i++) {
                f.coalitions.push_back(Coalition::fromJson(j["coalitions"][i]));
            }
        }
        if (j.has("daysInCurrentWar")) f.daysInCurrentWar = j["daysInCurrentWar"].asInt();
        if (j.has("currentCasusBelli")) f.currentCasusBelli = stringToCb(j["currentCasusBelli"].asString());
        
        if (j.has("truceUntil")) {
            for (const auto& kv : j["truceUntil"].obj_val) {
                f.truceUntil[kv.first] = kv.second.asInt();
            }
        }

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
    std::string phase = "recruitment"; // recruitment, espionage, execution, cover_up
    std::string agent_id; // NPC executing the plot

    

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
        obj.set("phase", phase);
        obj.set("agent_id", agent_id);
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
        if(j.has("phase")) i.phase = j["phase"].asString();
        if(j.has("agent_id")) i.agent_id = j["agent_id"].asString();
        return i;
    }
};

struct TrekEvent {
    std::string id;
    std::string description;
    std::string object_type;
    std::string sim_object_id;
    bool can_interact = true;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("description", description);
        obj.set("object_type", object_type);
        obj.set("sim_object_id", sim_object_id);
        obj.set("can_interact", can_interact);
        return obj;
    }
};

struct TrekState {
    bool active = false;
    bool paused = false;
    std::string destination_id;
    int total_hours = 0;
    int elapsed_hours = 0;
    int hours_since_last_bandit = 4;
    double current_x = 0.0;
    double current_y = 0.0;
    std::vector<std::pair<int,int>> path;
    int path_index = 0;
    std::set<std::string> seen_object_ids;
    std::vector<TrekEvent> pending_events;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("active", active);
        obj.set("paused", paused);
        obj.set("destination_id", destination_id);
        obj.set("total_hours", total_hours);
        obj.set("elapsed_hours", elapsed_hours);
        obj.set("hours_since_last_bandit", hours_since_last_bandit);
        obj.set("current_x", current_x);
        obj.set("current_y", current_y);
        obj.set("path_index", path_index);
        JsonValue pArr = JsonValue::array();
        for (const auto& pt : path) {
            JsonValue ptArr = JsonValue::array();
            ptArr.push(pt.first); ptArr.push(pt.second);
            pArr.push(ptArr);
        }
        obj.set("path", pArr);
        JsonValue seen = JsonValue::array();
        for (const auto& id : seen_object_ids) seen.push(JsonValue(id));
        obj.set("seen_object_ids", seen);
        return obj;
    }

    static TrekState fromJson(const JsonValue& j) {
        TrekState t;
        if (j.has("active")) t.active = j["active"].asBool();
        if (j.has("paused")) t.paused = j["paused"].asBool();
        if (j.has("destination_id")) t.destination_id = j["destination_id"].asString();
        if (j.has("total_hours")) t.total_hours = j["total_hours"].asInt();
        if (j.has("elapsed_hours")) t.elapsed_hours = j["elapsed_hours"].asInt();
        if (j.has("hours_since_last_bandit")) t.hours_since_last_bandit = j["hours_since_last_bandit"].asInt();
        if (j.has("current_x")) t.current_x = j["current_x"].asDouble();
        if (j.has("current_y")) t.current_y = j["current_y"].asDouble();
        if (j.has("path_index")) t.path_index = j["path_index"].asInt();
        if (j.has("path")) {
            for (size_t i = 0; i < j["path"].size(); i++) {
                if (j["path"][i].size() >= 2) {
                    t.path.push_back({j["path"][i][0].asInt(), j["path"][i][1].asInt()});
                }
            }
        }
        if (j.has("seen_object_ids")) {
            for (size_t i = 0; i < j["seen_object_ids"].size(); i++) {
                t.seen_object_ids.insert(j["seen_object_ids"][i].asString());
            }
        }
        return t;
    }
};

enum class TileType : uint8_t {
    OCEAN = 0,
    SHALLOW_WATER = 1,
    PLAINS = 2,
    FOREST = 3,
    MOUNTAINS = 4,
    HILLS = 5,
    DESERT = 6,
    SWAMP = 7,
    TUNDRA = 8,
    RUINS = 9,
    ANOMALY = 10,
    RIVER = 11,
    VOLCANO = 12,
    RIVERBANK = 13,
    LAKE = 14,
    FLOODPLAIN = 15,
    LAVA = 16,
    ASH = 17
};

struct MapTile {
    TileType type = TileType::OCEAN;
    uint8_t road_level = 0;   // 0-none, 1-dirt, 2-paved, 3-highway
    uint8_t bridge_flag = 0;  // 1-bridge
    uint8_t water_depth = 0;  // 0-5
    bool is_flooded = false;
    uint8_t road_condition = 0; // 0-normal, 1-ruined

    

    JsonValue toJson() const {
        JsonValue arr = JsonValue::array();
        arr.push(JsonValue((int)type));
        arr.push(JsonValue((int)road_level));
        arr.push(JsonValue((int)bridge_flag));
        arr.push(JsonValue((int)water_depth));
        arr.push(JsonValue(is_flooded));
        arr.push(JsonValue((int)road_condition));
        return arr;
    }

    static MapTile fromJson(const JsonValue& j) {
        MapTile t;
        if (j.type == JsonValue::ARRAY && j.size() >= 5) {
            t.type = static_cast<TileType>(j[0].asInt());
            t.road_level = j[1].asInt();
            t.bridge_flag = j[2].asInt();
            t.water_depth = j[3].asInt();
            t.is_flooded = j[4].asBool();
            if (j.size() >= 6) t.road_condition = j[5].asInt();
        }
        return t;
    }
};

struct Disaster {
    std::string id;
    std::string type;
    int epicenter_x = 0;
    int epicenter_y = 0;
    int radius = 0;
    int strength = 0;
    int days_active = 0;
    std::vector<std::pair<int,int>> affected_tiles;
    std::vector<std::string> affected_regions;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("type", type);
        obj.set("epicenter_x", epicenter_x);
        obj.set("epicenter_y", epicenter_y);
        obj.set("radius", radius);
        obj.set("strength", strength);
        obj.set("days_active", days_active);
        JsonValue tiles = JsonValue::array();
        for (const auto& pt : affected_tiles) {
            JsonValue ptArr = JsonValue::array();
            ptArr.push(JsonValue(pt.first)); ptArr.push(JsonValue(pt.second));
            tiles.push(ptArr);
        }
        obj.set("affected_tiles", tiles);
        JsonValue regs = JsonValue::array();
        for (const auto& r : affected_regions) regs.push(JsonValue(r));
        obj.set("affected_regions", regs);
        return obj;
    }

    static Disaster fromJson(const JsonValue& j) {
        Disaster d;
        if (j.has("id")) d.id = j["id"].asString();
        if (j.has("type")) d.type = j["type"].asString();
        if (j.has("epicenter_x")) d.epicenter_x = j["epicenter_x"].asInt();
        if (j.has("epicenter_y")) d.epicenter_y = j["epicenter_y"].asInt();
        if (j.has("radius")) d.radius = j["radius"].asInt();
        if (j.has("strength")) d.strength = j["strength"].asInt();
        if (j.has("days_active")) d.days_active = j["days_active"].asInt();
        if (j.has("affected_tiles")) {
            for (size_t i = 0; i < j["affected_tiles"].size(); i++) {
                if (j["affected_tiles"][i].size() >= 2) {
                    d.affected_tiles.push_back({j["affected_tiles"][i][0].asInt(), j["affected_tiles"][i][1].asInt()});
                }
            }
        }
        if (j.has("affected_regions")) {
            for (size_t i = 0; i < j["affected_regions"].size(); i++) d.affected_regions.push_back(j["affected_regions"][i].asString());
        }
        return d;
    }
};

struct MapLocation {
    std::string id;
    std::string name;
    int x = 0;
    int y = 0;
    std::string type;
    std::string faction;
    bool no_road = false;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("id", id);
        obj.set("name", name);
        obj.set("x", x);
        obj.set("y", y);
        obj.set("type", type);
        obj.set("faction", faction);
        obj.set("no_road", no_road);
        return obj;
    }

    static MapLocation fromJson(const JsonValue& j) {
        MapLocation loc;
        if (j.has("id")) loc.id = j["id"].asString();
        if (j.has("name")) loc.name = j["name"].asString();
        if (j.has("x")) loc.x = j["x"].asInt();
        if (j.has("y")) loc.y = j["y"].asInt();
        if (j.has("type")) loc.type = j["type"].asString();
        if (j.has("faction")) loc.faction = j["faction"].asString();
        if (j.has("no_road")) loc.no_road = j["no_road"].asBool();
        return loc;
    }
};

struct MapRoad {
    std::string from;
    std::string to;
    std::string condition;
    std::string type = "dirt"; // dirt, paved, bridge, tunnel, ferry, highway
    int integrity = 100;
    std::vector<std::pair<int, int>> waypoints;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("from", from);
        obj.set("to", to);
        obj.set("condition", condition);
        obj.set("type", type);
        obj.set("integrity", integrity);
        JsonValue wp = JsonValue::array();
        for (const auto& p : waypoints) {
            JsonValue pt = JsonValue::array();
            pt.push(JsonValue(p.first));
            pt.push(JsonValue(p.second));
            wp.push(pt);
        }
        obj.set("waypoints", wp);
        return obj;
    }

    static MapRoad fromJson(const JsonValue& j) {
        MapRoad r;
        if (j.has("from")) r.from = j["from"].asString();
        if (j.has("to")) r.to = j["to"].asString();
        if (j.has("condition")) r.condition = j["condition"].asString();
        if (j.has("type")) r.type = j["type"].asString();
        if (j.has("integrity")) r.integrity = j["integrity"].asInt();
        if (j.has("waypoints")) {
            for (size_t i = 0; i < j["waypoints"].size(); i++) {
                if (j["waypoints"][i].size() >= 2) {
                    r.waypoints.push_back({j["waypoints"][i][0].asInt(), j["waypoints"][i][1].asInt()});
                }
            }
        }
        return r;
    }
};

struct WorldMap {
    int width = 256;
    int height = 256;
    std::vector<MapTile> grid;
    std::map<std::string, MapLocation> locations;
    std::vector<MapRoad> roads;
    std::vector<Disaster> disasters;
    int generation_tick = 0;

    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("width", width);
        obj.set("height", height);
        obj.set("generation_tick", generation_tick);
        
        JsonValue gridArr = JsonValue::array();
        for (const auto& t : grid) gridArr.push(t.toJson());
        obj.set("grid", gridArr);
        
        JsonValue locs = JsonValue::object();
        for (const auto& [id, loc] : locations) {
            locs.set(id, loc.toJson());
        }
        obj.set("locations", locs);
        
        JsonValue rds = JsonValue::array();
        for (const auto& r : roads) {
            rds.push(r.toJson());
        }
        obj.set("roads", rds);

        JsonValue disArr = JsonValue::array();
        for (const auto& d : disasters) disArr.push(d.toJson());
        obj.set("disasters", disArr);
        
        return obj;
    }

    static WorldMap fromJson(const JsonValue& j) {
        WorldMap m;
        if (j.has("width")) m.width = j["width"].asInt();
        if (j.has("height")) m.height = j["height"].asInt();
        if (j.has("generation_tick")) m.generation_tick = j["generation_tick"].asInt();
        
        if (j.has("grid")) {
            for (size_t i = 0; i < j["grid"].size(); i++) {
                m.grid.push_back(MapTile::fromJson(j["grid"][i]));
            }
        } else if (j.has("tiles")) { // Legacy support
            for (size_t i = 0; i < j["tiles"].size(); i++) {
                MapTile t;
                t.type = static_cast<TileType>(j["tiles"][i].asInt());
                m.grid.push_back(t);
            }
            if (j.has("road_grid")) {
                for (size_t i = 0; i < j["road_grid"].size() && i < m.grid.size(); i++) {
                    m.grid[i].road_level = j["road_grid"][i].asInt();
                }
            }
        }
        if (j.has("disasters")) {
            for (size_t i = 0; i < j["disasters"].size(); i++) {
                m.disasters.push_back(Disaster::fromJson(j["disasters"][i]));
            }
        }
        
        if (j.has("locations")) {
            for (const auto& kv : j["locations"].obj_val) {
                m.locations[kv.first] = MapLocation::fromJson(kv.second);
            }
        }
        
        if (j.has("roads")) {
            for (size_t i = 0; i < j["roads"].size(); i++) {
                m.roads.push_back(MapRoad::fromJson(j["roads"][i]));
            }
        }
        return m;
    }
};


struct World {
    int tick = 0;
    int current_day = 0;
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
        int peaceBoredom = 0;
    } homeostasis;
    
    // Game objects
    std::unordered_map<std::string, Region> regions;
    std::unordered_map<std::string, Faction> factions;
    std::map<std::string, NPC> npcs;
    std::map<std::string, Business> businesses;
    std::vector<News> news;
    
    // GM intervention tracking
    std::vector<std::string> gmInterventionHistory;
    int lastDirectInjectionDay = -999;
    bool needsGlobalEvent = false;
    
    // Intrigues in progress
    std::vector<Intrigue> intrigues;
    std::map<std::string, JsonValue> nexusData;
    std::vector<Ship> ships;

    std::vector<Fleet> fleets;
    std::vector<EpicMonster> monsters;
    std::map<std::string, PortFacility> port_facilities;

        std::map<std::string, JsonValue> subLocations; // CityGen sublocations
    WorldMap map; // Global World Map
    TrekState player_trek;
    
    

    JsonValue toJson() const {
        JsonValue obj = JsonValue::object();
        obj.set("tick", tick);
        obj.set("current_day", current_day);
        obj.set("era", era);
        
        JsonValue t = JsonValue::object();
        t.set("accumulatedMinutes", time.accumulatedMinutes);
        t.set("lastEventPulse", time.lastEventPulse);
        t.set("internalHour", time.internalHour);
        obj.set("time", t);
        
        JsonValue h = JsonValue::object();
        h.set("warWeariness", homeostasis.warWeariness);
        h.set("fertility", homeostasis.fertility);
        h.set("peaceBoredom", homeostasis.peaceBoredom);
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

        JsonValue bus = JsonValue::object();
        for (const auto& [k, v] : businesses) bus.set(k, v.toJson());
        obj.set("businesses", bus);
        
        JsonValue newsArr = JsonValue::array();
        for (const auto& nw : news) newsArr.push(nw.toJson());
        obj.set("news", newsArr);
        
        JsonValue intrs = JsonValue::array();
        for (const auto& i : intrigues) intrs.push(i.toJson());
        obj.set("intrigues", intrs);
        
        JsonValue nd = JsonValue::object();
        for (const auto& [k, v] : nexusData) nd.set(k, v);
        obj.set("nexusData", nd);
        JsonValue shipsArr = JsonValue::array();
        for (const auto& s : ships) shipsArr.push(s.toJson());
        obj.set("ships", shipsArr);

        JsonValue fleetsArr = JsonValue::array();
        for (const auto& f : fleets) fleetsArr.push(f.toJson());
        obj.set("fleets", fleetsArr);
        JsonValue monsArr = JsonValue::array();
        for (const auto& m : monsters) monsArr.push(m.toJson());
        obj.set("monsters", monsArr);
        
        JsonValue portsObj = JsonValue::object();
        for (const auto& [k, v] : port_facilities) portsObj.set(k, v.toJson());
        obj.set("port_facilities", portsObj);

                JsonValue sl = JsonValue::object();
        for (const auto& [k, v] : subLocations) sl.set(k, v);
        obj.set("subLocations", sl);
        obj.set("map", map.toJson());
        obj.set("player_trek", player_trek.toJson());
        
        obj.set("needsGlobalEvent", needsGlobalEvent);
        obj.set("lastDirectInjectionDay", lastDirectInjectionDay);
        
        return obj;
    }
    
    static World fromJson(const JsonValue& j) {
        World w;
        w.tick = j["tick"].asInt();
        if (j.has("current_day")) w.current_day = j["current_day"].asInt();
        w.era = j["era"].asString();
        
        if (j.has("time")) {
            w.time.accumulatedMinutes = j["time"]["accumulatedMinutes"].asInt();
            w.time.lastEventPulse = j["time"]["lastEventPulse"].asInt();
            w.time.internalHour = j["time"]["internalHour"].asInt();
        }
        
        if (j.has("homeostasis")) {
            w.homeostasis.warWeariness = j["homeostasis"]["warWeariness"].asInt();
            w.homeostasis.fertility = j["homeostasis"]["fertility"].asDouble();
            if (j["homeostasis"].has("peaceBoredom")) w.homeostasis.peaceBoredom = j["homeostasis"]["peaceBoredom"].asInt();
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

        if (j.has("businesses")) {
            for (const auto& kv : j["businesses"].obj_val) {
                w.businesses[kv.first] = Business::fromJson(kv.second);
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
                if (j["news"][i].has("id")) nw.id = j["news"][i]["id"].asString();
                if (j["news"][i].has("causal_link")) nw.causal_link = j["news"][i]["causal_link"].asString();
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
        if (j.has("ships")) {
            for (size_t i = 0; i < j["ships"].size(); i++) {
                w.ships.push_back(Ship::fromJson(j["ships"][i]));
            }
        }

        if (j.has("fleets")) {
            for (size_t i = 0; i < j["fleets"].size(); i++) {
                w.fleets.push_back(Fleet::fromJson(j["fleets"][i]));
            }
        }
        if (j.has("monsters")) {
            for (size_t i = 0; i < j["monsters"].size(); i++) {
                w.monsters.push_back(EpicMonster::fromJson(j["monsters"][i]));
            }
        }
        if (j.has("port_facilities")) {
            for (const auto& kv : j["port_facilities"].obj_val) {
                w.port_facilities[kv.first] = PortFacility::fromJson(kv.second);
            }
        }

                if (j.has("subLocations")) {
            for (const auto& kv : j["subLocations"].obj_val) {
                w.subLocations[kv.first] = kv.second;
            }
        }
        if (j.has("map")) w.map = WorldMap::fromJson(j["map"]);
        w.needsGlobalEvent = j["needsGlobalEvent"].asBool();
        w.lastDirectInjectionDay = j["lastDirectInjectionDay"].asInt();
        if (j.has("player_trek")) w.player_trek = TrekState::fromJson(j["player_trek"]);
        
        return w;
    }
};

// Global world state
static World g_world;
static std::string g_playerId;

bool moveItem(const std::string& itemId, const std::string& targetContainerId) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    if (!g_items.count(itemId)) return false;
    if (!g_containers.count(targetContainerId)) return false;
    
    PhysicalItem& item = g_items[itemId];
    std::string oldOwner = "";
    
    // Remove from old container
    if (!item.container_id.empty() && g_containers.count(item.container_id)) {
        Storage& oldCont = g_containers[item.container_id];
        oldCont.cached_stocks[(int)item.prototype_id] -= item.stack_size;
        oldOwner = oldCont.owner_id;
        auto& vec = oldCont.item_ids;
        auto it = std::find(vec.begin(), vec.end(), itemId);
        if (it != vec.end()) {
            *it = std::move(vec.back());
            vec.pop_back();
        }
        auto& type_vec = oldCont.items_by_type[item.prototype_id];
        auto it2 = std::find(type_vec.begin(), type_vec.end(), itemId);
        if (it2 != type_vec.end()) {
            *it2 = std::move(type_vec.back());
            type_vec.pop_back();
        }
        oldCont.is_dirty = true;
    }
    
    // Add to new container
    std::string newOwner = g_containers[targetContainerId].owner_id;
    if (!oldOwner.empty() && !newOwner.empty() && oldOwner != newOwner) {
        item.history.push_back({g_world.current_day, "Владелец сменился: " + oldOwner + " -> " + newOwner});
    }

    item.container_id = targetContainerId;
    item.is_dirty = true;
    g_containers[targetContainerId].item_ids.push_back(itemId);
    g_containers[targetContainerId].items_by_type[item.prototype_id].push_back(itemId);
    g_containers[targetContainerId].cached_stocks[(int)item.prototype_id] += item.stack_size;
    g_containers[targetContainerId].is_dirty = true;
    
    return true;
}

// ============================================================================
// SIMULATION FUNCTIONS
// ============================================================================

// Shelf life in days for each good type
int getShelfLifeDays(GoodType type) {
    if (type == GoodType::COUNT) return 999999;
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

std::string addNews(const std::string& text, const std::string& location, int importance, const std::string& category = "misc", const std::string& causal_link = "") {
    std::lock_guard<std::mutex> lock(g_news_mutex);
    News nw;
    nw.id = "news_" + generateUUID();
    nw.text = text;
    nw.location = location;
    nw.importance = importance;
    nw.category = category;
    nw.day = g_world.current_day;
    nw.causal_link = causal_link;
    g_world.news.push_back(nw);
    // Лимит удален: храним историю за все время симуляции
    return nw.id;
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
    for (size_t i=0; i<g_containers.data.size(); ++i) {
        if (!g_containers.active[i]) continue;
        Storage& container = g_containers.data[i];
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
    std::string oldOwner = "";
            
            int maxLife = getShelfLifeDays(item.prototype_id);
            if (maxLife == 999999) continue;
            
            int age = g_world.current_day - item.batch_day;
            
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
                item.history.push_back({g_world.current_day, "Сгнило полностью"});
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
    for (size_t i=0; i<g_items.data.size(); ++i) {
        if (!g_items.active[i]) continue;
        if (g_items.data[i].stack_size <= 0) {
            toRemove.push_back(g_items.data[i].id);
        }
    }
    for (const auto& itemId : toRemove) {
        removeItem(itemId, 999999);
    }
}

// Process NPC consumption of food (Multi-threaded)
void processConsumption() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && npc.type != "ruler") active_npcs.push_back(&npc);
    
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                auto rit = g_world.regions.find(npc.currentLocation);
                if (rit == g_world.regions.end()) continue;
                Region& region = rit->second;
                if (region.vault_id.empty()) continue;
                
                npc.needs.hunger -= (1 + (thread_safe_rand() % 2));
                npc.needs.rest -= (2 + (thread_safe_rand() % 2));
                npc.needs.social -= 1;
                
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
                
                if (npc.needs.hunger < 25) {
                    npc.currentActivity = "Ищет еду";
                    int breadAvailable = countItemsInContainer(region.vault_id, GoodType::BREAD);
                    int foodPrice = (int)region.markets["bread"];
                    if (foodPrice == 0) foodPrice = 5;
                    
                    if (npc.gold >= foodPrice && breadAvailable > 0) {
                        npc.gold -= foodPrice;
                        consumeItemsFromContainer(region.vault_id, GoodType::BREAD, 1);
                        {
                            std::lock_guard<std::mutex> lock(*r_locks[region.id]);
                            region.moneySupply += foodPrice;
                        }
                        npc.needs.hunger = 100;
                        npc.currentActivity = "Ест";
                    } else {
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
                    int currentHour = g_world.time.internalHour;
                    for (const auto& sched : npc.schedule) {
                        if (currentHour >= sched.start && currentHour <= sched.end) {
                            npc.currentActivity = sched.activity;
                            if (sched.activity == "Работает") {
                                npc.needs.rest -= 2;
                                if (npc.profession == "Торговец") {
                                    npc.gold += (thread_safe_rand() % 15) + 5;
                                    if ((thread_safe_rand() % 10) == 0 && !npc.inventory_id.empty()) {
                                        if (!region.markets.empty()) {
                                            int idx = thread_safe_rand() % region.markets.size();
                                            int j = 0;
                                            std::string good;
                                            for (const auto& [g, p] : region.markets) {
                                                if (j == idx) { good = g; break; }
                                                j++;
                                            }
                                            if (!good.empty()) {
                                                int price = (int)region.markets[good];
                                                int available = countItemsInContainer(region.vault_id, stringToGoodType(good));
                                                if (npc.gold >= price && available > 0) {
                                                    npc.gold -= price;
                                                    consumeItemsFromContainer(region.vault_id, stringToGoodType(good), 1);
                                                    createItem(stringToGoodType(good), 1, npc.inventory_id, g_world.current_day, "Куплено");
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    int wage = std::max(1, (int)((region.moneySupply / std::max(1, region.population)) * npc.economy.skillLevel * 0.5));
                                    npc.gold += wage;
                                }
                            }
                            break;
                        }
                    }
                }
                
                npc.needs.hunger = std::max(0, std::min(100, npc.needs.hunger));
                npc.needs.rest = std::max(0, std::min(100, npc.needs.rest));
                npc.needs.social = std::max(0, std::min(100, npc.needs.social));
                
                if (npc.needs.hunger == 0 || npc.hp <= 0) {
                    npc.isAlive = false;
                    npc.currentActivity = (npc.needs.hunger == 0) ? "Мертв (Голод)" : "Мертв (Убит)";
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

    // Process caravans movement and delivery
    void processCaravans() {
        std::vector<Region*> active_regions;
        for (auto& [rid, r] : g_world.regions) {
            if (!r.caravans.empty()) active_regions.push_back(&r);
        }
        if (active_regions.empty()) return;

        std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
        std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
        for (const auto& road : g_world.map.roads) {
            if (road.condition == "blocked") {
                for (const auto& wp : road.waypoints) path_status[wp.second * g_world.map.width + wp.first] = 2;
            } else if (road.condition == "ruined") {
                for (const auto& wp : road.waypoints) {
                    path_status[wp.second * g_world.map.width + wp.first] = 1;
                    has_road[wp.second * g_world.map.width + wp.first] = true;
                }
            } else {
                for (const auto& wp : road.waypoints) has_road[wp.second * g_world.map.width + wp.first] = true;
            }
        }

    std::mutex arrived_mutex;
    struct ArrivedCaravan { Caravan c; std::string origin_id; };
    std::vector<ArrivedCaravan> arrived_caravans;

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_regions.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_regions.size(), (t + 1) * chunk_size);
        if (start_idx >= active_regions.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_regions, &arrived_mutex, &arrived_caravans, &has_road, &path_status]() {
            for (int idx = start_idx; idx < end_idx; ++idx) {
                Region& region = *active_regions[idx];
                for (int i = (int)region.caravans.size() - 1; i >= 0; i--) {
                    Caravan& caravan = region.caravans[i];
                    double speedMod = 1.0;
                    if (region.weather == "Метель" || region.weather == "Тропический ливень") speedMod = 0.5;
                    else if (region.weather == "Эфирный шторм") speedMod = 0.0;
                    else if (region.current_season == "winter") speedMod = 0.7;
                    
                    if (speedMod > 0) {
                        double speed = 0.5 * speedMod; // 0.5 тайла в час
                        while (speed > 0 && caravan.path_index < (int)caravan.path.size() - 1) {
                            double target_x = caravan.path[caravan.path_index + 1].first;
                            double target_y = caravan.path[caravan.path_index + 1].second;
                            
                            int nIdx = (int)target_y * g_world.map.width + (int)target_x;
                            bool is_goal = false;
                            if (g_world.map.locations.count(caravan.destination)) {
                                auto destLoc = g_world.map.locations.at(caravan.destination);
                                is_goal = ((int)target_x == destLoc.x && (int)target_y == destLoc.y);
                            }
                            
                            if (path_status[nIdx] == 1) {
                                speedMod /= 3.0;
                                if ((thread_safe_rand() % 1000) < 5) {
                                    caravan.wagons = std::max(0, caravan.wagons - 1);
                                    addNews("АВАРИЯ: Повозка каравана сломалась на разрушенной дороге. Часть груза потеряна.", caravan.origin, 2, "logistics");
                                }
                            }
                            if (path_status[nIdx] == 2 || (!has_road[nIdx] && !is_goal)) {
                                int goalX = 0, goalY = 0;
                                if (g_world.map.locations.count(caravan.destination)) {
                                    goalX = g_world.map.locations.at(caravan.destination).x;
                                    goalY = g_world.map.locations.at(caravan.destination).y;
                                }
                                auto new_path = findPath(g_world.map, caravan.x, caravan.y, goalX, goalY, has_road, path_status, MovementType::LAND, caravan.wagons * 20);
                                if (new_path.empty()) {
                                    new_path = findPath(g_world.map, caravan.x, caravan.y, goalX, goalY, has_road, path_status, MovementType::ANY, caravan.wagons * 20);
                                    if (!new_path.empty()) {
                                        MapRoad bypass;
                                        bypass.from = "bypass_" + generateUUID();
                                        bypass.to = caravan.destination;
                                        bypass.condition = "dirt";
                                        bypass.waypoints = new_path;
                                        g_world.map.roads.push_back(bypass);
                                        g_path_cache_dirty = true;
                                        for (const auto& wp : new_path) has_road[wp.second * g_world.map.width + wp.first] = true;
                                        addNews("ИНФРАСТРУКТУРА: Торговцы проложили новый тракт в обход опасных территорий на пути к " + caravan.destination + ".", region.id, 2, "misc");
                                    }
                                }
                                if (new_path.empty()) {
                                    addNews("КАТАСТРОФА: Караван из " + region.name + " сгинул в глуши из-за полной изоляции!", caravan.destination, 4, "disaster");
                                    region.caravans.erase(region.caravans.begin() + i);
                                    speed = 0;
                                    break;
                                } else {
                                    caravan.path = new_path;
                                    caravan.path_index = 0;
                                    target_x = caravan.path[1].first;
                                    target_y = caravan.path[1].second;
                                }
                            }
                            
                            double dx = target_x - caravan.x;
                            double dy = target_y - caravan.y;
                            double dist = std::hypot(dx, dy);
                            if (dist <= speed) {
                                caravan.x = target_x;
                                caravan.y = target_y;
                                speed -= dist;
                                caravan.path_index++;
                            } else {
                                caravan.x += (dx / dist) * speed;
                                caravan.y += (dy / dist) * speed;
                                speed = 0;
                            }
                        }
                    }
                    
                    if (caravan.path.empty() || caravan.path_index >= (int)caravan.path.size() - 1) {
                        std::lock_guard<std::mutex> lock(arrived_mutex);
                        arrived_caravans.push_back({caravan, region.id});
                        region.caravans.erase(region.caravans.begin() + i);
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();

    for (const auto& ac : arrived_caravans) {
        Caravan caravan = ac.c;
        std::string origin_id = ac.origin_id;
        Region& region = g_world.regions[origin_id];
        
        auto destIt = g_world.regions.find(caravan.destination);
        if (destIt != g_world.regions.end() && !caravan.chest_id.empty()) {
            Region& destRegion = destIt->second;
            
            if (destRegion.threat_level > 90) {
                if (thread_safe_rand() % 100 < 80) {
                    caravan.destination = caravan.origin;
                    caravan.origin = destRegion.id;
                    caravan.hoursLeft = 24 + (thread_safe_rand() % 48);
                    addNews("БЛОКАДА: Караван не смог пробиться в " + destRegion.name + " из-за чудовищ, блокирующих тракты!", destRegion.id, 4, "disaster");
                    region.caravans.push_back(caravan);
                    continue;
                }
            }
            
            int threat = destRegion.threat_level;
            int banditChance = std::min(80, threat);
            bool isAttacked = (thread_safe_rand() % 100) < banditChance;
            bool isRobbed = false;
            
            if (isAttacked) {
                int defenseChance = std::min(90, caravan.guards * 4);
                if ((thread_safe_rand() % 100) < defenseChance) {
                    addNews("НАПАДЕНИЕ: Бандиты атаковали караван из " + region.name + " в " + destRegion.name + ", но " + std::to_string(caravan.guards) + " наемников успешно отбили атаку!", destRegion.id, 2, "trade");
                } else {
                    isRobbed = true;
                    std::string guardLost = caravan.guards > 0 ? " Охрана перебита." : " Охраны не было.";
                    addNews("РАЗБОЙ: Караван из " + region.name + " разграблен бандитами в " + destRegion.name + "!" + guardLost, destRegion.id, 3, "disaster");
                }
            }
            
            if (isRobbed) {
                if (!caravan.merchant_id.empty() && g_world.npcs.count(caravan.merchant_id)) {
                    NPC& merchant = g_world.npcs[caravan.merchant_id];
                    if (thread_safe_rand() % 100 < 50) {
                        merchant.isAlive = false;
                        merchant.death_cause = "Убит бандитами в пути";
                    } else {
                        merchant.currentLocation = destRegion.id;
                        merchant.currentActivity = "Разорен бандитами";
                    }
                }
                
                if (destRegion.bandit_stash_id.empty() || !g_containers.count(destRegion.bandit_stash_id)) {
                    destRegion.bandit_stash_id = createContainer("bandit_stash", "bandits", 999999, 1000, destRegion.id);
                }
                
                if (g_containers.count(caravan.chest_id)) {
                    Storage& chest = g_containers[caravan.chest_id];
                    std::vector<std::string> items_to_move = chest.item_ids;
                    for (const auto& itemId : items_to_move) {
                        moveItem(itemId, destRegion.bandit_stash_id);
                    }
                    std::lock_guard<std::recursive_mutex> reg_lock(g_registry_mutex);
                    g_deleted_containers.push_back(caravan.chest_id);
                    g_containers.erase(caravan.chest_id);
                }
                continue;
            }
            
            if (g_containers.count(caravan.chest_id)) {
                Storage& chest = g_containers[caravan.chest_id];
                double totalRevenue = 0;
                std::map<GoodType, int> deliveredGoods;
                std::vector<std::string> items_to_move = chest.item_ids;
                
                for (const auto& itemId : items_to_move) {
                    if (!g_items.count(itemId)) continue;
                    
                    PhysicalItem& item = g_items[itemId];
                    deliveredGoods[item.prototype_id] += item.stack_size;
                    
                    double price = destRegion.markets[goodTypeToString(item.prototype_id)];
                    if (price == 0) price = BASE_PRICES[(int)item.prototype_id];
                    
                    double itemRev = item.stack_size * price;
                    if (destRegion.moneySupply >= itemRev) {
                        totalRevenue += itemRev;
                        destRegion.moneySupply -= itemRev;
                    } else {
                        totalRevenue += destRegion.moneySupply;
                        destRegion.moneySupply = 0;
                    }
                    
                    moveItem(itemId, destRegion.vault_id);
                }
                
                int tax = totalRevenue * 0.1;
                destRegion.moneySupply += tax;
                int netRevenue = totalRevenue - tax;

                if (!caravan.merchant_id.empty() && g_world.npcs.count(caravan.merchant_id)) {
                    NPC& merchant = g_world.npcs[caravan.merchant_id];
                    merchant.economy.savings += netRevenue;
                    merchant.currentLocation = caravan.destination;
                    merchant.currentActivity = "Торгует на рынке";
                } else {
                    if (g_world.regions.count(caravan.origin)) {
                        g_world.regions[caravan.origin].moneySupply += netRevenue;
                    }
                }
                
                std::string goodsList;
                for (const auto& [gt, amount] : deliveredGoods) {
                    if (!goodsList.empty()) goodsList += ", ";
                    goodsList += std::to_string(amount) + " " + getGoodName(gt);
                }
                if (goodsList.empty()) goodsList = "ничего";
                
                // Логируем все прибытия караванов для отладки и живого мира
                addNews(
                    "ЭКОНОМИКА: Караван из " + region.name + " прибыл в " + destRegion.name + 
                    "! Доставлено: " + goodsList + ". Выручка: " + std::to_string((int)totalRevenue) + " золотых.",
                    destRegion.name, 1, "trade"
                );

                if (g_world.factions.count(region.factionId) && g_world.factions.count(destRegion.factionId)) {
                    if (region.factionId != destRegion.factionId) {
                        // Снижаем бонус к отношениям от торговли и ставим кап, чтобы лорные враги не мирились так быстро
                        if (g_world.factions[region.factionId].relations[destRegion.factionId] < 50) {
                            g_world.factions[region.factionId].relations[destRegion.factionId] += 1;
                            g_world.factions[destRegion.factionId].relations[region.factionId] += 1;
                        }
                    }
                }
                
                std::lock_guard<std::recursive_mutex> reg_lock(g_registry_mutex);
                g_deleted_containers.push_back(caravan.chest_id);
                g_containers.erase(caravan.chest_id);
            }
        }
    }
}

// Process caravans movement and delivery
void processCaravans();

// === КАСКАДНАЯ МОДЕЛЬ ВРЕМЕНИ: ПОДСИСТЕМЫ ===

void updateWeather() {
    int month = ((g_world.current_day / 30) % 12) + 1;

    for (auto& [rid, region] : g_world.regions) {
        // Определение сезона с учетом климата
        std::string season = "spring";
        if (region.climate == "cold") {
            if (month >= 4 && month <= 5) season = "spring";
            else if (month >= 6 && month <= 7) season = "summer";
            else if (month >= 8 && month <= 9) season = "autumn";
            else season = "winter"; // Зима 6 месяцев
        } else if (region.climate == "tropical") {
            if (month >= 4 && month <= 9) season = "summer";
            else season = "spring"; // Нет настоящей зимы
        } else {
            if (month >= 3 && month <= 5) season = "spring";
            else if (month >= 6 && month <= 8) season = "summer";
            else if (month >= 9 && month <= 11) season = "autumn";
            else season = "winter";
        }
        region.current_season = season;

        if (region.weatherDaysLeft > 0) {
            region.weatherDaysLeft--;
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
            
            // Магические аномалии (1% шанс)
            if (rand() % 100 < 1) weathers.push_back("Эфирный шторм");
            
            region.weather = weathers[rand() % weathers.size()];
            region.weatherDaysLeft = 3 + (rand() % 4);
        }
    }
}


void processMigration() {
    // Отключено: миграция теперь обрабатывается в processMonthlyDemographics
}

void checkGlobalEvents() {
    // Еженедельные глобальные события
    if ((rand() % 100) < 2) {
        addNews("Глобальные эфирные течения изменили свое направление. Маги по всему миру чувствуют беспокойство.", "global", 3, "misc");
    }
}

bool hasPendingOrder(const std::string& containerId, GoodType good) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    if (!g_containers.count(containerId)) return false;
    const Storage& cont = g_containers[containerId];
    for (const auto& itemId : cont.item_ids) {
        if (g_items.count(itemId)) {
            const PhysicalItem& item = g_items[itemId];
    std::string oldOwner = "";
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


void processLogistics() {
    for (auto& [bId, bus] : g_world.businesses) {
        if (!bus.is_active) continue;
        for (auto& rule : bus.logistics) {
            rule.days_since_last++;
            if (rule.days_since_last >= rule.frequency_days) {
                if (rule.type == "transfer") {
                    std::string targetCont = rule.target_id;
                    std::string targetRegion = rule.target_id;
                    if (g_world.businesses.count(rule.target_id)) {
                        targetCont = g_world.businesses[rule.target_id].local_storage_id;
                        targetRegion = g_world.businesses[rule.target_id].region_id;
                    } else if (g_world.regions.count(rule.target_id)) {
                        targetCont = g_world.regions[rule.target_id].vault_id;
                    }
                    
                    if (!targetCont.empty() && g_containers.count(bus.local_storage_id)) {
                        int available = countItemsInContainer(bus.local_storage_id, rule.resource);
                        int to_move = std::min(available - rule.keep_reserve, rule.amount);
                        if (to_move > 0) {
                            int moved = 0;
                            Storage& src = g_containers[bus.local_storage_id];
                            std::vector<std::string> items_to_move;
                            for (const auto& itemId : src.item_ids) {
                                if (g_items.count(itemId) && g_items[itemId].prototype_id == rule.resource) items_to_move.push_back(itemId);
                            }
                            
                            std::vector<std::pair<int,int>> caravan_path;
                            if (targetRegion != bus.region_id && !targetRegion.empty()) {
                                if (g_path_cache.count({bus.region_id, targetRegion})) {
                                    caravan_path = g_path_cache[{bus.region_id, targetRegion}];
                                }
                                if (caravan_path.empty()) continue; // Нет пути
                            }

                            if (targetRegion == bus.region_id || targetRegion.empty()) {
                                // ЛОКАЛЬНОЕ ПЕРЕМЕЩЕНИЕ (Внутри города)
                                if (g_containers.count(targetCont)) {
                                    for (const auto& itemId : items_to_move) {
                                        if (moved >= to_move) break;
                                        if (!g_items.count(itemId)) continue;
                                        PhysicalItem& item = g_items[itemId];
                                        int take = std::min(item.stack_size, to_move - moved);
                                        if (take == item.stack_size) { moveItem(itemId, targetCont); moved += take; }
                                        else { removeItem(itemId, take); createItem(rule.resource, take, targetCont, g_world.current_day, "Логистика"); moved += take; }
                                    }
                                    if (moved > 0) addNews("ЛОГИСТИКА: Местные телеги перевезли " + std::to_string(moved) + " " + getGoodName(rule.resource) + " от предприятия (" + std::string(getFacilityName(bus.facility_type)) + ") на склад.", bus.region_id, 1, "logistics");
                                    if (moved > 0) bus.addLog(g_world.current_day, "📦 Местная доставка: отправлено " + std::to_string(moved) + " " + getGoodName(rule.resource) + " на склад.");
                                }
                            } else {
                                // ГЛОБАЛЬНОЕ ПЕРЕМЕЩЕНИЕ (Создаем караван)
                                std::string chestId = createContainer("caravan_chest", "business", 999999, 1000, bus.region_id);
                                for (const auto& itemId : items_to_move) {
                                    if (moved >= to_move) break;
                                    if (!g_items.count(itemId)) continue;
                                    PhysicalItem& item = g_items[itemId];
                                    int take = std::min(item.stack_size, to_move - moved);
                                    if (take == item.stack_size) { moveItem(itemId, chestId); moved += take; }
                                    else { removeItem(itemId, take); createItem(rule.resource, take, chestId, g_world.current_day, "Логистика"); moved += take; }
                                }
                                if (moved > 0) {
                                    Caravan caravan;
                                    caravan.id = "caravan_" + generateUUID();
                                    caravan.merchant_id = ""; // Корпоративный караван
                                    caravan.origin = bus.region_id;
                                    caravan.destination = targetRegion;
                                    caravan.chest_id = chestId;
                                    caravan.wagons = 1 + (moved / 50);
                                    caravan.guards = 2;
                                    caravan.hoursLeft = 24 + (rand() % 48);
                                    if (g_path_cache.count({bus.region_id, targetRegion})) {
                                        caravan.path = g_path_cache[{bus.region_id, targetRegion}];
                                        if (!caravan.path.empty()) { caravan.x = caravan.path[0].first; caravan.y = caravan.path[0].second; }
                                    }
                                    g_world.regions[bus.region_id].caravans.push_back(caravan);
                                    std::string destName = g_world.regions.count(targetRegion) ? g_world.regions[targetRegion].name : targetRegion;
                                    addNews("ЛОГИСТИКА: Предприятие (" + std::string(getFacilityName(bus.facility_type)) + ") отправило корпоративный караван в " + destName + " (" + std::to_string(moved) + " " + getGoodName(rule.resource) + ").", bus.region_id, 2, "logistics");
                                    bus.addLog(g_world.current_day, "🐪 Караван отправлен: " + std::to_string(moved) + " " + getGoodName(rule.resource) + " в " + destName + ".");
                                }
                            }
                        }
                    }
                }                 else if (rule.type == "pull") {
                    std::string sourceCont = rule.target_id;
                    std::string sourceRegion = rule.target_id;
                    if (g_world.businesses.count(rule.target_id)) {
                        sourceCont = g_world.businesses[rule.target_id].local_storage_id;
                        sourceRegion = g_world.businesses[rule.target_id].region_id;
                    } else if (g_world.regions.count(rule.target_id)) {
                        sourceCont = g_world.regions[rule.target_id].vault_id;
                    }
                    
                    if (!sourceCont.empty() && g_containers.count(sourceCont) && g_containers.count(bus.local_storage_id)) {
                        int available = countItemsInContainer(sourceCont, rule.resource);
                        int to_move = std::min(available - rule.keep_reserve, rule.amount);
                        if (to_move > 0) {
                            int moved = 0;
                            Storage& src = g_containers[sourceCont];
                            std::vector<std::string> items_to_move;
                            for (const auto& itemId : src.item_ids) {
                                if (g_items.count(itemId) && g_items[itemId].prototype_id == rule.resource) items_to_move.push_back(itemId);
                            }
                            
                            std::vector<std::pair<int,int>> caravan_path;
                            if (sourceRegion != bus.region_id && !sourceRegion.empty()) {
                                if (g_path_cache.count({sourceRegion, bus.region_id})) {
                                    caravan_path = g_path_cache[{sourceRegion, bus.region_id}];
                                }
                                if (caravan_path.empty()) continue; // Нет пути
                            }

                            if (sourceRegion == bus.region_id || sourceRegion.empty()) {
                                // ЛОКАЛЬНОЕ ПЕРЕМЕЩЕНИЕ
                                for (const auto& itemId : items_to_move) {
                                    if (moved >= to_move) break;
                                    if (!g_items.count(itemId)) continue;
                                    PhysicalItem& item = g_items[itemId];
                                    int take = std::min(item.stack_size, to_move - moved);
                                    if (take == item.stack_size) { moveItem(itemId, bus.local_storage_id); moved += take; }
                                    else { removeItem(itemId, take); createItem(rule.resource, take, bus.local_storage_id, g_world.current_day, "Логистика (забор)"); moved += take; }
                                }
                                if (moved > 0) addNews("ЛОГИСТИКА: Местные грузчики доставили " + std::to_string(moved) + " " + getGoodName(rule.resource) + " на предприятие (" + std::string(getFacilityName(bus.facility_type)) + ").", bus.region_id, 1, "logistics");
                                if (moved > 0) bus.addLog(g_world.current_day, "📥 Местный забор: получено " + std::to_string(moved) + " " + getGoodName(rule.resource) + " со склада.");
                            } else {
                                // ГЛОБАЛЬНОЕ ПЕРЕМЕЩЕНИЕ (Создаем караван из источника к бизнесу)
                                std::string chestId = createContainer("caravan_chest", "business", 999999, 1000, sourceRegion);
                                for (const auto& itemId : items_to_move) {
                                    if (moved >= to_move) break;
                                    if (!g_items.count(itemId)) continue;
                                    PhysicalItem& item = g_items[itemId];
                                    int take = std::min(item.stack_size, to_move - moved);
                                    if (take == item.stack_size) { moveItem(itemId, chestId); moved += take; }
                                    else { removeItem(itemId, take); createItem(rule.resource, take, chestId, g_world.current_day, "Логистика (забор)"); moved += take; }
                                }
                                if (moved > 0) {
                                    Caravan caravan;
                                    caravan.id = "caravan_" + generateUUID();
                                    caravan.merchant_id = ""; 
                                    caravan.origin = sourceRegion;
                                    caravan.destination = bus.region_id;
                                    caravan.chest_id = chestId;
                                    caravan.wagons = 1 + (moved / 50);
                                    caravan.guards = 2;
                                    caravan.hoursLeft = 24 + (rand() % 48);
                                    if (g_path_cache.count({sourceRegion, bus.region_id})) {
                                        caravan.path = g_path_cache[{sourceRegion, bus.region_id}];
                                        if (!caravan.path.empty()) { caravan.x = caravan.path[0].first; caravan.y = caravan.path[0].second; }
                                    }
                                    g_world.regions[sourceRegion].caravans.push_back(caravan);
                                    std::string sourceName = g_world.regions.count(sourceRegion) ? g_world.regions[sourceRegion].name : sourceRegion;
                                    addNews("ЛОГИСТИКА: Корпоративный караван забрал " + std::to_string(moved) + " " + getGoodName(rule.resource) + " из " + sourceName + " и направился к предприятию.", sourceRegion, 2, "logistics");
                                    bus.addLog(g_world.current_day, "🐪 Караван выехал к нам: везет " + std::to_string(moved) + " " + getGoodName(rule.resource) + " из " + sourceName + ".");
                                }
                            }
                        }
                    }
                } else if (rule.type == "order") {
                    if (g_world.regions.count(bus.region_id)) {
                        std::string vaultId = g_world.regions[bus.region_id].vault_id;
                        std::string orderId = createItem(GoodType::DOCUMENT_ORDER, 1, vaultId, g_world.current_day, "Логистический заказ");
                        if (g_items.count(orderId)) {
                            OrderData od;
                            od.issuer_id = bus.region_id;
                            od.issuer_name = "Бизнес: " + bus.id;
                            od.item_prototype = rule.resource;
                            od.quantity = rule.amount;
                            od.max_price_per_unit = rule.max_price > 0 ? rule.max_price : BASE_PRICES[(int)rule.resource] * 2;
                            od.deadline_days = 14;
                            od.status = "pending";
                            od.created_date = g_world.current_day;
                            od.target_container_id = bus.local_storage_id;
                            g_items[orderId].order_data = od;
                                                                    g_items[orderId].custom_props.set("name", "Заказ: " + goodTypeToString(rule.resource));
                                        addNews("ЛОГИСТИКА: Предприятие (" + std::string(getFacilityName(bus.facility_type)) + ") разместило заказ на " + std::to_string(rule.amount) + " " + getGoodName(rule.resource) + ".", bus.region_id, 1, "logistics");
                                        bus.addLog(g_world.current_day, "🛒 Размещен контракт на закупку: " + std::to_string(rule.amount) + " " + getGoodName(rule.resource) + ".");
                                    }
                                }
                            }
                rule.days_since_last = 0;
            }
        }
    }
}

void processPrivateProduction() {
    for (auto& [bId, bus] : g_world.businesses) {
        if (!bus.is_active || bus.employee_count <= 0 || bus.local_storage_id.empty() || bus.production_focus.empty() || (g_world.regions.count(bus.region_id) && g_world.regions[bus.region_id].productionBlockedDays > 0)) continue;
        
        GoodType focusType = stringToGoodType(bus.production_focus);
        if (focusType == GoodType::COUNT) continue;

                    bool isExtractor = (bus.facility_type == "farms" || bus.facility_type == "lumbermills" || bus.facility_type == "mines" || bus.facility_type == "apiaries" || bus.facility_type == "hunting_lodges" || bus.facility_type == "observatories");
            int capacity = bus.employee_count / 2;
            
            // --- ЛОГИКА ИГРОКА: Ручная регулировка эффективности ---
            double prodRatio = bus.target_efficiency / 100.0;
            double weatherMod = 1.0;
            if (g_world.regions.count(bus.region_id)) {
                Region& r = g_world.regions[bus.region_id];
                
                if (bus.facility_type == "farms") {
                    if (r.current_season == "winter" || r.weather == "Эфирный шторм") weatherMod = 0.0;
                    else if (r.weather == "Жара" || r.weather == "Снег" || r.weather == "Метель") weatherMod = 0.3;
                    else if (r.current_season == "spring") weatherMod = 1.2;
                    else if (r.current_season == "autumn") weatherMod = 1.5;
                } else if (bus.facility_type == "lumbermills") {
                    if (r.weather == "Эфирный шторм" || r.weather == "Метель") weatherMod = 0.2;
                }
            }
            
            if (isExtractor) {
                if (g_world.regions.count(bus.region_id)) {
                    Region& r = g_world.regions[bus.region_id];
                    if (r.available_raw_resources.count(focusType)) {
                        int amount = capacity * prodRatio * weatherMod;
                        if (bus.facility_type == "farms") amount *= 2;
                        if (amount > 0) {
                            createItem(focusType, amount, bus.local_storage_id, g_world.current_day, "Частное производство");
                            addNews("ПРОИЗВОДСТВО: Предприятие (" + std::string(getFacilityName(bus.facility_type)) + ") добыло " + std::to_string(amount) + " " + getGoodName(focusType) + " (Мощность: " + std::to_string((int)(prodRatio*100)) + "%).", bus.region_id, 1, "business");
                            bus.addLog(g_world.current_day, "⛏️ Добыто: " + std::to_string(amount) + " " + getGoodName(focusType) + " (Эфф: " + std::to_string((int)(prodRatio*100)) + "%).");
                        }
                    }
                }
            } else {
                const Recipe* activeRecipe = nullptr;
                for (const auto& r : RECIPES) {
                    if (r.facility == bus.facility_type && r.outputs.count(focusType)) {
                        activeRecipe = &r;
                        break;
                    }
                }
                if (activeRecipe) {
                    int maxCrafts = capacity * prodRatio * weatherMod;
                    for (const auto& in : activeRecipe->inputs) {
                        int avail = countItemsInContainer(bus.local_storage_id, in.first);
                        if (in.second > 0) maxCrafts = std::min(maxCrafts, avail / in.second);
                    }
                    if (maxCrafts > 0) {
                        for (const auto& in : activeRecipe->inputs) {
                            consumeItemsFromContainer(bus.local_storage_id, in.first, maxCrafts * in.second);
                        }
                        for (const auto& out : activeRecipe->outputs) {
                            createItem(out.first, maxCrafts * out.second, bus.local_storage_id, g_world.current_day, "Частное производство");
                            addNews("ПРОИЗВОДСТВО: Предприятие (" + std::string(getFacilityName(bus.facility_type)) + ") произвело " + std::to_string(maxCrafts * out.second) + " " + getGoodName(out.first) + " (Мощность: " + std::to_string((int)(prodRatio*100)) + "%).", bus.region_id, 1, "business");
                            bus.addLog(g_world.current_day, "⚙️ Произведено: " + std::to_string(maxCrafts * out.second) + " " + getGoodName(out.first) + " (Эфф: " + std::to_string((int)(prodRatio*100)) + "%).");
                        }
                    }
                }
            }
    }
}


void processFarmers() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && npc.economy.profession_type == "farmer") active_npcs.push_back(&npc);
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                if (!g_world.regions.count(npc.currentLocation)) continue;
                Region& r = g_world.regions[npc.currentLocation];
                std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                if (contId.empty()) continue;

                double seasonMod = 1.0;
                if (r.current_season == "spring") seasonMod = 1.2;
                else if (r.current_season == "summer") seasonMod = 1.5;
                else if (r.current_season == "autumn") seasonMod = 2.0;
                else if (r.current_season == "winter") seasonMod = 0.0;
                if (r.weather == "Эфирный шторм") seasonMod = 0.0;
                
                double yield = npc.economy.skillLevel * r.fertility * seasonMod;
                if (countItemsInContainer(contId, GoodType::SICKLE) > 0) yield *= 1.5;
                int amount = std::max(1, (int)yield);
                
                if (npc.profession == "Фермер" && r.facilities.count("farms") && r.facilities["farms"].level > 0) {
                    createItem(GoodType::WHEAT, amount, contId, g_world.current_day, "Урожай");
                    if (thread_safe_rand() % 100 < 30) createItem(GoodType::COTTON, amount / 2, contId, g_world.current_day, "Урожай");
                } else if (npc.profession == "Охотник" && r.facilities.count("hunting_lodges") && r.facilities["hunting_lodges"].level > 0) {
                    double raceModMeat = (npc.race == "orc") ? 1.5 : 1.0;
                    int amountMeat = std::max(1, (int)(amount * raceModMeat));
                    createItem(GoodType::MEAT, amountMeat, contId, g_world.current_day, "Охота");
                    createItem(GoodType::FUR, amountMeat / 2, contId, g_world.current_day, "Охота");
                } else if (npc.profession == "Пасечник" && r.facilities.count("apiaries") && r.facilities["apiaries"].level > 0) {
                    createItem(GoodType::HONEY, amount, contId, g_world.current_day, "Пасека");
                    createItem(GoodType::WAX, amount / 2, contId, g_world.current_day, "Пасека");
                }

                std::vector<GoodType> goodsToCheck = {GoodType::WHEAT, GoodType::COTTON, GoodType::MEAT, GoodType::FUR, GoodType::HONEY, GoodType::WAX};
                for (GoodType gt : goodsToCheck) {
                    int stock = countItemsInContainer(contId, gt);
                    if (stock > 10) {
                        std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                        bool merged = false;
                        for (auto& ex_offer : r.market_square) {
                            if (ex_offer.seller_id == npc.id && ex_offer.good == gt) {
                                ex_offer.quantity += (stock - 5);
                                merged = true; break;
                            }
                        }
                        if (!merged) {
                            MarketOffer offer;
                            offer.id = "offer_" + generateUUID();
                            offer.seller_id = npc.id;
                            offer.good = gt;
                            offer.quantity = stock - 5;
                            offer.price = BASE_PRICES[(int)gt];
                            r.market_square.push_back(offer);
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processGatherers() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && npc.economy.profession_type == "gatherer") active_npcs.push_back(&npc);
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                if (!g_world.regions.count(npc.currentLocation)) continue;
                Region& r = g_world.regions[npc.currentLocation];
                std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                
                if (npc.profession == "Астроном" && r.facilities.count("observatories") && r.facilities["observatories"].level > 0) {
                    if (thread_safe_rand() % 100 < 20) {
                        createItem(GoodType::ETHER_DUST, 1 + (npc.economy.skillLevel / 3), contId, g_world.current_day, "Наблюдения");
                        int stock = countItemsInContainer(contId, GoodType::ETHER_DUST);
                        if (stock > 2) {
                            std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                            bool merged = false;
                            for (auto& ex_offer : r.market_square) {
                                if (ex_offer.seller_id == npc.id && ex_offer.good == GoodType::ETHER_DUST) {
                                    ex_offer.quantity += (stock - 1);
                                    merged = true; break;
                                }
                            }
                            if (!merged) {
                                MarketOffer offer;
                                offer.id = "offer_" + generateUUID();
                                offer.seller_id = npc.id;
                                offer.good = GoodType::ETHER_DUST;
                                offer.quantity = stock - 1;
                                offer.price = BASE_PRICES[(int)GoodType::ETHER_DUST] * 1.5;
                                r.market_square.push_back(offer);
                            }
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processArtisans() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && npc.economy.profession_type == "artisan") active_npcs.push_back(&npc);
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                if (!g_world.regions.count(npc.currentLocation)) continue;
                Region& r = g_world.regions[npc.currentLocation];
                std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                if (contId.empty()) continue;

                std::string reqFacility = "";
                if (npc.profession == "Кузнец") reqFacility = "forges";
                else if (npc.profession == "Ткач") reqFacility = "weavers";
                else if (npc.profession == "Пекарь") reqFacility = "bakeries";
                else if (npc.profession == "Ювелир") reqFacility = "jewelers";
                
                if (reqFacility.empty() || !r.facilities.count(reqFacility) || r.facilities[reqFacility].level <= 0) continue;

                for (const auto& recipe : RECIPES) {
                    if (recipe.facility != reqFacility) continue;

                    bool canCraft = true;
                    int maxCrafts = npc.economy.skillLevel;
                    
                    for (const auto& in : recipe.inputs) {
                        int avail = countItemsInContainer(contId, in.first);
                        if (avail < in.second) {
                            canCraft = false;
                            double price = r.markets[goodTypeToString(in.first)];
                            if (price <= 0) price = BASE_PRICES[(int)in.first];
                            int cost = in.second * price;
                            
                            if (npc.economy.savings >= cost) {
                                std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                                for (auto it = r.market_square.begin(); it != r.market_square.end(); ++it) {
                                    if (it->good == in.first && it->quantity >= in.second) {
                                        npc.economy.savings -= cost;
                                        {
                                            std::lock_guard<std::mutex> npc_lock(g_npc_state_mutex);
                                            if (g_world.npcs.count(it->seller_id)) g_world.npcs[it->seller_id].economy.savings += cost;
                                        }
                                        it->quantity -= in.second;
                                        createItem(in.first, in.second, contId, g_world.current_day, "Закупка сырья");
                                        canCraft = true;
                                        break;
                                    }
                                }
                            }
                            if (!canCraft) break;
                        }
                        maxCrafts = std::min(maxCrafts, avail / in.second);
                    }
                    
                    if (canCraft && maxCrafts > 0) {
                        double raceMod = 1.0;
                        if (npc.race == "dwarf" && (recipe.facility == "forges" || recipe.facility == "smelters")) raceMod = 1.3;
                        if (npc.race == "elf" && (recipe.facility == "alchemists" || recipe.facility == "jewelers")) raceMod = 1.2;
                        
                        int finalCrafts = std::max(1, (int)(maxCrafts * raceMod));

                        for (const auto& in : recipe.inputs) {
                            consumeItemsFromContainer(contId, in.first, maxCrafts * in.second);
                        }
                        for (const auto& out : recipe.outputs) {
                            createItem(out.first, finalCrafts * out.second, contId, g_world.current_day, "Ремесло");
                            
                            std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                            bool merged = false;
                            for (auto& ex_offer : r.market_square) {
                                if (ex_offer.seller_id == npc.id && ex_offer.good == out.first) {
                                    ex_offer.quantity += (finalCrafts * out.second);
                                    merged = true; break;
                                }
                            }
                            if (!merged) {
                                MarketOffer offer;
                                offer.id = "offer_" + generateUUID();
                                offer.seller_id = npc.id;
                                offer.good = out.first;
                                offer.quantity = finalCrafts * out.second;
                                offer.price = BASE_PRICES[(int)out.first] * 1.5;
                                r.market_square.push_back(offer);
                            }
                        }
                        break; 
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}


// Old processArtisans removed to fix redefinition

void processMages() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && npc.economy.profession_type == "mage") active_npcs.push_back(&npc);
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                if (!g_world.regions.count(npc.currentLocation)) continue;
                Region& r = g_world.regions[npc.currentLocation];
                std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                if (contId.empty() || !r.facilities.count("alchemists") || r.facilities["alchemists"].level <= 0) continue;

                int herbsNeeded = 2;
                int dustNeeded = 1;
                
                int availHerbs = countItemsInContainer(contId, GoodType::HERBS);
                if (availHerbs < herbsNeeded) {
                    std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                    for (auto it = r.market_square.begin(); it != r.market_square.end(); ++it) {
                        if (it->good == GoodType::HERBS && it->quantity >= herbsNeeded) {
                            double price = r.markets[goodTypeToString(GoodType::HERBS)];
                            if (price <= 0) price = BASE_PRICES[(int)GoodType::HERBS];
                            int cost = herbsNeeded * price;
                            if (npc.economy.savings >= cost) {
                                npc.economy.savings -= cost;
                                {
                                    std::lock_guard<std::mutex> npc_lock(g_npc_state_mutex);
                                    if (g_world.npcs.count(it->seller_id)) g_world.npcs[it->seller_id].economy.savings += cost;
                                }
                                it->quantity -= herbsNeeded;
                                createItem(GoodType::HERBS, herbsNeeded, contId, g_world.current_day, "Закупка реагентов");
                                availHerbs += herbsNeeded;
                                break;
                            }
                        }
                    }
                }
                
                int availDust = countItemsInContainer(contId, GoodType::ETHER_DUST);
                if (availDust < dustNeeded) {
                    std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                    for (auto it = r.market_square.begin(); it != r.market_square.end(); ++it) {
                        if (it->good == GoodType::ETHER_DUST && it->quantity >= dustNeeded) {
                            double price = r.markets[goodTypeToString(GoodType::ETHER_DUST)];
                            if (price <= 0) price = BASE_PRICES[(int)GoodType::ETHER_DUST];
                            int cost = dustNeeded * price;
                            if (npc.economy.savings >= cost) {
                                npc.economy.savings -= cost;
                                {
                                    std::lock_guard<std::mutex> npc_lock(g_npc_state_mutex);
                                    if (g_world.npcs.count(it->seller_id)) g_world.npcs[it->seller_id].economy.savings += cost;
                                }
                                it->quantity -= dustNeeded;
                                createItem(GoodType::ETHER_DUST, dustNeeded, contId, g_world.current_day, "Закупка реагентов");
                                availDust += dustNeeded;
                                break;
                            }
                        }
                    }
                }
                
                if (availHerbs >= herbsNeeded && availDust >= dustNeeded) {
                    int maxCrafts = std::min({npc.economy.skillLevel, availHerbs / herbsNeeded, availDust / dustNeeded});
                    if (maxCrafts > 0) {
                        double raceMod = (npc.race == "elf") ? 1.2 : 1.0;
                        int finalCrafts = std::max(1, (int)(maxCrafts * raceMod));
                        
                        consumeItemsFromContainer(contId, GoodType::HERBS, maxCrafts * herbsNeeded);
                        consumeItemsFromContainer(contId, GoodType::ETHER_DUST, maxCrafts * dustNeeded);
                        createItem(GoodType::POTIONS, finalCrafts, contId, g_world.current_day, "Алхимия");
                        
                        std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                        bool merged = false;
                        for (auto& ex_offer : r.market_square) {
                            if (ex_offer.seller_id == npc.id && ex_offer.good == GoodType::POTIONS) {
                                ex_offer.quantity += finalCrafts;
                                merged = true; break;
                            }
                        }
                        if (!merged) {
                            MarketOffer offer;
                            offer.id = "offer_" + generateUUID();
                            offer.seller_id = npc.id;
                            offer.good = GoodType::POTIONS;
                            offer.quantity = finalCrafts;
                            offer.price = BASE_PRICES[(int)GoodType::POTIONS] * 2.0;
                            r.market_square.push_back(offer);
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processServices() {
    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive && (npc.economy.profession_type == "innkeeper" || npc.economy.profession_type == "cleric")) active_npcs.push_back(&npc);
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> r_locks;
    for (const auto& [rid, r] : g_world.regions) r_locks[rid] = std::make_unique<std::mutex>();

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs, &r_locks]() {
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                if (!g_world.regions.count(npc.currentLocation)) continue;
                Region& r = g_world.regions[npc.currentLocation];

                if (npc.economy.profession_type == "innkeeper") {
                    int visitors = 5 + (r.caravans.size() * 2) + (r.population / 1000);
                    int foodNeeded = visitors;
                    int foodBought = 0;
                    
                    std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                    for (auto it = r.market_square.begin(); it != r.market_square.end(); ) {
                        MarketOffer& offer = *it;
                        if (foodBought >= foodNeeded) break;
                        
                        if (offer.good == GoodType::BREAD || offer.good == GoodType::MEAT || offer.good == GoodType::FISH) {
                            int buy_qty = std::min(offer.quantity, foodNeeded - foodBought);
                            int cost = buy_qty * offer.price;
                            
                            if (npc.economy.savings >= cost) {
                                npc.economy.savings -= cost;
                                {
                                    std::lock_guard<std::mutex> npc_lock(g_npc_state_mutex);
                                    if (g_world.npcs.count(offer.seller_id)) {
                                        g_world.npcs[offer.seller_id].economy.savings += cost;
                                        std::string contId = g_world.npcs[offer.seller_id].economy.storage_id.empty() ? g_world.npcs[offer.seller_id].inventory_id : g_world.npcs[offer.seller_id].economy.storage_id;
                                        consumeItemsFromContainer(contId, offer.good, buy_qty);
                                    }
                                }
                                offer.quantity -= buy_qty;
                                foodBought += buy_qty;
                            }
                        }
                        if (offer.quantity <= 0) it = r.market_square.erase(it);
                        else ++it;
                    }
                    int profit = foodBought * 8;
                    npc.economy.savings += profit;
                    
                } else if (npc.economy.profession_type == "cleric") {
                    int rituals = 2 + (r.population / 2000);
                    int suppliesBought = 0;
                    
                    std::lock_guard<std::mutex> lock(*r_locks.at(r.id));
                    for (auto it = r.market_square.begin(); it != r.market_square.end(); ) {
                        MarketOffer& offer = *it;
                        if (suppliesBought >= rituals) break;
                        
                        if (offer.good == GoodType::WAX || offer.good == GoodType::HERBS) {
                            int buy_qty = std::min(offer.quantity, rituals - suppliesBought);
                            int cost = buy_qty * offer.price;
                            
                            if (npc.economy.savings >= cost) {
                                npc.economy.savings -= cost;
                                {
                                    std::lock_guard<std::mutex> npc_lock(g_npc_state_mutex);
                                    if (g_world.npcs.count(offer.seller_id)) {
                                        g_world.npcs[offer.seller_id].economy.savings += cost;
                                        std::string contId = g_world.npcs[offer.seller_id].economy.storage_id.empty() ? g_world.npcs[offer.seller_id].inventory_id : g_world.npcs[offer.seller_id].economy.storage_id;
                                        consumeItemsFromContainer(contId, offer.good, buy_qty);
                                    }
                                }
                                offer.quantity -= buy_qty;
                                suppliesBought += buy_qty;
                            }
                        }
                        if (offer.quantity <= 0) it = r.market_square.erase(it);
                        else ++it;
                    }
                    int donations = suppliesBought * 15 + (thread_safe_rand() % 10);
                    npc.economy.savings += donations;
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processDailyEconomy() {
    int month = ((g_world.current_day / 30) % 12) + 1;
    std::string season = "winter";
    if (month >= 3 && month <= 5) season = "spring";
    else if (month >= 6 && month <= 8) season = "summer";
    else if (month >= 9 && month <= 11) season = "autumn";

    std::vector<Region*> active_regions;
    for (auto& [rid, r] : g_world.regions) active_regions.push_back(&r);

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_regions.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_regions.size(), (t + 1) * chunk_size);
        if (start_idx >= active_regions.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_regions, season]() {
            for (int idx = start_idx; idx < end_idx; ++idx) {
                Region& region = *active_regions[idx];
                std::string rid = region.id;
                
                std::vector<int> vaultStocks((int)GoodType::COUNT, 0);
                {
                    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                    if (!region.vault_id.empty() && g_containers.count(region.vault_id)) {
                        vaultStocks = g_containers[region.vault_id].cached_stocks;
                    }
                }

                int totalFood = vaultStocks[(int)GoodType::BREAD] +
                                vaultStocks[(int)GoodType::MEAT] +
                                vaultStocks[(int)GoodType::FISH] +
                                vaultStocks[(int)GoodType::SMOKED_MEAT];
                
                double foodPerCapita = totalFood / (double)std::max(1, region.population);
                
                if (!g_bootstrap && region.population > 20000 && foodPerCapita < 0.5 && (thread_safe_rand() % 100) < 5) {
                    int deaths = region.population * (0.1 + (thread_safe_rand() % 10) / 100.0);
                    region.population = std::max(0, region.population - deaths);
                    addNews("Вспышка чумы в " + region.name + "! Голод и скученность привели к эпидемии. Погибло " + std::to_string(deaths) + " человек.", rid, 5, "disaster");
                }
                
                if (!g_bootstrap && (season == "summer" || region.weather == "Жара") && (thread_safe_rand() % 1000) < 2) {
                    int wheatAmount = vaultStocks[(int)GoodType::WHEAT];
                    int woodAmount = vaultStocks[(int)GoodType::WOOD];
                    int cw = consumeItemsFromContainer(region.vault_id, GoodType::WHEAT, wheatAmount * 0.8);
                    vaultStocks[(int)GoodType::WHEAT] -= cw;
                    int cwo = consumeItemsFromContainer(region.vault_id, GoodType::WOOD, woodAmount * 0.7);
                    vaultStocks[(int)GoodType::WOOD] -= cwo;
                    addNews("Ужасающая засуха поразила " + region.name + ". Урожай пшеницы погиб, леса горят.", rid, 4, "disaster");
                }
                
                int totalWorkforce = region.labor_force > 0 ? region.labor_force : (region.population * 0.6);
                int totalJobs = 0;
                for (const auto& [fId, fac] : region.facilities) {
                    totalJobs += fac.level * 100;
                }
                for (const auto& [bId, bus] : g_world.businesses) {
                    if (bus.region_id == rid && bus.is_active) totalJobs += bus.employee_count;
                }
                double employmentRate = totalWorkforce > 0 ? std::min(1.0, (double)totalJobs / totalWorkforce) : 1.0;
                int activeWorkers = totalWorkforce * employmentRate;

                if (!g_bootstrap && employmentRate < 0.15 && (thread_safe_rand() % 100) < 2) {
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

                region.reserveTargets["bread"] = region.population * 0.005 * 14;
                region.reserveTargets["meat"] = region.population * 0.005 * 7;
                region.reserveTargets["wheat"] = region.population * 0.005 * 30;
                region.reserveTargets["weapons"] = region.population * 0.1;

                auto getProdMod = [&](GoodType gt) -> double {
                    std::string gtStr = goodTypeToString(gt);
                    double curPrice = region.markets[gtStr];
                    double basePrice = BASE_PRICES[(int)gt];
                    if (curPrice <= 0) curPrice = basePrice;
                    
                    // Базовый модификатор от цены (рыночный сигнал)
                    double targetMod = std::clamp(curPrice / basePrice, 0.1, 1.2);
                    
                    // Корректировка от запасов (государственный контроль от перепроизводства)
                    int stock = vaultStocks[(int)gt];
                    int reserve = region.reserveTargets[gtStr];
                    if (reserve == 0) reserve = region.population * 0.05; // Фолбэк резерва
                    
                    if (stock > reserve * 3) targetMod *= 0.5; // Склады переполнены, снижаем
                    if (stock > reserve * 5) targetMod *= 0.2; // Критический избыток
                    if (stock < reserve) targetMod = 1.2;      // Дефицит, работаем на износ
                    
                    double currentMod = region.prodModifiers.count(gtStr) ? region.prodModifiers[gtStr] : 1.0;
                    
                    // Плавное изменение
                    if (targetMod > currentMod + 0.1) currentMod += 0.1;
                    else if (targetMod < currentMod - 0.1) currentMod -= 0.1;
                    else currentMod = targetMod;
                    
                    region.prodModifiers[gtStr] = currentMod;
                    return currentMod;
                };


                auto getToolEfficiency = [&](const std::string& facName, GoodType toolType, int workers) -> double {
                    if (toolType == GoodType::COUNT) return 1.0;
                    int toolsAvailable = vaultStocks[(int)toolType];
                    int toolsNeeded = std::max(1, workers / 50);
                    if (toolsAvailable >= toolsNeeded) {
                        int broken = 0;
                        for(int i=0; i<toolsNeeded; ++i) if(thread_safe_rand()%100 < 2) broken++;
                        if (broken > 0) {
                            int cb = consumeItemsFromContainer(region.vault_id, toolType, broken);
                            vaultStocks[(int)toolType] -= cb;
                        }
                        return 1.0;
                    } else {
                        double eff = 0.2 + 0.8 * ((double)toolsAvailable / toolsNeeded);
                        int deficit = toolsNeeded - toolsAvailable;
                        if (!hasPendingOrder(region.vault_id, toolType)) {
                            std::string orderId = createItem(GoodType::DOCUMENT_ORDER, 1, region.vault_id, g_world.current_day, "Заказ инструментов");
                            std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                            if (g_items.count(orderId)) {
                                OrderData od;
                                od.issuer_id = rid;
                                od.issuer_name = region.name + " (" + facName + ")";
                                od.item_prototype = toolType;
                                od.quantity = deficit + 5;
                                od.max_price_per_unit = BASE_PRICES[(int)toolType] * 3;
                                od.deadline_days = 21;
                                od.status = "pending";
                                od.created_date = g_world.current_day;
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
                    
                    double blessMod = 1.0;
                    if (g_world.nexusData.count("global_harvest_blessing") && g_world.nexusData["global_harvest_blessing"].asInt() > g_world.current_day) {
                        blessMod = 1.15;
                    }
                    
                    double modWheat = getProdMod(GoodType::WHEAT) * blessMod;
                    double modMeat = getProdMod(GoodType::MEAT);
                    double modFish = getProdMod(GoodType::FISH);
                    double modCotton = getProdMod(GoodType::COTTON);
                    double modHerbs = getProdMod(GoodType::HERBS);

                    int a1 = (workersPerSector * lvl * 2.0 * weatherMod * (fert * region.fertility) * eff * modWheat);
                    int a2 = (workersPerSector * lvl * 0.5 * weatherMod * (fert * region.fertility) * eff * modMeat);
                    int a3 = (workersPerSector * lvl * 0.5 * weatherMod * (fert * region.fertility) * eff * modFish);
                    int a4 = (workersPerSector * (lvl / 15.0) * weatherMod * (fert * region.fertility) * eff * modCotton);
                    int a5 = (workersPerSector * (lvl / 20.0) * weatherMod * (fert * region.fertility) * eff * modHerbs);
                    
                    if (region.available_raw_resources.count(GoodType::WHEAT) && a1 > 0) {
                        region.planned_harvests.push_back({14, GoodType::WHEAT, a1});
                    }
                    if (region.available_raw_resources.count(GoodType::MEAT) && a2 > 0) { createItem(GoodType::MEAT, a2, region.vault_id, g_world.current_day, "Животноводство"); vaultStocks[(int)GoodType::MEAT] += a2; }
                    if (region.available_raw_resources.count(GoodType::FISH) && a3 > 0) { createItem(GoodType::FISH, a3, region.vault_id, g_world.current_day, "Рыболовство"); vaultStocks[(int)GoodType::FISH] += a3; }
                    if (region.available_raw_resources.count(GoodType::COTTON) && a4 > 0) {
                        region.planned_harvests.push_back({14, GoodType::COTTON, a4});
                    }
                    if (region.available_raw_resources.count(GoodType::HERBS) && a5 > 0) {
                        region.planned_harvests.push_back({14, GoodType::HERBS, a5});
                    }
                }
                
                for (auto it = region.planned_harvests.begin(); it != region.planned_harvests.end(); ) {
                    it->days_left--;
                    if (it->days_left <= 0) {
                        if (it->amount > 0) {
                            createItem(it->good, it->amount, region.vault_id, g_world.current_day, "Сбор урожая");
                            vaultStocks[(int)it->good] += it->amount;
                        }
                        it = region.planned_harvests.erase(it);
                    } else {
                        ++it;
                    }
                }

                if (region.facilities.count("lumbermills") && region.facilities["lumbermills"].level > 0) {
                    int lvl = region.facilities["lumbermills"].level;
                    double eff = getToolEfficiency("lumbermills", GoodType::AXE, workersPerSector);
                    double modWood = getProdMod(GoodType::WOOD);
                    int a = (workersPerSector * (lvl / 10.0) * weatherMod * eff * modWood);
                    if (region.available_raw_resources.count(GoodType::WOOD) && a > 0) { createItem(GoodType::WOOD, a, region.vault_id, g_world.current_day, "Лесопилки"); vaultStocks[(int)GoodType::WOOD] += a; }
                }
                if (region.facilities.count("mines") && region.facilities["mines"].level > 0) {
                    int lvl = region.facilities["mines"].level;
                    double eff = getToolEfficiency("mines", GoodType::PICKAXE, workersPerSector);
                    
                    double rushMod = 1.0;
                    if (g_world.nexusData.count("global_gold_rush") && g_world.nexusData["global_gold_rush"].asInt() > g_world.current_day) {
                        rushMod = 2.0;
                    }
                    
                    double modIron = getProdMod(GoodType::IRON_ORE);
                    double modGold = getProdMod(GoodType::GOLD_ORE) * rushMod;
                    int a1 = (workersPerSector * (lvl / 10.0) * region.mineral_wealth * eff * modIron);
                    int a2 = (workersPerSector * (lvl / 30.0) * region.mineral_wealth * eff * modGold);
                    if (region.available_raw_resources.count(GoodType::IRON_ORE) && a1 > 0) { createItem(GoodType::IRON_ORE, a1, region.vault_id, g_world.current_day, "Шахты"); vaultStocks[(int)GoodType::IRON_ORE] += a1; }
                    if (region.available_raw_resources.count(GoodType::GOLD_ORE) && a2 > 0) { createItem(GoodType::GOLD_ORE, a2, region.vault_id, g_world.current_day, "Шахты"); vaultStocks[(int)GoodType::GOLD_ORE] += a2; }
                }

                if (region.facilities.count("hunting_lodges") && region.facilities["hunting_lodges"].level > 0) {
                    int lvl = region.facilities["hunting_lodges"].level;
                    double modFur = getProdMod(GoodType::FUR);
                    int a = (workersPerSector * (lvl / 10.0) * weatherMod * modFur);
                    if (region.available_raw_resources.count(GoodType::FUR) && a > 0) { createItem(GoodType::FUR, a, region.vault_id, g_world.current_day, "Охотничьи угодья"); vaultStocks[(int)GoodType::FUR] += a; }
                    if (region.available_raw_resources.count(GoodType::MEAT) && a > 0) { createItem(GoodType::MEAT, a, region.vault_id, g_world.current_day, "Охотничьи угодья"); vaultStocks[(int)GoodType::MEAT] += a; }
                }
                if (region.facilities.count("apiaries") && region.facilities["apiaries"].level > 0) {
                    int lvl = region.facilities["apiaries"].level;
                    double modHoney = getProdMod(GoodType::HONEY);
                    int a = (workersPerSector * (lvl / 15.0) * weatherMod * modHoney);
                    if (region.available_raw_resources.count(GoodType::HONEY) && a > 0) { createItem(GoodType::HONEY, a, region.vault_id, g_world.current_day, "Пасеки"); vaultStocks[(int)GoodType::HONEY] += a; }
                    if (region.available_raw_resources.count(GoodType::WAX) && a > 0) { createItem(GoodType::WAX, a/2, region.vault_id, g_world.current_day, "Пасеки"); vaultStocks[(int)GoodType::WAX] += a/2; }
                }
                if (region.facilities.count("observatories") && region.facilities["observatories"].level > 0) {
                    int lvl = region.facilities["observatories"].level;
                    double modDust = getProdMod(GoodType::ETHER_DUST);
                    int a = (workersPerSector * (lvl / 20.0) * modDust);
                    if (region.available_raw_resources.count(GoodType::ETHER_DUST) && a > 0) { createItem(GoodType::ETHER_DUST, a, region.vault_id, g_world.current_day, "Обсерватории"); vaultStocks[(int)GoodType::ETHER_DUST] += a; }
                }
                
                for (auto& [fId, fac] : region.facilities) {
                    if (fac.level > 0) {
                        if (thread_safe_rand() % 100 < 20) fac.durability--;
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
                    if (region.productionBlockedDays > 0) continue; // T3: Block production during riots
                    int facLevel = region.facilities[recipe.facility].level;
                    int capacity = workersPerSector * (facLevel / 2.0);
                    if (capacity <= 0) continue;
                    
                    GoodType reqTool = GoodType::COUNT;
                    if (recipe.facility == "forges" || recipe.facility == "smelters") reqTool = GoodType::HAMMER;
                    double eff = getToolEfficiency(recipe.facility, reqTool, workersPerSector);
                    double modRecipe = 1.0;
                    if (!recipe.outputs.empty()) {
                        modRecipe = getProdMod(recipe.outputs.begin()->first);
                    }
                    int maxCrafts = capacity * eff * modRecipe;
                    for (const auto& in : recipe.inputs) {
                        int avail = vaultStocks[(int)in.first];
                        if (in.second > 0) {
                            int possible = avail / in.second;
                            if (possible < capacity) {
                                maxCrafts = std::min(maxCrafts, possible);
                                int deficit = (capacity * in.second) - avail;
                                if (deficit > 0 && !hasPendingOrder(region.vault_id, in.first)) {
                                    std::string orderId = createItem(GoodType::DOCUMENT_ORDER, 1, region.vault_id, g_world.current_day, "Заказ сырья");
                                    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                                    if (g_items.count(orderId)) {
                                        OrderData od;
                                        od.issuer_id = rid;
                                        od.issuer_name = region.name + " (" + recipe.facility + ")";
                                        od.item_prototype = in.first;
                                        od.quantity = deficit * 7;
                                        od.max_price_per_unit = BASE_PRICES[(int)in.first] * 2;
                                        od.deadline_days = 14;
                                        od.status = "pending";
                                        od.created_date = g_world.current_day;
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
                                createItem(out.first, maxCrafts * out.second, region.vault_id, g_world.current_day, "Производство");
                                vaultStocks[(int)out.first] += maxCrafts * out.second;
                            }
                        }
                    }
                }
                
                                double baseFoodNeed = region.population * 0.005;
                double elasticFactor = std::clamp(foodPerCapita / 1.0, 0.4, 1.2);
                int reserveBread = region.reserveTargets["bread"];
                if (vaultStocks[(int)GoodType::BREAD] < reserveBread) {
                    elasticFactor *= 0.8;
                }
                int foodNeed = (int)(baseFoodNeed * elasticFactor);
                
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
                    region.starvation_days++;
                    if (region.starvation_days == 7 && (thread_safe_rand() % 100 < 20)) {
                        addNews("Голод начинает свирепствовать в " + region.name + ". Запасы истощены, люди слабеют.", rid, 3, "disaster");
                    }
                } else {
                    region.starvation_days = 0;
                }
                
                if (g_world.factions.count(region.factionId)) {
                    int taxRevenue = region.moneySupply * 0.02;
                    region.moneySupply -= taxRevenue;
                    if (taxRevenue > 0) {
                        createItem(GoodType::GOLD_INGOT, taxRevenue, region.vault_id, g_world.current_day, "Налоги");
                        vaultStocks[(int)GoodType::GOLD_INGOT] += taxRevenue;
                    }
                }
                
                for (int i=0; i<(int)GoodType::COUNT; i++) {
                    GoodType gt = (GoodType)i;
                    std::string gtStr = goodTypeToString(gt);
                    double base = BASE_PRICES[i];
                    int stock = vaultStocks[i];
                    int reserve = region.reserveTargets[gtStr];
                    int effective_stock = std::max(1, stock - reserve);
                    double demand = region.population * 0.01;
                    
                    // ФИКС: Смягчаем кривую цен (корень из соотношения) для избежания резких скачков
                    double ratio = demand / (double)effective_stock;
                    double soft_ratio = std::pow(ratio, 0.65);
                    double raw_price = base * soft_ratio;
                    
                    double avg7d = region.priceHistory[gtStr].getAvg(7);
                    double final_price = raw_price;
                    if (avg7d > 0.0) {
                        // ФИКС: Инерция рынка. Цена на 80% зависит от вчерашней, меняется плавно
                        final_price = 0.8 * avg7d + 0.2 * raw_price;
                    }

                    bool hasRuinedRoads = false;
                    for (const auto& road : g_world.map.roads) {
                        if ((road.from == rid || road.to == rid) && road.condition == "ruined") {
                            hasRuinedRoads = true; break;
                        }
                    }
                    if (hasRuinedRoads) final_price *= 1.5;
                    
                    final_price = std::clamp(final_price, base * 0.3, base * 4.0); // Суженный коридор цен
                    region.markets[gtStr] = final_price;
                    region.priceHistory[gtStr].add(final_price);
                    
                    // --- NEW MARKET NEWS SYSTEM ---
                    // 1. Moderate price change (informational, no panic)
                    if ((final_price >= base * 2.0 && final_price < base * 3.0) || (final_price <= base * 0.5 && final_price > base * 0.3)) {
                        if (thread_safe_rand() % 100 < 15) { // 15% chance per day
                            std::string direction = (final_price > base) ? "выросли" : "снизились";
                            addNews("Цены на '" + getGoodName(gt) + "' в " + region.name + " " + direction + " из-за изменения поставок.", rid, 1, "market");
                        }
                    }
                    // 2. Acute shortage (only when stocks are critical)
                    if (final_price >= base * 3.0 && effective_stock < demand * 0.2 && (thread_safe_rand() % 100 < 3)) {
                        // Avoid flooding: at most one news per good per region per 30 days (handled by addNews uniqueness not implemented, but we rely on low chance)
                        addNews("Острый дефицит '" + getGoodName(gt) + "' в " + region.name + ". Запасы на исходе, цены значительно выше нормы.", rid, 2, "market");
                    }
                    // 3. Overproduction (prices collapsed)
                    if (final_price <= base * 0.35 && effective_stock > demand * 10) {
                        if (thread_safe_rand() % 100 < 2) { // 2% chance per day
                            addNews("Перепроизводство '" + getGoodName(gt) + "' в " + region.name + ". Склады переполнены, цены рухнули.", rid, 1, "market");
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
    futures.clear();

    std::vector<NPC*> active_npcs;
    for (auto& [id, npc] : g_world.npcs) if (npc.isAlive) active_npcs.push_back(&npc);
    chunk_size = active_npcs.size() / num_threads + 1;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs]() {
            for (int idx = start_idx; idx < end_idx; ++idx) {
                NPC& npc = *active_npcs[idx];
                if (g_world.regions.count(npc.currentLocation)) {
                    Region& r = g_world.regions[npc.currentLocation];
                    if (!npc.economy.isEmployed && (thread_safe_rand() % 100) < 10) npc.economy.isEmployed = true;
                    else if (npc.economy.isEmployed && (thread_safe_rand() % 100) < 2) npc.economy.isEmployed = false;

                    if (npc.economy.isEmployed) {
                        int wage = std::max(1, (int)((r.moneySupply / std::max(1, r.population)) * npc.economy.skillLevel * 0.1));
                        npc.economy.savings += wage;
                    }
                    
                    int foodPrice = r.markets.count("bread") ? (int)r.markets["bread"] : 5;
                    int breadAvailable = countItemsInContainer(r.vault_id, GoodType::BREAD);
                    if (npc.economy.savings >= foodPrice && breadAvailable > 0) {
                        npc.economy.savings -= foodPrice;
                        consumeItemsFromContainer(r.vault_id, GoodType::BREAD, 1);
                        {
                            std::lock_guard<std::mutex> lock(g_npc_state_mutex);
                            r.moneySupply += foodPrice;
                        }
                        npc.needs.hunger = 100;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processMarkets() {
    std::vector<Region*> active_regions;
    for (auto& [rid, r] : g_world.regions) active_regions.push_back(&r);

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_regions.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_regions.size(), (t + 1) * chunk_size);
        if (start_idx >= active_regions.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_regions]() {
            for (int idx = start_idx; idx < end_idx; ++idx) {
                Region& r = *active_regions[idx];
                std::string rid = r.id;
                std::map<GoodType, int> supply;
                std::map<GoodType, int> demand;
                
                for (int i=0; i<(int)GoodType::COUNT; i++) {
                    GoodType gt = (GoodType)i;
                    double baseDemand = r.population * 0.03; // ФИКС: Базовый спрос увеличен в 3 раза
                    
                    if (gt == GoodType::BREAD || gt == GoodType::MEAT || gt == GoodType::FISH || gt == GoodType::WHEAT) {
                        baseDemand = r.population * 0.1; // ФИКС: Спрос на еду увеличен в 2 раза
                    }
                    
                    if (r.current_season == "winter") {
                        if (gt == GoodType::BREAD || gt == GoodType::MEAT || gt == GoodType::FISH || gt == GoodType::WHEAT) baseDemand *= 2.0;
                        if (gt == GoodType::WOOD || gt == GoodType::FUR) baseDemand *= 1.5;
                    } else if (r.current_season == "autumn") {
                        if (gt == GoodType::BREAD || gt == GoodType::MEAT || gt == GoodType::FISH || gt == GoodType::WHEAT) baseDemand *= 0.5;
                    } else if (r.current_season == "spring") {
                        if (gt == GoodType::SICKLE || gt == GoodType::PICKAXE || gt == GoodType::AXE || gt == GoodType::HAMMER) baseDemand *= 2.0;
                    } else if (r.current_season == "summer") {
                        if (gt == GoodType::CLOTHES || gt == GoodType::JEWELRY || gt == GoodType::POTIONS) baseDemand *= 1.5;
                    }
                    
                    demand[gt] = std::max(1, (int)baseDemand); 
                }

                for (const auto& offer : r.market_square) {
                    supply[offer.good] += offer.quantity;
                }

                for (const auto& [gt, sup] : supply) {
                    if (sup > 0) {
                        double base = BASE_PRICES[(int)gt];
                        double price = base * ((double)demand[gt] / sup);
                        price = std::clamp(price, base * 0.2, base * 5.0);
                        r.markets[goodTypeToString(gt)] = price;
                    }
                }

                for (auto& [npcId, npc] : g_world.npcs) {
                    if (!npc.isAlive || npc.currentLocation != rid) continue;
                    
                    std::vector<GoodType> shoppingList;
                    std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                    int invBread = countItemsInContainer(contId, GoodType::BREAD);
                    
                    if (npc.needs.hunger < 50 || invBread < npc.economy.reserve_food) shoppingList.push_back(GoodType::BREAD);
                    if (npc.needs.hunger < 30) shoppingList.push_back(GoodType::MEAT);
                    if (npc.economy.profession_type == "farmer" && thread_safe_rand() % 100 < 5) shoppingList.push_back(GoodType::SICKLE);
                    if (npc.economy.profession_type == "artisan" && thread_safe_rand() % 100 < 5) shoppingList.push_back(GoodType::HAMMER);
                    
                    // Покупка роскоши только если сбережения превышают резерв
                    if (npc.economy.savings > npc.economy.reserve_gold + 500 && thread_safe_rand() % 100 < 2) shoppingList.push_back(GoodType::JEWELRY);
                    
                    for (GoodType neededGood : shoppingList) {
                        for (auto it = r.market_square.begin(); it != r.market_square.end(); ) {
                            MarketOffer& offer = *it;
                            if (offer.good == neededGood && offer.quantity > 0) {
                                double price = r.markets[goodTypeToString(offer.good)];
                                if (price <= 0) price = BASE_PRICES[(int)offer.good];
                                int cost = price;
                                
                                if (npc.economy.savings >= cost) {
                                    std::lock_guard<std::mutex> lock(g_npc_state_mutex);
                                    if (npc.economy.savings >= cost) { // Double check inside lock
                                        npc.economy.savings -= cost;
                                        int tax = cost * 0.05;
                                        int net_profit = cost - tax;
                                        r.moneySupply += tax;
                                        
                                        if (g_world.npcs.count(offer.seller_id)) {
                                            NPC& seller = g_world.npcs[offer.seller_id];
                                            seller.economy.savings += net_profit;
                                            std::string sellerCont = seller.economy.storage_id.empty() ? seller.inventory_id : seller.economy.storage_id;
                                            consumeItemsFromContainer(sellerCont, offer.good, 1);
                                        }
                                        
                                        if (offer.good == GoodType::BREAD || offer.good == GoodType::MEAT || offer.good == GoodType::FISH) {
                                            npc.needs.hunger += 40;
                                        } else {
                                            std::string buyerCont = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
                                            createItem(offer.good, 1, buyerCont, g_world.current_day, "Покупка на рынке");
                                        }
                                        
                                        offer.quantity -= 1;
                                        demand[offer.good] -= 1;
                                        break;
                                    }
                                }
                            }
                            ++it;
                        }
                    }
                }

                for (auto it = r.market_square.begin(); it != r.market_square.end(); ) {
                    MarketOffer& offer = *it;
                    int buy_qty = std::min(offer.quantity, demand[offer.good]);
                    
                    if (buy_qty > 0 && r.moneySupply > 0) {
                        double price = r.markets[goodTypeToString(offer.good)];
                        if (price <= 0) price = BASE_PRICES[(int)offer.good];
                        int cost = buy_qty * price;
                        
                        if (r.moneySupply >= cost) {
                            std::lock_guard<std::mutex> lock(g_npc_state_mutex);
                            if (r.moneySupply >= cost) { // Double check
                                r.moneySupply -= cost;
                                int tax = cost * 0.05;
                                int net_profit = cost - tax;
                                r.moneySupply += tax;
                                
                                if (g_world.npcs.count(offer.seller_id)) {
                                    NPC& seller = g_world.npcs[offer.seller_id];
                                    seller.economy.savings += net_profit;
                                    std::string contId = seller.economy.storage_id.empty() ? seller.inventory_id : seller.economy.storage_id;
                                    consumeItemsFromContainer(contId, offer.good, buy_qty);
                                }
                                offer.quantity -= buy_qty;
                                demand[offer.good] -= buy_qty;
                            }
                        }
                    }
                    
                    if (offer.quantity <= 0) {
                        it = r.market_square.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
}

void processDailyMilitary() {
    std::unordered_map<std::string, std::vector<int>> vaultStocks;
    {
        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
        for (const auto& [rid, r] : g_world.regions) {
            if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
                vaultStocks[rid] = g_containers[r.vault_id].cached_stocks;
            } else {
                vaultStocks[rid].assign((int)GoodType::COUNT, 0);
            }
        }
    }

    std::vector<Faction*> active_factions;
    for (auto& [fid, f] : g_world.factions) active_factions.push_back(&f);

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_factions.size() / num_threads + 1;
    std::vector<std::future<void>> futures;

    // 1. PARALLEL UPKEEP AND DESERTION
    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_factions.size(), (t + 1) * chunk_size);
        if (start_idx >= active_factions.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_factions, &vaultStocks]() {
            for (int idx = start_idx; idx < end_idx; ++idx) {
                Faction& f = *active_factions[idx];
                std::string fid = f.id;
                
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
                    if (f.warType == DiplomaticState::PEACE) {
                        passiveIncome = regionCount * 1500; // Экономический бонус за мир (Peace Dividend)
                    }
                    if (passiveIncome > 0) {
                        createItem(GoodType::GOLD_INGOT, passiveIncome, g_world.regions[capitalRegionId].vault_id, g_world.current_day, "Пассивный доход");
                        vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT] += passiveIncome;
                    }
                }
                
                int armyUpkeep = f.armies.size() * 500; // Армии теперь очень дорогие
                int stateUpkeep = regionCount * 100;
                int luxuryUpkeep = 200;
                // T3: 4.3 Military Taxes & Mobilization
                if (f.warType >= DiplomaticState::LIMITED_WAR) {
                    for (const auto& rId : f.regions) {
                        if (g_world.regions.count(rId)) {
                            Region& r = g_world.regions[rId];
                            if (r.stability >= 40) {
                                int recruits = r.population * 0.01;
                                r.population = std::max(0, r.population - recruits);
                                int wAvail = vaultStocks[rId][(int)GoodType::WEAPONS];
                                int wTaken = wAvail * 0.005;
                                if (wTaken > 0) {
                                    consumeItemsFromContainer(r.vault_id, GoodType::WEAPONS, wTaken);
                                    vaultStocks[rId][(int)GoodType::WEAPONS] -= wTaken;
                                }
                            }
                        }
                    }
                }
                

                int goldToRemove = armyUpkeep + stateUpkeep + luxuryUpkeep;
                
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
                
                if (!capitalRegionId.empty() && g_world.regions.count(capitalRegionId)) {
                    Region& cap = g_world.regions[capitalRegionId];
                    int goldAvailable = vaultStocks[capitalRegionId][(int)GoodType::GOLD_INGOT];
                    
                    if (goldAvailable > 500) {
                        double wPrice = cap.markets[goodTypeToString(GoodType::WEAPONS)];
                        if (wPrice <= 0) wPrice = BASE_PRICES[(int)GoodType::WEAPONS];
                        int wToBuy = std::min(50, (int)(goldAvailable * 0.2 / wPrice));
                        if (wToBuy > 0 && cap.moneySupply >= 0) {
                            consumeItemsFromContainer(cap.vault_id, GoodType::GOLD_INGOT, wToBuy * wPrice);
                            cap.moneySupply += wToBuy * wPrice;
                            createItem(GoodType::WEAPONS, wToBuy, cap.vault_id, g_world.current_day, "Госзакупка");
                            vaultStocks[capitalRegionId][(int)GoodType::WEAPONS] += wToBuy;
                            goldAvailable -= wToBuy * wPrice;
                        }
                        
                        double lPrice = cap.markets[goodTypeToString(GoodType::JEWELRY)];
                        if (lPrice <= 0) lPrice = BASE_PRICES[(int)GoodType::JEWELRY];
                        int lToBuy = std::min(5, (int)(goldAvailable * 0.1 / lPrice));
                        if (lToBuy > 0) {
                            consumeItemsFromContainer(cap.vault_id, GoodType::GOLD_INGOT, lToBuy * lPrice);
                            cap.moneySupply += lToBuy * lPrice;
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
                        int armyIdx = thread_safe_rand() % f.armies.size();
                        Army& mutinousArmy = f.armies[armyIdx];
                        mutinousArmy.morale -= 5;
                        std::string homeRegion = mutinousArmy.location;
                        if (mutinousArmy.morale < 50) {
                            mutinousArmy.size = std::max(0, (int)(mutinousArmy.size * 0.9));
                            if (!homeRegion.empty() && g_world.regions.count(homeRegion)) {
                                g_world.regions[homeRegion].threat_level = std::min(100, g_world.regions[homeRegion].threat_level + 5);
                            }
                            addNews("Дезертирство! Из-за нехватки золота и низкой морали часть армии " + f.name + " дезертировала.", homeRegion, 3, "war");
                        }
                        if (mutinousArmy.morale < 20) {
                            for (const auto& rId : f.regions) {
                                if (g_world.regions.count(rId)) {
                                    int weaponsAvailable = vaultStocks[rId][(int)GoodType::WEAPONS];
                                    int toRemove = std::min(weaponsAvailable, goldToRemove / 10);
                                    if (toRemove > 0) {
                                        consumeItemsFromContainer(g_world.regions[rId].vault_id, GoodType::WEAPONS, toRemove);
                                        vaultStocks[rId][(int)GoodType::WEAPONS] -= toRemove;
                                        goldToRemove -= toRemove * 10;
                                        if (goldToRemove <= 0) break;
                                    }
                                }
                            }
                        }
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
                            if ((thread_safe_rand() % 100) < 10) {
                                addNews("Голодный бунт! Нехватка продовольствия в " + r.name + " привела к жертвам и росту преступности.", rId, 4, "disaster");
                            }
                        }
                    }
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
    futures.clear();

    // 2. SYNCHRONOUS COMBAT RESOLUTION (To prevent Data Races on array modifications)

    for (auto& [fid, faction] : g_world.factions) {
        std::vector<std::string> available_warships;
        for (auto& s : g_world.ships) {
            if (s.owner_id == fid && s.fleet_id.empty() && (s.type == ShipType::WAR_GALLEY || s.type == ShipType::WAR_FRIGATE)) {
                available_warships.push_back(s.id);
            }
        }
        if (available_warships.size() >= 2) {
            Fleet fl;
            fl.id = "fleet_" + generateUUID();
            fl.owner_id = fid;
            fl.ship_ids = available_warships;
            for (auto& [nid, npc] : g_world.npcs) {
                if (npc.isAlive && npc.factionId == fid && (npc.profession == "Адмирал" || npc.profession == "Моряк")) {
                    fl.admiral_id = nid; break;
                }
            }
            std::string targetPort = "";
            for (const auto& [enemyId, state] : faction.diplomacy) {
                if (state == "war" && g_world.factions.count(enemyId)) {
                    for (const auto& erid : g_world.factions[enemyId].regions) {
                        if (g_world.port_facilities.count(erid)) { targetPort = erid; break; }
                    }
                }
                if (!targetPort.empty()) break;
            }
            for (auto& s : g_world.ships) {
                if (s.id == available_warships[0]) { fl.x = s.x; fl.y = s.y; break; }
            }
            if (!targetPort.empty()) {
                fl.destination = targetPort;
                fl.mission = "blockade";
                std::string startRegion = "";
                for (const auto& [rid, loc] : g_world.map.locations) {
                    if (std::abs(loc.x - fl.x) <= 2 && std::abs(loc.y - fl.y) <= 2) { startRegion = rid; break; }
                }
                if (!startRegion.empty() && g_world.map.locations.count(targetPort)) {
                    auto loc1 = g_world.map.locations[startRegion];
                    auto loc2 = g_world.map.locations[targetPort];
                    std::vector<bool> dummy_has_road(g_world.map.width * g_world.map.height, false);
                    std::vector<int> dummy_path_status(g_world.map.width * g_world.map.height, 0);
                    fl.path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::WATER, 10);
                }
                addNews("ФЛОТ: Фракция " + faction.name + " сформировала военный флот и отправила его для блокады порта " + g_world.regions[targetPort].name + "!", targetPort, 5, "war");
            } else {
                fl.mission = "patrol";
            }
            for (auto& s : g_world.ships) {
                if (std::find(available_warships.begin(), available_warships.end(), s.id) != available_warships.end()) {
                    s.fleet_id = fl.id;
                }
            }
            g_world.fleets.push_back(fl);
        }
    }

    std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
    std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
    for (const auto& road : g_world.map.roads) {
        if (road.condition == "blocked") {
            for (const auto& wp : road.waypoints) path_status[wp.second * g_world.map.width + wp.first] = 2;
        } else if (road.condition == "ruined") {
            for (const auto& wp : road.waypoints) {
                path_status[wp.second * g_world.map.width + wp.first] = 1;
                has_road[wp.second * g_world.map.width + wp.first] = true;
            }
        } else {
            for (const auto& wp : road.waypoints) has_road[wp.second * g_world.map.width + wp.first] = true;
        }
    }

    for (auto& [fid, faction] : g_world.factions) {
        for (int i = faction.armies.size() - 1; i >= 0; i--) {
            Army& a = faction.armies[i];

            if (!a.embarked_ship_id.empty()) {
                bool ship_exists = false;
                for (auto& s : g_world.ships) {
                    if (s.id == a.embarked_ship_id) {
                        a.x = s.x; a.y = s.y;
                        ship_exists = true;
                        if (s.path.empty() && s.destination == a.destination) {
                            a.embarked_ship_id = "";
                            a.location = a.destination;
                            addNews("МОРСКОЙ ДЕСАНТ: Армия фракции " + faction.name + " успешно высадилась в " + g_world.regions[a.destination].name + "!", a.destination, 4, "war");
                        }
                        break;
                    }
                }
                if (!ship_exists) {
                    a.morale = 0;
                }
                continue;
            }

            // T3: 4.2 Army Supply
            if (!a.supply_chest_id.empty() && g_containers.count(a.supply_chest_id)) {
                int armyBread = countItemsInContainer(a.supply_chest_id, GoodType::BREAD);
                int armyMeat = countItemsInContainer(a.supply_chest_id, GoodType::MEAT);
                int armyFish = countItemsInContainer(a.supply_chest_id, GoodType::FISH);
                int armySmoked = countItemsInContainer(a.supply_chest_id, GoodType::SMOKED_MEAT);
                int armyWheat = countItemsInContainer(a.supply_chest_id, GoodType::WHEAT);
                
                int dailyNeed = std::max(1, (int)(a.size * 0.02)); 
                int consumed = 0;
                
                // Едят сначала то, что быстро портится
                if (armyMeat > 0) consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::MEAT, std::min(dailyNeed - consumed, armyMeat));
                if (consumed < dailyNeed && armyFish > 0) consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::FISH, std::min(dailyNeed - consumed, armyFish));
                if (consumed < dailyNeed && armyBread > 0) consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::BREAD, std::min(dailyNeed - consumed, armyBread));
                if (consumed < dailyNeed && armySmoked > 0) consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::SMOKED_MEAT, std::min(dailyNeed - consumed, armySmoked));
                if (consumed < dailyNeed && armyWheat > 0) consumed += consumeItemsFromContainer(a.supply_chest_id, GoodType::WHEAT, std::min(dailyNeed - consumed, armyWheat));
                
                // Фуражировка в пути (если еды не хватает, армия пытается найти еду на месте)
                if (consumed < dailyNeed && !a.location.empty() && g_world.regions.count(a.location)) {
                    Region& r = g_world.regions[a.location];
                    if (r.factionId != fid && r.threat_level < 100) { // Грабят вражеские или нейтральные земли
                        int forageAmount = std::min(dailyNeed - consumed, (int)(r.population * 0.01));
                        if (forageAmount > 0) {
                            int rBread = countItemsInContainer(r.vault_id, GoodType::BREAD);
                            int fTaken = consumeItemsFromContainer(r.vault_id, GoodType::BREAD, std::min(forageAmount, rBread));
                            consumed += fTaken;
                            r.threat_level = std::min(100, r.threat_level + 2); // Мародерство злит местных
                        }
                    }
                }

                if (consumed < dailyNeed) {
                    a.morale -= 5;
                } else {
                    a.morale = std::min(100, a.morale + 1);
                }
            } else {
                a.morale -= 5;
            }

            if (a.morale < 20 && a.morale > 0) {
                int deserters = std::max(1, (int)(a.size * 0.02));
                a.size -= deserters;
                if (!a.location.empty() && g_world.regions.count(a.location)) {
                    g_world.regions[a.location].population += deserters;
                }
            } else if (a.morale <= 0) {
                addNews("Армия фракции " + faction.name + " распалась из-за голода и нулевой морали!", a.location, 4, "war");
                if (!a.location.empty() && g_world.regions.count(a.location)) {
                    g_world.regions[a.location].population += a.size;
                }
                faction.armies.erase(faction.armies.begin() + i);
                continue;
            }

            if (!a.path.empty() && a.path_index < (int)a.path.size() - 1) {
                double speed = 3.0; // 3 тайла в день (ускорено для динамики)
                while (speed > 0 && a.path_index < (int)a.path.size() - 1) {
                    double target_x = a.path[a.path_index + 1].first;
                    double target_y = a.path[a.path_index + 1].second;
                    
                    int nIdx = (int)target_y * g_world.map.width + (int)target_x;
                    bool is_goal = false;
                    if (g_world.map.locations.count(a.destination)) {
                        auto destLoc = g_world.map.locations.at(a.destination);
                        is_goal = ((int)target_x == destLoc.x && (int)target_y == destLoc.y);
                    }
                    
                    if (path_status[nIdx] == 1) speed /= 3.0;

                    TileType t = g_world.map.grid[nIdx].type;
                    bool is_water = (t == TileType::OCEAN || t == TileType::SHALLOW_WATER || t == TileType::LAKE || t == TileType::RIVER);
                    bool has_bridge = g_world.map.grid[nIdx].bridge_flag;
                    
                    if (is_water && !has_bridge) {
                        bool boarded = false;
                        for (auto& s : g_world.ships) {
                            if (s.owner_id == fid && (s.type == ShipType::TRANSPORT || s.type == ShipType::WAR_GALLEY || s.type == ShipType::WAR_FRIGATE)) {
                                if (std::hypot(s.x - a.x, s.y - a.y) <= 3.0) {
                                    a.embarked_ship_id = s.id;
                                    s.destination = a.destination;
                                    auto loc1 = g_world.map.locations[a.location];
                                    auto loc2 = g_world.map.locations[a.destination];
                                    std::vector<bool> dummy_has_road(g_world.map.width * g_world.map.height, false);
                                    std::vector<int> dummy_path_status(g_world.map.width * g_world.map.height, 0);
                                    auto sea_path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::WATER, 10);
                                    if (!sea_path.empty()) {
                                        s.path = sea_path;
                                        s.path_index = 0;
                                    }
                                    addNews("ПОГРУЗКА: Армия " + faction.name + " взошла на корабли для морского десанта на " + g_world.regions[a.destination].name + ".", a.location, 3, "war");
                                    boarded = true;
                                    break;
                                }
                            }
                        }
                        if (!boarded) {
                            speed = 0;
                            if (g_world.port_facilities.count(a.location) && g_world.port_facilities[a.location].has_shipyard) {
                                bool already_building = false;
                                for (const auto& bq : g_world.port_facilities[a.location].build_queue) {
                                    if (bq.owner_id == fid && bq.type == ShipType::TRANSPORT) already_building = true;
                                }
                                if (!already_building) {
                                    ShipBuildOrder order;
                                    order.id = "build_" + generateUUID();
                                    order.type = ShipType::TRANSPORT;
                                    order.days_left = 10;
                                    order.owner_id = fid;
                                    g_world.port_facilities[a.location].build_queue.push_back(order);
                                    addNews("ВОЕННЫЙ ЗАКАЗ: Фракция " + faction.name + " срочно заложила транспортные суда для переброски войск.", a.location, 2, "war");
                                }
                            }
                            break;
                        }
                    }
                    if (path_status[nIdx] == 2 || (!has_road[nIdx] && !is_goal)) {
                        int goalX = 0, goalY = 0;
                        if (g_world.map.locations.count(a.destination)) {
                            goalX = g_world.map.locations.at(a.destination).x;
                            goalY = g_world.map.locations.at(a.destination).y;
                        }
                        auto new_path = findPath(g_world.map, a.x, a.y, goalX, goalY, has_road, path_status, MovementType::LAND, a.size);
                        if (new_path.empty()) {
                            new_path = findPath(g_world.map, a.x, a.y, goalX, goalY, has_road, path_status, MovementType::ANY, a.size);
                            if (!new_path.empty()) {
                                MapRoad bypass;
                                bypass.from = "bypass_" + generateUUID();
                                bypass.to = a.destination;
                                bypass.condition = "dirt";
                                bypass.waypoints = new_path;
                                g_world.map.roads.push_back(bypass);
                                g_path_cache_dirty = true;
                                for (const auto& wp : new_path) has_road[wp.second * g_world.map.width + wp.first] = true;
                                addNews("ИНФРАСТРУКТУРА: Инженерные войска " + faction.name + " проложили новую дорогу в обход препятствий на пути к " + a.destination + ".", a.location, 2, "war");
                            }
                        }
                        if (new_path.empty()) {
                            addNews("КАТАСТРОФА: Армия фракции " + faction.name + " застряла в абсолютно непроходимой местности и распалась!", a.location, 4, "war");
                            faction.armies.erase(faction.armies.begin() + i);
                            break;
                        } else {
                            a.path = new_path;
                            a.path_index = 0;
                            target_x = a.path[1].first;
                            target_y = a.path[1].second;
                        }
                    }
                    
                    double dx = target_x - a.x;
                    double dy = target_y - a.y;
                    double dist = std::hypot(dx, dy);
                    if (dist <= speed) {
                        a.x = target_x;
                        a.y = target_y;
                        speed -= dist;
                        a.path_index++;
                    } else {
                        a.x += (dx / dist) * speed;
                        a.y += (dy / dist) * speed;
                        speed = 0;
                    }
                }
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
                        if (eFaction.armies[j].destination == targetLoc || eFaction.armies[j].location == targetLoc) {
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

                auto assignGeneral = [&](Army& army, const std::string& fId) {
                    if (army.general_id.empty()) {
                        for (auto& [nid, npc] : g_world.npcs) {
                            if (npc.isAlive && npc.factionId == fId && (npc.type == "ruler" || npc.profession == "Генерал")) {
                                army.general_id = nid; break;
                            }
                        }
                    }
                };
                assignGeneral(a, fid);
                assignGeneral(defArmy, enemyFactionId);

                std::string locName = g_world.regions.count(targetLoc) ? g_world.regions[targetLoc].name : targetLoc;

                if (a.current_phase == "march" || a.current_phase == "") {
                    a.current_phase = "vanguard_clash";
                    defArmy.current_phase = "vanguard_clash";
                    addNews("СТОЛКНОВЕНИЕ: Авангарды армий " + faction.name + " и " + defender.name + " встретились у " + locName + ".", targetLoc, 3, "war");
                } else if (a.current_phase == "vanguard_clash") {
                    a.size -= a.size * 0.05;
                    defArmy.size -= defArmy.size * 0.05;
                    a.current_phase = "main_battle";
                    defArmy.current_phase = "main_battle";
                    addNews("БИТВА: Началось основное сражение между " + faction.name + " и " + defender.name + " у " + locName + ".", targetLoc, 4, "war");
                } else if (a.current_phase == "main_battle") {
                    double atkPower = a.size * (a.morale / 100.0) * ((thread_safe_rand() % 50) / 100.0 + 0.8);
                    double defPower = defArmy.size * (defArmy.morale / 100.0) * ((thread_safe_rand() % 50) / 100.0 + 1.0);

                    auto applyDisasterPenalty = [&](const std::string& fId, double& power, std::string& causeText, std::string& causalLink) {
                        std::string dayKey = fId + "_last_disaster_day";
                        if (g_world.nexusData.count(dayKey)) {
                            int dDay = g_world.nexusData[dayKey].asInt();
                            if (g_world.current_day - dDay <= 14) {
                                power *= 0.7;
                                causeText = "Ослабленная недавним событием (" + g_world.nexusData[fId + "_last_disaster_type"].asString() + "), ";
                                causalLink = g_world.nexusData[fId + "_last_disaster_news"].asString();
                            }
                        }
                    };

                    std::string atkCause, defCause, atkLink, defLink;
                    applyDisasterPenalty(fid, atkPower, atkCause, atkLink);
                    applyDisasterPenalty(enemyFactionId, defPower, defCause, defLink);

                    auto applyWound = [&](const std::string& genId) {
                        if (genId.empty() || !g_world.npcs.count(genId)) return;
                        NPC& gen = g_world.npcs[genId];
                        if (gen.isAlive && (thread_safe_rand() % 100) < 15) {
                            gen.hp -= (10 + thread_safe_rand() % 20);
                            if (gen.hp > 0) {
                                std::vector<std::string> wTypes = {"глубокая рана", "сломанная рука", "шрам"};
                                Wound w;
                                w.type = wTypes[thread_safe_rand() % wTypes.size()];
                                w.severity = 3 + thread_safe_rand() % 5;
                                w.day_received = g_world.current_day;
                                gen.wounds.push_back(w);
                                addNews("В пылу битвы полководец " + gen.name + " получил ранение: " + w.type + ".", targetLoc, 4, "war");
                            }
                        }
                    };
                    applyWound(a.general_id);
                    applyWound(defArmy.general_id);

                    if (atkPower > defPower) {
                        int casualties = std::max(1, (int)(defArmy.size * 0.6));
                        a.size -= std::max(1, (int)(a.size * 0.15));
                        std::string newsText = atkCause + "армия " + faction.name + " прорвала ряды " + defender.name + " у " + locName + "! ";
                        if (!defCause.empty()) newsText += defCause + "войска " + defender.name + " дрогнули. ";
                        newsText += "Враг разбит и бежит. Потери врага: " + std::to_string(casualties) + ".";
                        addNews(newsText, targetLoc, 5, "war", !defLink.empty() ? defLink : atkLink);
                        defender.armies.erase(defender.armies.begin() + defArmyIndex);
                        isCombatActive = false;
                        a.current_phase = "march";
                        defender.warExhaustion += 10;
                        if (a.size < 10) {
                            addNews("Остатки армии " + faction.name + " дезертировали после тяжелых боев у " + locName + ".", targetLoc, 3, "war");
                            faction.armies.erase(faction.armies.begin() + i);
                            armySurvived = false;
                        }
                    } else {
                        int casualties = std::max(1, (int)(a.size * 0.6));
                        defArmy.size -= std::max(1, (int)(defArmy.size * 0.15));
                        std::string newsText = defCause + "войска " + defender.name + " отбили атаку " + faction.name + " у " + locName + "! ";
                        if (!atkCause.empty()) newsText += atkCause + "нападающие разбиты. ";
                        newsText += "Потери: " + std::to_string(casualties) + ".";
                        addNews(newsText, targetLoc, 5, "war", !atkLink.empty() ? atkLink : defLink);
                        faction.armies.erase(faction.armies.begin() + i);
                        armySurvived = false;
                        defArmy.current_phase = "march";
                        faction.warExhaustion += 10;
                        if (defArmy.size < 10) {
                            addNews("Остатки армии " + defender.name + " дезертировали после тяжелых боев у " + locName + ".", targetLoc, 3, "war");
                            defender.armies.erase(defender.armies.begin() + defArmyIndex);
                        }
                    }
                }
            } else if (g_world.regions.count(targetLoc)) {
                Region& targetRegion = g_world.regions[targetLoc];
                if (faction.diplomacy[targetRegion.factionId] == "war") {
                    isCombatActive = true;
                    if (a.siegeDays == -1) {
                        a.siegeDays = 3 + thread_safe_rand() % 4;
                        addNews("Армия " + faction.name + " взяла в осаду " + targetRegion.name + "!", targetLoc, 4, "war");
                    } else if (a.siegeDays > 0) {
                        a.siegeDays--;
                        
                        // Разрушение мостов при осаде
                        if ((thread_safe_rand() % 100) < 10) {
                            for (auto& road : g_world.map.roads) {
                                if (road.type == "bridge" && (road.from == targetLoc || road.to == targetLoc)) {
                                    road.condition = "ruined";
                                    road.integrity = 0;
                                    g_path_cache_dirty = true;
                                }
                            }
                        }

                        // T3: Foraging during siege (Base supply is already consumed globally)
                        int cityBread = vaultStocks[targetLoc][(int)GoodType::BREAD];
                        if (cityBread > 0) {
                            int forage = std::min((int)(a.size * 0.2), cityBread / 10);
                            consumeItemsFromContainer(targetRegion.vault_id, GoodType::BREAD, forage);
                            vaultStocks[targetLoc][(int)GoodType::BREAD] -= forage;
                            if (!a.supply_chest_id.empty()) {
                                createItem(GoodType::BREAD, forage, a.supply_chest_id, g_world.current_day, "Фуражировка");
                            }
                        }
                        
                        if (armySurvived) {
                            std::string enemyCapital = g_world.factions[targetRegion.factionId].regions.empty() ? "" : g_world.factions[targetRegion.factionId].regions[0];
                            if (targetLoc == enemyCapital) {
                                targetRegion.population = std::max(0, (int)(targetRegion.population * 0.98));
                                int w = vaultStocks[targetLoc][(int)GoodType::WEAPONS];
                                int f1 = vaultStocks[targetLoc][(int)GoodType::BREAD];
                                int wLost = w * 0.05; int f1Lost = f1 * 0.05;
                                consumeItemsFromContainer(targetRegion.vault_id, GoodType::WEAPONS, wLost);
                                consumeItemsFromContainer(targetRegion.vault_id, GoodType::BREAD, f1Lost);
                                vaultStocks[targetLoc][(int)GoodType::WEAPONS] -= wLost;
                                vaultStocks[targetLoc][(int)GoodType::BREAD] -= f1Lost;
                            } else {
                                targetRegion.population = std::max(0, targetRegion.population - (thread_safe_rand() % 200));
                            }
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
                            std::string oldFactionId = targetRegion.factionId;
                            
                            if (targetRegion.isOccupied && targetRegion.factionId == fid) {
                                targetRegion.isOccupied = false;
                                targetRegion.occupierFactionId = "";
                                targetRegion.daysUnderOccupation = 0;
                                addNews("ОСВОБОЖДЕНИЕ: Армия " + faction.name + " освободила " + targetRegion.name + " от оккупации!", targetLoc, 5, "war");
                            } else {
                                targetRegion.isOccupied = true;
                                targetRegion.occupierFactionId = fid;
                                targetRegion.daysUnderOccupation = 0;
                                addNews("ОККУПАЦИЯ: " + targetRegion.name + " оккупирован войсками " + faction.name + "!", targetLoc, 5, "war");
                                
                                if (faction.warType == DiplomaticState::LIMITED_WAR && faction.activeWarGoal.targetRegionId == targetLoc) {
                                    faction.activeWarGoal.achieved = true;
                                    faction.warType = DiplomaticState::PEACE;
                                    faction.warExhaustion = 0;
                                    targetRegion.factionId = fid;
                                    targetRegion.isOccupied = false;
                                    if (g_world.factions.count(oldFactionId)) {
                                        auto& oldRegs = g_world.factions[oldFactionId].regions;
                                        oldRegs.erase(std::remove(oldRegs.begin(), oldRegs.end(), targetLoc), oldRegs.end());
                                    }
                                    faction.regions.push_back(targetLoc);
                                    addNews("ПОБЕДА: " + faction.name + " достигла целей войны, аннексировав " + targetRegion.name + ". Война окончена.", "global", 5, "diplomacy");
                                    for (auto& [otherId, state] : faction.diplomacy) {
                                        if (state == "war") {
                                            faction.diplomacy[otherId] = "neutral";
                                            faction.truceUntil[otherId] = g_world.current_day + 360; // Победитель диктует долгий мир
                                            if (g_world.factions.count(otherId)) {
                                                g_world.factions[otherId].diplomacy[fid] = "neutral";
                                                g_world.factions[otherId].truceUntil[fid] = g_world.current_day + 360;
                                            }
                                        }
                                    }
                                }
                                
                                std::string enemyCapital = g_world.factions[oldFactionId].regions.empty() ? "" : g_world.factions[oldFactionId].regions[0];
                                if (targetLoc == enemyCapital && faction.warType != DiplomaticState::PEACE) {
                                    addNews("ПАДЕНИЕ СТОЛИЦЫ: " + targetRegion.name + " захвачена! Фракция " + g_world.factions[oldFactionId].name + " капитулирует и полностью аннексирована " + faction.name + ".", "global", 5, "war");
                                    for (const auto& rId : g_world.factions[oldFactionId].regions) {
                                        if (g_world.regions.count(rId)) {
                                            g_world.regions[rId].factionId = fid;
                                            g_world.regions[rId].isOccupied = false;
                                            faction.regions.push_back(rId);
                                        }
                                    }
                                    g_world.factions[oldFactionId].regions.clear();
                                    g_world.factions[oldFactionId].warType = DiplomaticState::PEACE;
                                    faction.warType = DiplomaticState::PEACE;
                                    faction.warExhaustion = 0;
                                    for (auto& [otherId, state] : faction.diplomacy) {
                                        if (state == "war") {
                                            faction.diplomacy[otherId] = "neutral";
                                            faction.truceUntil[otherId] = g_world.current_day + 360; // Победитель диктует долгий мир
                                            if (g_world.factions.count(otherId)) {
                                                g_world.factions[otherId].diplomacy[fid] = "neutral";
                                                g_world.factions[otherId].truceUntil[fid] = g_world.current_day + 360;
                                            }
                                        }
                                    }
                                }
                            }
                            
                            targetRegion.moneySupply *= 0.5;
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
                if (faction.armies[i].size < 10) {
                    faction.armies.erase(faction.armies.begin() + i);
                    continue;
                }
                std::string homeRegionId = faction.armies[i].location;
                if (!homeRegionId.empty() && g_world.regions.count(homeRegionId)) {
                    Region& homeRegion = g_world.regions[homeRegionId];
                    
                    int survivors = faction.armies[i].size;
                    if (survivors > 0) {
                        homeRegion.population += survivors;
                        double addedPerYear = (double)survivors / 23.0;
                        for(int p = 18; p <= 40; p++) homeRegion.age_pyramid[p] += addedPerYear;
                    }

                    int returnedWeapons = faction.armies[i].size * 0.8;
                    if (returnedWeapons > 0) {
                        createItem(GoodType::WEAPONS, returnedWeapons, homeRegion.vault_id, g_world.current_day, "Возврат армии");
                        vaultStocks[homeRegionId][(int)GoodType::WEAPONS] += returnedWeapons;
                    }
                }
                faction.armies.erase(faction.armies.begin() + i);
            }
        }
    }
}


std::string getSubContainer(const std::string& parentId, const std::string& type) {
    for (size_t i=0; i<g_containers.data.size(); ++i) {
        if (!g_containers.active[i]) continue;
        const Storage& cont = g_containers.data[i];
        if (cont.type == type && cont.location.has("parent_container") && cont.location["parent_container"].asString() == parentId) {
            return cont.id;
        }
    }
    return "";
}

void processMonthlyDemographics() {
    for (auto& [rid, r] : g_world.regions) {
        if (r.age_pyramid.empty() || r.age_pyramid.size() < 121) {
            r.age_pyramid.assign(121, 0.0);
            double children = r.population * 0.20;
            double workers = r.population * 0.60;
            double elders = r.population * 0.20;
            for(int i=0; i<=17; i++) r.age_pyramid[i] = children / 18.0;
            for(int i=18; i<=65; i++) r.age_pyramid[i] = workers / 48.0;
            for(int i=66; i<=120; i++) r.age_pyramid[i] = elders / 55.0;
        }

        int food = countItemsInContainer(r.vault_id, GoodType::BREAD) +
                   countItemsInContainer(r.vault_id, GoodType::MEAT) +
                   countItemsInContainer(r.vault_id, GoodType::FISH) +
                   countItemsInContainer(r.vault_id, GoodType::SMOKED_MEAT);
        double food_per_capita = food / (double)std::max(1, r.population);

        double famine_mult = 1.0;
        if (food_per_capita < 0.5) famine_mult += (0.5 - food_per_capita) * 2.0;

        auto get_mortality = [](int age) -> double {
            if (age == 0) return 0.02 / 12.0;
            if (age <= 4) return 0.005 / 12.0;
            if (age <= 14) return 0.002 / 12.0;
            if (age <= 49) return 0.003 / 12.0;
            if (age <= 64) return 0.01 / 12.0;
            if (age <= 79) return 0.03 / 12.0;
            return 0.10 / 12.0;
        };

        for (int i = 0; i <= 120; i++) {
            double deaths = r.age_pyramid[i] * get_mortality(i) * famine_mult;
            if (g_bootstrap) deaths = 0.0;
            r.age_pyramid[i] = std::max(0.0, r.age_pyramid[i] - deaths);
        }

        double optimalCapacity = r.storage_capacity * 2.0;
        double birth_rate = 0.001 * std::clamp(1.0 - (r.population / optimalCapacity), 0.2, 1.0) * std::clamp(food_per_capita, 0.4, 1.0);

        double births = r.population * birth_rate;
        r.age_pyramid[0] += births;

        // Рождение NPC-агентов (5% от статистических рождений)
        int npc_births = (int)(births * 0.05);
        if (npc_births > 0) {
            std::vector<std::string> potential_parents;
            for (const auto& [nid, npc] : g_world.npcs) {
                if (npc.homeLocation == rid && npc.age_days >= 18 * 360 && npc.age_days <= 50 * 360) {
                    potential_parents.push_back(nid);
                }
            }
            for (int i = 0; i < npc_births; i++) {
                NPC child;
                child.id = "npc_" + generateUUID();
                child.name = "Ребенок_" + std::to_string(rand() % 1000);
                child.type = "npc";
                child.profession = "none";
                child.homeLocation = rid;
                child.currentLocation = rid;
                child.currentActivity = "Играет";
                child.age_days = 0;
                child.is_male = (rand() % 2 == 0);
                // Наследование расы от матери (или отца, если матери нет)
                if (!child.mother_id.empty() && g_world.npcs.count(child.mother_id)) {
                    child.race = g_world.npcs[child.mother_id].race;
                } else if (!child.father_id.empty() && g_world.npcs.count(child.father_id)) {
                    child.race = g_world.npcs[child.father_id].race;
                }
                child.immunity = 40 + rand() % 60;
                child.hp = 10;
                child.maxHp = 10;
                
                if (potential_parents.size() >= 2) {
                    std::string p1 = potential_parents[rand() % potential_parents.size()];
                    std::string p2 = potential_parents[rand() % potential_parents.size()];
                    if (p1 != p2) {
                        child.mother_id = p1;
                        child.father_id = p2;
                        g_world.npcs[p1].children_ids.push_back(child.id);
                        g_world.npcs[p2].children_ids.push_back(child.id);
                    }
                }
                g_world.npcs[child.id] = child;
            }
        }

        for (int i = 119; i >= 0; i--) {
            double moving = r.age_pyramid[i] / 12.0;
            r.age_pyramid[i] -= moving;
            r.age_pyramid[i+1] += moving;
        }

        double new_pop = 0;
        double new_labor = 0;
        for (int i = 0; i <= 120; i++) {
            new_pop += r.age_pyramid[i];
            if (i >= 18 && i <= 65) new_labor += r.age_pyramid[i];
        }
        r.population = (int)new_pop;
        r.labor_force = (int)new_labor;

        int totalJobs = 0;
        for (const auto& [fId, fac] : r.facilities) totalJobs += fac.level * 100;
        for (const auto& [bId, bus] : g_world.businesses) {
            if (bus.region_id == rid && bus.is_active) totalJobs += bus.employee_count;
        }

        r.unemployment_rate = r.labor_force > 0 ? std::max(0.0, 1.0 - (double)totalJobs / r.labor_force) : 0.0;

        if (r.unemployment_rate < 0.1) r.average_wage = std::min(200, (int)(r.average_wage * 1.1));
        else if (r.unemployment_rate > 0.3) r.average_wage = std::max(30, (int)(r.average_wage * 0.9));

        r.attractivenessIndex = (r.average_wage / 100.0) - (r.unemployment_rate * 3.0) + (food_per_capita * 5.0);
        if (r.migrationCooldown > 0) r.migrationCooldown--;
    }

    // Фаза миграции (Социальный гомеостаз)
    for (auto& [rid, r] : g_world.regions) {
        if (r.migrationCooldown > 0 || r.population <= 100) continue;

        std::string best_target = "";
        double max_diff = 0.0;

        for (const auto& road : g_world.map.roads) {
            if (road.condition == "blocked") continue;
            std::string neighbor_id = "";
            if (road.from == rid) neighbor_id = road.to;
            else if (road.to == rid) neighbor_id = road.from;

            if (!neighbor_id.empty() && g_world.regions.count(neighbor_id)) {
                Region& neighbor = g_world.regions[neighbor_id];
                
                bool atWar = false;
                if (g_world.factions.count(r.factionId) && g_world.factions[r.factionId].diplomacy.count(neighbor.factionId)) {
                    if (g_world.factions[r.factionId].diplomacy.at(neighbor.factionId) == "war") atWar = true;
                }
                if (atWar) continue;

                double diff = neighbor.attractivenessIndex - r.attractivenessIndex;
                if (diff > 2.0 && diff > max_diff) {
                    max_diff = diff;
                    best_target = neighbor_id;
                }
            }
        }

        if (!best_target.empty()) {
            Region& target = g_world.regions[best_target];
            double migration_rate = 0.01 + (r.unemployment_rate * 0.05);
            double migrants = r.population * migration_rate;

            double factor = 1.0 - (migrants / r.population);
            for (int i = 0; i <= 120; i++) {
                double moving = r.age_pyramid[i] * (1.0 - factor);
                r.age_pyramid[i] -= moving;
                target.age_pyramid[i] += moving;
            }
            r.population -= migrants;
            target.population += migrants;

            r.migrationCooldown = 3;
        }
    }
}

void placePrivateBusinessOnMap(Region& r, const Business& b, World& w) {
    std::vector<int> empty_spots;
    for (size_t i = 0; i < r.cityLayout.size(); i++) {
        if (r.cityLayout[i].type == "empty") empty_spots.push_back(i);
    }
    if (!empty_spots.empty()) {
        int idx = empty_spots[rand() % empty_spots.size()];
        r.cityLayout[idx].type = b.facility_type;
        r.cityLayout[idx].name = "Частное: " + b.facility_type;
        r.cityLayout[idx].linked_id = b.id;
        r.cityLayout[idx].sublocation_id = "sub_" + r.id + "_" + b.id;
        
        JsonValue subLoc = JsonValue::object();
        subLoc.set("id", r.cityLayout[idx].sublocation_id);
        subLoc.set("name", r.cityLayout[idx].name);
        subLoc.set("parentId", r.id);
        subLoc.set("type", b.facility_type);
        w.subLocations[r.cityLayout[idx].sublocation_id] = subLoc;
    }
}

void removePrivateBusinessFromMap(Region& r, const std::string& bId, World& w) {
    for (auto& block : r.cityLayout) {
        if (block.linked_id == bId) {
            block.type = "empty";
            block.name = "Заброшенное здание";
            block.linked_id = "";
            w.subLocations.erase(block.sublocation_id);
            block.sublocation_id = "";
            break;
        }
    }
}


void processMonthlyBusinesses() {
    // 1. Создание новых бизнесов агентами (ИИ Анализ рынка + Честный Капитал)
    for (auto& [id, npc] : g_world.npcs) {
        if (!npc.isAlive || npc.age_days < 18 * 360) continue;
        if (!g_world.regions.count(npc.currentLocation)) continue;
        
        Region& r = g_world.regions[npc.currentLocation];
        
        // Базовый расчет: 100 рабочих на уровень
        int monthly_payroll = 100 * r.average_wage;
        
        // Если у NPC нет денег даже на постройку + 2 месяца работы, он не тратит CPU на анализ рынка
        if (npc.economy.savings < 200 + (monthly_payroll * 2)) continue;
        
        if ((rand() % 100) < 10) {
            std::string best_type = "";
            double best_profit = -999999.0;
            
            std::string best_focus = "";
            for (const auto& recipe : RECIPES) {
                double cost = 0;
                for (const auto& in : recipe.inputs) cost += r.markets[goodTypeToString(in.first)] * in.second;
                double rev = 0;
                for (const auto& out : recipe.outputs) rev += r.markets[goodTypeToString(out.first)] * out.second;
                
                double margin = rev - cost;
                if (margin > best_profit) {
                    best_profit = margin;
                    best_type = recipe.facility;
                    best_focus = goodTypeToString(recipe.outputs.begin()->first);
                }
            }
            
            double farm_profit = r.markets["wheat"] * 5;
            if (farm_profit > best_profit) { best_profit = farm_profit; best_type = "farms"; best_focus = "wheat"; }
            double mine_profit = r.markets["iron_ore"] * 3;
            if (mine_profit > best_profit) { best_profit = mine_profit; best_type = "mines"; best_focus = "iron_ore"; }
            double lumber_profit = r.markets["wood"] * 4;
            if (lumber_profit > best_profit) { best_profit = lumber_profit; best_type = "lumbermills"; best_focus = "wood"; }
            
            if (!best_type.empty() && best_profit > 0) {
                int build_cost = 200;
                if (best_type == "farms" || best_type == "lumbermills" || best_type == "mills") build_cost = 500;
                if (best_type == "mines" || best_type == "smelters" || best_type == "banks") build_cost = 1000;
                
                // Строгое условие: Строительство + Оборотный капитал на 2 месяца убытков
                int required_capital = build_cost + (monthly_payroll * 2);
                
                if (npc.economy.savings >= required_capital) {
                    Business b;
                    b.id = "bus_" + generateUUID();
                    b.owner_ids.push_back(id);
                    b.region_id = npc.currentLocation;
                    b.facility_type = best_type;
                    b.level = 1;
                    b.cash_balance = monthly_payroll * 2; // Запас прочности
                    b.reinvestment_pool = 0;
                    b.employee_count = 100; // СТРОГО 100 рабочих
                    b.is_active = true;
                    b.months_loss_streak = 0;
                    b.production_focus = best_focus;
                    b.local_storage_id = createContainer("business_storage", id, 999999, 1000, npc.currentLocation);
                    
                    LogisticRule autoSell;
                    autoSell.id = "log_" + generateUUID();
                    autoSell.type = "transfer";
                    autoSell.resource = stringToGoodType(best_focus);
                    autoSell.target_id = npc.currentLocation; // Region ID -> Vault
                    autoSell.amount = 9999; // All of it
                    autoSell.frequency_days = 7;
                    b.logistics.push_back(autoSell);
                    
                    npc.economy.savings -= required_capital;
                    npc.owned_businesses.push_back(b.id);
                    g_world.businesses[b.id] = b;
                    
                    placePrivateBusinessOnMap(r, b, g_world);
                    addNews("Успешный купец " + npc.name + " вложил капитал в новое предприятие: " + b.facility_type + ".", b.region_id, 1, "business");
                }
            }
        }
    }

    // 2. Операционный цикл бизнесов
    std::vector<std::string> bankruptcies;
    for (auto& [bId, bus] : g_world.businesses) {
        if (!bus.is_active) continue;
        if (!g_world.regions.count(bus.region_id)) continue;
        
        Region& r = g_world.regions[bus.region_id];
        
        // Наем работников
        if (std::find(bus.owner_ids.begin(), bus.owner_ids.end(), "player") != bus.owner_ids.end()) {
            bus.employee_count = std::min(bus.target_employee_count, bus.level * 100);
        } else {
            bus.employee_count = bus.level * 100;
        }
        int wage_cost = bus.employee_count * r.average_wage;
        
        double market_mod = 1.0 + ((rand() % 50) - 15) / 100.0;
        if (r.threat_level > 50) market_mod -= 0.2;
        
        int revenue = (int)(wage_cost * market_mod);
        int profit = revenue - wage_cost;
        
        if (profit < 0) {
            bus.cash_balance += profit; 
            if (bus.cash_balance < 0) {
                bus.months_loss_streak++;
                if (bus.months_loss_streak >= 6) {
                    bus.is_active = false;
                    bankruptcies.push_back(bId);
                    removePrivateBusinessFromMap(r, bId, g_world);
                    addNews("Крупное предприятие (" + bus.facility_type + ") обанкротилось. " + std::to_string(bus.employee_count) + " рабочих оказались на улице.", bus.region_id, 2, "business");
                    bus.addLog(g_world.current_day, "❌ БАНКРОТСТВО: Предприятие закрыто из-за долгов.");
                }
            }
        } else {
            bus.months_loss_streak = 0;
            
            int dividend = profit * 0.50;
            int to_cash = profit * 0.30;
            int to_reinvest = profit - dividend - to_cash;
            
            bus.cash_balance += to_cash;
            bus.reinvestment_pool += to_reinvest;
            
            if (dividend > 0 && !bus.owner_ids.empty()) {
                int per_owner = dividend / bus.owner_ids.size();
                for (const auto& oId : bus.owner_ids) {
                    if (g_world.npcs.count(oId)) g_world.npcs[oId].economy.savings += per_owner;
                }
            }
            
            // Реинвестиции и расширение: цена апгрейда также зависит от уровня
            if (bus.reinvestment_pool > 2000 * bus.level) {
                bus.reinvestment_pool -= 2000 * bus.level;
                bus.level++;
                addNews("Предприятие (" + bus.facility_type + ") процветает и расширяется до уровня " + std::to_string(bus.level) + "! Наняты новые рабочие.", bus.region_id, 1, "business");
                bus.addLog(g_world.current_day, "⬆️ УЛУЧШЕНИЕ: Уровень повышен до " + std::to_string(bus.level) + "!");
            }
        }
        
        // АНАЛИЗ ГЛОБАЛЬНОГО РЫНКА (ЧАСТНЫЙ БИЗНЕС)
        // Агентам плевать на войны фракций, они ищут максимальную выгоду
        if (bus.is_active && !bus.production_focus.empty()) {
            std::string bestRegion = bus.region_id;
            double bestPrice = 0;
            if (g_world.regions.count(bus.region_id)) {
                bestPrice = g_world.regions[bus.region_id].markets[bus.production_focus];
            }
            
            for (const auto& [rid, r] : g_world.regions) {
                double price = r.markets.count(bus.production_focus) ? r.markets.at(bus.production_focus) : 0;
                // Учитываем риски и логистику (цена должна быть выше на 30%)
                if (price > bestPrice * 1.3) {
                    bestPrice = price;
                    bestRegion = rid;
                }
            }
            
            // Если найдена более выгодная точка сбыта, перенаправляем логистику
            if (!bus.logistics.empty() && bus.logistics[0].target_id != bestRegion) {
                bus.logistics[0].target_id = bestRegion;
                addNews("РЫНОК: Владелец предприятия (" + bus.facility_type + ") из " + g_world.regions[bus.region_id].name + " перенаправил поставки в " + g_world.regions[bestRegion].name + " из-за высоких цен, игнорируя политику.", bus.region_id, 1, "market");
            }
        }
    }
    
    for (const auto& bId : bankruptcies) {
        g_world.businesses.erase(bId);
    }
}


void processTaxation() {
    // Ежемесячный земельный налог (сбор урожая в казну)
    for (auto& [id, npc] : g_world.npcs) {
        if (!npc.isAlive || npc.economy.profession_type != "farmer") continue;
        if (!g_world.regions.count(npc.currentLocation)) continue;
        
        Region& r = g_world.regions[npc.currentLocation];
        std::string contId = npc.economy.storage_id.empty() ? npc.inventory_id : npc.economy.storage_id;
        if (contId.empty() || r.vault_id.empty()) continue;

        if (r.isOccupied && r.daysUnderOccupation < 30) continue; // T3: 7.2 No taxes during early occupation

        // Лорд забирает 25% пшеницы и мяса
        std::vector<GoodType> taxGoods = {GoodType::WHEAT, GoodType::MEAT};
        for (GoodType gt : taxGoods) {
            int stock = countItemsInContainer(contId, gt);
            int tax = stock * 0.25;
            if (tax > 0) {
                consumeItemsFromContainer(contId, gt, tax);
                createItem(gt, tax, r.vault_id, g_world.current_day, "Земельный налог");
            }
        }
    }

    // Ежегодный подушный налог (золотом)
    if (g_world.current_day > 0 && g_world.current_day % 360 == 0) {
        for (auto& [rid, r] : g_world.regions) {
            if (r.factionId.empty() || !g_world.factions.count(r.factionId)) continue;
            if (r.isOccupied && r.daysUnderOccupation < 30) continue; // T3: 7.2 No taxes during early occupation
            Faction& f = g_world.factions[r.factionId];
            std::string capId = f.regions.empty() ? "" : f.regions[0];
            if (capId.empty() || !g_world.regions.count(capId)) continue;
            
            Region& capital = g_world.regions[capId];
            
            // Налог: 1 золото с каждого трудоспособного
            int taxAmount = r.labor_force * 1;
            if (r.moneySupply >= taxAmount) {
                r.moneySupply -= taxAmount;
                createItem(GoodType::GOLD_INGOT, taxAmount, capital.vault_id, g_world.current_day, "Подушный налог");
            } else {
                // Недоимка вызывает рост угрозы (недовольство)
                r.threat_level = std::min(100, r.threat_level + 10);
            }
        }
    }
}

void processInfrastructureProjects() {
    for (auto& [fid, f] : g_world.factions) {
        std::string capId = f.regions.empty() ? "" : f.regions[0];
        if (capId.empty() || !g_world.regions.count(capId)) continue;
        
        int gold = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT);
        int wood = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::WOOD);
        int iron = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::IRON_INGOT);
        
        for (const auto& rid : f.regions) {
            if (!g_world.regions.count(rid)) continue;
            Region& r = g_world.regions[rid];
            
            // 1. Дамбы (Dams) - защита от наводнений и осушение пойм
            if (gold >= 5000 && wood >= 1000 && !r.custom_props.has("has_dam")) {
                bool has_river = false;
                if (g_world.map.locations.count(rid)) {
                    auto loc = g_world.map.locations[rid];
                    for (int dy = -3; dy <= 3; dy++) {
                        for (int dx = -3; dx <= 3; dx++) {
                            int nx = loc.x + dx, ny = loc.y + dy;
                            if (nx >= 0 && nx < g_world.map.width && ny >= 0 && ny < g_world.map.height) {
                                if (g_world.map.grid[ny * g_world.map.width + nx].type == TileType::RIVER) has_river = true;
                            }
                        }
                    }
                    if (has_river) {
                        consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, 5000);
                        consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::WOOD, 1000);
                        gold -= 5000; wood -= 1000;
                        r.custom_props.set("has_dam", true);
                        
                        for (int dy = -3; dy <= 3; dy++) {
                            for (int dx = -3; dx <= 3; dx++) {
                                int nx = loc.x + dx, ny = loc.y + dy;
                                if (nx >= 0 && nx < g_world.map.width && ny >= 0 && ny < g_world.map.height) {
                                    int idx = ny * g_world.map.width + nx;
                                    if (g_world.map.grid[idx].type == TileType::FLOODPLAIN) {
                                        g_world.map.grid[idx].type = TileType::PLAINS;
                                    }
                                }
                            }
                        }
                        g_world.map.generation_tick = g_world.tick;
                        addNews("ИНФРАСТРУКТУРА: Фракция " + f.name + " построила Дамбу в " + r.name + ", укротив реку и осушив поймы под пашни.", rid, 3, "politics");
                    }
                }
            }
            
            // 2. Акведуки (Aqueducts) - повышение фертильности
            if (gold >= 8000 && iron >= 500 && !r.custom_props.has("has_aqueduct")) {
                consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, 8000);
                consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::IRON_INGOT, 500);
                gold -= 8000; iron -= 500;
                r.custom_props.set("has_aqueduct", true);
                r.fertility += 0.5;
                addNews("ИНФРАСТРУКТУРА: В " + r.name + " возведен величественный Акведук. Урожайность земель значительно возросла.", rid, 3, "politics");
            }
            
            // 2.5 Колодцы (Санитария) - предотвращение эпидемий
            if (gold >= 2000 && r.population > r.storage_capacity && !r.custom_props.has("has_well")) {
                consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, 2000);
                gold -= 2000;
                r.custom_props.set("has_well", true);
                addNews("САНИТАРИЯ: В " + r.name + " построена сеть каменных колодцев, предотвращающая эпидемии.", rid, 2, "politics");
            }
        }
        
        // 3. Строительство новых мостов и трактов к изолированным регионам
        for (const auto& rid : f.regions) {
            if (gold >= 10000 && wood >= 2000) {
                if (g_world.map.locations.count(rid) && g_world.map.locations[rid].no_road) {
                    auto loc1 = g_world.map.locations[rid];
                    auto loc2 = g_world.map.locations[capId];
                    std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
                    std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
                    for (const auto& road : g_world.map.roads) {
                        for (const auto& wp : road.waypoints) has_road[wp.second * g_world.map.width + wp.first] = true;
                    }
                    auto path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, has_road, path_status, MovementType::ANY);
                    if (!path.empty()) {
                        consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, 10000);
                        consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::WOOD, 2000);
                        gold -= 10000; wood -= 2000;
                        g_world.map.locations[rid].no_road = false;
                        
                        MapRoad newRoad;
                        newRoad.from = rid; newRoad.to = capId; newRoad.condition = "paved"; newRoad.type = "paved";
                        newRoad.waypoints = path;
                        
                        for (auto wp : path) {
                            int idx = wp.second * g_world.map.width + wp.first;
                            TileType t = g_world.map.grid[idx].type;
                            if (t == TileType::RIVER || t == TileType::LAKE || t == TileType::OCEAN || t == TileType::SHALLOW_WATER) {
                                g_world.map.grid[idx].bridge_flag = 1;
                                newRoad.type = "bridge";
                            }
                        }
                        g_world.map.roads.push_back(newRoad);
                        g_path_cache_dirty = true;
                        g_world.map.generation_tick = g_world.tick;
                        addNews("ИНФРАСТРУКТУРА: Инженеры " + f.name + " проложили грандиозный тракт с мостами к изолированному региону " + g_world.regions[rid].name + ".", rid, 4, "politics");
                    }
                }
            }
        }
    }
}


void monthlyTick() {
    processMonthlyDemographics();
    processMonthlyBusinesses();
    processTaxation();
    processInfrastructureProjects();
}


void dailyTick();
void weeklyTick();
void processFactionTrade();
void processDisasters();
void processNavalCombat();




// Forward declarations
// void simulateOneDay(); (replaced by dailyTick)

// Simulate one hour
void processFleets() {
    for (auto& [rid, port] : g_world.port_facilities) port.is_blockaded = false;

    for (auto it = g_world.fleets.begin(); it != g_world.fleets.end(); ) {
        Fleet& f = *it;
        f.ship_ids.erase(std::remove_if(f.ship_ids.begin(), f.ship_ids.end(), [](const std::string& sid) {
            bool found = false;
            for (const auto& s : g_world.ships) if (s.id == sid && s.hull > 0) found = true;
            return !found;
        }), f.ship_ids.end());

        if (f.ship_ids.empty()) {
            it = g_world.fleets.erase(it);
            continue;
        }

        if (!f.path.empty() && f.path_index < (int)f.path.size() - 1) {
            double speed = 1.2;
            std::string current_region = "";
            for (const auto& [rid, loc] : g_world.map.locations) {
                if (std::abs(loc.x - f.x) <= 2 && std::abs(loc.y - f.y) <= 2) { current_region = rid; break; }
            }
            if (!current_region.empty() && g_world.regions.count(current_region)) {
                std::string w = g_world.regions[current_region].weather;
                if (w == "Эфирный шторм" || w == "Метель") speed *= 0.2;
                else if (w == "Дождь" || w == "Тропический ливень") speed *= 0.7;
                else if (w == "Туман") speed *= 0.5;
            }

            while (speed > 0 && f.path_index < (int)f.path.size() - 1) {
                double target_x = f.path[f.path_index + 1].first;
                double target_y = f.path[f.path_index + 1].second;
                double dx = target_x - f.x;
                double dy = target_y - f.y;
                double dist = std::hypot(dx, dy);
                
                if (dist <= speed) {
                    f.x = target_x; f.y = target_y;
                    speed -= dist; f.path_index++;
                } else {
                    f.x += (dx / dist) * speed; f.y += (dy / dist) * speed;
                    speed = 0;
                }
            }
        }

        for (auto& s : g_world.ships) {
            if (s.fleet_id == f.id) { s.x = f.x; s.y = f.y; }
        }

        if (f.path_index >= (int)f.path.size() - 1 && f.mission == "blockade") {
            if (g_world.port_facilities.count(f.destination)) {
                g_world.port_facilities[f.destination].is_blockaded = true;
            }
        }
        ++it;
    }
}


void processShips() {
    for (auto& ship : g_world.ships) {

        if (!ship.fleet_id.empty()) continue;

        if (ship.type == ShipType::SEA_MONSTER) {
            double min_dist = 9999;
            Ship* target = nullptr;
            for (auto& s : g_world.ships) {
                if (s.id != ship.id && s.type != ShipType::SEA_MONSTER && s.hull > 0) {
                    double d = std::hypot(s.x - ship.x, s.y - ship.y);
                    if (d < min_dist) { min_dist = d; target = &s; }
                }
            }
            if (target) {
                if (min_dist <= 1.5) {
                    if (target->cannons > 0 || target->marines > 0) {
                        ship.hull -= (target->cannons * 10 + target->marines * 5);
                        target->hull -= 30;
                        addNews("БИТВА С ЧУДОВИЩЕМ: Военный корабль сражается с морским левиафаном!", "global", 4, "war");
                        if (ship.hull <= 0) addNews("ПОБЕДА: Морское чудовище убито военным флотом!", "global", 5, "war");
                    } else {
                        target->hull -= 50;
                        addNews("КАТАСТРОФА: Морское чудовище атакует торговое судно!", "global", 4, "disaster");
                    }
                } else {
                    double dx = target->x - ship.x;
                    double dy = target->y - ship.y;
                    ship.x += (dx / min_dist) * ship.speed;
                    ship.y += (dy / min_dist) * ship.speed;
                }
            }
            continue;
        }
        if (ship.path.empty() || ship.path_index >= (int)ship.path.size() - 1) continue;
        
        double speed = ship.speed;
        if (g_world.regions.count(ship.destination)) {
            std::string w = g_world.regions[ship.destination].weather;
            if (w == "Эфирный шторм" || w == "Метель") speed *= 0.2;
            else if (w == "Дождь" || w == "Туман" || w == "Тропический ливень") speed *= 0.7;
        }
        
        while (speed > 0 && ship.path_index < (int)ship.path.size() - 1) {
            double target_x = ship.path[ship.path_index + 1].first;
            double target_y = ship.path[ship.path_index + 1].second;
            double dx = target_x - ship.x;
            double dy = target_y - ship.y;
            double dist = std::hypot(dx, dy);
            
            if (dist <= speed) {
                ship.x = target_x;
                ship.y = target_y;
                speed -= dist;
                ship.path_index++;
            } else {
                ship.x += (dx / dist) * speed;
                ship.y += (dy / dist) * speed;
                speed = 0;
            }
        }
        
        if (ship.path_index >= (int)ship.path.size() - 1) {
            std::string dest = ship.destination;
            if (g_world.regions.count(dest) && g_world.port_facilities.count(dest)) {
                if (g_world.port_facilities[dest].is_blockaded) {
                    addNews("БЛОКАДА: Торговое судно не смогло войти в заблокированный порт " + g_world.regions[dest].name + " и сбросило груз в море.", dest, 3, "trade");
                    ship.path.clear();
                    ship.path_index = 0;
                    continue;
                }
                Region& destReg = g_world.regions[dest];
                if (ship.type == ShipType::MERCHANT && g_containers.count(ship.chest_id)) {
                    Storage& chest = g_containers[ship.chest_id];
                    double totalRevenue = 0;
                    std::map<GoodType, int> deliveredGoods;
                    std::vector<std::string> items_to_move = chest.item_ids;
                    
                    for (const auto& itemId : items_to_move) {
                        if (!g_items.count(itemId)) continue;
                        PhysicalItem& item = g_items[itemId];
                        deliveredGoods[item.prototype_id] += item.stack_size;
                        
                        double price = destReg.markets[goodTypeToString(item.prototype_id)];
                        if (price == 0) price = BASE_PRICES[(int)item.prototype_id];
                        
                        double itemRev = item.stack_size * price;
                        if (destReg.moneySupply >= itemRev) {
                            totalRevenue += itemRev;
                            destReg.moneySupply -= itemRev;
                        } else {
                            totalRevenue += destReg.moneySupply;
                            destReg.moneySupply = 0;
                        }
                        moveItem(itemId, destReg.vault_id);
                    }
                    
                    int portFee = totalRevenue * 0.15;
                    destReg.moneySupply += portFee;
                    int netRevenue = totalRevenue - portFee;
                    
                    if (g_world.factions.count(ship.owner_id)) {
                        std::string capId = g_world.factions[ship.owner_id].regions.empty() ? "" : g_world.factions[ship.owner_id].regions[0];
                        if (!capId.empty() && g_world.regions.count(capId)) {
                            createItem(GoodType::GOLD_INGOT, netRevenue, g_world.regions[capId].vault_id, g_world.current_day, "Морская торговля");
                        }
                    } else if (g_world.npcs.count(ship.owner_id)) {
                        g_world.npcs[ship.owner_id].economy.savings += netRevenue;
                    }
                    
                    std::string goodsList;
                    for (const auto& [gt, amount] : deliveredGoods) {
                        if (!goodsList.empty()) goodsList += ", ";
                        goodsList += std::to_string(amount) + " " + getGoodName(gt);
                    }
                    if (goodsList.empty()) goodsList = "ничего";
                    
                    addNews("МОРСКАЯ ТОРГОВЛЯ: Судно прибыло в порт " + destReg.name + "! Доставлено: " + goodsList + ". Выручка: " + std::to_string((int)totalRevenue) + " з. (Пошлина: " + std::to_string(portFee) + " з.)", dest, 2, "trade");
                }
            }
            ship.path.clear();
            ship.path_index = 0;
        }
    }
}

void processNavalTrade() {
    std::vector<bool> dummy_has_road(g_world.map.width * g_world.map.height, false);
    std::vector<int> dummy_path_status(g_world.map.width * g_world.map.height, 0);


    for (auto& [rid, port] : g_world.port_facilities) {
        if (port.is_blockaded || !g_world.regions.count(rid)) continue;
        Region& reg = g_world.regions[rid];
        std::string factionId = reg.factionId;
        for (int i=0; i<(int)GoodType::COUNT; i++) {
            GoodType gt = (GoodType)i;
            if (GOOD_CATEGORIES[i] == GoodCategory::DOCUMENT) continue;
            std::string gtStr = goodTypeToString(gt);
            double price = reg.markets.count(gtStr) ? reg.markets.at(gtStr) : BASE_PRICES[i];
            if (price < BASE_PRICES[i] * 0.8) {
                int avail = countItemsInContainer(reg.vault_id, gt);
                if (avail > 100 && reg.moneySupply > price * 50) {
                    int buyAmount = 50;
                    reg.moneySupply -= buyAmount * price;
                    consumeItemsFromContainer(reg.vault_id, gt, buyAmount);
                    createItem(gt, buyAmount, port.dock_container_id, g_world.current_day, "Авто-закупка порта");
                }
            }
        }
        for (auto& ship : g_world.ships) {
            if (ship.type == ShipType::MERCHANT && ship.owner_id == factionId && ship.path.empty()) {
                if (g_world.map.locations.count(rid) && std::abs(ship.x - g_world.map.locations[rid].x) <= 1 && std::abs(ship.y - g_world.map.locations[rid].y) <= 1) {
                    std::string bestDest = "";
                    GoodType bestGood = GoodType::COUNT;
                    double maxProfit = 0;
                    for (int i=0; i<(int)GoodType::COUNT; i++) {
                        GoodType gt = (GoodType)i;
                        int inDock = countItemsInContainer(port.dock_container_id, gt);
                        if (inDock < 50) continue;
                        std::string gtStr = goodTypeToString(gt);
                        double localP = reg.markets.count(gtStr) ? reg.markets.at(gtStr) : BASE_PRICES[i];
                        for (const auto& [destId, destPort] : g_world.port_facilities) {
                            if (destId == rid || destPort.is_blockaded || !g_world.regions.count(destId)) continue;
                            double destP = g_world.regions[destId].markets.count(gtStr) ? g_world.regions[destId].markets.at(gtStr) : BASE_PRICES[i];
                            if (destP - localP > maxProfit) {
                                maxProfit = destP - localP;
                                bestDest = destId;
                                bestGood = gt;
                            }
                        }
                    }
                    if (!bestDest.empty() && bestGood != GoodType::COUNT) {
                        int loadAmount = std::min(countItemsInContainer(port.dock_container_id, bestGood), ship.cargo_capacity);
                        consumeItemsFromContainer(port.dock_container_id, bestGood, loadAmount);
                        createItem(bestGood, loadAmount, ship.chest_id, g_world.current_day, "Государственный экспорт");
                        auto loc1 = g_world.map.locations[rid];
                        auto loc2 = g_world.map.locations[bestDest];
                        auto path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::WATER, 10);
                        if (!path.empty()) {
                            ship.path = path;
                            ship.path_index = 0;
                            ship.destination = bestDest;
                            addNews("АВТО-ТОРГОВЛЯ: Порт " + reg.name + " отправил государственный торговый корабль в " + g_world.regions[bestDest].name + ".", rid, 2, "trade");
                        }
                    }
                }
            }
        }
    }
    
    for (auto& [npcId, merchant] : g_world.npcs) {
        if (!merchant.isAlive || (merchant.economy.profession_type != "merchant" && merchant.profession != "Торговец")) continue;
        std::string mLoc = merchant.currentLocation;
        if (g_world.port_facilities.count(mLoc) && g_world.port_facilities[mLoc].has_shipyard) {
            bool hasShip = false;
            for (const auto& s : g_world.ships) if (s.owner_id == npcId) { hasShip = true; break; }
            for (const auto& bq : g_world.port_facilities[mLoc].build_queue) if (bq.owner_id == npcId) { hasShip = true; break; }
            
            if (!hasShip && merchant.economy.savings >= 1500) {
                merchant.economy.savings -= 1500;
                ShipBuildOrder order;
                order.id = "build_" + generateUUID();
                order.type = ShipType::MERCHANT;
                order.days_left = 14;
                order.owner_id = npcId;
                g_world.port_facilities[mLoc].build_queue.push_back(order);
                addNews("ВЕРФЬ: Купец " + merchant.name + " заказал строительство торгового судна в " + g_world.regions[mLoc].name + ".", mLoc, 1, "trade");
            }
        }
    }

    for (auto& ship : g_world.ships) {

        if (ship.type == ShipType::SEA_MONSTER) {
            double min_dist = 9999;
            Ship* target = nullptr;
            for (auto& s : g_world.ships) {
                if (s.id != ship.id && s.type != ShipType::SEA_MONSTER && s.hull > 0) {
                    double d = std::hypot(s.x - ship.x, s.y - ship.y);
                    if (d < min_dist) { min_dist = d; target = &s; }
                }
            }
            if (target) {
                if (min_dist <= 1.5) {
                    if (target->cannons > 0 || target->marines > 0) {
                        ship.hull -= (target->cannons * 10 + target->marines * 5);
                        target->hull -= 30;
                        addNews("БИТВА С ЧУДОВИЩЕМ: Военный корабль сражается с морским левиафаном!", "global", 4, "war");
                        if (ship.hull <= 0) addNews("ПОБЕДА: Морское чудовище убито военным флотом!", "global", 5, "war");
                    } else {
                        target->hull -= 50;
                        addNews("КАТАСТРОФА: Морское чудовище атакует торговое судно!", "global", 4, "disaster");
                    }
                } else {
                    double dx = target->x - ship.x;
                    double dy = target->y - ship.y;
                    ship.x += (dx / min_dist) * ship.speed;
                    ship.y += (dy / min_dist) * ship.speed;
                }
            }
            continue;
        }
        if (ship.type != ShipType::MERCHANT || !ship.path.empty()) continue;
        
        std::string current_port = "";
        for (const auto& [rid, port] : g_world.port_facilities) {
            if (g_world.map.locations.count(rid)) {
                auto loc = g_world.map.locations[rid];
                if (std::abs(loc.x - ship.x) <= 1 && std::abs(loc.y - ship.y) <= 1) {
                    current_port = rid;
                    break;
                }
            }
        }
        
        if (current_port.empty() || !g_world.regions.count(current_port)) continue;
        if (g_world.port_facilities[current_port].is_blockaded) continue;
        Region& localReg = g_world.regions[current_port];
        
        std::string bestDest = "";
        GoodType bestGood = GoodType::COUNT;
        double maxProfit = 0;
        int buyPrice = 0;
        
        for (int i=0; i<(int)GoodType::COUNT; i++) {
            GoodType gt = (GoodType)i;
            if (GOOD_CATEGORIES[i] == GoodCategory::DOCUMENT) continue;
            std::string gtStr = goodTypeToString(gt);
            
            int available = countItemsInContainer(localReg.vault_id, gt);
            if (available < 50) continue;
            
            double localP = localReg.markets.count(gtStr) ? localReg.markets.at(gtStr) : BASE_PRICES[i];
            if (localP <= 0) localP = BASE_PRICES[i];
            
            for (const auto& [destId, destPort] : g_world.port_facilities) {
                if (destId == current_port) continue;
                if (destPort.is_blockaded) continue;
                if (!g_world.regions.count(destId)) continue;
                
                Region& destReg = g_world.regions[destId];
                double destP = destReg.markets.count(gtStr) ? destReg.markets.at(gtStr) : BASE_PRICES[i];
                double profitMargin = destP - localP;
                
                if (profitMargin > maxProfit && profitMargin > localP * 0.2) {
                    maxProfit = profitMargin;
                    bestDest = destId;
                    bestGood = gt;
                    buyPrice = localP;
                }
            }
        }
        
        if (!bestDest.empty() && bestGood != GoodType::COUNT) {
            int amountToBuy = std::min(countItemsInContainer(localReg.vault_id, bestGood), ship.cargo_capacity);
            bool canLoad = false;
            
            if (g_world.factions.count(ship.owner_id)) {
                if (localReg.factionId == ship.owner_id) {
                    canLoad = true;
                } else {
                    std::string capId = g_world.factions[ship.owner_id].regions.empty() ? "" : g_world.factions[ship.owner_id].regions[0];
                    if (!capId.empty() && g_world.regions.count(capId)) {
                        int gold = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT);
                        int cost = amountToBuy * buyPrice;
                        if (gold >= cost) {
                            consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, cost);
                            localReg.moneySupply += cost;
                            canLoad = true;
                        }
                    }
                }
            } else if (g_world.npcs.count(ship.owner_id)) {
                int cost = amountToBuy * buyPrice;
                if (g_world.npcs[ship.owner_id].economy.savings >= cost) {
                    g_world.npcs[ship.owner_id].economy.savings -= cost;
                    localReg.moneySupply += cost;
                    canLoad = true;
                }
            }
            
            if (canLoad) {
                consumeItemsFromContainer(localReg.vault_id, bestGood, amountToBuy);
                createItem(bestGood, amountToBuy, ship.chest_id, g_world.current_day, "Погрузка на корабль");
                
                int maxGuardsWanted = std::min(10, (int)(ship.cargo_capacity / 50));
                if (g_world.npcs.count(ship.owner_id)) {
                    NPC& merchant = g_world.npcs[ship.owner_id];
                    if (merchant.economy.savings >= maxGuardsWanted * 30) {
                        merchant.economy.savings -= maxGuardsWanted * 30;
                        ship.marines = maxGuardsWanted;
                    }
                } else if (g_world.factions.count(ship.owner_id)) {
                    ship.marines = maxGuardsWanted;
                }
                
                auto loc1 = g_world.map.locations[current_port];
                auto loc2 = g_world.map.locations[bestDest];
                
                auto path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::WATER, 10);
                if (!path.empty()) {
                    ship.path = path;
                    ship.path_index = 0;
                    ship.destination = bestDest;
                    addNews("ФЛОТ: Торговое судно загружено (" + std::to_string(amountToBuy) + " " + getGoodName(bestGood) + ") и отбыло из " + localReg.name + " в " + g_world.regions[bestDest].name + ".", current_port, 1, "logistics");
                } else {
                    consumeItemsFromContainer(ship.chest_id, bestGood, amountToBuy);
                    createItem(bestGood, amountToBuy, localReg.vault_id, g_world.current_day, "Разгрузка (нет пути)");
                }
            }
        }
    }
}


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
    std::string oldOwner = "";
                if (item.prototype_id == GoodType::DOCUMENT_ORDER && item.order_data.has_value()) {
                    OrderData& od = item.order_data.value();
                    if (od.status == "in_progress") {
                        int has_qty = countItemsInContainer(merchant.inventory_id, od.item_prototype);
                        if (has_qty >= od.quantity) {
                            if (merchant.currentLocation == od.issuer_id) {
                                if (g_world.regions.count(od.issuer_id)) {
                                    Region& targetReg = g_world.regions[od.issuer_id];
                                    consumeItemsFromContainer(merchant.inventory_id, od.item_prototype, od.quantity);
                                    std::string delivery_cont = od.target_container_id.empty() ? targetReg.vault_id : od.target_container_id;
                                    createItem(od.item_prototype, od.quantity, delivery_cont, g_world.current_day, "Доставка по заказу");
                                    int payment = od.quantity * od.max_price_per_unit;
                                    createItem(GoodType::GOLD_INGOT, payment, safeId, g_world.current_day, "Оплата заказа");
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
                                    createItem(od.item_prototype, needed, merchant.inventory_id, g_world.current_day, "Закупка для заказа");
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


void processTrekTick() {
    if (!g_world.player_trek.active || g_world.player_trek.paused) return;

    g_world.player_trek.elapsed_hours++;
    bool event_triggered = false;

    // 0. Движение игрока по узлам пути (A*)
    double speed = 0.5; // Базовая скорость игрока (тайлов в час)
    
    std::string current_region = g_world.player_trek.destination_id;
    double min_dist = 9999.0;
    // Предварительный поиск ближайшего региона для определения погодных штрафов
    for (const auto& [rid, loc] : g_world.map.locations) {
        double d = std::hypot(loc.x - g_world.player_trek.current_x, loc.y - g_world.player_trek.current_y);
        if (d < min_dist) {
            min_dist = d;
            current_region = rid;
        }
    }
    
    if (g_world.regions.count(current_region)) {
        std::string w = g_world.regions[current_region].weather;
        if (w == "Эфирный шторм" || w == "Метель") speed *= 0.2;
        else if (w == "Дождь" || w == "Тропический ливень" || w == "Снегопад") speed *= 0.7;
    }

    if (!g_world.player_trek.path.empty() && g_world.player_trek.path_index < (int)g_world.player_trek.path.size() - 1) {
        while (speed > 0 && g_world.player_trek.path_index < (int)g_world.player_trek.path.size() - 1) {
            double target_x = g_world.player_trek.path[g_world.player_trek.path_index + 1].first;
            double target_y = g_world.player_trek.path[g_world.player_trek.path_index + 1].second;
            
            double dx = target_x - g_world.player_trek.current_x;
            double dy = target_y - g_world.player_trek.current_y;
            double dist = std::hypot(dx, dy);
            
            if (dist <= speed) {
                g_world.player_trek.current_x = target_x;
                g_world.player_trek.current_y = target_y;
                speed -= dist;
                g_world.player_trek.path_index++;
            } else {
                g_world.player_trek.current_x += (dx / dist) * speed;
                g_world.player_trek.current_y += (dy / dist) * speed;
                speed = 0;
            }
        }
    }

    double cx = g_world.player_trek.current_x;
    double cy = g_world.player_trek.current_y;

    // 1. Финальное уточнение текущего региона после движения
    min_dist = 9999.0;
    for (const auto& [rid, loc] : g_world.map.locations) {
        double d = std::hypot(loc.x - cx, loc.y - cy);
        if (d < min_dist) {
            min_dist = d;
            current_region = rid;
        }
    }

    // 2. События на основе тайлов (Реки, Руины)
    int icx = (int)cx;
    int icy = (int)cy;
    if (icx >= 0 && icx < g_world.map.width && icy >= 0 && icy < g_world.map.height) {
        int idx = icy * g_world.map.width + icx;
        TileType t = g_world.map.grid[idx].type;

        // Т3 ФИКС: Если на тайле есть дорога (road_level > 0), мост считается существующим и событие не триггерится
        if (t == TileType::RIVER && g_world.map.grid[idx].bridge_flag == 0 && g_world.map.grid[idx].road_level == 0) {
            std::string rKey = "river_" + std::to_string(icx) + "_" + std::to_string(icy);
            if (g_world.player_trek.seen_object_ids.count(rKey) == 0) {
                g_world.player_trek.seen_object_ids.insert(rKey);
                TrekEvent ev;
                ev.id = "evt_" + generateUUID();
                ev.object_type = "river_crossing";
                ev.description = "Путь преграждает бурная река. Моста не видно, вода выглядит опасной. Придется искать брод или строить плот.";
                g_world.player_trek.pending_events.push_back(ev);
                event_triggered = true;
            }
        }
        
        if (t == TileType::RUINS && (thread_safe_rand() % 100) < 5) {
            std::string rKey = "ruin_flavor_" + std::to_string(icx) + "_" + std::to_string(icy);
            if (g_world.player_trek.seen_object_ids.count(rKey) == 0) {
                g_world.player_trek.seen_object_ids.insert(rKey);
                TrekEvent ev;
                ev.id = "evt_" + generateUUID();
                ev.object_type = "ruin_discovery";
                ev.description = "Среди серых пустошей вы натыкаетесь на остов древнего строения. Ветер воет в пустых окнах. Возможно, внутри есть что-то ценное.";
                g_world.player_trek.pending_events.push_back(ev);
                event_triggered = true;
            }
        }
    }

    // 3. Динамические сущности (Караваны и Армии)
    for (const auto& [rid, r] : g_world.regions) {
        for (const auto& c : r.caravans) {
            if (std::hypot(c.x - cx, c.y - cy) <= 2.0) {
                if (g_world.player_trek.seen_object_ids.count(c.id) == 0) {
                    g_world.player_trek.seen_object_ids.insert(c.id);
                    TrekEvent ev;
                    ev.id = "evt_" + generateUUID();
                    ev.object_type = "caravan";
                    ev.sim_object_id = c.id;
                    std::string destName = g_world.regions.count(c.destination) ? g_world.regions[c.destination].name : c.destination;
                    ev.description = "На горизонте показался торговый караван (" + std::to_string(c.wagons) + " повозок), направляющийся в " + destName + ". Их охраняет " + std::to_string(c.guards) + " наемников.";
                    g_world.player_trek.pending_events.push_back(ev);
                    event_triggered = true;
                }
            }
        }
    }

    for (const auto& [fid, f] : g_world.factions) {
        for (const auto& a : f.armies) {
            if (std::hypot(a.x - cx, a.y - cy) <= 2.0) {
                if (g_world.player_trek.seen_object_ids.count(a.id) == 0) {
                    g_world.player_trek.seen_object_ids.insert(a.id);
                    TrekEvent ev;
                    ev.id = "evt_" + generateUUID();
                    ev.object_type = "army";
                    ev.sim_object_id = a.id;
                    std::string stateDesc = (a.current_phase == "march") ? "на марше" : "готовится к бою";
                    if (a.morale < 40) stateDesc = "выглядит измотанной и голодной";
                    ev.description = "Вы наткнулись на армию фракции " + f.name + " (" + std::to_string(a.size) + " воинов). Войско " + stateDesc + ".";
                    g_world.player_trek.pending_events.push_back(ev);
                    event_triggered = true;
                }
            }
        }
    }

    // 4. Бандиты и Погода (на основе текущего региона)
    if (g_world.regions.count(current_region)) {
        Region& r = g_world.regions[current_region];
        
        if (g_world.player_trek.hours_since_last_bandit >= 12) {
            int banditChance = r.threat_level / 10;
            if ((thread_safe_rand() % 100) < banditChance) {
                g_world.player_trek.hours_since_last_bandit = 0;
                TrekEvent ev;
                ev.id = "evt_" + generateUUID();
                ev.object_type = "bandit";
                ev.sim_object_id = "bandits_" + generateUUID();
                ev.description = "Внезапно из укрытия появляются вооруженные люди! Засада бандитов! Они требуют кошелек или жизнь.";
                g_world.player_trek.pending_events.push_back(ev);
                event_triggered = true;
            }
        } else {
            g_world.player_trek.hours_since_last_bandit++;
        }
        
        std::string wKey = "weather_" + r.weather;
        if (g_world.player_trek.seen_object_ids.count(wKey) == 0) {
            g_world.player_trek.seen_object_ids.insert(wKey);
            TrekEvent ev;
            ev.id = "evt_" + generateUUID();
            ev.object_type = "weather";
            ev.sim_object_id = r.weather;
            ev.description = "Погода изменилась. Теперь здесь: " + r.weather + ".";
            ev.can_interact = false;
            g_world.player_trek.pending_events.push_back(ev);
        }
    }

    // 5. Зоны бедствий
    for (const auto& d : g_world.map.disasters) {
        if (std::hypot(cx - d.epicenter_x, cy - d.epicenter_y) <= d.radius) {
            if (g_world.player_trek.seen_object_ids.count(d.id) == 0) {
                g_world.player_trek.seen_object_ids.insert(d.id);
                TrekEvent ev;
                ev.id = "evt_" + generateUUID();
                ev.object_type = "disaster";
                ev.sim_object_id = d.id;
                std::string dName = d.type;
                if (d.type == "wildfire") dName = "Лесной пожар";
                else if (d.type == "aether_storm") dName = "Эфирный шторм";
                else if (d.type == "monster_invasion") dName = "Орда чудовищ";
                ev.description = "ВНИМАНИЕ! Вы вошли в зону активного бедствия: " + dName + ". Находиться здесь смертельно опасно.";
                g_world.player_trek.pending_events.push_back(ev);
                event_triggered = true;
            }
        }
    }

    if (event_triggered) {
        g_world.player_trek.paused = true;
    }

    if (g_world.player_trek.path.empty() || g_world.player_trek.path_index >= (int)g_world.player_trek.path.size() - 1) {
        g_world.player_trek.active = false;
        TrekEvent ev;
        ev.id = "evt_arrive";
        ev.object_type = "arrival";
        ev.description = "Вы успешно добрались до места назначения.";
        ev.can_interact = false;
        g_world.player_trek.pending_events.push_back(ev);
    }
}


void globalHomeostasis() {
    int starvingRegions = 0;
    std::vector<double> wealthList;
    
    for (const auto& [rid, r] : g_world.regions) {
        if (r.starvation_days > 7) starvingRegions++;
        wealthList.push_back(r.moneySupply);
    }
    
    double globalStarvation = g_world.regions.empty() ? 0.0 : (double)starvingRegions / g_world.regions.size();
    
    int warringFactions = 0;
    for (const auto& [fid, f] : g_world.factions) {
        for (const auto& [tid, status] : f.diplomacy) {
            if (status == "war") {
                warringFactions++;
                break;
            }
        }
    }
    double globalWarRate = g_world.factions.empty() ? 0.0 : (double)warringFactions / g_world.factions.size();
    
    double medianWealth = 0.0;
    if (!wealthList.empty()) {
        std::sort(wealthList.begin(), wealthList.end());
        medianWealth = wealthList[wealthList.size() / 2];
    }

    // 1. Благословение Урожая
    if (globalStarvation > 0.20) {
        g_world.nexusData["global_harvest_blessing"] = JsonValue(g_world.current_day + 180);
        addNews("ГЛОБАЛЬНОЕ СОБЫТИЕ: Боги сжалились над голодающим миром. Земли наполнились невиданным плодородием.", "global", 5, "misc");
    }

    // 2. Эпоха Усталости
    if (globalWarRate > 0.50) {
        for (auto& [fid, f] : g_world.factions) {
            f.warExhaustion += 30;
        }
        addNews("ГЛОБАЛЬНОЕ СОБЫТИЕ: Эпоха Усталости. Народы Метеры истощены бесконечными войнами. Правители ищут мира.", "global", 5, "misc");
    }

    // 3. Золотая Жила
    if (medianWealth < 5000.0) {
        g_world.nexusData["global_gold_rush"] = JsonValue(g_world.current_day + 90);
        addNews("ГЛОБАЛЬНОЕ СОБЫТИЕ: Золотая Лихорадка! В недрах земли открылись новые жилы драгоценных металлов.", "global", 5, "trade");
    }
}


void hourlyTick() {
    processConsumption();
    processCaravans();

    processFleets();

    processShips();

    processCouriers();

    processMerchantOrders();
    processTrekTick();
    
    if (g_world.time.internalHour % 4 == 0) {
        updateWeather();
    }
    
    g_world.time.internalHour++;
    if (g_world.time.internalHour >= 24) {
        g_world.time.internalHour = 0;
        dailyTick();
        if (g_world.current_day % 7 == 0) {
            weeklyTick();
        }
        if (g_world.current_day > 0 && g_world.current_day % 30 == 0) {
            monthlyTick();
        }
        if (g_world.current_day > 0 && g_world.current_day % 360 == 0) {
            globalHomeostasis();
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
        int food = vaultStocks[rid][(int)GoodType::BREAD] + vaultStocks[rid][(int)GoodType::MEAT] + vaultStocks[rid][(int)GoodType::FISH] + vaultStocks[rid][(int)GoodType::WHEAT] + vaultStocks[rid][(int)GoodType::SMOKED_MEAT];
        // Максимальный резерв ~14% от населения
        int possible = std::min(r.population / 7, weapons);
        if (food < possible * 0.5) continue;
        total += possible;
    }
    return total;
}

void processRulerDiplomacy() {
    std::unordered_map<std::string, std::vector<int>> vaultStocks;
    {
        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
        for (const auto& [rid, r] : g_world.regions) {
            if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
                vaultStocks[rid] = g_containers[r.vault_id].cached_stocks;
            } else {
                vaultStocks[rid].assign((int)GoodType::COUNT, 0);
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

        if ((rand() % 100) < 20) { // Проверка дипломатии раз в ~5 дней
            std::set<std::string> neighbors;
            for (const auto& road : g_world.map.roads) {
                if (road.condition == "blocked") continue;
                std::string f1 = g_world.regions.count(road.from) ? g_world.regions[road.from].factionId : "";
                std::string f2 = g_world.regions.count(road.to) ? g_world.regions[road.to].factionId : "";
                if (f1 == ruler.factionId && !f2.empty() && f2 != ruler.factionId) neighbors.insert(f2);
                if (f2 == ruler.factionId && !f1.empty() && f1 != ruler.factionId) neighbors.insert(f1);
            }
            
            int hostile_power = 0;
            int neutral_power = 0;
            std::string best_ally = "";
            int best_ally_power = -1;

            for (const auto& nid : neighbors) {
                if (g_world.factions.count(nid)) {
                    int nPower = availableManpower(g_world.factions[nid], vaultStocks);
                    if (faction.diplomacy[nid] == "war") hostile_power += nPower;
                    else {
                        neutral_power += nPower;
                        if (nPower > best_ally_power) {
                            best_ally_power = nPower;
                            best_ally = nid;
                        }
                    }
                }
            }

            // Динамическая паранойя при окружении
            if (hostile_power > power * 1.5) {
                ruler.rulerPersonality.paranoia = std::min(100, ruler.rulerPersonality.paranoia + 2);
            } else if (hostile_power < power * 0.5) {
                ruler.rulerPersonality.paranoia = std::max(0, ruler.rulerPersonality.paranoia - 1);
            }

            // Защитный союз при высокой паранойе
            if (ruler.rulerPersonality.paranoia > 70 && !best_ally.empty() && faction.diplomacy[best_ally] != "war") {
                if (g_world.current_day % 30 == 0) { // Искать союзы не чаще раза в месяц
                    ruler.currentGoal = "offer_alliance -> " + best_ally;
                    faction.relations[best_ally] = std::min(100, faction.relations[best_ally] + 20);
                    g_world.factions[best_ally].relations[ruler.factionId] = std::min(100, g_world.factions[best_ally].relations[ruler.factionId] + 20);
                    addNews("ДИПЛОМАТИЯ: Опасаясь агрессии, " + ruler.name + " заключает оборонительный пакт с фракцией " + g_world.factions[best_ally].name + ".", "global", 3, "diplomacy");
                }
                continue;
            }

            std::string targetF = "";
            if (!neighbors.empty()) {
                auto it = neighbors.begin();
                std::advance(it, rand() % neighbors.size());
                targetF = *it;
            } else {
                targetF = fKeys[rand() % fKeys.size()];
            }
            
            if (targetF == ruler.factionId || targetF.empty()) continue;
            Faction& targetFaction = g_world.factions[targetF];
            int targetPower = availableManpower(targetFaction, vaultStocks);

            // 1. ПОДДЕРЖКА И УСТАЛОСТЬ (Гомеостаз)
            int avgThreat = 0;
            for (const auto& rid : faction.regions) {
                if (g_world.regions.count(rid)) avgThreat += g_world.regions[rid].threat_level;
            }
            if (!faction.regions.empty()) avgThreat /= faction.regions.size();
            
            ruler.rulerPersonality.supportLevel = std::clamp(100 - avgThreat - faction.warExhaustion, 0, 100);
            
            if (faction.warExhaustion > 0 && (rand() % 100) < 10) faction.warExhaustion--;
            
            // Динамическая агрессивность
            if (faction.warExhaustion == 0 && ruler.rulerPersonality.ambition < 90) {
                if ((rand() % 100) < 5) ruler.rulerPersonality.ambition++;
            } else if (faction.warExhaustion > 50 && ruler.rulerPersonality.ambition > 20) {
                if ((rand() % 100) < 10) ruler.rulerPersonality.ambition--;
            }

            // 2. АВТОМАТИЧЕСКИЙ МИР ПРИ ИСТОЩЕНИИ
            if (faction.diplomacy[targetF] == "war") {
                if (faction.warExhaustion > 80 && targetFaction.warExhaustion > 80) {
                    faction.diplomacy[targetF] = "neutral";
                    targetFaction.diplomacy[ruler.factionId] = "neutral";
                    addNews("МИРНЫЙ ДОГОВОР: Истощенные долгой войной, " + faction.name + " и " + targetFaction.name + " заключили перемирие.", "global", 5, "war");
                    continue;
                }
            }

            // 3. НЕЧЕТКАЯ ЛОГИКА АГРЕССИИ
            double powerRatio = targetPower > 0 ? (double)power / targetPower : 2.0;
            double warDesire = (ruler.rulerPersonality.ambition * 0.6) + (powerRatio * 25.0) - (faction.warExhaustion * 0.5) + (ruler.rulerPersonality.supportLevel * 0.2);
            
            std::string aggroKey = ruler.factionId + "_aggro_against_" + targetF;
            if (g_world.nexusData.count(aggroKey) && g_world.nexusData[aggroKey].asInt() > g_world.current_day) {
                warDesire += 30.0;
            }

            bool hasTruce = faction.truceUntil.count(targetF) && faction.truceUntil[targetF] > g_world.current_day;
            // Запрет на войны в первый год (360 дней) для стабилизации экономики. 
            // Также требуем больше золота и желания войны.
                        if (g_world.current_day > 360 && warDesire > 75.0 && wealth >= 15000 && faction.warType == DiplomaticState::PEACE && faction.diplomacy[targetF] != "war" && !hasTruce) {
                CasusBelli cb = CasusBelli::BORDER_INCIDENT;
                std::string motiveText = "Территориальные претензии";
                if (targetPower < power * 0.5 && wealth < 20000) {
                    cb = CasusBelli::IMPERIALISM;
                    motiveText = "Захват ресурсов и расширение границ";
                } else if (faction.relations[targetF] < -50) {
                    cb = CasusBelli::BORDER_INCIDENT;
                    motiveText = "Кровная вражда и старые обиды";
                } else {
                    for (const auto& trid : targetFaction.regions) {
                        if (g_world.regions.count(trid) && g_world.regions[trid].unrest > 50) {
                            cb = CasusBelli::HUMANITARIAN;
                            motiveText = "Наведение порядка на нестабильных территориях";
                            break;
                        }
                    }
                }
                
                DiplomaticState newWarType = DiplomaticState::BORDER_CONFLICT;
                int totalFood = 0, totalWeapons = 0, totalGold = 0;
                for (const auto& rid : faction.regions) {
                    if (g_world.regions.count(rid) && !g_world.regions[rid].vault_id.empty() && g_containers.count(g_world.regions[rid].vault_id)) {
                        const Storage& vault = g_containers[g_world.regions[rid].vault_id];
                        for (const auto& itemId : vault.item_ids) {
                            if (g_items.count(itemId)) {
                                const PhysicalItem& item = g_items[itemId];
                                if (item.prototype_id == GoodType::BREAD || item.prototype_id == GoodType::MEAT || item.prototype_id == GoodType::FISH || item.prototype_id == GoodType::WHEAT || item.prototype_id == GoodType::SMOKED_MEAT) totalFood += item.stack_size;
                                else if (item.prototype_id == GoodType::WEAPONS) totalWeapons += item.stack_size;
                                else if (item.prototype_id == GoodType::GOLD_INGOT) totalGold += item.stack_size;
                            }
                        }
                    }
                }
                
                if (totalFood >= 3000 && totalWeapons >= 1000 && totalGold >= 5000) newWarType = DiplomaticState::TOTAL_WAR;
                else if (totalFood >= 500 && totalWeapons >= 200 && totalGold >= 1000) newWarType = DiplomaticState::LIMITED_WAR;
                else if (totalFood >= 100 && totalWeapons >= 50 && totalGold >= 200) newWarType = DiplomaticState::BORDER_CONFLICT;
                else newWarType = DiplomaticState::PEACE;
                
                if (newWarType != DiplomaticState::PEACE) {
                    ruler.currentGoal = "declare_war -> " + targetF;
                    faction.diplomacy[targetF] = "war";
                    targetFaction.diplomacy[ruler.factionId] = "war";
                    faction.warType = newWarType;
                    faction.currentCasusBelli = cb;
                    if (newWarType == DiplomaticState::LIMITED_WAR && !targetFaction.regions.empty()) {
                        faction.activeWarGoal.targetRegionId = targetFaction.regions[0];
                        faction.activeWarGoal.deadlineDays = 60;
                    }
                    addNews("ВОЙНА: " + ruler.name + " (" + faction.name + ") объявляет войну фракции " + targetFaction.name + "! Мотив: " + motiveText + ".", "global", 5, "war");
                }
            }
            // 3. ИНТРИГИ
            else if (ruler.rulerPersonality.paranoia > 40 && (targetPower + 50) >= power * 0.5) {
                std::string cdKey = ruler.factionId + "_intrigue_cooldown";
                if (!g_world.nexusData.count(cdKey) || g_world.nexusData[cdKey].asInt() <= g_world.current_day) {
                    int activeIntrigues = 0;
                    bool alreadyPlotting = false;
                    for (const auto& intr : g_world.intrigues) {
                        if (intr.initiatorFactionId == ruler.factionId) activeIntrigues++;
                        if (intr.initiatorFactionId == ruler.factionId && intr.targetFactionId == targetF) alreadyPlotting = true;
                    }
                    if (!alreadyPlotting && activeIntrigues < 2) {
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
                intr.startDay = g_world.current_day;
                    g_world.intrigues.push_back(intr);
                    
                    addNews("ИНТРИГА: " + ruler.name + " запускает тайную операцию (" + selectedType + ") против " + targetFaction.name + "!", "global", 3, "war");
                    }
                }
            }
            // 4. ЭКОНОМИКА И СОЮЗЫ
            else if (ruler.rulerPersonality.stewardship > 50 && wealth < 10000) {
                ruler.currentGoal = "trade_pact -> " + targetF;
                faction.relations[targetF] += 10;
                if (!capitalVault.empty()) {
                    createItem(GoodType::GOLD_INGOT, 2000, capitalVault, g_world.current_day, "Торговое соглашение");
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

            if (!homeRegionId.empty() && power > 100) {
                Region& homeRegion = g_world.regions[homeRegionId];
                std::string targetRegionId = "";
                if (faction.warType == DiplomaticState::LIMITED_WAR && !faction.activeWarGoal.targetRegionId.empty()) {
                    targetRegionId = faction.activeWarGoal.targetRegionId;
                } else {
                    targetRegionId = g_world.factions[atWarWith].regions.empty() ? "" : g_world.factions[atWarWith].regions[0];
                }
                
                bool alreadyAttacking = false;
                for (const auto& a : faction.armies) {
                    if (a.destination == targetRegionId) alreadyAttacking = true;
                }

                if (!targetRegionId.empty() && !alreadyAttacking) {
                    std::vector<std::pair<int,int>> army_path;
                    if (g_path_cache.count({homeRegionId, targetRegionId})) {
                        army_path = g_path_cache[{homeRegionId, targetRegionId}];
                    }
                    if (army_path.empty()) continue; // Нет наземного пути для атаки

                    // Генерал собирает от 40% до 75% доступного резерва, оставляя часть на защиту
                    int armySize = power * (0.40 + (rand() % 35) / 100.0);
                    if (armySize < 50) armySize = power;
                    if (armySize < 50) continue; // Армии меньше 50 человек не формируются

                    int weaponsAvailable = vaultStocks[homeRegionId][(int)GoodType::WEAPONS];
                    int foodAvailable = vaultStocks[homeRegionId][(int)GoodType::BREAD] + vaultStocks[homeRegionId][(int)GoodType::MEAT] + vaultStocks[homeRegionId][(int)GoodType::FISH] + vaultStocks[homeRegionId][(int)GoodType::WHEAT] + vaultStocks[homeRegionId][(int)GoodType::SMOKED_MEAT];
                    
                    // --- ИНТЕГРАЦИЯ ДЕМОГРАФИИ И АРМИИ ---
                    int maxDraft = homeRegion.population * 0.03;
                    if (armySize > maxDraft) armySize = maxDraft;
                    
                    // Забираем людей из региона
                    double draftFactor = 1.0 - ((double)armySize / std::max(1, homeRegion.population));
                    for (int p = 0; p <= 120; p++) homeRegion.age_pyramid[p] *= draftFactor;
                    homeRegion.population -= armySize;

                    int estimated_days = army_path.empty() ? 30 : (int)(army_path.size() / 3.0) * 2 + 15;
                    int daily_need = std::max(1, (int)(armySize * 0.02));
                    int calculatedFoodNeed = daily_need * estimated_days;
                    
                    // Оставляем городу еды минимум на неделю, чтобы не вызвать голодомор
                    int cityReserve = homeRegion.population * 0.005 * 7; 
                    int safeFoodAvailable = std::max(0, foodAvailable - cityReserve);
                    
                    int foodToTake = std::min(calculatedFoodNeed, safeFoodAvailable);
                    if (foodToTake < calculatedFoodNeed * 0.3) {
                        // Если еды критически мало, берем сколько нужно, но не больше чем есть вообще
                        foodToTake = std::min(calculatedFoodNeed, foodAvailable);
                    }
                    
                    int weaponsToTake = std::min(armySize, weaponsAvailable);
                    
                    int c = consumeItemsFromContainer(homeRegion.vault_id, GoodType::WEAPONS, weaponsToTake);
                    vaultStocks[homeRegionId][(int)GoodType::WEAPONS] -= c;
                    
                    int breadTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::BREAD, foodToTake);
                    int meatTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::MEAT, foodToTake - breadTaken);
                    int smokedTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::SMOKED_MEAT, foodToTake - breadTaken - meatTaken);
                    int fishTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::FISH, foodToTake - breadTaken - meatTaken - smokedTaken);
                    int wheatTaken = consumeItemsFromContainer(homeRegion.vault_id, GoodType::WHEAT, foodToTake - breadTaken - meatTaken - smokedTaken - fishTaken);
                    
                    std::string armyChestId = createContainer("army_supply_chest", fid, 999999, 1000, homeRegionId);
                    if (breadTaken > 0) createItem(GoodType::BREAD, breadTaken, armyChestId, g_world.current_day, "Припасы армии");
                    if (meatTaken > 0) createItem(GoodType::MEAT, meatTaken, armyChestId, g_world.current_day, "Припасы армии");
                    if (smokedTaken > 0) createItem(GoodType::SMOKED_MEAT, smokedTaken, armyChestId, g_world.current_day, "Припасы армии");
                    if (fishTaken > 0) createItem(GoodType::FISH, fishTaken, armyChestId, g_world.current_day, "Припасы армии");
                    if (wheatTaken > 0) createItem(GoodType::WHEAT, wheatTaken, armyChestId, g_world.current_day, "Припасы армии");
                    
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
                    
                    if (g_path_cache.count({homeRegionId, targetRegionId})) {
                        army.path = g_path_cache[{homeRegionId, targetRegionId}];
                        if (!army.path.empty()) {
                            army.x = army.path[0].first;
                            army.y = army.path[0].second;
                        }
                    }
                    
                    faction.armies.push_back(army);
                    std::string targetName = g_world.regions.count(targetRegionId) ? g_world.regions[targetRegionId].name : targetRegionId;
                    addNews("Снаряженная армия " + faction.name + " (" + std::to_string(armySize) + " воинов) выступила из " + homeRegion.name + " в поход на " + targetName + ".", homeRegionId, 4, "war");
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
                        createItem(GoodType::BREAD, 10, capReg.vault_id, g_world.current_day, "Госзакупка");
                        addNews("СНАБЖЕНИЕ: Казна фракции " + g_world.factions[npc.factionId].name + " пополнена продовольствием за счет налогов.", capital, 1, "trade");
                        continue; // Смерть отменяется
                    }

                    // Если и денег нет - тогда шанс смерти
                    if ((rand() % 1000) < 2) {
                        npc.isAlive = false;
                        npc.alive = false;
                        deadRulers.push_back(id);
                        std::string newsId = addNews("ТРАГЕДИЯ: Правитель " + npc.name + " умер от голода в пустой сокровищнице!", "global", 5, "disaster");
                        g_world.nexusData[npc.factionId + "_last_disaster_news"] = JsonValue(newsId);
                        g_world.nexusData[npc.factionId + "_last_disaster_day"] = JsonValue(g_world.current_day);
                        g_world.nexusData[npc.factionId + "_last_disaster_type"] = JsonValue("голод правителя");
                    }
                }
            }
            
            // Смерть от старости (0.01%)
            if (npc.isAlive && (rand() % 10000) < 1) {
                npc.isAlive = false;
                npc.alive = false;
                deadRulers.push_back(id);
                std::string newsId = addNews("ПЕЧАЛЬНЫЕ ВЕСТИ: Правитель " + npc.name + " мирно скончался от старости.", "global", 5, "misc");
                g_world.nexusData[npc.factionId + "_last_disaster_news"] = JsonValue(newsId);
                g_world.nexusData[npc.factionId + "_last_disaster_day"] = JsonValue(g_world.current_day);
                g_world.nexusData[npc.factionId + "_last_disaster_type"] = JsonValue("смерть правителя");
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
        
        std::string tName = intr.type == "assassination" ? "убийство" : intr.type == "sabotage" ? "саботаж" : intr.type == "bribery" ? "подкуп" : "мятеж";
        std::string initName = g_world.factions[intr.initiatorFactionId].name;
        std::string targetName = g_world.factions[intr.targetFactionId].name;

        if (intr.phase == "recruitment" || intr.phase == "") {
            for (auto& [nid, npc] : g_world.npcs) {
                if (npc.isAlive && npc.personality.greed > 60 && npc.factionId != intr.targetFactionId) {
                    intr.agent_id = nid;
                    intr.phase = "espionage";
                    break;
                }
            }
            if (intr.agent_id.empty()) {
                intr.progress += 1;
                if (intr.progress > 30) g_world.intrigues.erase(g_world.intrigues.begin() + i);
                continue;
            }
        } else if (intr.phase == "espionage") {
            intr.progress += intr.progressPerDay;
            if (intr.progress >= intr.requiredProgress) intr.phase = "execution";
        } else if (intr.phase == "execution") {
            if (!intr.isDiscovered && (rand() % 100) < intr.discoveryChance) {
                intr.isDiscovered = true;
                std::string cdKey = intr.initiatorFactionId + "_intrigue_cooldown";
                g_world.nexusData[cdKey] = JsonValue(g_world.current_day + 30);
                addNews("СКАНДАЛ! Раскрыт заговор (" + tName + ") фракции " + initName + " против " + targetName + "!", "global", 4, "war");
                g_world.factions[intr.targetFactionId].relations[intr.initiatorFactionId] -= 60;
                
                if (!intr.agent_id.empty() && g_world.npcs.count(intr.agent_id)) {
                    NPC& agent = g_world.npcs[intr.agent_id];
                    agent.currentLocation = "Темница " + targetName;
                    agent.currentActivity = "В тюрьме";
                    agent.hp -= 10;
                    addNews("Агент " + agent.name + " схвачен при попытке совершить " + tName + ".", "global", 3, "misc");
                }

                if (g_world.factions[intr.targetFactionId].relations[intr.initiatorFactionId] < -50) {
                    if (g_world.current_day > 180) { // Война из-за интриг только после полугода
                        g_world.factions[intr.targetFactionId].diplomacy[intr.initiatorFactionId] = "war";
                        g_world.factions[intr.initiatorFactionId].diplomacy[intr.targetFactionId] = "war";
                        addNews("ВОЙНА ИЗ-ЗА ИНТРИГ: Оскорбленная фракция объявляет войну!", "global", 5, "war");
                    } else {
                        addNews("ДИПЛОМАТИЧЕСКИЙ КРИЗИС: Отношения разорваны из-за интриг, но правители пока не готовы к открытой войне.", "global", 4, "diplomacy");
                    }
                }
                g_world.intrigues.erase(g_world.intrigues.begin() + i);
                continue;
            }

            if (intr.type == "assassination" && !intr.targetRulerId.empty() && g_world.npcs.count(intr.targetRulerId)) {
                g_world.npcs[intr.targetRulerId].isAlive = false;
                g_world.npcs[intr.targetRulerId].alive = false;
                std::string newsId = addNews("ТЕМНЫЕ ДЕЛА: Убит правитель " + targetName + "! По слухам, за этим стоит " + initName + ".", "global", 5, "war");
                g_world.nexusData[intr.targetFactionId + "_last_disaster_news"] = JsonValue(newsId);
                g_world.nexusData[intr.targetFactionId + "_last_disaster_day"] = JsonValue(g_world.current_day);
                g_world.nexusData[intr.targetFactionId + "_last_disaster_type"] = JsonValue("убийство правителя");
            } else if (intr.type == "sabotage") {
                std::string cap;
                if (!g_world.factions[intr.targetFactionId].regions.empty()) cap = g_world.factions[intr.targetFactionId].regions[0];
                if (!cap.empty() && g_world.regions.count(cap)) {
                    std::string vault = g_world.regions[cap].vault_id;
                    int w = countItemsInContainer(vault, GoodType::WEAPONS);
                    consumeItemsFromContainer(vault, GoodType::WEAPONS, w * 0.1);
                    addNews("ДИВЕРСИЯ: Часть складов оружия " + g_world.regions[cap].name + " (" + targetName + ") взорвана саботажниками из " + initName + "!", cap, 3, "disaster");
                }
            } else if (intr.type == "rebellion") {
                for (const auto& rid : g_world.factions[intr.targetFactionId].regions) {
                    if(g_world.regions.count(rid)) {
                        g_world.regions[rid].population *= 0.7;
                        int w = countItemsInContainer(g_world.regions[rid].vault_id, GoodType::WEAPONS);
                        consumeItemsFromContainer(g_world.regions[rid].vault_id, GoodType::WEAPONS, w * 0.4);
                        std::string newsId = addNews("МЯТЕЖ: В " + g_world.regions[rid].name + " (" + targetName + ") вспыхнуло кровопролитное восстание, проплаченное золотом " + initName + "!", rid, 5, "war");
                        g_world.nexusData[intr.targetFactionId + "_last_disaster_news"] = JsonValue(newsId);
                        g_world.nexusData[intr.targetFactionId + "_last_disaster_day"] = JsonValue(g_world.current_day);
                        g_world.nexusData[intr.targetFactionId + "_last_disaster_type"] = JsonValue("мятеж");
                        break; 
                    }
                }
            } else if (intr.type == "bribery") {
                std::string cap;
                if (!g_world.factions[intr.targetFactionId].regions.empty()) cap = g_world.factions[intr.targetFactionId].regions[0];
                if (!cap.empty() && g_world.regions.count(cap)) {
                    double stolen = g_world.regions[cap].moneySupply * 0.1;
                    g_world.regions[cap].moneySupply -= stolen;
                    std::string initCap = g_world.factions[intr.initiatorFactionId].regions.empty() ? "" : g_world.factions[intr.initiatorFactionId].regions[0];
                    if (!initCap.empty() && g_world.regions.count(initCap)) {
                        g_world.regions[initCap].moneySupply += stolen;
                        int goldAmount = stolen;
                        consumeItemsFromContainer(g_world.regions[cap].vault_id, GoodType::GOLD_INGOT, goldAmount);
                        createItem(GoodType::GOLD_INGOT, goldAmount, g_world.regions[initCap].vault_id, g_world.current_day, "Подкуп");
                    }
                    addNews("КОРРУПЦИЯ: Высшие чины " + targetName + " подкуплены! Казна разворована агентами " + initName + ".", cap, 4, "war");
                }
            }
            std::string aggroKey = intr.initiatorFactionId + "_aggro_against_" + intr.targetFactionId;
            g_world.nexusData[aggroKey] = JsonValue(g_world.current_day + 90);
            std::string cdKey = intr.initiatorFactionId + "_intrigue_cooldown";
            g_world.nexusData[cdKey] = JsonValue(g_world.current_day + 60);
            intr.phase = "cover_up";
        } else if (intr.phase == "cover_up") {
            if ((rand() % 100) < 20) {
                if (!intr.agent_id.empty() && g_world.npcs.count(intr.agent_id)) {
                    NPC& agent = g_world.npcs[intr.agent_id];
                    agent.currentLocation = "Темница " + targetName;
                    agent.currentActivity = "В тюрьме";
                    addNews("Агент " + agent.name + " был пойман при попытке скрыться после успешной операции.", "global", 3, "misc");
                }
            }
            g_world.intrigues.erase(g_world.intrigues.begin() + i);
        }
    }
}

std::string processGmIntervention(const JsonValue& command) {
    std::string cmd = command["command"].asString();
    const JsonValue& args = command["args"];
    std::string feedback = "";

    if (cmd == "buildShip") {
        std::string regionId = args["regionId"].asString();
        ShipType sType = stringToShipType(args["shipType"].asString());
        std::string ownerId = args["ownerId"].asString();

        if (g_world.regions.count(regionId) && g_world.port_facilities.count(regionId)) {
            PortFacility& port = g_world.port_facilities[regionId];
            if (!port.has_shipyard) {
                feedback = "[Верфь] Отказ: В порту " + g_world.regions[regionId].name + " нет верфи.";
            } else {
                std::string capitalRegionId = "";
                if (g_world.factions.count(ownerId) && !g_world.factions[ownerId].regions.empty()) {
                    capitalRegionId = g_world.factions[ownerId].regions[0];
                }
                std::string vaultToUse = capitalRegionId.empty() ? g_world.regions[regionId].vault_id : g_world.regions[capitalRegionId].vault_id;
                
                int woodCost = (sType == ShipType::WAR_GALLEY || sType == ShipType::WAR_FRIGATE) ? 800 : 500;
                int ironCost = (sType == ShipType::WAR_GALLEY || sType == ShipType::WAR_FRIGATE) ? 300 : 100;
                int clothCost = (sType == ShipType::MERCHANT || sType == ShipType::TRANSPORT) ? 50 : 0;
                int weaponCost = (sType == ShipType::WAR_GALLEY || sType == ShipType::WAR_FRIGATE) ? 50 : 0;
                int goldCost = (sType == ShipType::WAR_GALLEY || sType == ShipType::WAR_FRIGATE) ? 1000 : 0;

                int woodAvail = countItemsInContainer(vaultToUse, GoodType::WOOD);
                int ironAvail = countItemsInContainer(vaultToUse, GoodType::IRON_INGOT);
                int clothAvail = countItemsInContainer(vaultToUse, GoodType::CLOTH);
                int weaponAvail = countItemsInContainer(vaultToUse, GoodType::WEAPONS);
                int goldAvail = countItemsInContainer(vaultToUse, GoodType::GOLD_INGOT);

                if (woodAvail >= woodCost && ironAvail >= ironCost && clothAvail >= clothCost && weaponAvail >= weaponCost && goldAvail >= goldCost) {
                    consumeItemsFromContainer(vaultToUse, GoodType::WOOD, woodCost);
                    consumeItemsFromContainer(vaultToUse, GoodType::IRON_INGOT, ironCost);
                    if (clothCost > 0) consumeItemsFromContainer(vaultToUse, GoodType::CLOTH, clothCost);
                    if (weaponCost > 0) consumeItemsFromContainer(vaultToUse, GoodType::WEAPONS, weaponCost);
                    if (goldCost > 0) consumeItemsFromContainer(vaultToUse, GoodType::GOLD_INGOT, goldCost);

                    ShipBuildOrder order;
                    order.id = "build_" + generateUUID();
                    order.type = sType;
                    order.days_left = (sType == ShipType::WAR_GALLEY || sType == ShipType::WAR_FRIGATE) ? 30 : 14;
                    order.owner_id = ownerId;
                    port.build_queue.push_back(order);
                    
                    feedback = "[Верфь] Заложено строительство корабля (" + shipTypeToString(sType) + ") в " + g_world.regions[regionId].name + ". Ресурсы списаны.";
                    g_world.gmInterventionHistory.push_back(cmd);
                } else {
                    feedback = "[Верфь] Отказ: Недостаточно ресурсов для постройки корабля.";
                }
            }
        }
    } else if (cmd == "buildPort") {
        std::string regionId = args["regionId"].asString();
        std::string factionId = args["factionId"].asString();
        if (g_world.regions.count(regionId)) {
            if (g_world.port_facilities.count(regionId)) {
                feedback = "[Инфраструктура] Отказ: В " + g_world.regions[regionId].name + " уже есть порт.";
            } else {
                Region& reg = g_world.regions[regionId];
                std::string capitalRegionId = "";
                if (g_world.factions.count(factionId) && !g_world.factions[factionId].regions.empty()) {
                    capitalRegionId = g_world.factions[factionId].regions[0];
                }
                std::string vaultToUse = capitalRegionId.empty() ? reg.vault_id : g_world.regions[capitalRegionId].vault_id;

                int stoneCost = 2000;
                int woodCost = 1000;
                int goldCost = 5000;

                int stoneAvail = countItemsInContainer(vaultToUse, GoodType::STONE);
                int woodAvail = countItemsInContainer(vaultToUse, GoodType::WOOD);
                int goldAvail = countItemsInContainer(vaultToUse, GoodType::GOLD_INGOT);

                if (stoneAvail >= stoneCost && woodAvail >= woodCost && goldAvail >= goldCost) {
                    consumeItemsFromContainer(vaultToUse, GoodType::STONE, stoneCost);
                    consumeItemsFromContainer(vaultToUse, GoodType::WOOD, woodCost);
                    consumeItemsFromContainer(vaultToUse, GoodType::GOLD_INGOT, goldCost);

                    PortFacility port;
                    port.type = PortType::TRADE;
                    port.dock_container_id = createContainer("port_dock", factionId, 999999, 1000, regionId);
                    port.has_shipyard = false;
                    g_world.port_facilities[regionId] = port;

                    feedback = "[Инфраструктура] Построен новый порт в " + reg.name + ".";
                    addNews("ИНФРАСТРУКТУРА: Фракция " + g_world.factions[factionId].name + " возвела новый порт в " + reg.name + "!", regionId, 4, "politics");
                    g_world.gmInterventionHistory.push_back(cmd);
                } else {
                    feedback = "[Инфраструктура] Отказ: Недостаточно ресурсов (нужно 2000 камня, 1000 дерева, 5000 золота).";
                }
            }
        } else {
            feedback = "[Инфраструктура] Отказ: Регион не найден.";
        }
    } else if (cmd == "navalBlockade") {
        std::string factionId = args["factionId"].asString();
        std::string regionId = args["regionId"].asString();
        if (g_world.regions.count(regionId) && g_world.port_facilities.count(regionId)) {
            g_world.port_facilities[regionId].is_blockaded = true;
            feedback = "[Флот] Фракция " + g_world.factions[factionId].name + " установила морскую блокаду порта " + g_world.regions[regionId].name + "!";
            addNews("МОРСКАЯ БЛОКАДА: Военный флот перекрыл доступ к порту " + g_world.regions[regionId].name + ".", regionId, 4, "war");
            g_world.gmInterventionHistory.push_back(cmd);
        } else {
            feedback = "[Флот] Отказ: Порт не найден.";
        }
    } else if (cmd == "upgradePort") {
        std::string regionId = args["regionId"].asString();
        if (g_world.regions.count(regionId) && g_world.port_facilities.count(regionId)) {
            PortFacility& port = g_world.port_facilities[regionId];
            std::string factionId = g_world.regions[regionId].factionId;
            std::string capitalRegionId = "";
            if (g_world.factions.count(factionId) && !g_world.factions[factionId].regions.empty()) {
                capitalRegionId = g_world.factions[factionId].regions[0];
            }
            std::string vaultToUse = capitalRegionId.empty() ? g_world.regions[regionId].vault_id : g_world.regions[capitalRegionId].vault_id;

            int stoneCost = 1000 * port.level;
            int woodCost = 500 * port.level;
            
            int stoneAvail = countItemsInContainer(vaultToUse, GoodType::STONE);
            int woodAvail = countItemsInContainer(vaultToUse, GoodType::WOOD);

            if (stoneAvail >= stoneCost && woodAvail >= woodCost) {
                consumeItemsFromContainer(vaultToUse, GoodType::STONE, stoneCost);
                consumeItemsFromContainer(vaultToUse, GoodType::WOOD, woodCost);
                port.level++;
                if (port.level >= 3 && !port.has_shipyard) port.has_shipyard = true;
                feedback = "[Инфраструктура] Порт в " + g_world.regions[regionId].name + " улучшен до уровня " + std::to_string(port.level) + ".";
                g_world.gmInterventionHistory.push_back(cmd);
            } else {
                feedback = "[Инфраструктура] Отказ: Недостаточно камня или дерева для улучшения порта.";
            }
        }
    } else if (cmd == "gmPurchaseGoods") {
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
                createItem(goodType, quantity, g_world.regions[capitalRegionId].vault_id, g_world.current_day, "Закупка ГМ");
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
                createItem(goodType, quantity, reg.vault_id, g_world.current_day, "Продажа ГМ");
                reg.moneySupply = std::max(0.0, reg.moneySupply - revenue);
                createItem(GoodType::GOLD_INGOT, revenue, g_world.regions[capitalRegionId].vault_id, g_world.current_day, "Выручка ГМ");

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
            int foodNeeded = std::max(1, (int)(drafts * 0.02 * 14)); // Еда на 2 недели обороны
            int weaponsTaken = consumeItemsFromContainer(reg.vault_id, GoodType::WEAPONS, weaponsNeeded);
            int breadTaken = consumeItemsFromContainer(reg.vault_id, GoodType::BREAD, foodNeeded);
            int meatTaken = consumeItemsFromContainer(reg.vault_id, GoodType::MEAT, foodNeeded - breadTaken);
            int wheatTaken = consumeItemsFromContainer(reg.vault_id, GoodType::WHEAT, foodNeeded - breadTaken - meatTaken);

            std::string militiaChestId = createContainer("army_supply_chest", fac.id, 999999, 1000, regionId);
            if (breadTaken > 0) createItem(GoodType::BREAD, breadTaken, militiaChestId, g_world.current_day, "Ополчение");
            if (meatTaken > 0) createItem(GoodType::MEAT, meatTaken, militiaChestId, g_world.current_day, "Ополчение");
            if (wheatTaken > 0) createItem(GoodType::WHEAT, wheatTaken, militiaChestId, g_world.current_day, "Ополчение");

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
    } else if (cmd == "gmBuildHighway") {
        std::string from = args["from"].asString();
        std::string to = args["to"].asString();
        bool found = false;
        for (auto& road : g_world.map.roads) {
            if ((road.from == from && road.to == to) || (road.from == to && road.to == from)) {
                road.type = "highway";
                road.condition = "paved";
                road.integrity = 100;
                found = true;
            }
        }
        if (found) {
            feedback = "[Инфраструктура] ГМ приказал возвести Имперский Тракт между " + from + " и " + to + ".";
            addNews("ИМПЕРИЯ СТРОИТ: По указу свыше проложен новый Имперский Тракт!", from, 4, "logistics");
            g_world.gmInterventionHistory.push_back(cmd);
            g_path_cache_dirty = true;
        } else {
            feedback = "[ERROR] Дорога между указанными регионами не найдена.";
        }

            } else if (cmd == "gmDirectResourceInjection") {
            std::string regionId = args["regionId"].asString();
            GoodType goodType = stringToGoodType(args["goodType"].asString());
            int quantity = args["quantity"].asInt();

            if (g_world.current_day - g_world.lastDirectInjectionDay < 7) {
                feedback = "[ERROR] gmDirectResourceInjection на кулдауне.";
            } else if (g_world.regions.count(regionId)) {
                g_world.lastDirectInjectionDay = g_world.current_day;
                createItem(goodType, quantity, g_world.regions[regionId].vault_id, g_world.current_day, "Божественное вмешательство");
                feedback = "[Вмешательство] В " + g_world.regions[regionId].name + " появилось " + std::to_string(quantity) + " " + goodTypeToString(goodType) + ".";
                g_world.gmInterventionHistory.push_back(cmd);
            } else {
                feedback = "[ERROR] Регион не найден.";
            }
        } else if (cmd == "spawnMonster") {
            std::string regionId = args["regionId"].asString();
            std::string typeStr = args["type"].asString();
            if (g_world.regions.count(regionId)) {
                EpicMonster m;
                m.id = "epic_" + generateUUID();
                m.type = stringToMonsterType(typeStr);
                m.name = "Призванный " + typeStr;
                m.region_id = regionId;
                if (g_world.map.locations.count(regionId)) {
                    m.lair_x = g_world.map.locations[regionId].x;
                    m.lair_y = g_world.map.locations[regionId].y;
                }
                m.treasure_chest_id = createContainer("monster_lair", "monster", 999999, 100, regionId);
                g_world.monsters.push_back(m);
                feedback = "[ГМ] Призван эпический монстр в " + g_world.regions[regionId].name + ".";
                addNews("ВТОРЖЕНИЕ: Мастер Игры призвал чудовище в " + g_world.regions[regionId].name + "!", regionId, 5, "disaster");
                g_world.gmInterventionHistory.push_back(cmd);
            } else {
                feedback = "[ERROR] Регион не найден.";
            }
        } else if (cmd == "killMonster") {
            std::string monsterId = args["monsterId"].asString();
            bool found = false;
            for (auto& m : g_world.monsters) {
                if (m.id == monsterId) {
                    m.health = 0;
                    found = true;
                    feedback = "[ГМ] Чудовище " + m.name + " уничтожено волей Мастера.";
                    g_world.gmInterventionHistory.push_back(cmd);
                    break;
                }
            }
            if (!found) feedback = "[ERROR] Монстр не найден.";
        } else if (cmd == "triggerDisaster") {
        std::string type = args["type"].asString();
        std::string regionId = args["regionId"].asString();
        int strength = args.has("strength") ? args["strength"].asInt() : 5;

        if (g_world.regions.count(regionId) && g_world.map.locations.count(regionId)) {
            auto loc = g_world.map.locations[regionId];
            Disaster d;
            d.id = "dis_" + generateUUID();
            d.type = type;
            d.epicenter_x = loc.x;
            d.epicenter_y = loc.y;
            d.radius = strength;
            d.strength = strength;
            d.affected_regions.push_back(regionId);
            d.days_active = strength * 2;
            
            if (type == "flood") {
                for (int y = std::max(0, d.epicenter_y - d.radius); y <= std::min(g_world.map.height - 1, d.epicenter_y + d.radius); ++y) {
                    for (int x = std::max(0, d.epicenter_x - d.radius); x <= std::min(g_world.map.width - 1, d.epicenter_x + d.radius); ++x) {
                        if (std::hypot(x - d.epicenter_x, y - d.epicenter_y) <= d.radius) {
                            int idx = y * g_world.map.width + x;
                            TileType t = g_world.map.grid[idx].type;
                            if (t == TileType::RIVERBANK || t == TileType::FLOODPLAIN || t == TileType::PLAINS) {
                                g_world.map.grid[idx].is_flooded = true;
                                d.affected_tiles.push_back({x, y});
                            }
                        }
                    }
                }
                g_world.map.generation_tick = g_world.tick;
            } else if (type == "earthquake") {
                for (auto& road : g_world.map.roads) {
                    if (road.from == regionId || road.to == regionId) {
                        road.condition = "ruined";
                        road.integrity = 0;
                    }
                }
                g_path_cache_dirty = true;
            }
            g_world.map.disasters.push_back(d);
            feedback = "[Катастрофа] ГМ вызвал " + type + " в " + g_world.regions[regionId].name + ".";
            addNews("ГНЕВ БОГОВ: " + type + " обрушивается на " + g_world.regions[regionId].name + "!", regionId, 5, "disaster");
            g_world.gmInterventionHistory.push_back(cmd);
        } else {
            feedback = "[ERROR] Регион не найден.";
        }
    } else if (cmd == "gmDeclareWar") {
        std::string f1 = args["fromFactionId"].asString();
        std::string f2 = args["toFactionId"].asString();
        if (g_world.factions.count(f1) && g_world.factions.count(f2)) {
            g_world.factions[f1].diplomacy[f2] = "war";
            g_world.factions[f2].diplomacy[f1] = "war";
            g_world.factions[f1].warType = DiplomaticState::LIMITED_WAR;
            g_world.factions[f2].warType = DiplomaticState::LIMITED_WAR;
            g_world.factions[f1].currentCasusBelli = CasusBelli::IMPERIALISM;
            g_world.factions[f2].currentCasusBelli = CasusBelli::IMPERIALISM;
            if (!g_world.factions[f2].regions.empty()) {
                g_world.factions[f1].activeWarGoal.targetRegionId = g_world.factions[f2].regions[0];
                g_world.factions[f1].activeWarGoal.deadlineDays = 60;
            }
            if (!g_world.factions[f1].regions.empty()) {
                g_world.factions[f2].activeWarGoal.targetRegionId = g_world.factions[f1].regions[0];
                g_world.factions[f2].activeWarGoal.deadlineDays = 60;
            }
            feedback = "[Дипломатия] ГМ принудительно объявил войну между " + g_world.factions[f1].name + " и " + g_world.factions[f2].name + ".";
            addNews("ВОЙНА: По воле высших сил " + g_world.factions[f1].name + " и " + g_world.factions[f2].name + " вступают в конфликт!", "global", 5, "war");
            g_world.gmInterventionHistory.push_back(cmd);
        } else {
            feedback = "[ERROR] Фракции для gmDeclareWar не найдены.";
        }
    } else if (cmd == "gmForcePeace") {
        std::string f1 = args["factionId1"].asString();
        std::string f2 = args["factionId2"].asString();
        if (g_world.factions.count(f1) && g_world.factions.count(f2)) {
            g_world.factions[f1].diplomacy[f2] = "neutral";
            g_world.factions[f2].diplomacy[f1] = "neutral";
            g_world.factions[f1].warType = DiplomaticState::PEACE;
            g_world.factions[f2].warType = DiplomaticState::PEACE;
            g_world.factions[f1].warExhaustion = 0;
            g_world.factions[f2].warExhaustion = 0;
            feedback = "[Дипломатия] ГМ принудительно установил мир между " + g_world.factions[f1].name + " и " + g_world.factions[f2].name + ".";
            addNews("МИР: По воле высших сил " + g_world.factions[f1].name + " и " + g_world.factions[f2].name + " прекращают боевые действия.", "global", 5, "diplomacy");
            g_world.gmInterventionHistory.push_back(cmd);
        } else {
            feedback = "[ERROR] Фракции для gmForcePeace не найдены.";
        }
    } else if (cmd == "gmChangeRulerTrait") {
        std::string rId = args["rulerId"].asString();
        std::string trait = args["trait"].asString();
        int val = args["value"].asInt();
        if (g_world.npcs.count(rId) && g_world.npcs[rId].type == "ruler") {
            auto& p = g_world.npcs[rId].rulerPersonality;
            if (trait == "ambition") p.ambition = val;
            else if (trait == "paranoia") p.paranoia = val;
            else if (trait == "wisdom") p.wisdom = val;
            else if (trait == "cruelty") p.cruelty = val;
            else if (trait == "diplomacy") p.diplomacy = val;
            else if (trait == "military") p.military = val;
            else if (trait == "stewardship") p.stewardship = val;
            feedback = "[Правитель] Черта '" + trait + "' правителя " + g_world.npcs[rId].name + " изменена на " + std::to_string(val) + ".";
            g_world.gmInterventionHistory.push_back(cmd);
        } else {
            feedback = "[ERROR] Правитель не найден.";
        }
    }

    return feedback;
}


    void processMerchants() {
        if (g_path_cache_dirty) {
            g_path_cache.clear();
            std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
            std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
            for (const auto& road : g_world.map.roads) {
                if (road.condition == "blocked") {
                    for (const auto& wp : road.waypoints) path_status[wp.second * g_world.map.width + wp.first] = 2;
                } else if (road.condition == "ruined") {
                    for (const auto& wp : road.waypoints) {
                        path_status[wp.second * g_world.map.width + wp.first] = 1;
                        has_road[wp.second * g_world.map.width + wp.first] = true;
                    }
                } else {
                    for (const auto& wp : road.waypoints) has_road[wp.second * g_world.map.width + wp.first] = true;
                }
            }
            for (const auto& [r1, reg1] : g_world.regions) {
                for (const auto& [r2, reg2] : g_world.regions) {
                    if (r1 != r2 && g_world.map.locations.count(r1) && g_world.map.locations.count(r2)) {
                        auto loc1 = g_world.map.locations[r1];
                        auto loc2 = g_world.map.locations[r2];
                        g_path_cache[{r1, r2}] = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, has_road, path_status, MovementType::LAND);
                    }
                }
            }
            g_path_cache_dirty = false;
        }

        // Структура для возврата результата из параллельного потока
    struct MerchantTask {
        std::string merchant_id;
        std::string origin;
        std::string bestDest;
        GoodType bestGood;
        int buyPrice;
        bool execute;
    };

    std::vector<std::future<MerchantTask>> futures;

    for (auto& [npcId, merchant] : g_world.npcs) {
        if (!merchant.isAlive || (merchant.economy.profession_type != "merchant" && merchant.profession != "Торговец")) continue;
        
        // Если купец уже в пути, пропускаем (синхронное чтение безопасно)
        bool inTransit = false;
        for (const auto& [rid, r] : g_world.regions) {
            for (const auto& c : r.caravans) {
                if (c.merchant_id == npcId) { inTransit = true; break; }
            }
            if (inTransit) break;
        }
        if (inTransit) continue;
        if (!g_world.regions.count(merchant.currentLocation)) continue;

        std::string mId = npcId;
        std::string mLoc = merchant.currentLocation;
        int mSavings = merchant.economy.savings;

        // ЗАПУСК ТЯЖЕЛЫХ РАСЧЕТОВ (A* И АНАЛИЗ РЫНКА) В ПАРАЛЛЕЛЬНЫХ ПОТОКАХ
        futures.push_back(getThreadPool()->enqueue([mId, mLoc, mSavings]() -> MerchantTask {
            MerchantTask task = {mId, mLoc, "", GoodType::COUNT, 0, false};
            if (!g_world.regions.count(mLoc)) return task;
            
            const Region& localReg = g_world.regions.at(mLoc);
            double maxProfit = 0;

            for (int i=0; i<(int)GoodType::COUNT; i++) {
                GoodType gt = (GoodType)i;
                if (GOOD_CATEGORIES[i] == GoodCategory::DOCUMENT) continue; // Документы не для рыночных спекуляций
                std::string gtStr = goodTypeToString(gt);
                double localP = localReg.markets.count(gtStr) ? localReg.markets.at(gtStr) : BASE_PRICES[i];
                if (localP <= 0) localP = BASE_PRICES[i];

                for (const auto& [destId, destReg] : g_world.regions) {
                    if (destId == mLoc) continue;
                    double destP = destReg.markets.count(gtStr) ? destReg.markets.at(gtStr) : BASE_PRICES[i];
                    double profitMargin = destP - localP;
                    
                    if (profitMargin > maxProfit && profitMargin > localP * 0.2) {
                                                    // ИСПОЛЬЗУЕМ КЭШИРОВАННЫЕ МАРШРУТЫ (O(1) вместо O(N^2))
                            if (g_path_cache.count({mLoc, destId})) {
                                auto path = g_path_cache.at({mLoc, destId});
                                if (!path.empty()) {
                                    maxProfit = profitMargin;
                                    task.bestDest = destId;
                                    task.bestGood = gt;
                                    task.buyPrice = localP;
                                    task.execute = true;
                                }
                            }
                    }
                }
            }
            return task;
        }));
    }

    // СИНХРОННОЕ ПРИМЕНЕНИЕ РЕЗУЛЬТАТОВ (Избегаем Data Races при изменении инвентарей)
    for (auto& f : futures) {
        MerchantTask task = f.get(); // Дожидаемся завершения потока
        if (task.execute && g_world.npcs.count(task.merchant_id) && g_world.regions.count(task.origin)) {
            NPC& merchant = g_world.npcs[task.merchant_id];
            Region& localReg = g_world.regions[task.origin];
            
            int availableGoods = countItemsInContainer(localReg.vault_id, task.bestGood);
            int safePrice = std::max(1, task.buyPrice);
            int maxAffordable = merchant.economy.savings / safePrice;
            int maxCarryable = (GOOD_CATEGORIES[(int)task.bestGood] == GoodCategory::VEHICLE) ? (1 + rand() % 3) : 200;
            int amountToBuy = std::min({availableGoods, maxAffordable, maxCarryable});

            if (amountToBuy > 0) {
                int cost = amountToBuy * safePrice;
                merchant.economy.savings -= cost;
                localReg.moneySupply += cost;
                consumeItemsFromContainer(localReg.vault_id, task.bestGood, amountToBuy);

                std::string chestId = createContainer("caravan_chest", merchant.id, 999999, 1000, merchant.currentLocation);
                createItem(task.bestGood, amountToBuy, chestId, g_world.current_day, "Купеческий товар");

                int guardsHired = 0;
                int maxGuardsWanted = std::min(5, merchant.economy.savings / 20);
                
                for (auto& [mercId, merc] : g_world.npcs) {
                    if (guardsHired >= maxGuardsWanted) break;
                    if (merc.isAlive && merc.economy.profession_type == "mercenary" && merc.currentLocation == merchant.currentLocation && merc.currentActivity != "Охраняет караван") {
                        merc.currentActivity = "Охраняет караван";
                        merc.travelDestination = task.bestDest;
                        merc.travelHoursLeft = 24 + (rand() % 48);
                        merchant.economy.savings -= 20;
                        merc.economy.savings += 20;
                        guardsHired++;
                    }
                }
                
                int abstractGuards = 0;
                if (guardsHired < maxGuardsWanted) {
                    abstractGuards = maxGuardsWanted - guardsHired;
                    merchant.economy.savings -= abstractGuards * 20;
                }
                int totalGuards = guardsHired + abstractGuards;

                Caravan caravan;
                caravan.id = "caravan_" + generateUUID();
                caravan.merchant_id = merchant.id;
                caravan.origin = merchant.currentLocation;
                caravan.destination = task.bestDest;
                caravan.chest_id = chestId;
                caravan.wagons = (GOOD_CATEGORIES[(int)task.bestGood] == GoodCategory::VEHICLE) ? amountToBuy : (1 + (amountToBuy / 50));
                caravan.guards = totalGuards;
                caravan.guard_cost = totalGuards * 20;
                caravan.hoursLeft = 24 + (rand() % 48);
                
                if (g_path_cache.count({merchant.currentLocation, task.bestDest})) {
                    caravan.path = g_path_cache[{merchant.currentLocation, task.bestDest}];
                    if (!caravan.path.empty()) {
                        caravan.x = caravan.path[0].first;
                        caravan.y = caravan.path[0].second;
                    }
                }
                
                localReg.caravans.push_back(caravan);
                merchant.currentActivity = "В пути в " + g_world.regions[task.bestDest].name;

                addNews("ЛОГИСТИКА: Караван купца " + merchant.name + " отправился из " + localReg.name + " в " + g_world.regions[task.bestDest].name + ".", task.origin, 1, "trade");
            }
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
        if (unemploymentRate > 0.3) delta += 1 + (int)((unemploymentRate - 0.3) * 5);

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

        // 3.5 Спавн Эпических Монстров и Блокировка Дорог
        bool global_dragon_exists = g_world.nexusData.count("global_dragon_active") && g_world.nexusData["global_dragon_active"].asBool();
        if (r.threat_level >= 100 && !g_world.nexusData.count(rid + "_dragon_spawned") && !global_dragon_exists) {
            g_world.nexusData[rid + "_dragon_spawned"] = JsonValue(true);
            g_world.nexusData["global_dragon_active"] = JsonValue(true);
            
            int blockX = -1, blockY = -1;
            for (const auto& road : g_world.map.roads) {
                if (road.from == rid || road.to == rid) {
                    if (road.waypoints.size() > 15) {
                        int wpIdx = (road.from == rid) ? 10 : road.waypoints.size() - 11;
                        blockX = road.waypoints[wpIdx].first;
                        blockY = road.waypoints[wpIdx].second;
                        break;
                    }
                }
            }
            if (blockX == -1 && g_world.map.locations.count(rid)) {
                blockX = g_world.map.locations[rid].x;
                blockY = g_world.map.locations[rid].y;
            }
            
            if (blockX != -1) {
                g_world.nexusData[rid + "_dragon_x"] = JsonValue(blockX);
                g_world.nexusData[rid + "_dragon_y"] = JsonValue(blockY);
                int radius = 6;
                
                std::vector<MapRoad> new_roads;
                for (auto& road : g_world.map.roads) {
                    if (road.condition == "ruined" || road.condition == "blocked") {
                        new_roads.push_back(road);
                        continue;
                    }
                    MapRoad current_segment;
                    current_segment.from = road.from;
                    current_segment.to = road.to;
                    current_segment.condition = road.condition;
                    bool in_ruin = false;
                    for (size_t i = 0; i < road.waypoints.size(); ++i) {
                        auto wp = road.waypoints[i];
                        bool inside = std::hypot(wp.first - blockX, wp.second - blockY) <= radius;
                        if (inside && !in_ruin) {
                            if (!current_segment.waypoints.empty()) {
                                current_segment.waypoints.push_back(wp);
                                new_roads.push_back(current_segment);
                            }
                            current_segment.waypoints.clear();
                            current_segment.condition = "ruined";
                            current_segment.waypoints.push_back(wp);
                            in_ruin = true;
                        } else if (!inside && in_ruin) {
                            if (!current_segment.waypoints.empty()) {
                                current_segment.waypoints.push_back(wp);
                                new_roads.push_back(current_segment);
                            }
                            current_segment.waypoints.clear();
                            current_segment.condition = road.condition;
                            current_segment.waypoints.push_back(wp);
                            in_ruin = false;
                        } else {
                            current_segment.waypoints.push_back(wp);
                        }
                    }
                    if (!current_segment.waypoints.empty()) new_roads.push_back(current_segment);
                }
                g_world.map.roads = new_roads;
                
                for (int y = std::max(0, blockY - radius); y <= std::min(g_world.map.height - 1, blockY + radius); ++y) {
                    for (int x = std::max(0, blockX - radius); x <= std::min(g_world.map.width - 1, blockX + radius); ++x) {
                        if (std::hypot(x - blockX, y - blockY) <= radius) {
                            g_world.map.grid[y * g_world.map.width + x].type = TileType::VOLCANO;
                        }
                    }
                }
                g_world.map.generation_tick = g_world.tick;
                g_path_cache_dirty = true;
            }
            addNews("КАТАСТРОФА: В регионе " + r.name + " поселился древний Дракон! Образовался кратер, торговые пути перерезаны!", rid, 5, "disaster");
        }
        
        // Разблокировка дорог, если угроза спала (например, игрок убил дракона и снизил threat_level)
        if (r.threat_level < 50 && g_world.nexusData.count(rid + "_dragon_spawned")) {
            int blockX = g_world.nexusData.count(rid + "_dragon_x") ? g_world.nexusData[rid + "_dragon_x"].asInt() : -1;
            int blockY = g_world.nexusData.count(rid + "_dragon_y") ? g_world.nexusData[rid + "_dragon_y"].asInt() : -1;
            int radius = 6;

            g_world.nexusData.erase(rid + "_dragon_spawned");
            g_world.nexusData.erase(rid + "_dragon_x");
            g_world.nexusData.erase(rid + "_dragon_y");
            g_world.nexusData.erase("global_dragon_active");
            
            addNews("ОСВОБОЖДЕНИЕ: Угроза в " + r.name + " миновала. Дороги ремонтируются, кратеры засыпаются.", rid, 4, "trade");
            
            if (blockX != -1) {
                for (auto& road : g_world.map.roads) {
                    if (road.condition == "ruined" || road.condition == "blocked") {
                        bool inside = true;
                        for (auto wp : road.waypoints) {
                            if (std::hypot(wp.first - blockX, wp.second - blockY) > radius + 2) {
                                inside = false;
                                break;
                            }
                        }
                        if (inside) {
                            road.condition = "paved"; // Восстанавливаем дорогу
                            g_path_cache_dirty = true;
                        }
                    }
                }
                for (int y = std::max(0, blockY - radius); y <= std::min(g_world.map.height - 1, blockY + radius); ++y) {
                    for (int x = std::max(0, blockX - radius); x <= std::min(g_world.map.width - 1, blockX + radius); ++x) {
                        if (std::hypot(x - blockX, y - blockY) <= radius) {
                            if (g_world.map.grid[y * g_world.map.width + x].type == TileType::VOLCANO) {
                                g_world.map.grid[y * g_world.map.width + x].type = TileType::PLAINS;
                            }
                        }
                    }
                }
                g_world.map.generation_tick = g_world.tick;
            }
        }

        // 4. Автоматическое снижение угрозы при благополучии
        if (unemploymentRate < 0.3 && foodPerCapita > 0.8) {
            r.threat_level = std::max(0, r.threat_level - (1 + rand() % 2));
        }
        
        // Уникальная логика типов локаций
        if (r.base_type == "fort") {
            r.threat_level = std::max(0, r.threat_level - 2); // Форты подавляют угрозу
        } else if (r.base_type == "anomaly") {
            r.dread = std::min(100, r.dread + 2); // Аномалии генерируют ужас
        } else if (r.base_type == "ruins" || r.population == 0) {
            r.threat_level = std::max(80, r.threat_level); // Руины всегда опасны
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


void processDailyNPCs() {
    std::vector<NPC*> active_npcs;
    std::vector<std::string> already_dead;
    for (auto& [id, npc] : g_world.npcs) {
        if (npc.isAlive) active_npcs.push_back(&npc);
        else already_dead.push_back(id);
    }

    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    int chunk_size = active_npcs.size() / num_threads + 1;
    std::vector<std::future<std::vector<std::string>>> futures;

    for (int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        int end_idx = std::min((int)active_npcs.size(), (t + 1) * chunk_size);
        if (start_idx >= active_npcs.size()) break;

        futures.push_back(getThreadPool()->enqueue([start_idx, end_idx, &active_npcs]() {
            std::vector<std::string> local_to_delete;
            for (int i = start_idx; i < end_idx; ++i) {
                NPC& npc = *active_npcs[i];
                
                npc.age_days++;
                npc.currentWealthLevel = npc.economy.savings + npc.gold;
                
                bool needsJob = (npc.age_days == 18 * 360 && npc.economy.profession_type == "none");
                bool wantsJobChange = false;
                
                if (!needsJob && !npc.economy.isEmployed && npc.type != "ruler") {
                    int desperation = (npc.currentWealthLevel < 50) ? 5 : 1;
                    if (g_world.current_day - npc.professionChangeTimestamp > 30 && (thread_safe_rand() % 100) < desperation) {
                        wantsJobChange = true;
                    }
                }

                if (needsJob || wantsJobChange) {
                    if (g_world.regions.count(npc.currentLocation)) {
                        Region& r = g_world.regions[npc.currentLocation];
                        
                        std::map<std::string, int> current_workers;
                        for (const auto& [oid, onpc] : g_world.npcs) {
                            if (onpc.isAlive && onpc.currentLocation == npc.currentLocation) {
                                current_workers[onpc.economy.profession_type]++;
                            }
                        }
                        
                        std::map<std::string, int> job_demand;
                        job_demand["farmer"] = ((r.facilities.count("farms") ? r.facilities["farms"].level * 100 : 0) + 50) - current_workers["farmer"];
                        job_demand["artisan"] = ((r.facilities.count("forges") ? r.facilities["forges"].level * 50 : 0) + (r.facilities.count("weavers") ? r.facilities["weavers"].level * 50 : 0)) - current_workers["artisan"];
                        job_demand["merchant"] = (r.population / 500) - current_workers["merchant"];
                        job_demand["mercenary"] = (r.threat_level * 2) - current_workers["mercenary"];
                        job_demand["cleric"] = (r.population / 1000) - current_workers["cleric"];
                        job_demand["gatherer"] = 20 - current_workers["gatherer"];

                        std::string best_prof = "farmer";
                        int max_demand = -1;
                        for (const auto& [prof, demand] : job_demand) {
                            if (demand > max_demand) {
                                max_demand = demand;
                                best_prof = prof;
                            }
                        }

                        npc.economy.profession_type = best_prof;
                        if (best_prof == "farmer") {
                            int r_prof = thread_safe_rand() % 3;
                            if (r_prof == 0) npc.profession = "Фермер";
                            else if (r_prof == 1) npc.profession = "Охотник";
                            else npc.profession = "Пасечник";
                        }
                        else if (best_prof == "artisan") npc.profession = (thread_safe_rand()%2==0) ? "Кузнец" : "Ткач";
                        else if (best_prof == "gatherer") npc.profession = "Астроном";
                        else if (best_prof == "merchant") npc.profession = "Торговец";
                        else if (best_prof == "mercenary") npc.profession = "Наемник";
                        else if (best_prof == "cleric") npc.profession = "Священник";
                        
                        if (wantsJobChange) {
                            npc.economy.skillLevel = 1; // Сброс навыка при смене профессии
                            npc.professionChangeTimestamp = g_world.current_day;
                        }
                    }
                }

                npc.needs.hunger = std::max(0, npc.needs.hunger - 1);
                if (g_world.current_day % 2 == 0) npc.needs.rest = std::max(0, npc.needs.rest - 1);
                if (g_world.current_day % 5 == 0) npc.needs.social = std::max(0, npc.needs.social - 1);

                if (npc.diseases.empty()) {
                    double sick_chance = 0.001 * (200.0 / std::max(1, npc.immunity));
                    if (npc.age_days > 23400) sick_chance *= 2.0;
                    if ((thread_safe_rand() % 10000) < (sick_chance * 10000)) {
                        npc.diseases.push_back("common_cold");
                    }
                } else {
                    npc.hp -= 1;
                    if ((thread_safe_rand() % 100) < (npc.immunity / 2)) {
                        npc.diseases.clear();
                    }
                }

                if (!npc.diseases.empty() || !npc.wounds.empty()) {
                    std::lock_guard<std::mutex> lock(g_npc_state_mutex);
                    for (auto& [docId, doctor] : g_world.npcs) {
                        if (doctor.isAlive && doctor.currentLocation == npc.currentLocation && doctor.profession == "Маг") {
                            if (npc.gold >= 15) {
                                npc.gold -= 15;
                                doctor.gold += 15;
                                npc.diseases.clear();
                                npc.wounds.clear();
                                npc.relationships[docId] += 10;
                                doctor.relationships[npc.id] += 5;
                                break;
                            }
                        }
                    }
                }

                if (npc.needs.social < 50) {
                    std::lock_guard<std::mutex> lock(g_npc_state_mutex);
                    for (auto& [otherId, otherNpc] : g_world.npcs) {
                        if (otherId != npc.id && otherNpc.isAlive && otherNpc.currentLocation == npc.currentLocation) {
                            npc.relationships[otherId] += 1;
                            otherNpc.relationships[npc.id] += 1;
                            npc.needs.social += 30;
                            otherNpc.needs.social += 10;
                            break;
                        }
                    }
                }

                if (npc.age_days > 23400) {
                    double death_chance = 0.0001 + ((npc.age_days - 23400) / 360.0) * 0.00005;
                    if ((thread_safe_rand() % 100000) < (death_chance * 100000)) {
                        npc.hp = 0;
                        npc.death_cause = "old_age";
                    }
                }

                if (g_bootstrap && npc.hp <= 1) npc.hp = 1;
                if (npc.hp <= 0) {
                    if (g_bootstrap) {
                        npc.hp = 1;
                    } else {
                        npc.isAlive = false;
                        npc.death_day = g_world.current_day;
                        if (npc.death_cause.empty()) npc.death_cause = "health_failure";
                        addNews("Житель " + npc.name + " скончался. Причина: " + npc.death_cause, npc.currentLocation, 1, "misc");
                        local_to_delete.push_back(npc.id);
                    }
                }
            }
            return local_to_delete;
        }));
    }

    std::vector<std::string> to_delete = already_dead;
    for (auto& f : futures) {
        auto res = f.get();
        to_delete.insert(to_delete.end(), res.begin(), res.end());
    }

    // --- SYNCHRONOUS INHERITANCE LOGIC ---
    for (const auto& id : to_delete) {
        if (!g_world.npcs.count(id)) continue;
        NPC& dead_npc = g_world.npcs[id];
        
        std::string heir_id = "";
        if (!dead_npc.spouse_id.empty() && g_world.npcs.count(dead_npc.spouse_id) && g_world.npcs[dead_npc.spouse_id].isAlive) {
            heir_id = dead_npc.spouse_id;
        } else {
            for (const auto& child_id : dead_npc.children_ids) {
                if (g_world.npcs.count(child_id) && g_world.npcs[child_id].isAlive) {
                    heir_id = child_id;
                    break;
                }
            }
        }
        if (heir_id.empty() && !dead_npc.father_id.empty() && g_world.npcs.count(dead_npc.father_id) && g_world.npcs[dead_npc.father_id].isAlive) {
            heir_id = dead_npc.father_id;
        }
        if (heir_id.empty() && !dead_npc.mother_id.empty() && g_world.npcs.count(dead_npc.mother_id) && g_world.npcs[dead_npc.mother_id].isAlive) {
            heir_id = dead_npc.mother_id;
        }

        for (const auto& bId : dead_npc.owned_businesses) {
            if (g_world.businesses.count(bId)) {
                Business& bus = g_world.businesses[bId];
                bus.owner_ids.erase(std::remove(bus.owner_ids.begin(), bus.owner_ids.end(), id), bus.owner_ids.end());
                
                if (!heir_id.empty()) {
                    if (std::find(bus.owner_ids.begin(), bus.owner_ids.end(), heir_id) == bus.owner_ids.end()) {
                        bus.owner_ids.push_back(heir_id);
                    }
                    if (std::find(g_world.npcs[heir_id].owned_businesses.begin(), g_world.npcs[heir_id].owned_businesses.end(), bId) == g_world.npcs[heir_id].owned_businesses.end()) {
                        g_world.npcs[heir_id].owned_businesses.push_back(bId);
                    }
                    addNews("Предприятие (" + bus.facility_type + ") перешло по наследству к " + g_world.npcs[heir_id].name + ".", bus.region_id, 1, "trade");
                } else if (bus.owner_ids.empty()) {
                    if (g_world.regions.count(bus.region_id)) {
                        g_world.regions[bus.region_id].moneySupply += (bus.cash_balance * 0.5) + (bus.level * 250);
                    }
                    addNews("Предприятие (" + bus.facility_type + ") ликвидировано из-за отсутствия наследников.", bus.region_id, 1, "trade");
                    g_world.businesses.erase(bId);
                }
            }
        }

        int total_wealth = dead_npc.economy.savings + dead_npc.gold;
        if (total_wealth > 0) {
            if (!heir_id.empty()) {
                g_world.npcs[heir_id].economy.savings += total_wealth;
            } else if (g_world.regions.count(dead_npc.currentLocation)) {
                g_world.regions[dead_npc.currentLocation].moneySupply += total_wealth;
            }
        }

        if (g_world.regions.count(dead_npc.currentLocation)) {
            Region& r = g_world.regions[dead_npc.currentLocation];
            int age_years = dead_npc.age_days / 360;
            if (age_years > 120) age_years = 120;
            r.age_pyramid[age_years] = std::max(0.0, r.age_pyramid[age_years] - 1.0);
            r.population = std::max(0, r.population - 1);
        }

        g_world.npcs.erase(id);
    }
}


void processDiplomacy() {
    for (auto& [fid, f] : g_world.factions) {
        // T3: 5.1 Casus Belli & 3.1 War Exhaustion
        if (f.warType >= DiplomaticState::BORDER_CONFLICT) {
            f.daysInCurrentWar++;
            double weGain = 1.0;
            
            if (f.currentCasusBelli == CasusBelli::NONE) {
                weGain += 2.0;
                if (f.daysInCurrentWar == 1) {
                    for (auto& [nid, nfac] : g_world.factions) {
                        if (nid != fid && f.diplomacy[nid] != "war") {
                            f.relations[nid] = std::max(-100, f.relations[nid] - 20);
                            nfac.relations[fid] = std::max(-100, nfac.relations[fid] - 20);
                        }
                    }
                    addNews("АГРЕССИЯ: Фракция " + f.name + " начала войну без повода! Мировое сообщество осуждает это.", "global", 5, "diplomacy");
                }
            }

            bool targetedByCoalition = false;
            for (const auto& [cid, cfac] : g_world.factions) {
                for (const auto& coal : cfac.coalitions) {
                    if (coal.targetFactionId == fid) targetedByCoalition = true;
                }
            }
            if (targetedByCoalition) weGain *= 1.5; // T3: 5.3 Coalition forces peace faster

            if (g_world.current_day % 7 == 0) f.warExhaustion = std::min(100, f.warExhaustion + (int)weGain); // Усталость растет раз в неделю, а не каждый день
        } else {
            f.daysInCurrentWar = 0;
        }

        // T3: 3.3 De-escalation (Forced Peace)
        if (f.warType != DiplomaticState::PEACE && f.warType != DiplomaticState::COLD_WAR) {
            if (f.warExhaustion >= 100) {
                f.warType = DiplomaticState::PEACE;
                f.legitimacy -= 30;
                f.warExhaustion = 0;
                addNews("ПРИНУДИТЕЛЬНЫЙ МИР: Фракция " + f.name + " полностью истощена войной и капитулирует на условиях статус-кво.", "global", 5, "diplomacy");
                for (auto& [otherId, state] : f.diplomacy) {
                    if (state == "war") {
                        f.diplomacy[otherId] = "neutral";
                        f.truceUntil[otherId] = g_world.current_day + 180; // Перемирие на полгода после истощения
                        if (g_world.factions.count(otherId)) {
                            g_world.factions[otherId].diplomacy[fid] = "neutral";
                            g_world.factions[otherId].warType = DiplomaticState::PEACE;
                            g_world.factions[otherId].truceUntil[fid] = g_world.current_day + 180;
                        }
                    }
                }
            } else if (f.warExhaustion >= 80 && (thread_safe_rand() % 100) < 10) {
                addNews("ИСТОЩЕНИЕ: Фракция " + f.name + " умоляет о мире.", "global", 4, "diplomacy");
            }
        }

        // T3: 3.2 Escalation
        if (f.warType == DiplomaticState::LIMITED_WAR) {
            if (f.activeWarGoal.deadlineDays > 0) {
                f.activeWarGoal.deadlineDays--;
                if (f.activeWarGoal.deadlineDays == 0 && !f.activeWarGoal.achieved && f.legitimacy > 50) {
                    f.warType = DiplomaticState::TOTAL_WAR;
                    addNews("ТОТАЛЬНАЯ ВОЙНА: " + f.name + " не достигла целей ограниченной войны и бросает все силы в бой!", "global", 5, "war");
                }
            }
        }

        // T3: 5.2 Ultimatums Processing
        for (auto it = f.ultimatums.begin(); it != f.ultimatums.end(); ) {
            it->expiresDay--;
            if (it->expiresDay <= 0) {
                                if (!it->accepted) {
                    if (g_world.factions.count(it->fromFactionId)) {
                        Faction& issuer = g_world.factions[it->fromFactionId];
                        if (g_world.current_day > 180) { // Война по ультиматуму только после полугода
                            issuer.warType = DiplomaticState::LIMITED_WAR;
                            issuer.diplomacy[fid] = "war";
                            f.diplomacy[issuer.id] = "war";
                            addNews("УЛЬТИМАТУМ ОТВЕРГНУТ: " + issuer.name + " вступает в войну против " + f.name + "!", "global", 5, "diplomacy");
                            
                            bool hasCoalition = false;
                            for (auto& c : issuer.coalitions) {
                                if (c.targetFactionId == fid) { hasCoalition = true; break; }
                            }
                            if (!hasCoalition) {
                                Coalition c;
                                c.leaderFactionId = issuer.id;
                                c.targetFactionId = fid;
                                c.formedOnDay = g_world.current_day;
                                c.members.push_back(issuer.id);
                                issuer.coalitions.push_back(c);
                                addNews("Сформирована коалиция против " + f.name + " во главе с " + issuer.name + ".", "global", 5, "diplomacy");
                            }
                        } else {
                            addNews("УЛЬТИМАТУМ ОТВЕРГНУТ: " + issuer.name + " грозит последствиями, но открытая война пока не начата.", "global", 4, "diplomacy");
                        }
                    }
                }
                it = f.ultimatums.erase(it);
            } else {
                ++it;
            }
        }

        // T3: 5.2 AI issuing ultimatums
        if (f.warType >= DiplomaticState::BORDER_CONFLICT && f.warExhaustion > 50) {
            std::string victimId = "";
            for (const auto& [otherId, state] : f.diplomacy) {
                if (state == "war") { victimId = otherId; break; }
            }
            if (!victimId.empty()) {
                for (auto& [nId, nFac] : g_world.factions) {
                    if (nId != fid && nId != victimId && nFac.warType == DiplomaticState::PEACE) {
                        if (nFac.relations[victimId] > 60) {
                            bool alreadyIssued = false;
                            for (const auto& u : f.ultimatums) if (u.fromFactionId == nId) alreadyIssued = true;
                            if (!alreadyIssued && (thread_safe_rand() % 100) < 5) {
                                Ultimatum u;
                                u.fromFactionId = nId;
                                u.toFactionId = fid;
                                u.demand = "stop_war";
                                u.expiresDay = 7;
                                f.ultimatums.push_back(u);
                                addNews("УЛЬТИМАТУМ: " + nFac.name + " требует от " + f.name + " немедленно прекратить войну, иначе они вмешаются!", "global", 5, "diplomacy");
                            }
                        }
                    }
                }
            }
        }
        
        // T3: 5.4 Mediation
        if (f.warType >= DiplomaticState::BORDER_CONFLICT) {
            std::string enemyId = "";
            for (const auto& [otherId, state] : f.diplomacy) {
                if (state == "war") { enemyId = otherId; break; }
            }
            if (!enemyId.empty() && g_world.factions.count(enemyId)) {
                for (auto& [nId, nFac] : g_world.factions) {
                    if (nId != fid && nId != enemyId && nFac.warType == DiplomaticState::PEACE) {
                        if (nFac.relations[fid] > 40 && nFac.relations[enemyId] > 40 && (thread_safe_rand() % 100) < 2) {
                            f.warExhaustion = std::max(0, f.warExhaustion - 10);
                            g_world.factions[enemyId].warExhaustion = std::max(0, g_world.factions[enemyId].warExhaustion - 10);
                            addNews("ПОСРЕДНИЧЕСТВО: " + nFac.name + " организовала мирные переговоры между " + f.name + " и " + g_world.factions[enemyId].name + ". Напряжение спадает.", "global", 4, "diplomacy");
                        }
                    }
                }
            }
        }
    }
}


void processInternalPolitics() {
    std::vector<std::string> regionsToRebel;

    for (auto& [rid, r] : g_world.regions) {
        if (r.factionId.empty() || !g_world.factions.count(r.factionId)) continue;
        Faction& f = g_world.factions[r.factionId];
        
        if (r.productionBlockedDays > 0) r.productionBlockedDays--;
        if (r.daysUnderOccupation > 0) r.daysUnderOccupation++;

        // T3: 7.2 Occupation Effects
        if (r.isOccupied) {
            r.unrest = std::min(100, r.unrest + 5);
            if (r.daysUnderOccupation >= 30 && r.unrest < 50) {
                r.isOccupied = false;
                std::string oldFac = r.factionId;
                r.factionId = r.occupierFactionId;
                r.occupierFactionId = "";
                if (g_world.factions.count(oldFac)) {
                    auto& regs = g_world.factions[oldFac].regions;
                    regs.erase(std::remove(regs.begin(), regs.end(), rid), regs.end());
                }
                if (g_world.factions.count(r.factionId)) {
                    g_world.factions[r.factionId].regions.push_back(rid);
                }
                addNews("АССИМИЛЯЦИЯ: Оккупированный регион " + r.name + " полностью интегрирован во фракцию " + g_world.factions[r.factionId].name + ".", rid, 4, "politics");
            }
        }

        // 6.1 Stability Modifiers
        int targetStability = 70;
        if (f.warType == DiplomaticState::LIMITED_WAR) targetStability -= 10;
        else if (f.warType == DiplomaticState::TOTAL_WAR) targetStability -= 20;
        
        if (r.starvation_days > 0) targetStability -= 20;
        if (f.warType >= DiplomaticState::LIMITED_WAR) targetStability -= 10; // Taxes
        
        targetStability += (f.legitimacy - 50) / 2;

        bool hasGarrison = false;
        for (const auto& a : f.armies) {
            if (a.location == rid || a.destination == rid) { hasGarrison = true; break; }
        }
        if (hasGarrison) targetStability += 10;

        // Move current stability towards target
        if (r.stability > targetStability) r.stability--;
        else if (r.stability < targetStability) r.stability++;

        r.stability = std::clamp(r.stability, 0, 100);

        // 6.2 Riots (FIXED: Added unrest check to prevent riot spam, reduced daily chance to 3%)
        if (r.stability < 30 && r.unrest < 50 && (thread_safe_rand() % 100) < 3) {
            r.unrest = 100;
            r.productionBlockedDays = 5;
            r.population = std::max(0, (int)(r.population * 0.95));
            
            int foodLost = countItemsInContainer(r.vault_id, GoodType::BREAD) * 0.1;
            int weaponsLost = countItemsInContainer(r.vault_id, GoodType::WEAPONS) * 0.1;
            if (foodLost > 0) consumeItemsFromContainer(r.vault_id, GoodType::BREAD, foodLost);
            if (weaponsLost > 0) consumeItemsFromContainer(r.vault_id, GoodType::WEAPONS, weaponsLost);

            addNews("БУНТ! В регионе " + r.name + " вспыхнуло восстание из-за низкой стабильности. Производство остановлено, склады разграблены.", rid, 4, "disaster");

            std::string capitalId = f.regions.empty() ? "" : f.regions[0];
            if (rid == capitalId) {
                f.legitimacy = std::max(0, f.legitimacy - 10);
                addNews("Бунт в столице! Легитимность правителя фракции " + f.name + " стремительно падает.", rid, 5, "politics");
            }
        } else {
            r.unrest = std::max(0, r.unrest - 10);
        }

                // AI Risk Analysis for Disasters
        std::string riskKey = rid + "_disaster_count";
        int capRisk = g_world.nexusData.count(riskKey) ? g_world.nexusData[riskKey].asInt() : 0;
        
        if (capRisk > 3) {
            int gold = countItemsInContainer(r.vault_id, GoodType::GOLD_INGOT);
            int defLevel = r.custom_props.has("disaster_defense") ? r.custom_props["disaster_defense"].asInt() : 0;
            int cost = (defLevel + 1) * 5000;
            
            if (gold >= cost) {
                consumeItemsFromContainer(r.vault_id, GoodType::GOLD_INGOT, cost);
                r.custom_props.set("disaster_defense", defLevel + 1);
                addNews("ИНФРАСТРУКТУРА: Фракция " + f.name + " возвела защитные сооружения (ур. " + std::to_string(defLevel+1) + ") в " + r.name + " для защиты от катаклизмов.", rid, 3, "politics");
                g_world.nexusData[riskKey] = JsonValue(std::max(0, capRisk - 2));
            } else if (f.regions.size() > 1 && capRisk > 5 && rid == f.regions[0]) { // Only move if it's the capital
                std::string newCap = "";
                int minRisk = 999;
                for (const auto& rId : f.regions) {
                    int rRisk = g_world.nexusData.count(rId + "_disaster_count") ? g_world.nexusData[rId + "_disaster_count"].asInt() : 0;
                    if (rRisk < minRisk) { minRisk = rRisk; newCap = rId; }
                }
                if (!newCap.empty() && newCap != rid) {
                    auto it = std::find(f.regions.begin(), f.regions.end(), newCap);
                    if (it != f.regions.end()) {
                        f.regions.erase(it);
                        f.regions.insert(f.regions.begin(), newCap);
                        addNews("ПЕРЕНОС СТОЛИЦЫ: Из-за постоянных разрушительных катаклизмов фракция " + f.name + " перенесла столицу в более безопасный регион " + g_world.regions[newCap].name + ".", newCap, 5, "politics");
                    }
                }
            }
        }


// 6.1 Independence
        if (r.stability < 10 && !hasGarrison) {
            regionsToRebel.push_back(rid);
        }
    }

    for (const auto& rid : regionsToRebel) {
        Region& r = g_world.regions[rid];
        std::string oldFaction = r.factionId;
        r.factionId = ""; // Becomes neutral/rebel
        if (g_world.factions.count(oldFaction)) {
            auto& regs = g_world.factions[oldFaction].regions;
            regs.erase(std::remove(regs.begin(), regs.end(), rid), regs.end());
            g_world.factions[oldFaction].legitimacy -= 10;
        }
        r.stability = 50; // Reset stability after rebellion
        addNews("НЕЗАВИСИМОСТЬ! Регион " + r.name + " вышел из состава фракции из-за катастрофической нестабильности.", rid, 5, "politics");
    }

    // 6.3 Ruler Legitimacy & Coups
    for (auto& [fid, f] : g_world.factions) {
        if (f.legitimacy < 20 && (thread_safe_rand() % 100) < 10) {
            if (g_world.npcs.count(f.rulerId)) {
                NPC& ruler = g_world.npcs[f.rulerId];
                ruler.isAlive = false;
                ruler.alive = false;
                ruler.death_cause = "Убит во время дворцового переворота";
                
                f.warType = DiplomaticState::PEACE;
                f.warExhaustion = 0;
                f.legitimacy = 50; // New ruler starts fresh

                addNews("ПЕРЕВОРОТ! Правитель " + ruler.name + " свергнут. Новая власть фракции " + f.name + " объявляет о прекращении всех войн.", "global", 5, "politics");
            }
        }
        
        // Обновление глобальной стабильности фракции для UI
        int totalStab = 0;
        int count = 0;
        for (const auto& rid : f.regions) {
            if (g_world.regions.count(rid)) {
                totalStab += g_world.regions[rid].stability;
                count++;
            }
        }
        if (count > 0) f.stability = totalStab / count;
    }
}


void processShipyards() {
    for (auto& [rid, port] : g_world.port_facilities) {
        if (!port.has_shipyard) continue;
        for (auto it = port.build_queue.begin(); it != port.build_queue.end(); ) {
            it->days_left--;
            if (it->days_left <= 0) {
                Ship s;
                s.id = "ship_" + generateUUID();
                s.owner_id = it->owner_id;
                s.type = it->type;
                s.hull = (it->type == ShipType::WAR_GALLEY || it->type == ShipType::WAR_FRIGATE) ? 200 : 100;
                s.sailors = (it->type == ShipType::WAR_GALLEY) ? 40 : 15;
                s.cargo_capacity = (it->type == ShipType::WAR_GALLEY) ? 100 : 500;
                s.chest_id = createContainer("ship_hold", it->owner_id, 999999, 100, rid);
                s.speed = (it->type == ShipType::WAR_GALLEY) ? 1.2 : 1.5;
                if (it->type == ShipType::WAR_GALLEY || it->type == ShipType::WAR_FRIGATE) {
                    s.cannons = 10; s.marines = 20;
                }
                if (g_world.map.locations.count(rid)) {
                    s.x = g_world.map.locations[rid].x;
                    s.y = g_world.map.locations[rid].y;
                }
                g_world.ships.push_back(s);
                addNews("ВЕРФЬ: В порту " + g_world.regions[rid].name + " завершено строительство нового корабля!", rid, 2, "trade");
                it = port.build_queue.erase(it);
            } else {
                ++it;
            }
        }
    }
}


void processMonsterHunts() {
    for (auto& m : g_world.monsters) {
        if (m.health <= 0 || m.state != "ACTIVE") continue;
        if (!g_world.regions.count(m.region_id)) continue;
        Region& r = g_world.regions[m.region_id];
        if (r.factionId.empty() || !g_world.factions.count(r.factionId)) continue;
        Faction& f = g_world.factions[r.factionId];

        bool alreadyHunting = false;
        for (const auto& a : f.armies) {
            if (a.target_monster_id == m.id) { alreadyHunting = true; break; }
        }

        if (!alreadyHunting && (thread_safe_rand() % 100) < 10) {
            std::string capId = f.regions.empty() ? "" : f.regions[0];
            if (!capId.empty() && g_world.regions.count(capId)) {
                int huntSize = 500 + (thread_safe_rand() % 500);
                if (g_world.regions[capId].population > huntSize * 2) {
                    g_world.regions[capId].population -= huntSize;
                    Army hunter;
                    hunter.id = "army_" + generateUUID();
                    hunter.size = huntSize;
                    hunter.morale = 100;
                    hunter.location = capId;
                    hunter.destination = m.region_id;
                    hunter.target_monster_id = m.id;
                    hunter.current_phase = "march";
                    
                    std::vector<bool> dummy_has_road(g_world.map.width * g_world.map.height, false);
                    std::vector<int> dummy_path_status(g_world.map.width * g_world.map.height, 0);
                    if (g_world.map.locations.count(capId) && g_world.map.locations.count(m.region_id)) {
                        auto loc1 = g_world.map.locations[capId];
                        auto loc2 = g_world.map.locations[m.region_id];
                        hunter.path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::ANY, huntSize);
                        if (!hunter.path.empty()) {
                            hunter.x = hunter.path[0].first;
                            hunter.y = hunter.path[0].second;
                        }
                    }
                    
                    f.armies.push_back(hunter);
                    addNews("ВЕЛИКАЯ ОХОТА: Фракция " + f.name + " собрала армию ветеранов и отправила её на уничтожение чудовища " + m.name + "!", capId, 5, "war");
                }
            }
        }
    }

    for (auto& [fid, f] : g_world.factions) {
        for (auto& a : f.armies) {
            if (!a.target_monster_id.empty()) {
                EpicMonster* target = nullptr;
                for (auto& m : g_world.monsters) {
                    if (m.id == a.target_monster_id && m.health > 0) {
                        target = &m; break;
                    }
                }

                if (target) {
                    if (a.location == target->region_id) {
                        // Анти-монстровая коалиция: если монстр сильный, враги прекращают войну
                        if (target->level >= 5) {
                            for (auto& [ofid, of] : g_world.factions) {
                                if (f.diplomacy[ofid] == "war") {
                                    f.diplomacy[ofid] = "neutral";
                                    of.diplomacy[f.id] = "neutral";
                                    f.truceUntil[ofid] = 999999; // Временный вечный мир до убийства монстра
                                    of.truceUntil[f.id] = 999999;
                                    addNews("ВЕЛИКАЯ УГРОЗА: Фракции " + f.name + " и " + of.name + " прекратили войну, чтобы объединиться против чудовища " + target->name + "!", a.location, 5, "diplomacy");
                                }
                            }
                        }

                        double armyPower = a.size * (a.morale / 100.0);
                        double monsterPower = target->attack * (target->health / (double)target->maxHealth) * target->level * 10;

                        int armyDmg = (int)(monsterPower * ((thread_safe_rand() % 50 + 50) / 100.0));
                        int monsterDmg = (int)(armyPower * ((thread_safe_rand() % 50 + 50) / 100.0));

                        a.size -= armyDmg;
                        target->health -= monsterDmg;

                        if (target->health <= 0) {
                            target->health = 0;
                            std::string locName1 = g_world.regions.count(a.location) ? g_world.regions[a.location].name : a.location;
                            addNews("ЭПИЧЕСКАЯ ПОБЕДА: Армия " + f.name + " ценой огромных потерь уничтожила чудовище " + target->name + " в " + locName1 + "!", a.location, 5, "war");
                            // Коалиции распадаются после победы
                            for (auto& [ofid, of] : g_world.factions) {
                                if (of.diplomacy[f.id] == "neutral" && of.truceUntil[f.id] == 999999) {
                                    of.truceUntil[f.id] = g_world.current_day + 30;
                                    f.truceUntil[ofid] = g_world.current_day + 30;
                                }
                            }
                            f.legitimacy = std::min(100, f.legitimacy + 20);
                            a.target_monster_id = "";
                            a.destination = a.location;
                            a.path.clear();
                            createItem(GoodType::MONSTER_PARTS, target->level * 2, g_world.regions[a.location].vault_id, g_world.current_day, "Трофеи с монстра");
                            createItem(GoodType::GOLD_INGOT, target->level * 1000, g_world.regions[a.location].vault_id, g_world.current_day, "Трофеи с монстра");
                        } else if (a.size <= 0) {
                            std::string locName2 = g_world.regions.count(a.location) ? g_world.regions[a.location].name : a.location;
                            addNews("ТРАГЕДИЯ: Армия " + f.name + " была полностью уничтожена чудовищем " + target->name + " в " + locName2 + "!", a.location, 5, "disaster");
                            f.legitimacy = std::max(0, f.legitimacy - 15);
                            if (!a.general_id.empty() && g_world.npcs.count(a.general_id)) {
                                NPC& gen = g_world.npcs[a.general_id];
                                if (gen.type == "ruler") {
                                    gen.health = 0;
                                    gen.isAlive = false;
                                    addNews("ПАДЕНИЕ КОРОЛЯ: Правитель " + gen.name + " пал в бою с чудовищем!", a.location, 5, "disaster");
                                }
                            }
                        } else {
                            addNews("БИТВА С ЧУДОВИЩЕМ: Армия " + f.name + " ведет кровопролитный бой с " + target->name + ". Обе стороны несут потери.", a.location, 4, "war");
                        }
                    } else {
                        if (a.destination != target->region_id) {
                            a.destination = target->region_id;
                            std::vector<bool> dummy_has_road(g_world.map.width * g_world.map.height, false);
                            std::vector<int> dummy_path_status(g_world.map.width * g_world.map.height, 0);
                            if (g_world.map.locations.count(a.location) && g_world.map.locations.count(target->region_id)) {
                                auto loc1 = g_world.map.locations[a.location];
                                auto loc2 = g_world.map.locations[target->region_id];
                                a.path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::ANY, a.size);
                                a.path_index = 0;
                            }
                        }
                    }
                } else {
                    a.target_monster_id = "";
                    a.destination = a.location;
                    a.path.clear();
                }
            }
        }
        f.armies.erase(std::remove_if(f.armies.begin(), f.armies.end(), [](const Army& a) { return a.size <= 0; }), f.armies.end());
    }
}



void processDreadAndMonsters() {
    for (auto& [rid, r] : g_world.regions) {
        if (r.threat_level > 50) r.dread += 1;
        if (r.unrest > 50) r.dread += 1;
        
        if (g_world.map.locations.count(rid)) {
            std::string locType = g_world.map.locations[rid].type;
            if (locType == "ruins" || locType == "anomaly") r.dread += 1;
            if (locType == "ruins" && (thread_safe_rand() % 1000) < 5) {
                r.dread += 50;
                addNews("СЛУХИ: Искатели сокровищ слишком глубоко проникли в руины " + r.name + ", пробудив нечто зловещее...", rid, 3, "misc");
            }
        }
        for (const auto& intr : g_world.intrigues) {
            if (intr.targetFactionId == r.factionId && !intr.isDiscovered) r.dread += 1;
        }

        if (r.threat_level < 20) r.dread = std::max(0, r.dread - 2);
        r.dread = std::clamp(r.dread, 0, 100);

                if (r.dread > 80 && (thread_safe_rand() % 100) < (r.dread - 80)) {
            int nextSpawnDay = 0;
            if (g_world.nexusData.count("next_epic_monster_spawn_day")) {
                nextSpawnDay = g_world.nexusData["next_epic_monster_spawn_day"].asInt();
            }

            if (g_world.current_day >= nextSpawnDay) {
                bool hasMonster = false;
                for (const auto& m : g_world.monsters) if (m.region_id == rid && m.health > 0) hasMonster = true;
                
                if (!hasMonster) {
                    EpicMonster m;
                    m.id = "epic_" + generateUUID();
                    if (r.placement_type == "mountain") { m.type = MonsterType::DRAGON; m.name = "Древний Дракон"; }
                    else if (r.placement_type == "water" || r.placement_type == "coast") { m.type = MonsterType::KRAKEN; m.name = "Ужас Глубин"; }
                    else if (r.placement_type == "forest") { m.type = MonsterType::HYDRA; m.name = "Лесная Гидра"; }
                    else if (r.placement_type == "desert") { m.type = MonsterType::LICH_KING; m.name = "Владыка Песков"; }
                    else { m.type = MonsterType::BEHOLDER; m.name = "Око Бездны"; }
                    
                    m.region_id = rid;
                    m.is_visible_on_map = false;
                    if (g_world.map.locations.count(rid)) {
                        m.lair_x = g_world.map.locations[rid].x;
                        m.lair_y = g_world.map.locations[rid].y;
                        
                        int radius = 2;
                        TileType corruptType = TileType::ASH;
                        if (m.type == MonsterType::LICH_KING || m.type == MonsterType::VAMPIRE_LORD) corruptType = TileType::SWAMP;
                        if (m.type == MonsterType::KRAKEN || m.type == MonsterType::LEVIATHAN) corruptType = TileType::SHALLOW_WATER;
                        
                        for (int dy = -radius; dy <= radius; dy++) {
                            for (int dx = -radius; dx <= radius; dx++) {
                                if (std::hypot(dx, dy) <= radius) {
                                    int nx = m.lair_x + dx;
                                    int ny = m.lair_y + dy;
                                    if (nx >= 0 && nx < g_world.map.width && ny >= 0 && ny < g_world.map.height) {
                                        g_world.map.grid[ny * g_world.map.width + nx].type = corruptType;
                                    }                            }
                            }
                        }
                        g_world.map.generation_tick = g_world.tick;
                    }
                    m.treasure_chest_id = createContainer("monster_lair", "monster", 999999, 100, rid);
                    createItem(GoodType::GOLD_INGOT, 5000 + (thread_safe_rand() % 5000), m.treasure_chest_id, g_world.current_day, "Сокровища логова");
                    g_world.monsters.push_back(m);
                    
                    // Устанавливаем глобальный кулдаун на спавн эпических монстров: 900 дней (2.5 года)
                    // Это гарантирует максимум 2 монстра за 5 лет (1800 дней)
                    g_world.nexusData["next_epic_monster_spawn_day"] = JsonValue(g_world.current_day + 900);
                    
                    addNews("ПРОБУЖДЕНИЕ ЗЛА: В регионе " + r.name + " появилось чудовище - " + m.name + "!", rid, 5, "disaster");
                    r.dread = 0;
                }
            }
        }
    }

    for (auto it = g_world.monsters.begin(); it != g_world.monsters.end(); ) {
        if (it->health <= 0) {
            addNews("ПОБЕДА: Чудовище " + it->name + " было повержено!", it->region_id, 5, "misc");
            it = g_world.monsters.erase(it);
            continue;
        }
        
        it->days_active++;
        if (it->days_active % 30 == 0 && it->level < 10) {
            it->level++;
            it->maxHealth += 500;
            it->health += 500;
            it->attack += 10;
            it->defense += 5;
            addNews("УГРОЗА РАСТЕТ: Чудовище " + it->name + " стало сильнее (Уровень " + std::to_string(it->level) + ")!", it->region_id, 4, "disaster");
        }

        if (it->state == "ACTIVE" && g_world.regions.count(it->region_id)) {
            Region& r = g_world.regions[it->region_id];
            
            if (!it->is_visible_on_map) {
                if (r.dread >= 100) it->is_visible_on_map = true;
                if (!r.factionId.empty() && g_world.factions.count(r.factionId)) {
                    for (const auto& a : g_world.factions[r.factionId].armies) {
                        if (a.location == it->region_id) it->is_visible_on_map = true;
                    }
                }
            }

            if ((thread_safe_rand() % 100) < 20) r.stability = std::max(0, r.stability - 1);
            r.threat_level = std::min(100, r.threat_level + 5);
            int deaths = r.population * 0.01;
            r.population = std::max(0, r.population - deaths);
            
            r.fertility = std::max(0.0, r.fertility - 0.05);
            for (auto& [good, price] : r.markets) {
                price *= 1.05;
            }
            
            if (!r.facilities.empty() && (thread_safe_rand() % 100) < 20) {
                auto fac_it = r.facilities.begin();
                std::advance(fac_it, thread_safe_rand() % r.facilities.size());
                fac_it->second.durability -= 20;
                if (fac_it->second.durability < 0) fac_it->second.durability = 0;
            }
            if ((thread_safe_rand() % 100) < 20) {
                for (auto& road : g_world.map.roads) {
                    if (road.from == it->region_id || road.to == it->region_id) {
                        road.condition = "ruined";
                        road.integrity = 0;
                        g_path_cache_dirty = true;
                    }
                }
                if (g_world.port_facilities.count(it->region_id)) {
                    g_world.port_facilities[it->region_id].durability -= 20;
                }
            }

            if (!r.factionId.empty() && g_world.factions.count(r.factionId)) {
                std::string capId = g_world.factions[r.factionId].regions.empty() ? "" : g_world.factions[r.factionId].regions[0];
                if (!capId.empty() && g_world.regions.count(capId)) {
                    int gold = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT);
                    if (gold >= 5000 && (thread_safe_rand() % 100) < 10) {
                        consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, 5000);
                        int mercDmg = 500 + (thread_safe_rand() % 500);
                        it->health -= mercDmg;
                        addNews("ЗАКАЗ НА ЧУДОВИЩЕ: Фракция " + g_world.factions[r.factionId].name + " наняла героев. Чудовище " + it->name + " получило тяжелые раны!", it->region_id, 4, "war");
                    }
                }
            }

            if (it->days_active % 7 == 0 && (thread_safe_rand() % 100) < 30) {
                std::vector<std::string> neighbors;
                for (const auto& road : g_world.map.roads) {
                    if (road.from == it->region_id) neighbors.push_back(road.to);
                    if (road.to == it->region_id) neighbors.push_back(road.from);
                }
                if (!neighbors.empty()) {
                    std::string best_reg = neighbors[0];
                    double best_score = -1;
                    for (const auto& n : neighbors) {
                        if (g_world.regions.count(n)) {
                            double score = g_world.regions[n].population + g_world.regions[n].moneySupply;
                            if (score > best_score) { best_score = score; best_reg = n; }
                        }
                    }
                    addNews("МИГРАЦИЯ ЧУДОВИЩА: " + it->name + " покинуло " + r.name + " и направилось в " + g_world.regions[best_reg].name + "!", best_reg, 5, "disaster");
                    it->region_id = best_reg;
                    if (g_world.map.locations.count(best_reg)) {
                        it->lair_x = g_world.map.locations[best_reg].x;
                        it->lair_y = g_world.map.locations[best_reg].y;
                        
                        int radius = 2;
                        TileType corruptType = TileType::ASH;
                        if (it->type == MonsterType::LICH_KING || it->type == MonsterType::VAMPIRE_LORD) corruptType = TileType::SWAMP;
                        if (it->type == MonsterType::KRAKEN || it->type == MonsterType::LEVIATHAN) corruptType = TileType::SHALLOW_WATER;
                        
                        for (int dy = -radius; dy <= radius; dy++) {
                            for (int dx = -radius; dx <= radius; dx++) {
                                if (std::hypot(dx, dy) <= radius) {
                                    int nx = it->lair_x + dx;
                                    int ny = it->lair_y + dy;
                                    if (nx >= 0 && nx < g_world.map.width && ny >= 0 && ny < g_world.map.height) {
                                        g_world.map.grid[ny * g_world.map.width + nx].type = corruptType;
                                    }
                                }
                            }
                        }
                        g_world.map.generation_tick = g_world.tick;
                    }
                }
            }
        }
        ++it;
    }
}



void dailyTick() {
    g_world.current_day++;
    
    // Очистка фантомных регионов (багфикс)
    for (auto it = g_world.regions.begin(); it != g_world.regions.end(); ) {
        if (it->second.name.empty() && it->second.population == 0 && it->second.vault_id.empty()) {
            it = g_world.regions.erase(it);
        } else {
            ++it;
        }
    }

    if (!g_bootstrap) {
        int activeWars = 0;
        for (auto& [fid, faction] : g_world.factions) {
            for (auto& [tid, status] : faction.diplomacy) {
                if (status == "war") activeWars++;
            }
        }
        activeWars /= 2;
        if (activeWars >= 2) {
            g_world.homeostasis.warWeariness = std::min(100, g_world.homeostasis.warWeariness + 4);
            g_world.homeostasis.peaceBoredom = 0;
        } else if (activeWars == 0) {
            g_world.homeostasis.warWeariness = std::max(0, g_world.homeostasis.warWeariness - 2);
            g_world.homeostasis.peaceBoredom++;
        } else {
            g_world.homeostasis.peaceBoredom = 0;
        }
    }
    
    processSpoilage();
    processLogistics();
    processPrivateProduction();
    processDailyEconomy();
    processMarkets();
    processFarmers();
    processGatherers();
    processArtisans();
    processMages();
    processServices();
    
    if (!g_bootstrap) {
        processDailyMilitary();

        processNavalCombat();
        processRulerDiplomacy();
        processIntrigues();
        processDiplomacy();
        processInternalPolitics();
    }

    processMerchants();

    processNavalTrade();
    
    if (!g_bootstrap) {
        processDailyThreat();
        processDreadAndMonsters();
        processMonsterHunts();
        checkRulerDeaths();
    }
    processDailyNPCs();

    processShipyards();

    if (!g_bootstrap) {
        processDisasters();
    }



}

void processNavalCombat() {
    for (const auto& [rid, port] : g_world.port_facilities) {
        if (g_world.regions.count(rid)) {
            Region& r = g_world.regions[rid];
            int pirate_base_chance = (r.weather == "Туман") ? 10 : 2;
            if (r.threat_level > 80 && (thread_safe_rand() % 100) < pirate_base_chance) {
                auto loc = g_world.map.locations[rid];
                int bx = loc.x + (rand()%15 - 7);
                int by = loc.y + (rand()%15 - 7);
                bx = std::clamp(bx, 1, g_world.map.width - 2);
                by = std::clamp(by, 1, g_world.map.height - 2);
                if (g_world.map.grid[by * g_world.map.width + bx].type == TileType::OCEAN) {
                    std::string baseId = "pirate_base_" + generateUUID();
                    MapLocation pBase;
                    pBase.id = baseId;
                    pBase.name = "Пиратская бухта";
                    pBase.x = bx; pBase.y = by;
                    pBase.type = "pirate_base";
                    pBase.faction = "pirates";
                    g_world.map.locations[baseId] = pBase;
                    addNews("ПИРАТЫ: В водах близ " + r.name + " обнаружена скрытая пиратская база!", rid, 4, "war");
                }
            }
            if (r.threat_level > 60 && (thread_safe_rand() % 100) < 5) {
                Ship p;
                p.id = "ship_" + generateUUID();
                p.owner_id = "pirates";
                p.type = ShipType::PIRATE;
                p.hull = 100;
                p.sailors = 20;
                p.cargo_capacity = 200;
                p.chest_id = createContainer("ship_hold", "pirates", 999999, 100, rid);
                p.speed = 1.8;
                p.cannons = 5;
                p.marines = 15;
                if (g_world.map.locations.count(rid)) {
                    p.x = g_world.map.locations[rid].x;
                    p.y = g_world.map.locations[rid].y;
                }
                for (const auto& target : g_world.ships) {
                    if (target.type == ShipType::MERCHANT && target.owner_id != "pirates") {
                        p.destination = target.destination;
                        p.path = target.path;
                        p.path_index = target.path_index;
                        break;
                    }
                }
                g_world.ships.push_back(p);
                addNews("ПИРАТЫ: В водах близ " + r.name + " замечены пиратские корабли!", rid, 3, "war");
            }
        }
    }

    for (size_t i = 0; i < g_world.fleets.size(); i++) {
        for (size_t j = i + 1; j < g_world.fleets.size(); j++) {
            Fleet& f1 = g_world.fleets[i];
            Fleet& f2 = g_world.fleets[j];
            double dist = std::hypot(f1.x - f2.x, f1.y - f2.y);
            if (dist <= 4.0) {
                bool hostile = false;
                if (g_world.factions.count(f1.owner_id) && g_world.factions.count(f2.owner_id)) {
                    if (g_world.factions[f1.owner_id].diplomacy[f2.owner_id] == "war") hostile = true;
                }
                if (hostile) {
                    int f1_cannons = 0, f2_cannons = 0;
                    int f1_marines = 0, f2_marines = 0;
                    for (auto& s : g_world.ships) {
                        if (s.fleet_id == f1.id && s.hull > 0) { f1_cannons += s.cannons; f1_marines += s.marines; }
                        if (s.fleet_id == f2.id && s.hull > 0) { f2_cannons += s.cannons; f2_marines += s.marines; }
                    }
                    if ((f1_cannons > 0 || f1_marines > 0) && (f2_cannons > 0 || f2_marines > 0)) {
                        std::string n1 = g_world.factions.count(f1.owner_id) ? g_world.factions[f1.owner_id].name : f1.owner_id;
                        std::string n2 = g_world.factions.count(f2.owner_id) ? g_world.factions[f2.owner_id].name : f2.owner_id;
                        if (dist > 1.0) {
                            for (auto& s : g_world.ships) {
                                if (s.fleet_id == f1.id && s.hull > 0) s.hull -= (f2_cannons * 2) / std::max(1, (int)f1.ship_ids.size());
                                if (s.fleet_id == f2.id && s.hull > 0) s.hull -= (f1_cannons * 2) / std::max(1, (int)f2.ship_ids.size());
                            }
                            addNews("МОРСКОЙ БОЙ (Артиллерия): Флоты " + n1 + " и " + n2 + " ведут перестрелку на дистанции!", "global", 4, "war");
                            double dx = f2.x - f1.x; double dy = f2.y - f1.y;
                            f1.x += (dx / dist) * 0.5; f1.y += (dy / dist) * 0.5;
                        } else {
                            if (f1_marines > f2_marines) {
                                for (auto& s : g_world.ships) if (s.fleet_id == f2.id && s.hull > 0) { s.owner_id = f1.owner_id; s.fleet_id = f1.id; s.marines = 0; }
                                addNews("МОРСКОЙ БОЙ (Абордаж): Флот " + n1 + " взял на абордаж и захватил корабли " + n2 + "!", "global", 5, "war");
                            } else {
                                for (auto& s : g_world.ships) if (s.fleet_id == f1.id && s.hull > 0) { s.owner_id = f2.owner_id; s.fleet_id = f2.id; s.marines = 0; }
                                addNews("МОРСКОЙ БОЙ (Абордаж): Флот " + n2 + " взял на абордаж и захватил корабли " + n1 + "!", "global", 5, "war");
                            }
                        }
                    }
                }
            }
        }
    }


    for (size_t i = 0; i < g_world.ships.size(); i++) {
        for (size_t j = i + 1; j < g_world.ships.size(); j++) {
            Ship& s1 = g_world.ships[i];
            Ship& s2 = g_world.ships[j];
            if (s1.hull <= 0 || s2.hull <= 0) continue;
            
            bool hostile = false;
            if (s1.owner_id == "pirates" || s2.owner_id == "pirates") hostile = true;
            else if (g_world.factions.count(s1.owner_id) && g_world.factions.count(s2.owner_id)) {
                if (g_world.factions[s1.owner_id].diplomacy[s2.owner_id] == "war") hostile = true;
            }
            
            if (hostile) {
                double dist = std::hypot(s1.x - s2.x, s1.y - s2.y);
                if (dist <= 2.0) {
                    s2.hull -= s1.cannons * (0.5 + (thread_safe_rand() % 10) / 10.0) * 10;
                    s1.hull -= s2.cannons * (0.5 + (thread_safe_rand() % 10) / 10.0) * 10;
                    
                    if (s1.hull > 0 && s2.hull > 0 && dist <= 1.0) {
                        int s1_power = s1.marines + s1.sailors / 2;
                        int s2_power = s2.marines + s2.sailors / 2;
                        if (s1_power > s2_power) {
                            s2.marines = 0; s2.sailors /= 2; s2.hull -= 20;
                        } else {
                            s1.marines = 0; s1.sailors /= 2; s1.hull -= 20;
                        }
                    }
                    addNews("МОРСКОЙ БОЙ: Столкновение кораблей (" + s1.owner_id + " vs " + s2.owner_id + ").", "global", 3, "war");
                }
            }
        }
    }
    
    for (const auto& [lid, loc] : g_world.map.locations) {
        int spawn_chance = 10;
        if (g_world.regions.count(lid) && g_world.regions[lid].weather == "Туман") spawn_chance = 30;
        if (loc.type == "pirate_base" && (thread_safe_rand() % 100) < spawn_chance) {
            Ship p;
            p.id = "ship_" + generateUUID();
            p.owner_id = "pirates";
            p.type = ShipType::PIRATE;
            p.hull = 100; p.sailors = 20; p.cargo_capacity = 200;
            p.chest_id = createContainer("ship_hold", "pirates", 999999, 100, lid);
            p.speed = 1.8; p.cannons = 5; p.marines = 15;
            p.x = loc.x; p.y = loc.y;
            for (const auto& target : g_world.ships) {
                if (target.type == ShipType::MERCHANT && target.owner_id != "pirates") {
                    p.destination = target.destination;
                    p.path = target.path;
                    p.path_index = target.path_index;
                    break;
                }
            }
            g_world.ships.push_back(p);
        }
    }
    
    for (auto& ship : g_world.ships) {

        if (ship.type == ShipType::SEA_MONSTER) {
            double min_dist = 9999;
            Ship* target = nullptr;
            for (auto& s : g_world.ships) {
                if (s.id != ship.id && s.type != ShipType::SEA_MONSTER && s.hull > 0) {
                    double d = std::hypot(s.x - ship.x, s.y - ship.y);
                    if (d < min_dist) { min_dist = d; target = &s; }
                }
            }
            if (target) {
                if (min_dist <= 1.5) {
                    if (target->cannons > 0 || target->marines > 0) {
                        ship.hull -= (target->cannons * 10 + target->marines * 5);
                        target->hull -= 30;
                        addNews("БИТВА С ЧУДОВИЩЕМ: Военный корабль сражается с морским левиафаном!", "global", 4, "war");
                        if (ship.hull <= 0) addNews("ПОБЕДА: Морское чудовище убито военным флотом!", "global", 5, "war");
                    } else {
                        target->hull -= 50;
                        addNews("КАТАСТРОФА: Морское чудовище атакует торговое судно!", "global", 4, "disaster");
                    }
                } else {
                    double dx = target->x - ship.x;
                    double dy = target->y - ship.y;
                    ship.x += (dx / min_dist) * ship.speed;
                    ship.y += (dy / min_dist) * ship.speed;
                }
            }
            continue;
        }
        if (ship.type == ShipType::WAR_GALLEY || ship.type == ShipType::WAR_FRIGATE) {
            for (auto it = g_world.map.locations.begin(); it != g_world.map.locations.end(); ) {
                if (it->second.type == "pirate_base") {
                    if (std::hypot(ship.x - it->second.x, ship.y - it->second.y) <= 2.0) {
                        std::string nearest_region = "";
                        double min_d = 9999;
                        for (const auto& [rid, rloc] : g_world.map.locations) {
                            if (rloc.type != "pirate_base" && g_world.regions.count(rid)) {
                                double d = std::hypot(it->second.x - rloc.x, it->second.y - rloc.y);
                                if (d < min_d) { min_d = d; nearest_region = rid; }
                            }
                        }
                        if (!nearest_region.empty()) {
                            g_world.regions[nearest_region].threat_level = std::max(0, g_world.regions[nearest_region].threat_level - 20);
                        }
                        
                        if (g_world.factions.count(ship.owner_id)) {
                            std::string capId = g_world.factions[ship.owner_id].regions.empty() ? "" : g_world.factions[ship.owner_id].regions[0];
                            if (!capId.empty() && g_world.regions.count(capId)) {
                                createItem(GoodType::GOLD_INGOT, 2000, g_world.regions[capId].vault_id, g_world.current_day, "Награда за пиратов");
                            }
                        }
                        
                        addNews("ФЛОТ: Военный корабль фракции " + g_world.factions[ship.owner_id].name + " уничтожил пиратскую базу! Угроза в регионе снижена, захвачены сокровища.", "global", 4, "war");
                        it = g_world.map.locations.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
    }

    for (auto it = g_world.ships.begin(); it != g_world.ships.end(); ) {
        if (it->hull <= 0) {
            addNews("КОРАБЛЕКРУШЕНИЕ: Судно фракции " + it->owner_id + " пошло ко дну.", "global", 3, "disaster");
            if (g_containers.count(it->chest_id)) {
                g_deleted_containers.push_back(it->chest_id);
                g_containers.erase(it->chest_id);
            }
            it = g_world.ships.erase(it);
        } else {
            ++it;
        }
    }
}


void processDisasters() {
    std::vector<Disaster> new_disasters;

    for (auto it = g_world.map.disasters.begin(); it != g_world.map.disasters.end(); ) {
        it->days_active--;
        if (it->days_active <= 0) {
            if (it->type == "flood") {
                for (auto pt : it->affected_tiles) {
                    int idx = pt.second * g_world.map.width + pt.first;
                    g_world.map.grid[idx].is_flooded = false;
                }
                g_world.map.generation_tick = g_world.tick;
                g_path_cache_dirty = true;
                addNews("Наводнение отступило. Вода возвращается в берега.", "global", 2, "disaster");
            } else if (it->type == "volcano") {
                addNews("Извержение вулкана прекратилось, но лава и пепел навсегда изменили ландшафт.", "global", 3, "disaster");
            }
            
            // Каскадные катастрофы
            if (it->type == "earthquake" && (thread_safe_rand() % 100) < 30) {
                Disaster cascade;
                cascade.id = "dis_" + generateUUID();
                cascade.type = "flood"; // Цунами/наводнение
                cascade.epicenter_x = it->epicenter_x;
                cascade.epicenter_y = it->epicenter_y;
                cascade.radius = it->radius + 2;
                cascade.strength = it->strength;
                cascade.days_active = 5;
                cascade.affected_regions = it->affected_regions;
                new_disasters.push_back(cascade);
                addNews("КАСКАДНОЕ БЕДСТВИЕ: Землетрясение вызвало разрушительное цунами/наводнение!", "global", 5, "disaster");
            }

            it = g_world.map.disasters.erase(it);
        } else {
            for (const auto& rid : it->affected_regions) {
                if (g_world.regions.count(rid)) {
                    Region& r = g_world.regions[rid];
                    int defense = r.custom_props.has("disaster_defense") ? r.custom_props["disaster_defense"].asInt() : 0;
                    int eff_str = std::max(1, it->strength - defense);

                    if (it->type == "storm" || it->type == "sea_monster") {
                        for (auto& ship : g_world.ships) {

        if (ship.type == ShipType::SEA_MONSTER) {
            double min_dist = 9999;
            Ship* target = nullptr;
            for (auto& s : g_world.ships) {
                if (s.id != ship.id && s.type != ShipType::SEA_MONSTER && s.hull > 0) {
                    double d = std::hypot(s.x - ship.x, s.y - ship.y);
                    if (d < min_dist) { min_dist = d; target = &s; }
                }
            }
            if (target) {
                if (min_dist <= 1.5) {
                    if (target->cannons > 0 || target->marines > 0) {
                        ship.hull -= (target->cannons * 10 + target->marines * 5);
                        target->hull -= 30;
                        addNews("БИТВА С ЧУДОВИЩЕМ: Военный корабль сражается с морским левиафаном!", "global", 4, "war");
                        if (ship.hull <= 0) addNews("ПОБЕДА: Морское чудовище убито военным флотом!", "global", 5, "war");
                    } else {
                        target->hull -= 50;
                        addNews("КАТАСТРОФА: Морское чудовище атакует торговое судно!", "global", 4, "disaster");
                    }
                } else {
                    double dx = target->x - ship.x;
                    double dy = target->y - ship.y;
                    ship.x += (dx / min_dist) * ship.speed;
                    ship.y += (dy / min_dist) * ship.speed;
                }
            }
            continue;
        }
                            if (std::hypot(ship.x - it->epicenter_x, ship.y - it->epicenter_y) <= it->radius) {
                                ship.hull -= (it->type == "sea_monster") ? 30 : 10;
                            }
                        }
                        for (auto& road : g_world.map.roads) {
                            if (road.type == "sea_route") {
                                for (auto wp : road.waypoints) {
                                    if (std::hypot(wp.first - it->epicenter_x, wp.second - it->epicenter_y) <= it->radius) {
                                        road.integrity -= 50;
                                        if (road.integrity <= 0) {
                                            road.condition = "ruined";
                                            road.integrity = 0;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    } else if (it->type == "tsunami") {
                        r.population = std::max(0, r.population - eff_str * 50);
                        if (g_world.port_facilities.count(rid)) {
                            g_world.port_facilities[rid].durability -= eff_str * 10;
                            std::string dockId = g_world.port_facilities[rid].dock_container_id;
                            if (!dockId.empty() && g_containers.count(dockId)) {
                                Storage& dock = g_containers[dockId];
                                std::vector<std::string> items_to_check = dock.item_ids;
                                for (const auto& itemId : items_to_check) {
                                    if (g_items.count(itemId)) {
                                        PhysicalItem& item = g_items[itemId];
                                        int lost = std::max(1, (int)(item.stack_size * (0.1 * eff_str)));
                                        consumeItemsFromContainer(dockId, item.prototype_id, lost);
                                    }
                                }
                            }
                        }
                    } else if (it->type == "earthquake") {
                        for (auto& [fid, fac] : r.facilities) {
                            fac.durability -= eff_str * 2;
                            if (fac.durability < 0) fac.durability = 0;
                        }
                    } else if (it->type == "flood" || it->type == "wildfire" || it->type == "monster_invasion") {
                        r.population = std::max(0, r.population - eff_str * 2);
                        int foodLost = countItemsInContainer(r.vault_id, GoodType::BREAD) * (0.05 * eff_str);
                        consumeItemsFromContainer(r.vault_id, GoodType::BREAD, foodLost);
                    }

                    if (it->type == "drought") {
                        r.fertility = 0.0;
                        r.planned_harvests.clear();
                        r.starvation_days += 2;
                        if ((thread_safe_rand() % 100) < 5) {
                            Disaster cascade;
                            cascade.id = "dis_" + generateUUID();
                            cascade.type = "wildfire";
                            cascade.epicenter_x = it->epicenter_x;
                            cascade.epicenter_y = it->epicenter_y;
                            cascade.radius = it->radius;
                            cascade.strength = it->strength;
                            cascade.days_active = 3;
                            cascade.affected_regions = it->affected_regions;
                            new_disasters.push_back(cascade);
                            addNews("КАСКАДНОЕ БЕДСТВИЕ: Засуха спровоцировала лесные пожары!", rid, 4, "disaster");
                        }
                    } else if (it->type == "plague") {
                        r.population = std::max(0, r.population - eff_str * 10);
                        if ((thread_safe_rand() % 100) < 20) r.stability = std::max(0, r.stability - 1);
                        r.labor_force = std::max(0, r.labor_force - eff_str * 5);
                        for (auto& [k, v] : r.prodModifiers) v *= 0.5; // Длительный штраф к производству
                    } else if (it->type == "aether_storm") {
                        createItem(GoodType::ETHER_DUST, eff_str * 5, r.vault_id, g_world.current_day, "Эфирный шторм");
                        for (auto& c : r.caravans) {
                            if ((thread_safe_rand() % 100) < 20) {
                                c.x += (thread_safe_rand() % 10) - 5;
                                c.y += (thread_safe_rand() % 10) - 5;
                                c.path.clear();
                            }
                        }
                        for (auto& [fid, fac] : g_world.factions) {
                            for (auto& a : fac.armies) {
                                if (a.location == rid || a.destination == rid) {
                                    if ((thread_safe_rand() % 100) < 20) {
                                        a.x += (thread_safe_rand() % 10) - 5;
                                        a.y += (thread_safe_rand() % 10) - 5;
                                        a.path.clear();
                                    }
                                }
                            }
                        }
                    } else if (it->type == "monster_invasion") {
                        r.threat_level = 100;
                        r.population = std::max(0, r.population - eff_str * 15); // Убийство населения
                        for (auto& road : g_world.map.roads) {
                            if (road.from == rid || road.to == rid) {
                                road.condition = "ruined";
                                road.integrity = 0;
                            }
                        }
                        g_path_cache_dirty = true;
                    } else if (it->type == "volcano" && it->days_active == it->strength * 2 - 1) {
                        for (auto pt : it->affected_tiles) {
                            int idx = pt.second * g_world.map.width + pt.first;
                            if (std::hypot(pt.first - it->epicenter_x, pt.second - it->epicenter_y) <= it->radius / 2.0) {
                                g_world.map.grid[idx].type = TileType::LAVA;
                            } else {
                                g_world.map.grid[idx].type = TileType::ASH;
                            }
                        }
                        r.fertility *= 0.5;
                        g_world.map.generation_tick = g_world.tick;
                    }
                }
            }
            ++it;
        }
    }

    for (const auto& d : new_disasters) {
        g_world.map.disasters.push_back(d);
    }

    if ((thread_safe_rand() % 1000) < 5) {
        if (g_world.regions.empty()) return;
        auto it = g_world.regions.begin();
        std::advance(it, thread_safe_rand() % g_world.regions.size());
        std::string rid = it->first;
        Region& r = it->second;
        
        if (!g_world.map.locations.count(rid)) return;
        auto loc = g_world.map.locations[rid];

        std::string riskKey = rid + "_disaster_count";
        int risk = g_world.nexusData.count(riskKey) ? g_world.nexusData[riskKey].asInt() : 0;
        g_world.nexusData[riskKey] = JsonValue(risk + 1);

        Disaster d;
        d.id = "dis_" + generateUUID();
        d.epicenter_x = loc.x;
        d.epicenter_y = loc.y;
        d.radius = 5 + (thread_safe_rand() % 10);
        d.strength = 3 + (thread_safe_rand() % 7);
        d.affected_regions.push_back(rid);

        std::vector<std::string> types = {"earthquake", "monster_invasion", "aether_storm"};
        if (r.population > r.storage_capacity * 1.2 && (!r.custom_props.has("has_well") || !r.custom_props["has_well"].asBool())) {
            types.push_back("plague");
        }
        if (r.weather == "Тропический ливень" || r.weather == "Дождь") types.push_back("flood");
        if (r.weather == "Жара") { types.push_back("wildfire"); types.push_back("drought"); }
        if (g_world.map.grid[loc.y * g_world.map.width + loc.x].type == TileType::VOLCANO) types.push_back("volcano");

        if (r.available_raw_resources.count(GoodType::FISH)) {
            types.push_back("storm");
            types.push_back("tsunami");
            types.push_back("sea_monster");
        }

        d.type = types[thread_safe_rand() % types.size()];

        if (d.type == "flood") {
            if (r.custom_props.has("has_dam") && r.custom_props["has_dam"].asBool()) {
                addNews("СТИХИЯ УКРОЩЕНА: Дамба в " + r.name + " выдержала напор воды, предотвратив катастрофическое наводнение.", rid, 3, "politics");
                return;
            }
            d.days_active = 5 + (thread_safe_rand() % 10);
            for (int y = std::max(0, d.epicenter_y - d.radius); y <= std::min(g_world.map.height - 1, d.epicenter_y + d.radius); ++y) {
                for (int x = std::max(0, d.epicenter_x - d.radius); x <= std::min(g_world.map.width - 1, d.epicenter_x + d.radius); ++x) {
                    if (std::hypot(x - d.epicenter_x, y - d.epicenter_y) <= d.radius) {
                        int idx = y * g_world.map.width + x;
                        TileType t = g_world.map.grid[idx].type;
                        if (t == TileType::RIVERBANK || t == TileType::FLOODPLAIN || t == TileType::PLAINS) {
                            g_world.map.grid[idx].is_flooded = true;
                            d.affected_tiles.push_back({x, y});
                            if (g_world.map.grid[idx].bridge_flag && (thread_safe_rand() % 100) < 30) {
                                for (auto& road : g_world.map.roads) {
                                    if (road.type == "bridge") {
                                        for (auto wp : road.waypoints) {
                                            if (wp.first == x && wp.second == y) {
                                                road.condition = "ruined";
                                                road.integrity = 0;
                                                g_path_cache_dirty = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            g_world.map.generation_tick = g_world.tick;
            addNews("НАВОДНЕНИЕ! Реки вышли из берегов в регионе " + r.name + ".", rid, 4, "disaster");
        } else if (d.type == "earthquake") {
            d.days_active = 1;
            for (auto& road : g_world.map.roads) {
                if (road.type == "tunnel") continue; // Тоннели неуязвимы к землетрясениям
                if (road.from == rid || road.to == rid) {
                    if ((thread_safe_rand() % 100) < 40) {
                        road.condition = "ruined";
                        road.integrity = 0;
                        g_path_cache_dirty = true;
                    }
                }
            }
            addNews("ЗЕМЛЕТРЯСЕНИЕ! Мощные толчки сотрясли " + r.name + ", разрушая здания и дороги.", rid, 5, "disaster");
        } else if (d.type == "wildfire") {
            d.days_active = 3 + (thread_safe_rand() % 7);
            for (int y = std::max(0, d.epicenter_y - d.radius); y <= std::min(g_world.map.height - 1, d.epicenter_y + d.radius); ++y) {
                for (int x = std::max(0, d.epicenter_x - d.radius); x <= std::min(g_world.map.width - 1, d.epicenter_x + d.radius); ++x) {
                    if (std::hypot(x - d.epicenter_x, y - d.epicenter_y) <= d.radius) {
                        int idx = y * g_world.map.width + x;
                        if (g_world.map.grid[idx].type == TileType::FOREST) {
                            g_world.map.grid[idx].type = TileType::ASH;
                            d.affected_tiles.push_back({x, y});
                        }
                    }
                }
            }
            g_world.map.generation_tick = g_world.tick;
            addNews("ЛЕСНОЙ ПОЖАР! Леса вокруг " + r.name + " охвачены пламенем.", rid, 4, "disaster");
        } else if (d.type == "plague") {
            d.days_active = 14 + (thread_safe_rand() % 14);
            r.stability -= 20;
            addNews("ЭПИДЕМИЯ! Смертельная болезнь распространяется в " + r.name + ".", rid, 5, "disaster");
        } else if (d.type == "drought") {
            d.days_active = 10 + (thread_safe_rand() % 20);
            for (int y = std::max(0, d.epicenter_y - d.radius); y <= std::min(g_world.map.height - 1, d.epicenter_y + d.radius); ++y) {
                for (int x = std::max(0, d.epicenter_x - d.radius); x <= std::min(g_world.map.width - 1, d.epicenter_x + d.radius); ++x) {
                    if (std::hypot(x - d.epicenter_x, y - d.epicenter_y) <= d.radius) {
                        int idx = y * g_world.map.width + x;
                        TileType t = g_world.map.grid[idx].type;
                        if (t == TileType::PLAINS || t == TileType::FOREST || t == TileType::SWAMP || t == TileType::FLOODPLAIN || t == TileType::RIVERBANK) {
                            g_world.map.grid[idx].type = TileType::DESERT;
                            d.affected_tiles.push_back({x, y});
                        }
                    }
                }
            }
            g_world.map.generation_tick = g_world.tick;
            addNews("ЗАСУХА! Урожай в " + r.name + " гибнет под палящим солнцем.", rid, 4, "disaster");
        } else if (d.type == "aether_storm") {
            d.days_active = 2 + (thread_safe_rand() % 4);
            addNews("ЭФИРНЫЙ ШТОРМ! Магические аномалии искажают реальность в " + r.name + ".", rid, 5, "disaster");
        } else if (d.type == "monster_invasion") {
            d.days_active = 7 + (thread_safe_rand() % 14);
            addNews("НАШЕСТВИЕ МОНСТРОВ! Орды чудовищ атаковали " + r.name + ", перерезая дороги.", rid, 5, "disaster");
        } else if (d.type == "volcano") {
            d.days_active = d.strength * 2;
            for (int y = std::max(0, d.epicenter_y - d.radius); y <= std::min(g_world.map.height - 1, d.epicenter_y + d.radius); ++y) {
                for (int x = std::max(0, d.epicenter_x - d.radius); x <= std::min(g_world.map.width - 1, d.epicenter_x + d.radius); ++x) {
                    if (std::hypot(x - d.epicenter_x, y - d.epicenter_y) <= d.radius) {
                        d.affected_tiles.push_back({x, y});
                    }
                }
            }
            addNews("ИЗВЕРЖЕНИЕ ВУЛКАНА! Лава и пепел уничтожают всё вокруг " + r.name + ".", rid, 5, "disaster");
        } else if (d.type == "storm") {
            d.days_active = 2 + (thread_safe_rand() % 3);
            addNews("ШТОРМ! Яростные волны топят корабли у берегов " + r.name + ".", rid, 4, "disaster");
        } else if (d.type == "tsunami") {
            d.days_active = 1;
            addNews("ЦУНАМИ! Гигантская волна обрушилась на порты " + r.name + "!", rid, 5, "disaster");
        } else if (d.type == "sea_monster") {
            d.days_active = 5 + (thread_safe_rand() % 5);
            addNews("МОРСКОЕ ЧУДОВИЩЕ! Кракен замечен в водах " + r.name + ", торговля парализована.", rid, 5, "disaster");
        }

        g_world.map.disasters.push_back(d);
    }
}

void processRoadDegradation() {
    for (auto& road : g_world.map.roads) {
        if (road.condition == "ruined") continue;
        if (road.type == "tunnel") continue; // Тоннели неуязвимы к погоде и износу
        
        int deg = 1;
        if (road.type == "dirt") deg = 2;
        if (road.type == "bridge") deg = 3;
        
        road.integrity -= deg;
        if (road.integrity <= 0) {
            road.integrity = 0;
            road.condition = "ruined";
            g_path_cache_dirty = true;
            addNews("ИНФРАСТРУКТУРА: Дорога между " + road.from + " и " + road.to + " пришла в полную негодность.", road.from, 2, "logistics");
        }
    }

    for (auto& [fid, f] : g_world.factions) {
        std::string capId = f.regions.empty() ? "" : f.regions[0];
        if (capId.empty() || !g_world.regions.count(capId)) continue;
        int gold = countItemsInContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT);
        
        for (auto& road : g_world.map.roads) {
            bool from_match = g_world.regions.count(road.from) && g_world.regions[road.from].factionId == fid;
            bool to_match = g_world.regions.count(road.to) && g_world.regions[road.to].factionId == fid;
            if (road.integrity < 50 && (from_match || to_match)) {
                int cost = (100 - road.integrity) * 10;
                if (road.type == "sea_route") cost = (100 - road.integrity) * 5;
                if (gold >= cost) {
                    consumeItemsFromContainer(g_world.regions[capId].vault_id, GoodType::GOLD_INGOT, cost);
                    gold -= cost;
                    road.integrity = 100;
                    if (road.condition == "ruined") {
                        road.condition = road.type == "paved" ? "paved" : (road.type == "sea_route" ? "water" : "dirt");
                        g_path_cache_dirty = true;
                        std::string msg = (road.type == "sea_route") ? "восстановила буи и маяки на морском пути" : "восстановила дорогу";
                        addNews("ИНФРАСТРУКТУРА: Фракция " + f.name + " " + msg + " между " + road.from + " и " + road.to + ".", road.from, 2, "logistics");
                    }
                }
            }
        }
    }
}

void processFactionTrade() {
    std::unordered_map<std::string, std::vector<int>> vaultStocks;
    {
        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
        for (const auto& [rid, r] : g_world.regions) {
            if (!r.vault_id.empty() && g_containers.count(r.vault_id)) {
                vaultStocks[rid] = g_containers[r.vault_id].cached_stocks;
            } else {
                vaultStocks[rid].assign((int)GoodType::COUNT, 0);
            }
        }
    }

    for (auto& [fid, f] : g_world.factions) {
        if (f.regions.empty()) continue;
        std::string capitalId = f.regions[0];
        if (!g_world.regions.count(capitalId)) continue;
        
        int totalFood = 0;
        int totalPop = 0;
        int capitalGold = vaultStocks[capitalId][(int)GoodType::GOLD_INGOT];
        
        for (const auto& rid : f.regions) {
            if (g_world.regions.count(rid)) {
                totalPop += g_world.regions[rid].population;
                totalFood += vaultStocks[rid][(int)GoodType::BREAD] + vaultStocks[rid][(int)GoodType::MEAT];
            }
        }
        
        double foodRatio = totalPop > 0 ? (double)totalFood / totalPop : 1.0;
        
        // 1. Внутреннее перераспределение (из столицы в голодающие регионы)
        if (foodRatio > 0.3) {
            int capFood = vaultStocks[capitalId][(int)GoodType::BREAD];
            if (capFood > g_world.regions[capitalId].population * 0.5) {
                for (const auto& rid : f.regions) {
                    if (rid == capitalId) continue;
                    if (g_world.regions.count(rid) && g_world.regions[rid].starvation_days > 3) {
                        int sendAmount = std::min(capFood / 2, g_world.regions[rid].population / 2);
                        if (sendAmount > 100) {
                            std::vector<std::pair<int,int>> caravan_path;
                            if (g_path_cache.count({capitalId, rid})) caravan_path = g_path_cache[{capitalId, rid}];
                            if (caravan_path.empty()) continue;

                            std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                            consumeItemsFromContainer(g_world.regions[capitalId].vault_id, GoodType::BREAD, sendAmount);
                            vaultStocks[capitalId][(int)GoodType::BREAD] -= sendAmount;
                            
                            std::string chestId = createContainer("caravan_chest", fid, 999999, 1000, capitalId);
                            createItem(GoodType::BREAD, sendAmount, chestId, g_world.current_day, "Внутренние поставки");
                            
                            Caravan caravan;
                            caravan.id = "caravan_" + generateUUID();
                            caravan.origin = capitalId;
                            caravan.destination = rid;
                            caravan.chest_id = chestId;
                            caravan.wagons = 1 + (sendAmount / 50);
                            caravan.guards = 5;
                            caravan.hoursLeft = 24 + (rand() % 48);
                            
                            if (g_path_cache.count({capitalId, rid})) {
                                caravan.path = g_path_cache[{capitalId, rid}];
                                if (!caravan.path.empty()) { caravan.x = caravan.path[0].first; caravan.y = caravan.path[0].second; }
                            }
                            g_world.regions[capitalId].caravans.push_back(caravan);
                            addNews("СНАБЖЕНИЕ: Фракция " + f.name + " отправила обоз с продовольствием в голодающий регион " + g_world.regions[rid].name + ".", rid, 2, "logistics");
                        }
                    }
                }
            }
        }
        
        // 2. Внешние закупки (Импорт), если фракция голодает, но есть деньги
        if (foodRatio < 0.2 && capitalGold > 5000) {
            std::string bestSellerId = "";
            int bestSellerFood = 0;
            
            for (const auto& [ofid, of] : g_world.factions) {
                if (ofid == fid || f.diplomacy[ofid] == "war") continue; // Не торгуем с врагами
                if (of.regions.empty()) continue;
                std::string oCapId = of.regions[0];
                if (g_world.regions.count(oCapId)) {
                    int oFood = vaultStocks[oCapId][(int)GoodType::BREAD];
                    if (oFood > g_world.regions[oCapId].population * 1.0 && oFood > bestSellerFood) {
                        bestSellerFood = oFood;
                        bestSellerId = ofid;
                    }
                }
            }
            
            if (!bestSellerId.empty()) {
                std::string sellerCapId = g_world.factions[bestSellerId].regions[0];
                int buyAmount = std::min(bestSellerFood / 3, capitalGold / 10); // Хлеб по 10з (завышенная цена за экспорт)
                if (buyAmount > 100) {
                    std::vector<std::pair<int,int>> caravan_path;
                    if (g_path_cache.count({sellerCapId, capitalId})) caravan_path = g_path_cache[{sellerCapId, capitalId}];
                    if (caravan_path.empty()) continue;

                    int cost = buyAmount * 10;
                    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                    consumeItemsFromContainer(g_world.regions[capitalId].vault_id, GoodType::GOLD_INGOT, cost);
                    vaultStocks[capitalId][(int)GoodType::GOLD_INGOT] -= cost;
                    
                    consumeItemsFromContainer(g_world.regions[sellerCapId].vault_id, GoodType::BREAD, buyAmount);
                    vaultStocks[sellerCapId][(int)GoodType::BREAD] -= buyAmount;
                    createItem(GoodType::GOLD_INGOT, cost, g_world.regions[sellerCapId].vault_id, g_world.current_day, "Экспорт продовольствия");
                    
                    std::string chestId = createContainer("caravan_chest", fid, 999999, 1000, sellerCapId);
                    createItem(GoodType::BREAD, buyAmount, chestId, g_world.current_day, "Импорт продовольствия");
                    
                    Caravan caravan;
                    caravan.id = "caravan_" + generateUUID();
                    caravan.origin = sellerCapId;
                    caravan.destination = capitalId;
                    caravan.chest_id = chestId;
                    caravan.wagons = 1 + (buyAmount / 50);
                    caravan.guards = 10;
                    caravan.hoursLeft = 48 + (rand() % 48);
                    
                    if (g_path_cache.count({sellerCapId, capitalId})) {
                        caravan.path = g_path_cache[{sellerCapId, capitalId}];
                        if (!caravan.path.empty()) { caravan.x = caravan.path[0].first; caravan.y = caravan.path[0].second; }
                    }
                    g_world.regions[sellerCapId].caravans.push_back(caravan);
                    addNews("ГОСЗАКУПКА: Голодающая фракция " + f.name + " закупила " + std::to_string(buyAmount) + " продовольствия у " + g_world.factions[bestSellerId].name + " за " + std::to_string(cost) + " золота.", capitalId, 4, "trade");
                }
            }
        }
        
        // 3. Гуманитарная помощь (Экспорт), если фракция процветает
        if (foodRatio > 1.5) {
            for (const auto& [ofid, of] : g_world.factions) {
                if (ofid == fid || f.diplomacy[ofid] == "war") continue; // Врагам не помогаем
                if (of.regions.empty()) continue;
                std::string oCapId = of.regions[0];
                if (g_world.regions.count(oCapId) && g_world.regions[oCapId].starvation_days > 5) {
                    int aidAmount = std::min(vaultStocks[capitalId][(int)GoodType::BREAD] / 4, g_world.regions[oCapId].population / 2);
                    if (aidAmount > 100) {
                        std::vector<std::pair<int,int>> caravan_path;
                        if (g_path_cache.count({capitalId, oCapId})) caravan_path = g_path_cache[{capitalId, oCapId}];
                        if (caravan_path.empty()) continue;

                        std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
                        consumeItemsFromContainer(g_world.regions[capitalId].vault_id, GoodType::BREAD, aidAmount);
                        vaultStocks[capitalId][(int)GoodType::BREAD] -= aidAmount;
                        
                        std::string chestId = createContainer("caravan_chest", fid, 999999, 1000, capitalId);
                        createItem(GoodType::BREAD, aidAmount, chestId, g_world.current_day, "Гуманитарная помощь");
                        
                        Caravan caravan;
                        caravan.id = "caravan_" + generateUUID();
                        caravan.origin = capitalId;
                        caravan.destination = oCapId;
                        caravan.chest_id = chestId;
                        caravan.wagons = 1 + (aidAmount / 50);
                        caravan.guards = 5;
                        caravan.hoursLeft = 48 + (rand() % 48);
                        
                        if (g_path_cache.count({capitalId, oCapId})) {
                            caravan.path = g_path_cache[{capitalId, oCapId}];
                            if (!caravan.path.empty()) { caravan.x = caravan.path[0].first; caravan.y = caravan.path[0].second; }
                        }
                        g_world.regions[capitalId].caravans.push_back(caravan);
                        
                        f.relations[ofid] = std::min(100, f.relations[ofid] + 30);
                        g_world.factions[ofid].relations[fid] = std::min(100, g_world.factions[ofid].relations[fid] + 30);
                        
                        addNews("ПОМОЩЬ: Фракция " + f.name + " отправила гуманитарный конвой с едой в страдающую от голода фракцию " + g_world.factions[ofid].name + ". Отношения значительно улучшились.", oCapId, 4, "diplomacy");
                        break; // Помогаем только одной фракции за раз
                    }
                }
            }
        }
    }
}

void weeklyTick() {
    processMigration();
    checkGlobalEvents();
    processFactionTrade();

    if (!g_bootstrap) {
        processRoadDegradation();
    }
}

class PerlinNoise {
    std::vector<int> p;
    double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
    double lerp(double t, double a, double b) { return a + t * (b - a); }
    double grad(int hash, double x, double y, double z) {
        int h = hash & 15;
        double u = h < 8 ? x : y;
        double v = h < 4 ? y : h == 12 || h == 14 ? x : z;
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }
public:
    PerlinNoise(unsigned int seed) {
        p.resize(256);
        std::iota(p.begin(), p.end(), 0);
        std::default_random_engine engine(seed);
        std::shuffle(p.begin(), p.end(), engine);
        p.insert(p.end(), p.begin(), p.end());
    }
    double noise(double x, double y, double z) {
        int X = (int)floor(x) & 255;
        int Y = (int)floor(y) & 255;
        int Z = (int)floor(z) & 255;
        x -= floor(x); y -= floor(y); z -= floor(z);
        double u = fade(x), v = fade(y), w = fade(z);
        int A = p[X]+Y, AA = p[A]+Z, AB = p[A+1]+Z;
        int B = p[X+1]+Y, BA = p[B]+Z, BB = p[B+1]+Z;
        return lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z),
                                       grad(p[BA], x-1, y, z)),
                               lerp(u, grad(p[AB], x, y-1, z),
                                       grad(p[BB], x-1, y-1, z))),
                       lerp(v, lerp(u, grad(p[AA+1], x, y, z-1),
                                       grad(p[BA+1], x-1, y, z-1)),
                               lerp(u, grad(p[AB+1], x, y-1, z-1),
                                       grad(p[BB+1], x-1, y-1, z-1))));
    }
    double fbm(double x, double y, int octaves, double persistence, double lacunarity) {
        double total = 0;
        double frequency = 1;
        double amplitude = 1;
        double maxValue = 0;
        for(int i=0; i<octaves; i++) {
            total += noise(x * frequency, y * frequency, 0) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }
};

void generateWorldMapTerrain(WorldMap& map, int seed) {
    map.grid.resize(map.width * map.height);
    std::vector<double> elevation(map.width * map.height);
    
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    std::vector<std::future<void>> futures;
    int chunk_size = map.height / num_threads;

    for (int t = 0; t < num_threads; ++t) {
        int start_y = t * chunk_size;
        int end_y = (t == num_threads - 1) ? map.height : (t + 1) * chunk_size;

        futures.push_back(getThreadPool()->enqueue([start_y, end_y, &map, &elevation, seed]() {
            PerlinNoise perlin(seed);
            PerlinNoise temp_noise(seed + 1);
            PerlinNoise moist_noise(seed + 2);
            
            for (int y = start_y; y < end_y; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    double nx = (double)x / map.width;
                    double ny = (double)y / map.height;
                    
                    double e = perlin.fbm(nx * 4.0, ny * 4.0, 5, 0.5, 2.0);
                    elevation[y * map.width + x] = e;
                    
                    double dist_to_equator = std::abs(y - map.height / 2.0) / (map.height / 2.0);
                    double base_t = 1.0 - dist_to_equator; 
                    double t_noise = temp_noise.fbm(nx * 3.0, ny * 3.0, 3, 0.5, 2.0) * 0.5;
                    double temperature = std::clamp(base_t + t_noise - 0.1, 0.0, 1.0);

                    double m = moist_noise.fbm(nx * 5.0, ny * 5.0, 4, 0.5, 2.0) + 0.5;
                    
                    TileType type = TileType::OCEAN;
                    if (e < -0.25) type = TileType::OCEAN;
                    else if (e < -0.1) type = TileType::SHALLOW_WATER;
                    else if (e > 0.65) type = TileType::MOUNTAINS;
                    else if (e > 0.45) type = TileType::HILLS;
                    else {
                        if (temperature < 0.35) {
                            type = TileType::TUNDRA;
                        } else if (temperature > 0.65) {
                            if (m < 0.4) type = TileType::DESERT;
                            else if (m > 0.65) type = TileType::SWAMP;
                            else type = TileType::PLAINS;
                        } else {
                            if (m > 0.55) type = TileType::FOREST;
                            else type = TileType::PLAINS;
                        }
                    }
                    map.grid[y * map.width + x].type = type;
                }
            }
        }));
    }
    for (auto& f : futures) f.get();

    int dx[] = {0, 1, 0, -1, 1, 1, -1, -1};
    int dy[] = {-1, 0, 1, 0, -1, 1, 1, -1};

    // Алгоритм Рек на основе Шума (Minecraft-style)
    int w = map.width, h = map.height;
    PerlinNoise river_noise(seed + 123);
    
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            if (map.grid[idx].type == TileType::OCEAN || map.grid[idx].type == TileType::SHALLOW_WATER) continue;
            
            double nx = (double)x / w;
            double ny = (double)y / h;
            
            // Генерируем "хребты" шума. Там, где значение близко к 0 - течет река.
            double r_val = river_noise.fbm(nx * 4.0, ny * 4.0, 4, 0.5, 2.0);
            
            // Чем ниже высота, тем шире река (имитация скопления воды)
            double e = elevation[idx];
            double threshold = 0.025; 
            if (e < 0.2) threshold = 0.04; // На равнинах реки шире
            if (e > 0.6) threshold = 0.015; // В горах реки узкие (ручьи)
            
            if (std::abs(r_val) < threshold) {
                map.grid[idx].type = TileType::RIVER;
                map.grid[idx].water_depth = (e < 0.2) ? 3 : 1;
            }
        }
    }

    // Добавление берегов, пойм и озер
    std::vector<TileType> new_types(w * h);
    for (int i = 0; i < w * h; ++i) new_types[i] = map.grid[i].type;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (map.grid[y * w + x].type == TileType::RIVER) {
                int depth = map.grid[y * w + x].water_depth;
                int radius = (depth >= 3) ? 2 : 1;
                
                // Проверяем соседей для создания берегов
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                            TileType orig_t = map.grid[ny * w + nx].type;
                            if (orig_t != TileType::RIVER && orig_t != TileType::LAKE && orig_t != TileType::OCEAN && orig_t != TileType::SHALLOW_WATER) {
                                double dist = std::hypot(dx, dy);
                                
                                if (orig_t == TileType::TUNDRA) {
                                    // В тундре реки прорезают снег, зеленых берегов нет
                                    continue;
                                } else if (orig_t == TileType::DESERT) {
                                    // В пустыне только узкая полоска зелени (оазис)
                                    if (dist <= 1.5) new_types[ny * w + nx] = TileType::RIVERBANK;
                                } else if (orig_t == TileType::SWAMP) {
                                    // В болоте берега остаются болотом
                                    continue;
                                } else {
                                    // Нормальные равнины и леса
                                    if (dist <= 1.5) {
                                        new_types[ny * w + nx] = TileType::RIVERBANK;
                                    } else if (dist <= 2.5 && new_types[ny * w + nx] != TileType::RIVERBANK) {
                                        new_types[ny * w + nx] = TileType::FLOODPLAIN;
                                    }
                                }
                            }
                        }
                    }
                }
                
                // Если река разливается слишком широко, превращаем центр в озеро
                int river_neighbors = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h && map.grid[ny * w + nx].type == TileType::RIVER) {
                            river_neighbors++;
                        }
                    }
                }
                if (river_neighbors >= 8) {
                    new_types[y * w + x] = TileType::LAKE;
                }
            }
        }
    }
    for (int i = 0; i < w * h; ++i) map.grid[i].type = new_types[i];

    // Генерация вулканов
    int num_volcanoes = 5;
    for (int i = 0; i < num_volcanoes; ++i) {
        int vx = rand() % map.width;
        int vy = rand() % map.height;
        int attempts = 0;
        while (map.grid[vy * map.width + vx].type != TileType::MOUNTAINS && attempts < 100) {
            vx = rand() % map.width;
            vy = rand() % map.height;
            attempts++;
        }
        if (attempts < 100) {
            map.grid[vy * map.width + vx].type = TileType::VOLCANO;
            for (int d = 0; d < 8; ++d) {
                int nx = vx + dx[d];
                int ny = vy + dy[d];
                if (nx >= 0 && nx < map.width && ny >= 0 && ny < map.height) {
                    if (map.grid[ny * map.width + nx].type != TileType::VOLCANO && map.grid[ny * map.width + nx].type != TileType::OCEAN) {
                        map.grid[ny * map.width + nx].type = TileType::DESERT; // Выжженная земля
                    }
                }
            }
        }
    }
}


int getTileCost(TileType t) {
    switch(t) {
        case TileType::PLAINS: return 1;
        case TileType::DESERT: return 1;
        case TileType::TUNDRA: return 1;
        case TileType::FOREST: return 2;
        case TileType::HILLS: return 2;
        case TileType::SWAMP: return 4;
        case TileType::SHALLOW_WATER: return 4;
        case TileType::RUINS: return 2;
        case TileType::ANOMALY: return 10;
        case TileType::RIVER: return 10;
        case TileType::RIVERBANK: return 2;
        case TileType::FLOODPLAIN: return 2;
        case TileType::LAKE: return 999;
        case TileType::LAVA: return 999;
        case TileType::ASH: return 2;
        case TileType::VOLCANO: return 999;
        case TileType::MOUNTAINS: return 999;
        case TileType::OCEAN: return 999;
        default: return 999;
    }
}

struct AStarNode {
    int x, y, g, f;
    bool operator>(const AStarNode& other) const { return f > other.f; }
};

std::vector<std::pair<int,int>> findPath(const WorldMap& map, int startX, int startY, int goalX, int goalY, const std::vector<bool>& has_road, const std::vector<int>& path_status, MovementType moveType, int entity_size) {
    int w = map.width;
    int h = map.height;
    std::vector<int> g_cost(w * h, 1e9);
    std::vector<int> parent(w * h, -1);
    std::priority_queue<AStarNode, std::vector<AStarNode>, std::greater<AStarNode>> pq;

    int startIdx = startY * w + startX;
    g_cost[startIdx] = 0;
    pq.push({startX, startY, 0, 0});

    int dx[] = {0, 1, 0, -1, 1, 1, -1, -1};
    int dy[] = {-1, 0, 1, 0, -1, 1, 1, -1};

    while (!pq.empty()) {
        auto curr = pq.top();
        pq.pop();

        if (curr.x == goalX && curr.y == goalY) break;
        if (curr.g > g_cost[curr.y * w + curr.x]) continue;

        for (int i = 0; i < 8; ++i) {
            int nx = curr.x + dx[i];
            int ny = curr.y + dy[i];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;

            int nIdx = ny * w + nx;
            int cost = 1;
            
            if (path_status[nIdx] == 2) {
                cost = 9999; // Заблокировано
            } else if (moveType == MovementType::LAND) {
                TileType t = map.grid[nIdx].type;
                bool is_water = (t == TileType::RIVER || t == TileType::SHALLOW_WATER || t == TileType::OCEAN || t == TileType::LAKE);
                
                if (has_road[nIdx]) {
                    if (is_water && path_status[nIdx] == 1) {
                        if (t == TileType::RIVER && map.grid[nIdx].water_depth <= 1) {
                            cost = 10; // Брод
                        } else {
                            cost = 9999; // Разрушенный мост/паром непроходим
                        }
                    } else if (is_water && map.grid[nIdx].water_depth >= 3 && entity_size > 50) {
                        cost = 9999; // Паром не выдержит крупную армию или караван
                    } else {
                        cost = 1;
                        if (path_status[nIdx] == 1) cost *= 3;
                    }
                } else {
                    if (is_water) {
                        if (t == TileType::RIVER && map.grid[nIdx].water_depth <= 1) {
                            cost = 10; // Брод
                        } else {
                            cost = 9999; // Строгий запрет на пересечение глубокой воды без моста
                        }
                    } else if (t == TileType::MOUNTAINS || t == TileType::VOLCANO) {
                        cost = 9999; // Горы непроходимы без дорог (тоннелей)
                    } else {
                        cost = getTileCost(t) * 2; // По бездорожью медленнее
                    }
                }
            } else if (moveType == MovementType::WATER) {
                TileType t = map.grid[nIdx].type;
                bool is_water = (t == TileType::RIVER || t == TileType::SHALLOW_WATER || t == TileType::OCEAN || t == TileType::LAKE);
                if (!is_water) {
                    cost = 9999;
                } else {
                    if (t == TileType::OCEAN) cost = 1;
                    else if (t == TileType::SHALLOW_WATER || t == TileType::LAKE) cost = 2;
                    else if (t == TileType::RIVER) {
                        if (map.grid[nIdx].water_depth < 2 && entity_size > 50) cost = 9999;
                        else cost = 3;
                    }
                }
                                } else if (moveType == MovementType::ANY) {
                        cost = getTileCost(map.grid[nIdx].type);
                        if (!has_road[nIdx]) cost *= 4; else cost = 1;
                        if (path_status[nIdx] == 1) cost *= 3;
                    }
                    
                    if (cost >= 999) {
                        if (nx == goalX && ny == goalY) cost = 5;
                        else continue;
                    }

            int new_g = curr.g + cost * (i >= 4 ? 14 : 10);
            if (new_g < g_cost[nIdx]) {
                g_cost[nIdx] = new_g;
                parent[nIdx] = curr.y * w + curr.x;
                int h_cost = (std::abs(nx - goalX) + std::abs(ny - goalY)) * 10;
                pq.push({nx, ny, new_g, new_g + h_cost});
            }
        }
    }

    std::vector<std::pair<int,int>> path;
    int currIdx = goalY * w + goalX;
    if (parent[currIdx] == -1) return path;

    while (currIdx != startIdx) {
        path.push_back({currIdx % w, currIdx / w});
        currIdx = parent[currIdx];
    }
    path.push_back({startX, startY});
    std::reverse(path.begin(), path.end());
    return path;
}

void placeRegionsOnMap(WorldMap& map, const World& w) {
    std::vector<bool> occupied(map.width * map.height, false);
    int cx = map.width / 2;
    int cy = map.height / 2;

    for (const auto& [rid, r] : w.regions) {
        TileType pref = TileType::PLAINS;
        bool require_water = false;
        bool require_coast = false;
        bool require_center = false;

        if (r.placement_type == "water") require_water = true;
        else if (r.placement_type == "coast") require_coast = true;
        else if (r.placement_type == "center") require_center = true;
        else if (r.placement_type == "mountain") pref = TileType::MOUNTAINS;
        else if (r.placement_type == "forest") pref = TileType::FOREST;
        else if (r.placement_type == "desert") pref = TileType::DESERT;
        else {
            if (r.factionId == "khazadrim") pref = TileType::MOUNTAINS;
            else if (r.factionId == "sylvanesti" || r.factionId == "greencode") pref = TileType::FOREST;
            else if (r.factionId == "gronnar") pref = TileType::DESERT;
            else if (r.factionId == "consortium") require_coast = true;
        }

        std::vector<std::pair<int, int>> candidates_strict;
        std::vector<std::pair<int, int>> candidates_fallback_1; // Любой биом, дистанция >= 15
        std::vector<std::pair<int, int>> candidates_fallback_2; // Любой биом, дистанция >= 5
        std::vector<std::pair<int, int>> candidates_fallback_3; // Любая суша

        for (int y = 5; y < map.height - 5; ++y) {
            for (int x = 5; x < map.width - 5; ++x) {
                if (occupied[y * map.width + x]) continue;
                TileType t = map.grid[y * map.width + x].type;
                
                bool is_ocean = (t == TileType::OCEAN || t == TileType::SHALLOW_WATER);
                
                // Проверяем дистанцию до других городов
                int min_dist = 9999;
                for (const auto& [oid, oloc] : map.locations) {
                    int d = std::abs(oloc.x - x) + std::abs(oloc.y - y);
                    if (d < min_dist) min_dist = d;
                }

                // Fallback 3: Любая подходящая поверхность (суша или вода, если требуется)
                if ((require_water && is_ocean) || (!require_water && !is_ocean)) {
                    candidates_fallback_3.push_back({x, y});
                    
                    // Fallback 2: Дистанция >= 5
                    if (min_dist >= 5) {
                        candidates_fallback_2.push_back({x, y});
                        
                        // Fallback 1: Дистанция >= 15
                        if (min_dist >= 15) {
                            candidates_fallback_1.push_back({x, y});
                            
                            // Strict: Совпадение биома
                            bool match = false;
                            if (require_water) {
                                match = is_ocean;
                            } else if (require_coast) {
                                if (!is_ocean && t != TileType::MOUNTAINS) {
                                    for(int dy=-1; dy<=1; dy++) {
                                        for(int dx=-1; dx<=1; dx++) {
                                            TileType nt = map.grid[(y+dy)*map.width + (x+dx)].type;
                                            if (nt == TileType::SHALLOW_WATER || nt == TileType::OCEAN || nt == TileType::LAKE) match = true;
                                        }
                                    }
                                }
                            } else if (pref == TileType::MOUNTAINS) {
                                match = (t == TileType::MOUNTAINS || t == TileType::HILLS);
                            } else {
                                match = (t == pref);
                            }
                            
                            if (match) {
                                candidates_strict.push_back({x, y});
                            }
                        }
                    }
                }
            }
        }

        // Выбираем лучший доступный пул кандидатов
        std::vector<std::pair<int, int>>* final_candidates = &candidates_strict;
        if (final_candidates->empty()) final_candidates = &candidates_fallback_1;
        if (final_candidates->empty()) final_candidates = &candidates_fallback_2;
        if (final_candidates->empty()) final_candidates = &candidates_fallback_3;

        int bestX = -1, bestY = -1;

        if (!final_candidates->empty()) {
            if (require_center) {
                // Ищем точку, ближайшую к центру карты
                int best_dist = 999999;
                for (auto& pt : *final_candidates) {
                    int d = std::abs(pt.first - cx) + std::abs(pt.second - cy);
                    if (d < best_dist) {
                        best_dist = d;
                        bestX = pt.first;
                        bestY = pt.second;
                    }
                }
            } else {
                // Выбираем СЛУЧАЙНУЮ точку из всех подходящих (исправляет скучивание)
                auto spot = (*final_candidates)[rand() % final_candidates->size()];
                bestX = spot.first;
                bestY = spot.second;
            }
        }

        if (bestX != -1) {
            occupied[bestY * map.width + bestX] = true;
            MapLocation loc;
            loc.id = rid;
            loc.name = r.name;
            loc.x = bestX;
            loc.y = bestY;
            
            if (!r.base_type.empty()) {
                loc.type = r.base_type;
            } else {
                loc.type = (r.population > 5000) ? "city" : (r.population > 0 ? "village" : "ruins");
            }
            
            if (require_water && r.population > 0 && loc.type == "village") loc.type = "ruins"; // Legacy override
            
            loc.faction = r.factionId;
            loc.no_road = r.no_road;
            map.locations[rid] = loc;
        }
    }
}

void generateRoads(WorldMap& map, const World& w) {
    std::vector<bool> pathfinding_helper(map.width * map.height, false);
    std::vector<int> path_status_helper(map.width * map.height, 0);

    auto processRoadSegment = [&](MapRoad& road, int8_t quality) {
        std::vector<MapRoad> new_segments;
        MapRoad current_segment;
        current_segment.from = road.from;
        current_segment.to = road.to;
        current_segment.condition = road.condition;
        current_segment.type = (quality == 2) ? "paved" : "dirt";

        for (const auto& wp : road.waypoints) {
            int idx = wp.second * map.width + wp.first;
            if (quality > map.grid[idx].road_level) map.grid[idx].road_level = quality;
            pathfinding_helper[idx] = true;

            TileType t = map.grid[idx].type;
            bool is_water = (t == TileType::RIVER || t == TileType::SHALLOW_WATER || t == TileType::OCEAN || t == TileType::LAKE);
            
            if (is_water) {
                map.grid[idx].bridge_flag = 1;
                map.grid[idx].road_level = quality; // Принудительно ставим уровень дороги на тайл воды
                std::string rtype = (map.grid[idx].water_depth >= 3) ? "ferry" : "bridge";
                if (current_segment.type != rtype) {
                    if (!current_segment.waypoints.empty()) new_segments.push_back(current_segment);
                    current_segment.waypoints.clear();
                    current_segment.type = rtype;
                }
            } else if (t == TileType::MOUNTAINS || t == TileType::HILLS) {
                if (current_segment.type != "tunnel") {
                    if (!current_segment.waypoints.empty()) new_segments.push_back(current_segment);
                    current_segment.waypoints.clear();
                    current_segment.type = "tunnel";
                }
            } else {
                std::string rtype = (quality == 2) ? "paved" : "dirt";
                if (current_segment.type == "bridge" || current_segment.type == "tunnel" || current_segment.type == "ferry") {
                    if (!current_segment.waypoints.empty()) new_segments.push_back(current_segment);
                    current_segment.waypoints.clear();
                    current_segment.type = rtype;
                } else {
                    current_segment.type = rtype;
                }
            }
            current_segment.waypoints.push_back(wp);
        }
        if (!current_segment.waypoints.empty()) new_segments.push_back(current_segment);
        return new_segments;
    };

    // 1. Сначала прокладываем внутренние (грунтовые) дороги
    for (const auto& [fid, f] : w.factions) {
        if (f.regions.empty()) continue;
        std::string capital = f.regions[0];
        if (!map.locations.count(capital)) continue;
        auto capLoc = map.locations.at(capital);

        for (size_t i = 1; i < f.regions.size(); ++i) {
            std::string rid = f.regions[i];
            if (!map.locations.count(rid)) continue;
            if (map.locations.at(rid).no_road) continue; // Пропуск изолированных точек
            auto rLoc = map.locations.at(rid);

            auto path = findPath(map, rLoc.x, rLoc.y, capLoc.x, capLoc.y, pathfinding_helper, path_status_helper, MovementType::ANY);
            if (!path.empty()) {
                MapRoad road; road.from = rid; road.to = capital; road.condition = "dirt"; road.waypoints = path;
                auto segments = processRoadSegment(road, 1);
                map.roads.insert(map.roads.end(), segments.begin(), segments.end());
            }
        }
    }

    // 2. Затем международные (мощеные) - они будут поглощать грунтовые
    std::vector<std::string> capitals;
    for (const auto& [fid, f] : w.factions) {
        if (!f.regions.empty() && map.locations.count(f.regions[0])) capitals.push_back(f.regions[0]);
    }

    for (size_t i = 0; i < capitals.size(); ++i) {
        for (size_t j = i + 1; j < capitals.size(); ++j) {
            std::string cap1 = capitals[i]; std::string cap2 = capitals[j];
            if (map.locations.at(cap1).no_road || map.locations.at(cap2).no_road) continue; // Пропуск
            if (w.factions.at(map.locations.at(cap1).faction).diplomacy.count(map.locations.at(cap2).faction)) {
                if (w.factions.at(map.locations.at(cap1).faction).diplomacy.at(map.locations.at(cap2).faction) == "war") continue;
            }

            auto loc1 = map.locations.at(cap1); auto loc2 = map.locations.at(cap2);
            auto path = findPath(map, loc1.x, loc1.y, loc2.x, loc2.y, pathfinding_helper, path_status_helper, MovementType::ANY);
            if (!path.empty()) {
                MapRoad road; road.from = cap1; road.to = cap2; road.condition = "paved"; road.waypoints = path;
                auto segments = processRoadSegment(road, 2);
                map.roads.insert(map.roads.end(), segments.begin(), segments.end());
            }
        }
    }
}



void generateSeaRoutes(WorldMap& map, World& w) {
    std::vector<bool> dummy_has_road(map.width * map.height, false);
    std::vector<int> dummy_path_status(map.width * map.height, 0);
    
    std::vector<std::string> port_regions;
    for (const auto& [rid, port] : w.port_facilities) {
        if (map.locations.count(rid)) port_regions.push_back(rid);
    }
    
    for (size_t i = 0; i < port_regions.size(); ++i) {
        for (size_t j = i + 1; j < port_regions.size(); ++j) {
            std::string r1 = port_regions[i];
            std::string r2 = port_regions[j];
            
            auto loc1 = map.locations.at(r1);
            auto loc2 = map.locations.at(r2);
            
            auto path = findPath(map, loc1.x, loc1.y, loc2.x, loc2.y, dummy_has_road, dummy_path_status, MovementType::WATER, 10);
            if (!path.empty()) {
                MapRoad sea_route;
                sea_route.from = r1;
                sea_route.to = r2;
                sea_route.condition = "water";
                sea_route.type = "sea_route";
                sea_route.integrity = 100;
                sea_route.waypoints = path;
                map.roads.push_back(sea_route);
            }
        }
    }
}


void generateCityLayout(Region& r, World& w) {
    // 1. Динамический размер города в зависимости от населения
    if (r.population < 2000) { r.layoutWidth = 10; r.layoutHeight = 10; }
    else if (r.population <= 10000) { r.layoutWidth = 16; r.layoutHeight = 16; }
    else { r.layoutWidth = 24; r.layoutHeight = 24; }

    r.cityLayout.clear();
    r.cityLayout.resize(r.layoutWidth * r.layoutHeight);
    for(int y=0; y<r.layoutHeight; ++y) {
        for(int x=0; x<r.layoutWidth; ++x) {
            CityBlock& b = r.cityLayout[y * r.layoutWidth + x];
            b.x = x; b.y = y; b.type = "empty"; b.name = "Пустырь";
        }
    }

    auto getBlock = [&](int x, int y) -> CityBlock& { return r.cityLayout[y * r.layoutWidth + x]; };

    // 2. Генерация дорожной сети (Квартальная система)
    int midX = r.layoutWidth / 2;
    int midY = r.layoutHeight / 2;
    
    for(int y=0; y<r.layoutHeight; ++y) {
        for(int x=0; x<r.layoutWidth; ++x) {
            bool isRoad = false;
            std::string roadName = "Улица";

            // Главные тракты (Крест)
            if (x == midX || y == midY) { isRoad = true; roadName = "Главный тракт"; }
            // Кольцевая дорога (Стены/Граница)
            else if (x == 1 || x == r.layoutWidth - 2 || y == 1 || y == r.layoutHeight - 2) { isRoad = true; roadName = "Кольцевая дорога"; }
            // Второстепенные улицы (сетка кварталов)
            else if (x % 4 == 0 || y % 4 == 0) { isRoad = true; roadName = "Переулок"; }

            // Центральная площадь
            if (std::abs(x - midX) <= 1 && std::abs(y - midY) <= 1) {
                isRoad = true; roadName = "Центральная площадь";
            }

            if (isRoad) {
                getBlock(x, y).type = "road";
                getBlock(x, y).name = roadName;
            }
        }
    }

    // 3. Зонирование (Центр, Жилые кварталы, Окраины)
    std::vector<std::pair<int,int>> centerSpots;
    std::vector<std::pair<int,int>> edgeSpots;
    std::vector<std::pair<int,int>> midSpots;

    for(int y=0; y<r.layoutHeight; ++y) {
        for(int x=0; x<r.layoutWidth; ++x) {
            if (getBlock(x, y).type != "empty") continue;
            
            int distToCenter = std::abs(x - midX) + std::abs(y - midY);
            if (distToCenter <= 4) centerSpots.push_back({x, y});
            else if (x <= 2 || x >= r.layoutWidth - 3 || y <= 2 || y >= r.layoutHeight - 3) edgeSpots.push_back({x, y});
            else midSpots.push_back({x, y});
        }
    }

    thread_local std::mt19937 gen_local(static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()) + static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id())) + 2);
    std::shuffle(centerSpots.begin(), centerSpots.end(), gen_local);
    std::shuffle(edgeSpots.begin(), edgeSpots.end(), gen_local);
    std::shuffle(midSpots.begin(), midSpots.end(), gen_local);

    int counter = 1;
    auto placeBuilding = [&](const std::string& type, const std::string& name, const std::string& linked_id, std::vector<std::pair<int,int>>& prefZone, std::vector<std::pair<int,int>>& altZone) {
        std::pair<int,int> spot = {-1, -1};
        if (!prefZone.empty()) { spot = prefZone.back(); prefZone.pop_back(); }
        else if (!altZone.empty()) { spot = altZone.back(); altZone.pop_back(); }
        
        if (spot.first != -1) {
            auto& b = getBlock(spot.first, spot.second);
            b.type = type;
            b.name = name;
            b.linked_id = linked_id;
            b.sublocation_id = "sub_" + r.id + "_" + type + "_" + std::to_string(counter++);

            JsonValue subLoc = JsonValue::object();
            subLoc.set("id", b.sublocation_id);
            subLoc.set("name", b.name);
            subLoc.set("parentId", r.id);
            subLoc.set("type", b.type);
            
            std::lock_guard<std::mutex> lock(g_sublocations_mutex);
            w.subLocations[b.sublocation_id] = subLoc;
        }
    };

    std::vector<std::string> tavern_names = {"Пьяный Дракон", "Хромой Кабан", "Золотая Кружка", "Ржавый Якорь", "Слепой Бехолдер", "Уютный Уголок", "Дырявый Котел", "Сытый Орк", "Кривая Подкова", "Веселый Монах"};
    std::vector<std::string> forge_names = {"Горячая Наковальня", "Стальной Молот", "Искры и Угли", "Железный Кулак", "Пламя Горна", "Звонкий Молот", "Медное Горнило"};
    std::vector<std::string> temple_names = {"Храм Света", "Святилище Предков", "Обитель Тишины", "Зал Благодати", "Часовня Забвения", "Алтарь Надежды"};
    std::vector<std::string> market_names = {"Главный Рынок", "Торговые Ряды", "Пестрый Базар", "Ярмарка Чудес", "Нижний Рынок"};

    // 4. Экономический ребаланс и расстановка
    // Центр: Храмы, Рынки, Офисы, Банки
    if (r.population > 5000) placeBuilding("temple", temple_names[thread_safe_rand() % temple_names.size()], "", centerSpots, midSpots);
    if (r.facilities.count("banks") && r.facilities["banks"].level > 0) placeBuilding("banks", "Банк", "banks", centerSpots, midSpots);
    
    int merchantCount = 0;
    for (const auto& [nId, npc] : w.npcs) {
        if (npc.homeLocation == r.id && npc.profession == "Торговец" && !npc.economy.workplaceId.empty()) {
            merchantCount++;
            placeBuilding("office", "Лавка '" + npc.name + "'", npc.economy.workplaceId, centerSpots, midSpots);
        }
    }
    if (merchantCount >= 2) placeBuilding("market", market_names[thread_safe_rand() % market_names.size()], "", centerSpots, midSpots);

    // Окраины: Производство (масштабируется от уровня)
    for (const auto& [fId, fac] : r.facilities) {
        if (fac.level > 0 && fId != "banks") {
            int tileCount = std::max(1, fac.level / 3);
            tileCount = std::min(tileCount, 4); 
            for(int i=0; i<tileCount; ++i) {
                std::string bName = getFacilityName(fId);
                if (fId == "forges") bName = "Кузница '" + forge_names[thread_safe_rand() % forge_names.size()] + "'";
                placeBuilding(fId, bName, fId, edgeSpots, midSpots);
            }
        }
    }

    // Жилые кварталы и Таверны
    int maxHouses = std::min(r.population / 50, (int)(midSpots.size() + edgeSpots.size() + centerSpots.size()) - 5);
    if (maxHouses < 0) maxHouses = 0;
    
    int taverns = std::max(1, maxHouses / 15);
    taverns = std::min(taverns, 5);
    
    for(int i=0; i<taverns; ++i) placeBuilding("tavern", "Таверна '" + tavern_names[thread_safe_rand() % tavern_names.size()] + "'", "", midSpots, centerSpots);
    
    for(int i=0; i<maxHouses; ++i) {
        placeBuilding("house", "Жилой дом", "", midSpots, edgeSpots);
    }
}


// Build initial world
void buildWorld(const std::string& playerId, const std::string& era, int initialAgents, const JsonValue& globalLocs, int startDay) {
    g_playerId = playerId;
    g_world = World();
    g_world.era = era.empty() ? "rebirth" : era;
    g_world.current_day = startDay;
    
    // Очистка реестров от предыдущих сессий (Fix Memory Leak)
    g_items.clear();
    g_containers.clear();
    g_deleted_items.clear();
    g_deleted_containers.clear();
    
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
                // Базовые отношения от -10 до +40 (избегаем мгновенной ненависти на старте)
                int baseRel = (rand() % 50) - 10;
                
                // Лорные корректировки для классической эпохи (rebirth)
                if (g_world.era == "rebirth") {
                    if ((f1 == "aquilon" && f2 == "sylvanesti") || (f1 == "sylvanesti" && f2 == "aquilon")) baseRel -= 30;
                    if ((f1 == "crimson" && f2 == "gronnar") || (f1 == "gronnar" && f2 == "crimson")) baseRel -= 60;
                    if ((f1 == "aquilon" && f2 == "khazadrim") || (f1 == "khazadrim" && f2 == "aquilon")) baseRel += 40;
                    if ((f1 == "consortium" && f2 == "aquilon") || (f1 == "aquilon" && f2 == "consortium")) baseRel -= 15;
                }
                
                g_world.factions[f1].relations[f2] = std::clamp(baseRel, -100, 100);
                g_world.factions[f1].diplomacy[f2] = "neutral";
                
                // ГЛОБАЛЬНЫЙ СТАРТОВЫЙ ПАКТ: Запрет на войны в первые 30-90 дней симуляции
                // Это дает экономике время на стабилизацию и предотвращает хаос 1-го дня
                g_world.factions[f1].truceUntil[f2] = 180 + (rand() % 180); // Полгода-год гарантированного мира на старте
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
        Region r;
        r.id = key;
        r.name = globalLocs.has(key) && globalLocs[key].has("name") ? globalLocs[key]["name"].asString() : key;
        if (globalLocs.has(key) && globalLocs[key].has("placement")) {
            r.placement_type = globalLocs[key]["placement"].asString();
        }

        if (globalLocs.has(key) && globalLocs[key].has("type")) {
            r.base_type = globalLocs[key]["type"].asString();
        }

        // Эвристика определения руин/аномалий (Фолбэк, если типа нет)
        if (r.base_type.empty()) {
            std::string lowerName = r.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c){ return std::tolower(c); });
            std::string lowerId = key;
            std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), [](unsigned char c){ return std::tolower(c); });

            if (lowerName.find("руин") != std::string::npos || lowerId.find("ruin") != std::string::npos) r.base_type = "ruins";
            else if (lowerName.find("аномал") != std::string::npos || lowerId.find("anomaly") != std::string::npos || lowerId.find("scar") != std::string::npos || lowerId.find("void") != std::string::npos) r.base_type = "anomaly";
            else if (lowerName.find("форт") != std::string::npos || lowerId.find("fort") != std::string::npos) r.base_type = "fort";
            else if (lowerName.find("лагерь") != std::string::npos || lowerId.find("camp") != std::string::npos) r.base_type = "camp";
            else if (lowerName.find("обсерват") != std::string::npos || lowerId.find("obs") != std::string::npos) r.base_type = "observatory";
        }

        bool is_ruin = (r.base_type == "ruins" || r.base_type == "anomaly");

        std::string ownerId = "";
        bool isDwarf = false;
        bool isElf = false;

        if (is_ruin) {
            r.factionId = "";
            r.population = 0;
            r.moneySupply = 0;
            r.fertility = 0.0;
            r.mineral_wealth = 0.0;
        } else {
            ownerId = locMap.count(key) ? locMap[key] : fKeys[rand() % fKeys.size()];
            isDwarf = (ownerId == "khazadrim");
            isElf = (ownerId == "sylvanesti" || ownerId == "greencode");
            r.factionId = ownerId;
            r.population = 250 + (rand() % 2250);
            r.moneySupply = 5000 + (rand() % 10000);
            r.fertility = 0.5 + (rand() % 100) / 100.0;
            r.mineral_wealth = 0.5 + (rand() % 100) / 100.0;
        }

        if (globalLocs.has(key) && globalLocs[key].has("no_road")) {
            r.no_road = globalLocs[key]["no_road"].asBool();
        }

        // Инициализация возрастной пирамиды (0-120 лет)
        r.age_pyramid.assign(121, 0.0);
        if (!is_ruin) {
            double children = r.population * 0.20;
            double workers = r.population * 0.60;
            double elders = r.population * 0.20;
            for(int i=0; i<=17; i++) r.age_pyramid[i] = children / 18.0;
            for(int i=18; i<=65; i++) r.age_pyramid[i] = workers / 48.0;
            for(int i=66; i<=120; i++) r.age_pyramid[i] = elders / 55.0;
            r.labor_force = (int)workers;
        } else {
            r.labor_force = 0;
        }
        r.climate = "temperate";
        r.weather = "Ясно";

        r.vault_id = createContainer(is_ruin ? "ruins_stash" : "faction_vault", is_ruin ? "none" : ownerId, 999999, 1000, key);
        r.storage_capacity = is_ruin ? 5000 : 10000 + (r.population / 5);
        r.threat_level = is_ruin ? 80 + (rand() % 21) : 10 + (rand() % 20);

        for (int i = 0; i < (int)GoodType::COUNT; i++) {
            GoodType gt = (GoodType)i;
            int baseAmount = 0;

            // ФИКС: Выдаем на склад ТОЛЬКО то, что регион добывает сам, либо жизненно важный хлеб/золото
            if (r.available_raw_resources.count(gt)) {
                baseAmount = (r.population * 0.3) + (rand() % 200);
            } else if (gt == GoodType::BREAD) {
                baseAmount = r.population * 0.2; // Минимальный паек на первые дни
            } else if (gt == GoodType::GOLD_INGOT) {
                baseAmount = 5000 + (rand() % 5000); // Снижена стартовая казна
            }

            if (baseAmount > 0) {
                createItem(gt, baseAmount, r.vault_id, 0, "Начальные запасы");
            }
            // Устанавливаем базовую цену для всех товаров, даже если их нет на складе
            r.markets[goodTypeToString(gt)] = BASE_PRICES[i];
        }

        if (!is_ruin) {
            r.facilities["farms"] = {isElf ? 15 : (rand() % 6) + 3, 100};
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
            r.facilities["apiaries"] = {isElf ? 5 : rand() % 3, 100};
            r.facilities["hunting_lodges"] = {(rand() % 5) + 1, 100};
            r.facilities["observatories"] = {ownerId == "crimson" || ownerId == "aquilon" ? 3 : rand() % 2, 100};
        }

        r.animals.herbivores = isElf ? 10000 : 500 + (rand() % 2000);
        r.animals.carnivores = isElf ? 1000 : 50 + (rand() % 200);

        // ФИКС: Жесткая специализация. Регион имеет только 1-2 ресурса. 
        r.available_raw_resources.clear();
        if (r.placement_type == "mountain") {
            r.available_raw_resources.insert((rand() % 100 < 30) ? GoodType::GOLD_ORE : GoodType::IRON_ORE);
            r.available_raw_resources.insert(GoodType::ETHER_DUST);
        } else if (r.placement_type == "forest") {
            r.available_raw_resources.insert(GoodType::WOOD);
            r.available_raw_resources.insert(GoodType::FUR);
            r.available_raw_resources.insert(GoodType::HONEY);
            r.available_raw_resources.insert(GoodType::WAX);
            if (rand() % 100 < 50) r.available_raw_resources.insert(GoodType::HERBS);
        } else if (r.placement_type == "desert") {
            r.available_raw_resources.insert(GoodType::IRON_ORE);
            r.available_raw_resources.insert(GoodType::ETHER_DUST);
        } else if (r.placement_type == "coast" || r.placement_type == "water") {
            r.available_raw_resources.insert(GoodType::FISH);
            r.available_raw_resources.insert(GoodType::FUR);
        } else {
            // Равнины и центр - только сельское хозяйство
            r.available_raw_resources.insert(GoodType::WHEAT);
            r.available_raw_resources.insert(GoodType::HONEY);
            r.available_raw_resources.insert(GoodType::WAX);
            if (rand() % 100 < 40) r.available_raw_resources.insert(GoodType::COTTON);
        }

        g_world.regions[key] = r;
        if (!is_ruin && !ownerId.empty()) {
            g_world.factions[ownerId].regions.push_back(key);
        }
    }

                for (auto& [rid, r] : g_world.regions) {
                if (g_world.map.locations.count(rid)) {
                    auto loc = g_world.map.locations[rid];
                    bool near_water = false;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = loc.x + dx;
                            int ny = loc.y + dy;
                            if (nx >= 0 && nx < g_world.map.width && ny >= 0 && ny < g_world.map.height) {
                                TileType t = g_world.map.grid[ny * g_world.map.width + nx].type;
                                if (t == TileType::RIVER || t == TileType::LAKE || t == TileType::OCEAN || t == TileType::SHALLOW_WATER) {
                                    near_water = true;
                                }
                            }
                        }
                    }
                    if (near_water) {
                r.available_raw_resources.insert(GoodType::FISH);
                r.available_raw_resources.insert(GoodType::WHEAT);
                r.fertility += 0.5;
                
                PortFacility port;
                port.type = PortType::TRADE;
                port.dock_container_id = createContainer("port_dock", r.factionId, 999999, 1000, rid);
                port.has_shipyard = (rand() % 100 < 40);
                g_world.port_facilities[rid] = port;
            }
        }
    }
    
    // --- Балансировка начальных условий (Асимметрия) ---
    // Случайные войны удалены, так как теперь работает адаптивный политический гомеостаз.
    if (locKeys.size() >= 2) {
        g_world.regions[locKeys[0]].fertility = 1.5;
        g_world.regions[locKeys[0]].mineral_wealth = 1.5;
        g_world.regions[locKeys[0]].population *= 2;
        g_world.regions[locKeys[0]].moneySupply *= 2;
        
        g_world.regions[locKeys[1]].fertility = 0.5;
        g_world.regions[locKeys[1]].mineral_wealth = 0.5;
        g_world.regions[locKeys[1]].population /= 2;
        g_world.regions[locKeys[1]].moneySupply /= 2;
    }

    // Create NPCs
    std::vector<std::string> names = {"Боб", "Грег", "Элиза", "Торбин", "Лиара", "Каэль", "Морган", "Сильвия"};
    std::vector<std::string> professions = {"Кузнец", "Фермер", "Стражник", "Торговец", "Маг", "Трактирщик", "Гонец", "Клерк", "Ткач", "Пекарь", "Ювелир", "Астроном", "Охотник", "Пасечник", "Корабел", "Рыбак", "Моряк", "Пират"};
    
    std::vector<std::string> regionIds;
    for (auto& [rid, r] : g_world.regions) regionIds.push_back(rid);
    
    if (!regionIds.empty()) {
        for (int i = 0; i < initialAgents; i++) {
        NPC npc;
        npc.id = "npc_" + generateUUID();
        npc.name = names[rand() % names.size()] + " " + professions[rand() % professions.size()];
        npc.type = "npc";
        
        // Определение расы на основе фракции региона
        std::string homeReg = regionIds[rand() % regionIds.size()];
        if (g_world.regions.count(homeReg)) {
            std::string facId = g_world.regions[homeReg].factionId;
            if (facId == "khazadrim") npc.race = "dwarf";
            else if (facId == "sylvanesti" || facId == "greencode") npc.race = "elf";
            else if (facId == "gronnar") npc.race = "orc";
            else npc.race = "human";
        }

        npc.profession = professions[rand() % professions.size()];
        if (npc.profession == "Фермер" || npc.profession == "Охотник" || npc.profession == "Пасечник") npc.economy.profession_type = "farmer";
        else if (npc.profession == "Кузнец" || npc.profession == "Ткач" || npc.profession == "Пекарь" || npc.profession == "Ювелир") npc.economy.profession_type = "artisan";
        else if (npc.profession == "Астроном") npc.economy.profession_type = "gatherer";
        else if (npc.profession == "Торговец") npc.economy.profession_type = "merchant";
        else if (npc.profession == "Стражник") npc.economy.profession_type = "mercenary";
        else if (npc.profession == "Маг") npc.economy.profession_type = "mage";
        else if (npc.profession == "Трактирщик") npc.economy.profession_type = "innkeeper";
        else if (npc.profession == "Корабел") npc.economy.profession_type = "shipwright";
        else if (npc.profession == "Рыбак") npc.economy.profession_type = "fisherman";
        else if (npc.profession == "Моряк") npc.economy.profession_type = "sailor";
        else if (npc.profession == "Пират") npc.economy.profession_type = "pirate";
        else npc.economy.profession_type = "farmer";
        
        npc.homeLocation = regionIds[rand() % regionIds.size()];
        npc.currentLocation = npc.homeLocation;
        npc.currentActivity = "Отдыхает";
        
        // Demographics
        npc.age_days = (18 + rand() % 40) * 360 + (rand() % 360);
        npc.is_male = (rand() % 2 == 0);
        npc.immunity = 50 + rand() % 50;
        npc.hp = 20;
        npc.maxHp = 20;
        
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
        if (fid == "khazadrim") ruler.race = "dwarf";
        else if (fid == "sylvanesti" || fid == "greencode") ruler.race = "elf";
        else if (fid == "gronnar") ruler.race = "orc";
        else ruler.race = "human";
        
        ruler.homeLocation = g_world.factions[fid].regions.empty() ? "" : g_world.factions[fid].regions[0];
        ruler.currentLocation = ruler.homeLocation;
        ruler.isAlive = true;
        ruler.alive = true;
        ruler.health = 100;
        
        ruler.rulerPersonality.ambition = 40 + (rand() % 40); // Нормальные амбиции
        ruler.rulerPersonality.wisdom = 40 + (rand() % 40);
        ruler.rulerPersonality.military = 40 + (rand() % 40); // Нормальный милитаризм
        ruler.rulerPersonality.cruelty = 30 + (rand() % 40);
        ruler.rulerPersonality.diplomacy = 40 + (rand() % 40);
        ruler.rulerPersonality.paranoia = 30 + (rand() % 40); // Нормальная паранойя
        ruler.rulerPersonality.stewardship = 60 + (rand() % 40); // Фокус на экономике
        
        g_world.npcs[ruler.id] = ruler;
        g_world.factions[fid].rulerId = ruler.id;
    }

    // Create Lord Monopolies (Монополии феодалов)
    for (auto& [fid, faction] : g_world.factions) {
        if (faction.regions.empty() || faction.rulerId.empty()) continue;
        std::string capital = faction.regions[0];
        
        std::vector<std::string> monopolyTypes = {"mines", "lumbermills", "farms"}; // Убраны кузницы, так как им нужно сырье
        int numMonopolies = 1 + (rand() % 2);
        
        for (int i = 0; i < numMonopolies; i++) {
            std::string facType = monopolyTypes[rand() % monopolyTypes.size()];
            
            Business b;
            b.id = "bus_" + generateUUID();
            b.owner_ids.push_back(faction.rulerId);
            b.region_id = capital;
            b.facility_type = facType;
            b.level = 3; // Монополии лордов сразу крупные
            b.employee_count = b.level * 100; // ФИКС: Назначаем рабочих сразу, чтобы производство началось в 1-й день
            b.target_employee_count = b.level * 100;
            b.cash_balance = 5000;
            b.is_active = true;
            b.local_storage_id = createContainer("business_storage", faction.rulerId, 999999, 1000, capital);
            
            // Определяем фокус производства и гарантируем наличие ресурса в регионе
            if (facType == "mines") { b.production_focus = "iron_ore"; g_world.regions[capital].available_raw_resources.insert(GoodType::IRON_ORE); }
            else if (facType == "lumbermills") { b.production_focus = "wood"; g_world.regions[capital].available_raw_resources.insert(GoodType::WOOD); }
            else if (facType == "farms") { b.production_focus = "wheat"; g_world.regions[capital].available_raw_resources.insert(GoodType::WHEAT); }
            
            LogisticRule autoSell;
            autoSell.id = "log_" + generateUUID();
            autoSell.type = "transfer";
            autoSell.resource = stringToGoodType(b.production_focus);
            autoSell.target_id = capital; // Продажа в казну/рынок региона
                                autoSell.amount = 9999;
                    autoSell.frequency_days = 7;
                    b.logistics.push_back(autoSell);
            
            g_world.businesses[b.id] = b;
            g_world.npcs[faction.rulerId].owned_businesses.push_back(b.id);
            
            if (g_world.regions.count(capital)) {
                placePrivateBusinessOnMap(g_world.regions[capital], b, g_world);
            }
        }
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

    
    // Generate City Layouts for all regions (Multi-threaded)
    std::vector<std::future<void>> city_futures;
    for (auto& [rid, r] : g_world.regions) {
        city_futures.push_back(getThreadPool()->enqueue([&r]() {
            generateCityLayout(r, g_world);
        }));
    }
    for (auto& f : city_futures) f.get();


    // Generate Global World Map
    g_world.map.width = 256;
    g_world.map.height = 256;
    generateWorldMapTerrain(g_world.map, rand());
    placeRegionsOnMap(g_world.map, g_world);
    generateRoads(g_world.map, g_world);

    generateSeaRoutes(g_world.map, g_world);

    for (const auto& [rid, port] : g_world.port_facilities) {
        if (g_world.regions.count(rid)) {
            std::string factionId = g_world.regions[rid].factionId;
            
            Ship s;
            s.id = "ship_" + generateUUID();
            s.owner_id = factionId;
            s.type = ShipType::MERCHANT;
            s.hull = 100;
            s.sailors = 15;
            s.cargo_capacity = 500;
            s.chest_id = createContainer("ship_hold", factionId, 999999, 100, rid);
            s.speed = 1.5;
            if (g_world.map.locations.count(rid)) {
                s.x = g_world.map.locations[rid].x;
                s.y = g_world.map.locations[rid].y;
            }
            g_world.ships.push_back(s);
            
            if (port.has_shipyard) {
                Ship warship;
                warship.id = "ship_" + generateUUID();
                warship.owner_id = factionId;
                warship.type = ShipType::WAR_GALLEY;
                warship.hull = 200;
                warship.sailors = 40;
                warship.cargo_capacity = 100;
                warship.chest_id = createContainer("ship_hold", factionId, 999999, 50, rid);
                warship.speed = 1.2;
                warship.cannons = 10;
                warship.marines = 20;
                if (g_world.map.locations.count(rid)) {
                    warship.x = g_world.map.locations[rid].x;
                    warship.y = g_world.map.locations[rid].y;
                }
                g_world.ships.push_back(warship);
            }
        }
    }
    g_world.map.generation_tick = g_world.tick;


    // Initial news
    addNews("Мир создан. Эпоха " + g_world.era + " начинается.", "Весь мир", 3, "misc");
}

void bootstrapWorld(int days, int targetStartDay) {
    g_bootstrap = true;
    g_world.current_day = targetStartDay - days;
    if (g_world.current_day < 0) g_world.current_day = 0;
    
    // 1. Initial resources
    for (auto& [rid, r] : g_world.regions) {
        if (r.vault_id.empty()) continue;
        createItem(GoodType::BREAD, r.population * 0.15, r.vault_id, 0, "Bootstrap");
        createItem(GoodType::MEAT, r.population * 0.05, r.vault_id, 0, "Bootstrap");
        createItem(GoodType::WOOD, 200 + thread_safe_rand() % 200, r.vault_id, 0, "Bootstrap");
        createItem(GoodType::IRON_ORE, 100 + thread_safe_rand() % 200, r.vault_id, 0, "Bootstrap");
        createItem(GoodType::GOLD_INGOT, 500 + thread_safe_rand() % 500, r.vault_id, 0, "Bootstrap");
        createItem(GoodType::WEAPONS, 50 + thread_safe_rand() % 50, r.vault_id, 0, "Bootstrap");
    }

    // 2. Initial caravans
    std::vector<std::string> capitals;
    for (const auto& [fid, f] : g_world.factions) {
        if (!f.regions.empty()) capitals.push_back(f.regions[0]);
    }
    if (capitals.size() >= 2) {
        for (size_t i = 0; i < capitals.size(); ++i) {
            std::string origin = capitals[i];
            std::string dest = capitals[(i + 1) % capitals.size()];
            
            std::vector<std::pair<int,int>> caravan_path;
            if (g_path_cache.count({origin, dest})) caravan_path = g_path_cache[{origin, dest}];
            if (caravan_path.empty()) continue;

            std::string chestId = createContainer("caravan_chest", "bootstrap", 999999, 1000, origin);
            int num_goods = 2 + thread_safe_rand() % 3;
            for (int g = 0; g < num_goods; ++g) {
                GoodType gt = static_cast<GoodType>(thread_safe_rand() % (int)GoodType::COUNT);
                if (GOOD_CATEGORIES[(int)gt] == GoodCategory::DOCUMENT) { g--; continue; }
                createItem(gt, 20 + thread_safe_rand() % 51, chestId, 0, "Bootstrap Caravan");
            }
            
            Caravan c;
            c.id = "caravan_" + generateUUID();
            c.origin = origin;
            c.destination = dest;
            c.chest_id = chestId;
            c.wagons = 2;
            c.guards = 5;
            c.hoursLeft = 24 + thread_safe_rand() % 49;
            if (g_path_cache.count({origin, dest})) {
                c.path = g_path_cache[{origin, dest}];
                if (!c.path.empty()) { c.x = c.path[0].first; c.y = c.path[0].second; }
            }
            g_world.regions[origin].caravans.push_back(c);
        }
    }

    // 3. Simulate
    int ticks = days * 24;
    for (int i = 0; i < ticks; i++) {
        hourlyTick();
        g_world.tick++;
    }

    // 4. Cleanup
    g_world.news.clear();
    g_world.needsGlobalEvent = false;
    g_world.lastDirectInjectionDay = -999;
    g_world.tick = 0;
    g_world.current_day = targetStartDay;
    g_world.time.accumulatedMinutes = 0;
    g_world.time.internalHour = 0;
    updateWeather();
    g_bootstrap = false;
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

        // Отправляем прогресс каждый день (24 тика), чтобы интерфейс не зависал
        if (g_world.tick > 0 && g_world.tick % 24 == 0) {
            std::string lastNewsText = "";
            if (!g_world.news.empty()) {
                lastNewsText = g_world.news.back().text;
            }

            // Очищаем флаги и списки удаленных, чтобы не текла память, но НЕ отправляем их в Node.js
            // для экономии IPC трафика при пресимуляции.
            for (size_t i=0; i<g_items.data.size(); ++i) if (g_items.active[i]) g_items.data[i].is_dirty = false;
            for (size_t i=0; i<g_containers.data.size(); ++i) if (g_containers.active[i]) g_containers.data[i].is_dirty = false;
            g_deleted_items.clear();
            g_deleted_containers.clear();

            JsonValue emptyArr = JsonValue::array();
            reportProgress(g_world.tick, lastNewsText, emptyArr, emptyArr, emptyArr, emptyArr);
        }
    }
}

// ============================================================================
// REAL-TIME SIMULATION & PROTOCOL HANDLER
// ============================================================================

std::atomic<bool> g_realtime_active{false};
std::atomic<int> g_realtime_interval{500};
std::mutex g_main_mutex;
std::thread* g_realtime_thread = nullptr;

void serializeRegistriesGlobal(JsonValue& response, bool full = false) {
    std::lock_guard<std::recursive_mutex> lock(g_registry_mutex);
    JsonValue itemsArr = JsonValue::array();
    for (size_t i=0; i<g_items.data.size(); ++i) {
        if (!g_items.active[i]) continue;
        if (full || g_items.data[i].is_dirty) {
            JsonValue pair = JsonValue::array();
            pair.push(JsonValue(g_items.data[i].id));
            pair.push(g_items.data[i].toJson());
            itemsArr.push(pair);
            g_items.data[i].is_dirty = false;
        }
    }
    response.set("items", itemsArr);
    
    JsonValue contArr = JsonValue::array();
    for (size_t i=0; i<g_containers.data.size(); ++i) {
        if (!g_containers.active[i]) continue;
        if (full || g_containers.data[i].is_dirty) {
            JsonValue pair = JsonValue::array();
            pair.push(JsonValue(g_containers.data[i].id));
            pair.push(g_containers.data[i].toJson());
            contArr.push(pair);
            g_containers.data[i].is_dirty = false;
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
}

void realtimeLoop() {
    while (g_realtime_active) {
        {
            std::lock_guard<std::mutex> lock(g_main_mutex);
            if (!g_realtime_active) break;
            
            for (int i = 0; i < 24; i++) {
                hourlyTick();
                g_world.tick++;
            }
            
            JsonValue response = JsonValue::object();
            response.set("status", "realtime_update");
            response.set("tick", g_world.tick);
            
            JsonValue eventsArr = JsonValue::array();
            for (const auto& ev : g_world.player_trek.pending_events) {
                eventsArr.push(ev.toJson());
            }
            g_world.player_trek.pending_events.clear();
            response.set("trek_events", eventsArr);

            response.set("world", g_world.toJson());
            serializeRegistriesGlobal(response, false);
            
            std::cout << response.toString() << std::endl;
            std::cout.flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(g_realtime_interval.load()));
    }
}

int main() {
    std::string line;
    
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        
        JsonValue command = parseJson(line);
        JsonValue response = JsonValue::object();
        
        std::string cmd = command["command"].asString();

        if (cmd == "startRealtime") {
            g_realtime_interval = command.has("interval") ? command["interval"].asInt() : 500;
            if (!g_realtime_active) {
                g_realtime_active = true;
                if (g_realtime_thread) { g_realtime_thread->join(); delete g_realtime_thread; }
                g_realtime_thread = new std::thread(realtimeLoop);
            }
            response.set("status", "ok");
            std::cout << response.toString() << std::endl;
            continue;
        }
        else if (cmd == "stopRealtime") {
            g_realtime_active = false;
            response.set("status", "ok");
            std::cout << response.toString() << std::endl;
            continue;
        }

        std::lock_guard<std::mutex> lock(g_main_mutex);
        
        auto serializeRegistries = [&](bool full = false) {
            serializeRegistriesGlobal(response, full);
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
            int startDay = command.has("start_day") ? command["start_day"].asInt() : 0;
            
            buildWorld(playerId, era, initialAgents, globalLocs, startDay);
            
            response.set("status", "ok");
            response.set("tick", g_world.tick);
                        JsonValue eventsArr = JsonValue::array();
            for (const auto& ev : g_world.player_trek.pending_events) {
                eventsArr.push(ev.toJson());
            }
            g_world.player_trek.pending_events.clear();
            response.set("trek_events", eventsArr);

response.set("world", g_world.toJson());
            serializeRegistries(true);
        }
        else if (cmd == "bootstrapWorld") {
            int days = command.has("days") ? command["days"].asInt() : 45;
            int startDay = command.has("start_day") ? command["start_day"].asInt() : 0;
            bootstrapWorld(days, startDay);
            response.set("status", "ok");
            response.set("message", "Bootstrap completed");
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
            rebuildContainerIndices();
            g_deleted_items.clear();
            g_deleted_containers.clear();
            response.set("status", "ok");
        }
        else if (cmd == "getFullState") {
            response.set("status", "ok");
                        JsonValue eventsArr = JsonValue::array();
            for (const auto& ev : g_world.player_trek.pending_events) {
                eventsArr.push(ev.toJson());
            }
            g_world.player_trek.pending_events.clear();
            response.set("trek_events", eventsArr);

response.set("world", g_world.toJson());
            serializeRegistries(true);
        }
        else if (cmd == "bootstrapWorld") {
            int days = command.has("days") ? command["days"].asInt() : 45;
            int startDay = command.has("start_day") ? command["start_day"].asInt() : 0;
            bootstrapWorld(days, startDay);
            response.set("status", "ok");
            response.set("message", "Bootstrap completed");
            response.set("world", g_world.toJson());
            serializeRegistries(true);
        }
        else if (cmd == "gmIntervention") {
            std::string feedback = processGmIntervention(command);
            response.set("status", "ok");
            response.set("feedback", feedback);
                        JsonValue eventsArr = JsonValue::array();
            for (const auto& ev : g_world.player_trek.pending_events) {
                eventsArr.push(ev.toJson());
            }
            g_world.player_trek.pending_events.clear();
            response.set("trek_events", eventsArr);

response.set("world", g_world.toJson());
            serializeRegistries(false);
        }
        else if (cmd == "playerManageBusiness") {
            std::string action = command["action"].asString();
            const JsonValue& args = command["args"];
            
            if (action == "create") {
                std::string regionId = args["regionId"].asString();
                std::string facilityType = args["facilityType"].asString();
                std::string name = args["name"].asString();
                
                Business b;
                b.id = "bus_" + generateUUID();
                b.owner_ids.push_back("player");
                b.region_id = regionId;
                b.facility_type = facilityType;
                b.level = 1;
                b.cash_balance = 0;
                b.target_efficiency = 100;
                b.is_active = true;
                b.local_storage_id = createContainer("business_storage", "player", 999999, 1000, regionId);
                
                g_world.businesses[b.id] = b;
                if (g_world.regions.count(regionId)) {
                    placePrivateBusinessOnMap(g_world.regions[regionId], b, g_world);
                }

                // Динамическое добавление поселения на глобальную карту и прокладка дороги
                if (g_world.map.locations.count(regionId)) {
                    auto parentLoc = g_world.map.locations[regionId];
                    // Ищем свободный тайл рядом с родительским регионом для нового бизнеса
                    int bx = parentLoc.x + (rand() % 7) - 3;
                    int by = parentLoc.y + (rand() % 7) - 3;
                    bx = std::clamp(bx, 1, g_world.map.width - 2);
                    by = std::clamp(by, 1, g_world.map.height - 2);
                    
                    MapLocation newLoc;
                    newLoc.id = b.id;
                    newLoc.name = name;
                    newLoc.x = bx;
                    newLoc.y = by;
                    newLoc.type = "village";
                    newLoc.faction = g_world.regions[regionId].factionId;
                    g_world.map.locations[b.id] = newLoc;

                    // Прокладка новой дороги от бизнеса к городу
                    std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
                    std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
                    for (const auto& road : g_world.map.roads) {
                        for (const auto& wp : road.waypoints) {
                            has_road[wp.second * g_world.map.width + wp.first] = true;
                        }
                    }
                    
                    auto path = findPath(g_world.map, bx, by, parentLoc.x, parentLoc.y, has_road, path_status, MovementType::ANY);
                    if (!path.empty()) {
                        MapRoad newRoad;
                        newRoad.from = b.id;
                        newRoad.to = regionId;
                        newRoad.condition = "dirt";
                        newRoad.waypoints = path;
                        g_world.map.roads.push_back(newRoad);
                    }
                    g_world.map.generation_tick = g_world.tick;
                }

                response.set("status", "ok");
                response.set("business_id", b.id);
            }
            else if (action == "set_focus") {
                std::string bId = args["businessId"].asString();
                if (g_world.businesses.count(bId)) {
                    g_world.businesses[bId].production_focus = args["focus"].asString();
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "add_rule") {
                std::string bId = args["businessId"].asString();
                if (g_world.businesses.count(bId)) {
                    LogisticRule r = LogisticRule::fromJson(args["rule"]);
                    r.id = "log_" + generateUUID();
                    g_world.businesses[bId].logistics.push_back(r);
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "remove_rule") {
                std::string bId = args["businessId"].asString();
                std::string ruleId = args["ruleId"].asString();
                if (g_world.businesses.count(bId)) {
                    auto& logs = g_world.businesses[bId].logistics;
                    logs.erase(std::remove_if(logs.begin(), logs.end(), [&](const LogisticRule& r){ return r.id == ruleId; }), logs.end());
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "set_employees") {
                std::string bId = args["businessId"].asString();
                if (g_world.businesses.count(bId)) {
                    g_world.businesses[bId].target_employee_count = args["count"].asInt();
                    g_world.businesses[bId].employee_count = std::min(args["count"].asInt(), g_world.businesses[bId].level * 100);
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "set_efficiency") {
                std::string bId = args["businessId"].asString();
                if (g_world.businesses.count(bId)) {
                    g_world.businesses[bId].target_efficiency = std::clamp(args["efficiency"].asInt(), 0, 100);
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "deposit_cash") {
                std::string bId = args["businessId"].asString();
                int amount = args["amount"].asInt();
                if (g_world.businesses.count(bId)) {
                    g_world.businesses[bId].cash_balance += amount;
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
            else if (action == "withdraw_cash") {
                std::string bId = args["businessId"].asString();
                int amount = args["amount"].asInt();
                if (g_world.businesses.count(bId) && g_world.businesses[bId].cash_balance >= amount) {
                    g_world.businesses[bId].cash_balance -= amount;
                    response.set("status", "ok");
                } else response.set("status", "error");
            }
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
                rebuildContainerIndices();
            }
            
            simulateTicks(ticks);
            
            response.set("status", "ok");
            response.set("tick", g_world.tick);
            response.set("news_count", (int)g_world.news.size());
                        JsonValue eventsArr = JsonValue::array();
            for (const auto& ev : g_world.player_trek.pending_events) {
                eventsArr.push(ev.toJson());
            }
            g_world.player_trek.pending_events.clear();
            response.set("trek_events", eventsArr);

response.set("world", g_world.toJson());
            serializeRegistries(false);
        }
                else if (cmd == "startTrek") {
            std::string start_id = command["start_id"].asString();
            std::string dest_id = command["destination_id"].asString();
            
            g_world.player_trek.active = true;
            g_world.player_trek.paused = false;
            g_world.player_trek.destination_id = dest_id;
            g_world.player_trek.elapsed_hours = 0;
            g_world.player_trek.hours_since_last_bandit = 4;
            g_world.player_trek.seen_object_ids.clear();
            g_world.player_trek.pending_events.clear();
            
            if (g_world.map.locations.count(start_id) && g_world.map.locations.count(dest_id)) {
                auto loc1 = g_world.map.locations[start_id];
                auto loc2 = g_world.map.locations[dest_id];
                g_world.player_trek.current_x = loc1.x;
                g_world.player_trek.current_y = loc1.y;
                
                std::vector<bool> has_road(g_world.map.width * g_world.map.height, false);
                std::vector<int> path_status(g_world.map.width * g_world.map.height, 0);
                for (const auto& road : g_world.map.roads) {
                    if (road.condition == "blocked") {
                        for (const auto& wp : road.waypoints) path_status[wp.second * g_world.map.width + wp.first] = 2;
                    } else if (road.condition == "ruined") {
                        for (const auto& wp : road.waypoints) {
                            path_status[wp.second * g_world.map.width + wp.first] = 1;
                            has_road[wp.second * g_world.map.width + wp.first] = true;
                        }
                    } else {
                        for (const auto& wp : road.waypoints) has_road[wp.second * g_world.map.width + wp.first] = true;
                    }
                }
                
                g_world.player_trek.path = findPath(g_world.map, loc1.x, loc1.y, loc2.x, loc2.y, has_road, path_status, MovementType::ANY, 1);
                g_world.player_trek.path_index = 0;
                
                double total_dist = 0;
                for(size_t i=0; i+1<g_world.player_trek.path.size(); ++i) {
                    total_dist += std::hypot(g_world.player_trek.path[i+1].first - g_world.player_trek.path[i].first, 
                                             g_world.player_trek.path[i+1].second - g_world.player_trek.path[i].second);
                }
                g_world.player_trek.total_hours = std::max(1, (int)(total_dist / 0.5));
            } else {
                g_world.player_trek.total_hours = 24;
            }
            
            response.set("status", "ok");
            response.set("total_hours", g_world.player_trek.total_hours);
            response.set("message", "Trek started");
        }
        else if (cmd == "pauseTrek") {
            g_world.player_trek.paused = true;
            response.set("status", "ok");
        }
        else if (cmd == "resumeTrek") {
            g_world.player_trek.paused = false;
            response.set("status", "ok");
        }
        else if (cmd == "cancelTrek") {
            g_world.player_trek.active = false;
            response.set("status", "ok");
        }
        else if (cmd == "interactWithObject") {
            g_world.player_trek.paused = true;
            std::string obj_type = command["object_type"].asString();
            std::string sim_id = command["sim_object_id"].asString();
            JsonValue objData = JsonValue::object();
            
            if (obj_type == "caravan") {
                for (const auto& [rid, r] : g_world.regions) {
                    for (const auto& c : r.caravans) {
                        if (c.id == sim_id) { objData = c.toJson(); break; }
                    }
                }
            } else if (obj_type == "army") {
                for (const auto& [fid, f] : g_world.factions) {
                    for (const auto& a : f.armies) {
                        if (a.id == sim_id) { objData = a.toJson(); objData.set("faction_name", f.name); break; }
                    }
                }
            }
            response.set("status", "ok");
            response.set("object_data", objData);
        }

        else if (cmd == "getWorldMap") {
            response.set("status", "ok");
            response.set("map", g_world.map.toJson());
        }
        else if (cmd == "gmModifyTerrain") {
            std::string regionId = command["args"]["regionId"].asString();
            int radius = command["args"]["radius"].asInt();
            int newTypeInt = command["args"]["newType"].asInt();
            TileType newType = static_cast<TileType>(newTypeInt);
            
            if (g_world.map.locations.count(regionId)) {
                auto loc = g_world.map.locations[regionId];
                for (int y = std::max(0, loc.y - radius); y <= std::min(g_world.map.height - 1, loc.y + radius); ++y) {
                    for (int x = std::max(0, loc.x - radius); x <= std::min(g_world.map.width - 1, loc.x + radius); ++x) {
                        if (std::hypot(x - loc.x, y - loc.y) <= radius) {
                            g_world.map.grid[y * g_world.map.width + x].type = newType;
                        }
                    }
                }
                g_world.map.generation_tick = g_world.tick;
                response.set("status", "ok");
                response.set("message", "Terrain modified");
            } else {
                response.set("status", "error");
                response.set("message", "Region not found on map");
            }
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
