# Nexus Engine — нативный движок симуляции

## Структура
```
engine/
├── generate_data.py      # Генератор C++ кода из JSON
├── generated_data.h      # Авто-генерируемые данные (не редактировать!)
├── meterea_engine.cpp    # Исходный код движка
├── meterea_engine        # Скомпилированный бинарник
└── README.md             # Этот файл
```

## Быстрый старт

### 1. Генерация данных из JSON
```bash
cd engine
python generate_data.py
```

### 2. Компиляция
```bash
g++ -std=c++17 -O2 -o meterea_engine meterea_engine.cpp
```

### 3. Тестирование
```bash
# Инициализация
echo '{"command":"init"}' | ./meterea_engine

# Построение мира
echo '{"command":"buildWorld","player_id":1}' | ./meterea_engine

# Симуляция 5 тиков
echo '{"command":"simulateTicks","ticks":5}' | ./meterea_engine
```

## Протокол взаимодействия

Все команды передаются через stdin в формате JSON, завершённом `\n`.
Ответ возвращается в stdout также в формате JSON с `\n`.

### Команды

#### `init`
Инициализирует движок.
```json
{"command":"init"}
```
Ответ:
```json
{"status":"ok","message":"Engine initialized"}
```

#### `buildWorld`
Создаёт начальное состояние мира.
```json
{"command":"buildWorld","player_id":1}
```
Ответ:
```json
{
  "status":"ok",
  "world": {
    "tick": 0,
    "regions": [{"id":"region_1","name":"Долина Рассвета","population":5000}]
  }
}
```

#### `simulateTicks`
Запускает симуляцию на указанное количество тиков.
```json
{"command":"simulateTicks","ticks":24}
```
Ответ:
```json
{"status":"ok","tick":24,"news_count":5}
```

## Автоматическая синхронизация данных

При добавлении нового товара в `data/economy_items.json`:

1. Запускаешь `python generate_data.py`
2. Пересобираешь движок: `g++ -std=c++17 -O2 -o meterea_engine meterea_engine.cpp`
3. Всё! Движок автоматически знает о новом товаре, его цене, категории и названиях для всех 4 эр.

**Никакого ручного дублирования кода!**

## Интеграция с Electron

В `main.js`:
```javascript
const { spawn } = require('child_process');

class NexusEngine {
    constructor() {
        this.process = spawn('./engine/meterea_engine');
        this.buffer = '';
        
        this.process.stdout.on('data', (data) => {
            this.buffer += data.toString();
            if (this.buffer.endsWith('\n')) {
                const response = JSON.parse(this.buffer.trim());
                this.handleResponse(response);
                this.buffer = '';
            }
        });
    }
    
    send(command) {
        this.process.stdin.write(JSON.stringify(command) + '\n');
    }
    
    handleResponse(response) {
        console.log('Engine:', response);
    }
}
```

## Текущая реализация

✅ Генератор данных из JSON  
✅ Enum товаров (21 предмет)  
✅ Категории товаров (11 категорий)  
✅ Базовые цены  
✅ Рецепты крафта (12 рецептов)  
✅ Система порчи предметов  
✅ Производство по рецептам  
✅ Генерация новостей  
✅ Протокол stdin/stdout  

🔄 Сохранение состояния между вызовами (статичная переменная)  
⏳ Политика и дипломатия  
⏳ Караваны и торговля  
⏳ Полная сериализация мира в JSON  
