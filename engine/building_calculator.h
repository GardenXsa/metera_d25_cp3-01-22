#pragma once

#include <string>
#include <map>
#include <cmath>
#include "settlement_generator.h"

// Структура для хранения настроек коэффициентов (позволяет настраивать эмпирические значения)
struct BuildingCoefficients {
    // Жилые дома (на 100 человек)
    double poor_house_per_100 = 8.0;      // Бедные дома
    double mid_house_per_100 = 4.0;       // Средние дома
    double rich_house_per_100 = 1.0;      // Богатые дома

    // Общественные здания (на количество населения)
    int tavern_population_divisor = 500;   // 1 таверна на 500 чел.
    int temple_population_divisor = 1000;  // 1 храм на 1000 чел.
    int market_population_divisor = 800;   // 1 рынок на 800 чел.
    int inn_population_divisor = 600;      // 1 гостиница на 600 чел.
    int school_population_divisor = 700;   // 1 школа/библиотека на 700 чел.
    int clinic_population_divisor = 900;   // 1 лечебница на 900 чел.
    int warehouse_population_divisor = 1000; // 1 склад на 1000 чел.
    int prison_population_divisor = 2000;  // 1 тюрьма на 2000 чел.

    // Административные и военные (зависят от типа и статуса)
    int barracks_population_divisor = 400; // 1 казарма на 400 чел. (для военных поселений меньше)
    
    // Минимальное количество зданий для любого поселения (>0)
    int min_taverns = 1;
    int min_markets = 1;
    int min_warehouses = 1;
};

// Результат расчета: словарь (тип здания -> количество)
using BuildingCountMap = std::map<std::string, int>;

class BuildingCalculator {
public:
    BuildingCalculator(const BuildingCoefficients& coeffs = BuildingCoefficients()) 
        : coefficients(coeffs) {}

    /**
     * Рассчитывает количество непроизводственных зданий для поселения.
     * 
     * @param settlement Структура поселения с полями population, wealth_level, settlement_type.
     * @return Map с названиями типов зданий и их количеством.
     */
    BuildingCountMap calculateNonProductionBuildings(const Settlement& settlement) {
        BuildingCountMap result;
        int pop = settlement.population;
        int wealth = settlement.wealth_level; // 0..10

        if (pop <= 0) return result;

        // --- 1. Жилые дома (распределение по богатству) ---
        // Логика: чем выше wealth_level, тем больше доля средних и богатых домов.
        // Базовое распределение (сумма долей ~ 100% от необходимого жилья):
        // wealth 0-2: 80% бедных, 18% средних, 2% богатых
        // wealth 5: 40% бедных, 50% средних, 10% богатых
        // wealth 10: 10% бедных, 50% средних, 40% богатых
        
        double poor_ratio = std::max(0.1, 0.8 - (wealth / 10.0) * 0.7);
        double rich_ratio = std::min(0.4, (wealth / 10.0) * 0.4);
        double mid_ratio = 1.0 - poor_ratio - rich_ratio;

        // Общее количество жилых единиц (условно 1 дом на 4-5 человек, но зависит от типа)
        // Используем коэффициенты из настроек как базу на 100 человек
        int total_houses_base = static_cast<int>(std::ceil(pop / 4.5)); 
        
        int poor_count = static_cast<int>(total_houses_base * poor_ratio);
        int mid_count = static_cast<int>(total_houses_base * mid_ratio);
        int rich_count = static_cast<int>(total_houses_base * rich_ratio);

        // Корректировка, чтобы сумма совпадала (из-за округления)
        int diff = total_houses_base - (poor_count + mid_count + rich_count);
        mid_count += diff; 

        result["house_poor"] = std::max(1, poor_count);
        result["house_mid"] = std::max(0, mid_count);
        result["house_rich"] = std::max(0, rich_count);

        // --- 2. Общественные здания (эмпирические формулы) ---

        // Таверны
        int taverns = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.tavern_population_divisor));
        result["tavern"] = std::max(coefficients.min_taverns, taverns);

        // Рынки
        int markets = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.market_population_divisor));
        result["market"] = std::max(coefficients.min_markets, markets);

        // Склады
        int warehouses = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.warehouse_population_divisor));
        result["warehouse"] = std::max(coefficients.min_warehouses, warehouses);

        // Храмы (зависят от типа поселения, в руинах могут отсутствовать)
        if (settlement.settlement_type != "ruin_outpost") {
            int temples = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.temple_population_divisor));
            // В маленьких поселениях храм может быть один общий или отсутствовать, если совсем мало людей
            if (pop > 50 || settlement.settlement_type == "village" || settlement.settlement_type == "town" || settlement.settlement_type == "city") {
                result["temple"] = std::max(1, temples);
            } else {
                result["temple"] = temples;
            }
        } else {
            result["temple"] = 0;
        }

        // Гостиницы (Inn) - важны для городов и торговых путей
        if (settlement.settlement_type == "city" || settlement.settlement_type == "town" || settlement.settlement_type == "fort") {
            int inns = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.inn_population_divisor));
            result["inn"] = std::max(1, inns);
        } else {
            // В деревнях гостиница может быть объединена с таверной, считаем отдельно только если население большое
            int inns = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.inn_population_divisor));
            if (inns > 0) result["inn"] = inns;
        }

        // Школы / Библиотеки
        if (settlement.settlement_type == "city" || settlement.settlement_type == "town") {
            int schools = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.school_population_divisor));
            result["school"] = std::max(1, schools);
            if (settlement.settlement_type == "city") {
                result["library"] = std::max(1, schools / 2); // Библиотека каждые 2 школы
            }
        }

        // Лечебницы
        if (settlement.settlement_type != "ruin_outpost" && settlement.settlement_type != "nomad_camp") {
            int clinics = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.clinic_population_divisor));
            if (pop > 100) {
                result["clinic"] = std::max(1, clinics);
            }
        }

        // Тюрьмы (только в крупных поселениях с администрацией)
        if (settlement.settlement_type == "city" || settlement.settlement_type == "town" || settlement.settlement_type == "fort") {
            int prisons = static_cast<int>(std::ceil(static_cast<double>(pop) / coefficients.prison_population_divisor));
            result["prison"] = std::max(1, prisons);
        }

        // Казармы (зависят от типа поселения)
        int barracks_divisor = coefficients.barracks_population_divisor;
        if (settlement.settlement_type == "fort") {
            barracks_divisor /= 3; // В фортах много казарм
        } else if (settlement.settlement_type == "city") {
            barracks_divisor /= 2;
        }
        
        if (settlement.settlement_type == "fort" || settlement.settlement_type == "city" || settlement.settlement_type == "town") {
            int barracks = static_cast<int>(std::ceil(static_cast<double>(pop) / barracks_divisor));
            result["barracks"] = std::max(1, barracks);
        } else if (settlement.settlement_type == "village") {
            // В деревне может быть небольшой гарнизон
            result["barracks"] = (pop > 300) ? 1 : 0;
        }

        // Ратуша (только одна в крупных поселениях)
        if (settlement.settlement_type == "city" || settlement.settlement_type == "town") {
            result["town_hall"] = 1;
        }

        return result;
    }

    // Метод для изменения коэффициентов на лету
    void setCoefficients(const BuildingCoefficients& newCoeffs) {
        coefficients = newCoeffs;
    }

private:
    BuildingCoefficients coefficients;
};
