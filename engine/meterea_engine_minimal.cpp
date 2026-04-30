// Nexus Engine - Native World Simulation Core
// Communicates via stdin/stdout JSON protocol
// Этап 2: Полная сериализация состояния мира

#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>
#include <random>
#include <algorithm>

#include "generated_data.h"

// === Структуры данных мира ===

struct PhysicalItem {
    GoodType prototype_id;
