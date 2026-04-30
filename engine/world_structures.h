// World Structure Engine (WSE) - Data Structures
// T3 Implementation - Stage 1: Basic Structure Definitions
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations / Stub structures for World Structure Engine
// These will be fully implemented in subsequent stages

struct Settlement {
    std::string id;
    std::string name;
    std::string settlement_type;
    int population;
    int wealth_level;
    std::string faction_id;
    std::string era;
    double coordinates_x;
    double coordinates_y;
    bool defensive_wall;
    std::vector<std::string> districts;  // List of district names
};

struct Building {
    std::string id;
    std::string type;
    std::string settlement_id;
    int wealth_level;
    std::string era;
    std::string facility_id;  // empty if not a production building
    int floors;
    int width;   // For room generation
    int height;  // For room generation (depth)
    // rooms, etc. will be added later
};

struct Room {
    std::string id;
    std::string name;
    std::string type;
    std::string building_id;
    double size_x;
    double size_y;
    std::string floor_material;
    double ceiling_height;
    // wall_segments_ids, doors_ids, windows_ids, furniture_ids will be added later
    double light_level;
    double temperature_modifier;
    double humidity;
    // description will be added later
    
    // Additional fields for detailed generation
    int x;              // Position in building grid
    int y;              // Position in building grid
    int width;          // Room width
    int height;         // Room height (depth)
    int floor_level;    // Floor number (1-based)
    std::string purpose; // Room purpose (bedroom, kitchen, etc.)
    bool is_outdoor;    // Is this an outdoor space
};

struct WallSegment {
    std::string id;
    std::string material;
    std::string segment_type;  // load_bearing, partition, exterior, fence
    double health;
    bool flammable;
    double thermal_insulation;
    // connected rooms, position data will be added later
    
    // Additional fields for detailed generation
    std::string room_id;
    std::string building_id;
    int x, y, z;        // Position coordinates
    int length;         // Length of segment
    std::string axis;   // "x" or "y" orientation
    int thickness;
    int height;
    bool is_destroyed;
    std::string material_id; // Alias for material catalog lookup
};

struct Door {
    std::string id;
    std::string room_a_id;
    std::string room_b_id;  // empty if leads outside
    bool is_open;
    bool is_locked;
    bool is_broken;
    double health;
    int lock_difficulty;
    std::string key_id;  // empty if no key required
    
    // Additional fields for detailed generation
    std::string room_id;
    std::string building_id;
    int x, y, z;        // Position coordinates
    std::string axis;   // "x" or "y" orientation
};

struct Window {
    std::string id;
    std::string room_id;
    std::string wall_segment_id;
    bool is_open;
    bool is_broken;
    double light_transmission;
    
    // Additional fields for detailed generation
    std::string building_id;
    int x, y, z;        // Position coordinates
    std::string axis;   // "x" or "y" orientation
    std::string material_id;
};

struct Stairs {
    std::string id;
    std::string room_lower_id;
    std::string room_upper_id;
    bool is_broken;
    // position data will be added later
    
    // Additional fields for detailed generation
    std::string building_id;
    int x, y, z;        // Position coordinates (starting floor)
    std::string direction; // "up" or "down"
    int steps_count;
};

struct Furniture {
    std::string id;
    std::string type;
    std::string room_id;
    std::string material;
    double health;
    double weight;
    bool flammable;
    bool is_container;
    int max_weight;       // if container
    int max_slots;        // if container
    int lock_difficulty;  // if container can be locked
    double trap_chance;   // if container can be trapped
    std::string key_id;   // if locked
    // items content will be populated in Stage 2
    
    // Additional fields for detailed generation
    std::string building_id;
    std::string template_id;  // Reference to catalog
    std::string name;         // Display name
    int x, y, z;              // Position coordinates
};

// Main World Structures Registry
struct WorldStructures {
    std::unordered_map<std::string, Settlement> settlements;
    std::unordered_map<std::string, Building> buildings;
    std::unordered_map<std::string, Room> rooms;
    std::unordered_map<std::string, WallSegment> wallSegments;
    std::unordered_map<std::string, Door> doors;
    std::unordered_map<std::string, Window> windows;
    std::unordered_map<std::string, Stairs> stairs;
    std::unordered_map<std::string, Furniture> furniture;
};
