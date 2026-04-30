#!/usr/bin/env python3
import subprocess
import json
import sys

def run_command(cmd_data):
    exe_name = './meterea_engine.exe' if sys.platform == 'win32' else './meterea_engine'
    proc = subprocess.Popen(
        [exe_name],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding='utf-8'
    )
    
    stdout, stderr = proc.communicate(input=json.dumps(cmd_data, ensure_ascii=False) + '\n')
    if stderr:
        print(f"STDERR: {stderr}")
    
    lines = stdout.strip().split('\n')
    final_result = None
    
    for line in lines:
        if not line.strip():
            continue
        try:
            data = json.loads(line)
            if data.get("status") == "progress":
                # Выводим прогресс в консоль теста, чтобы видеть работу движка
                print(f"      [ENGINE] {data.get('message')}")
            else:
                final_result = data
        except json.JSONDecodeError as e:
            print(f"❌ Ошибка парсинга строки: {line}")
            raise e
            
    return final_result

print("🧪 Тест 1: init команда")
result = run_command({"command": "init"})
assert result["status"] == "ok", f"Ожидался 'ok', получено: {result}"
print(f"   ✅ init: {result['message']}")

print("\n🧪 Тест 2: buildWorld команда")
result = run_command({"command": "buildWorld", "player_id": 1})
assert result["status"] == "ok", f"Ожидался 'ok', получено: {result}"
assert "world" in result, "Отсутствует поле 'world'"
world = result["world"]
assert world["tick"] == 0, f"Ожидался tick=0, получено: {world['tick']}"
assert len(world["regions"]) > 0, "Нет регионов в мире"
print(f"   ✅ buildWorld: tick={world['tick']}, регионов={len(world['regions'])}")

print("\n🧪 Тест 3: simulateTicks команда (передача состояния)")
world_json = json.dumps(world)
result = run_command({
    "command": "simulateTicks",
    "world": world,
    "ticks": 5
})
assert result["status"] == "ok", f"Ожидался 'ok', получено: {result}"
assert result["tick"] == 5, f"Ожидался tick=5, получено: {result['tick']}"
assert "world" in result, "Отсутствует поле 'world' в ответе"
new_world = result["world"]
assert new_world["tick"] == 5, f"Мир не обновился: tick={new_world['tick']}"
print(f"   ✅ simulateTicks: tick={result['tick']}, news_count={result['news_count']}")

print("\n🧪 Тест 4: непрерывная симуляция (сохранение состояния)")
# Передаём мир из предыдущего шага дальше
result2 = run_command({
    "command": "simulateTicks",
    "world": new_world,
    "ticks": 10
})
assert result2["tick"] == 15, f"Ожидался tick=15, получено: {result2['tick']}"
print(f"   ✅ Непрерывная симуляция: tick={result2['tick']}")

print("\n🧪 Тест 5: проверка структуры мира")
w = result2["world"]
assert "era" in w, "Отсутствует era"
assert "regions" in w, "Отсутствуют regions"
assert "factions" in w, "Отсутствуют factions"
assert "npcs" in w, "Отсутствуют npcs"
assert "news" in w, "Отсутствуют news"
region = list(w["regions"].values())[0]
assert "id" in region, "У региона нет id"
assert "name" in region, "У региона нет name"
assert "facilities" in region, "У региона нет facilities"
print(f"   ✅ Структура мира корректна")

print("\n🧪 Тест 6: Длительная симуляция (30 дней = 720 тиков)")
items = result2.get("items", [])
containers = result2.get("containers", [])
result3 = run_command({
    "command": "simulateTicks",
    "world": w,
    "items": items,
    "containers": containers,
    "ticks": 720
})
assert result3["status"] == "ok", f"Ожидался 'ok', получено: {result3['status']}"
assert result3["tick"] == 735, f"Ожидался tick=735, получено: {result3['tick']}"
print(f"   ✅ Длительная симуляция пройдена: tick={result3['tick']}, новостей={result3['news_count']}")

print("\n🧪 Тест 7: Стресс-тест (1 год = 8640 тиков)")
result4 = run_command({
    "command": "simulateTicks",
    "world": result3["world"],
    "items": result3.get("items", []),
    "containers": result3.get("containers", []),
    "ticks": 8640
})
assert result4["status"] == "ok", f"Ожидался 'ok', получено: {result4['status']}"
print(f"   ✅ Стресс-тест пройден: tick={result4['tick']}, новостей={result4['news_count']}")

print("\n" + "="*50)
print("✅ ВСЕ ТЕСТЫ ПРОЙДЕНЫ!")
print("="*50)
