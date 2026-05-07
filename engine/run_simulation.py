#!/usr/bin/env python3
import customtkinter as ctk
import tkinter as tk
from tkinter import messagebox
import subprocess
import threading
import json
import sys
import os
import random

try:
    from PIL import Image, ImageTk, ImageDraw
except ImportError:
    print("КРИТИЧЕСКАЯ ОШИБКА: Библиотека Pillow не установлена.")
    print("Откройте консоль и выполните команду: pip install Pillow")
    sys.exit(1)

ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

class EngineProcess:
    def __init__(self, on_progress, on_result, on_error):
        self.proc = None
        self.on_progress = on_progress
        self.on_result = on_result
        self.on_error = on_error
        self.is_running = False

    def start(self):
        # Абсолютное разрешение путей (защита от запуска из другой папки)
        base_dir = os.path.dirname(os.path.abspath(__file__))
        exe_name = 'meterea_engine.exe' if sys.platform == 'win32' else 'meterea_engine'
        exe_path = os.path.join(base_dir, exe_name)
        
        if not os.path.exists(exe_path):
            self.on_error(f"Движок не найден по пути:\n{exe_path}\nСкомпилируйте C++ код.")
            return False
        
        try:
            # Запускаем процесс, жестко привязывая рабочую директорию (cwd) к папке engine
            self.proc = subprocess.Popen(
                [exe_path],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding='utf-8',
                cwd=base_dir
            )
            self.is_running = True
            threading.Thread(target=self._read_loop, daemon=True).start()
            return True
        except Exception as e:
            self.on_error(f"Критическая ошибка запуска процесса: {e}")
            return False

    def send(self, cmd_data):
        if self.is_running and self.proc:
            try:
                self.proc.stdin.write(json.dumps(cmd_data, ensure_ascii=False) + '\n')
                self.proc.stdin.flush()
            except Exception as e:
                self.on_error(f"Ошибка отправки данных в движок: {e}")

    def _read_loop(self):
        while self.is_running and self.proc:
            line = self.proc.stdout.readline()
            if not line:
                # Если readline вернул пустоту, значит процесс умер или закрыл stdout
                break
            
            line = line.strip()
            if not line: continue
            
            try:
                data = json.loads(line)
                if data.get("status") == "progress":
                    self.on_progress(data.get("message", ""))
                elif data.get("status") == "ok":
                    self.on_result(data)

                elif data.get("status") == "realtime_update":
                    self.on_result(data)
                elif data.get("status") == "error":
                    self.on_error(data.get("message", "Неизвестная ошибка движка"))
            except json.JSONDecodeError:
                print(f"[RAW ENGINE OUTPUT] {line}")
                
        self.is_running = False
        
        # Проверка на тихое падение (Silent Crash)
        if self.proc:
            self.proc.poll()
            if self.proc.returncode is not None and self.proc.returncode != 0:
                err_output = self.proc.stderr.read()
                error_msg = f"Движок аварийно завершился (Код: {self.proc.returncode}).\n"
                if "dll" in err_output.lower() or self.proc.returncode == 3221225781: # 0xC0000135
                    error_msg += "\nПохоже, не хватает системных библиотек C++ (DLL). Перекомпилируйте движок с флагом -static:\n"
                    error_msg += "g++ -std=c++17 -O2 -static -o meterea_engine meterea_engine.cpp"
                else:
                    error_msg += f"Вывод ошибок:\n{err_output}"
                self.on_error(error_msg)

    def stop(self):
        self.is_running = False
        if self.proc:
            self.proc.terminate()
            self.proc = None


class SimulationApp(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Nexus Engine - Панель Симуляции и Картографии")
        self.geometry("1300x850")
        self.minsize(900, 600)
        
        self.engine = EngineProcess(self.handle_progress, self.handle_result, self.handle_error)
        self.world_data = None
        self.current_filter = "all"
        self.map_photo = None
        self.map_zoom = 4
        self.drag_data = {"x": 0, "y": 0}
        
        self.setup_ui()
        
    def setup_ui(self):
        # Левая панель (Управление)
        self.sidebar = ctk.CTkFrame(self, width=300, corner_radius=0)
        self.sidebar.pack(side="left", fill="y")
        self.sidebar.pack_propagate(False)
        
        ctk.CTkLabel(self.sidebar, text="NEXUS ENGINE", font=ctk.CTkFont(size=20, weight="bold"), text_color="#5dade2").pack(pady=(20, 5))
        ctk.CTkLabel(self.sidebar, text="World Simulator Control", font=ctk.CTkFont(size=12, slant="italic"), text_color="#7f8c8d").pack(pady=(0, 20))
        
        # Настройки генерации
        settings_frame = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        settings_frame.pack(fill="x", padx=15)
        
        ctk.CTkLabel(settings_frame, text="Эпоха:", anchor="w").pack(fill="x")
        self.era_var = ctk.StringVar(value="rebirth")
        self.era_menu = ctk.CTkOptionMenu(settings_frame, variable=self.era_var, values=["rebirth", "architects", "sundering", "silence"])
        self.era_menu.pack(fill="x", pady=(0, 15))
        
        ctk.CTkLabel(settings_frame, text="Начальное население (Агенты):", anchor="w").pack(fill="x")
        self.agents_var = ctk.IntVar(value=100)
        self.agents_slider = ctk.CTkSlider(settings_frame, from_=50, to=500, variable=self.agents_var)
        self.agents_slider.pack(fill="x", pady=(0, 5))
        self.agents_lbl = ctk.CTkLabel(settings_frame, textvariable=self.agents_var)
        self.agents_lbl.pack(pady=(0, 15))
        
        ctk.CTkLabel(settings_frame, text="Время симуляции:", anchor="w").pack(fill="x")
        self.time_var = ctk.StringVar(value="1 год")
        self.time_menu = ctk.CTkOptionMenu(settings_frame, variable=self.time_var, values=["1 месяц", "6 месяцев", "1 год", "2 года", "5 лет", "10 лет"])
        self.time_menu.pack(fill="x", pady=(0, 20))
        
        # Кнопки управления
        self.btn_init = ctk.CTkButton(self.sidebar, text="1. Инициализация и Генерация", command=self.start_generation, fg_color="#27ae60", hover_color="#2ecc71")
        self.btn_init.pack(fill="x", padx=15, pady=5)
        
        self.btn_sim = ctk.CTkButton(self.sidebar, text="2. Шаг Симуляции", command=self.start_simulation, state="disabled", fg_color="#2980b9", hover_color="#3498db")
        self.btn_sim.pack(fill="x", padx=15, pady=5)
        
        self.btn_realtime = ctk.CTkButton(self.sidebar, text="▶ Реал-тайм Симуляция", command=self.toggle_realtime, state="disabled", fg_color="#8e44ad", hover_color="#9b59b6")
        self.btn_realtime.pack(fill="x", padx=15, pady=5)
        
        self.speed_var = ctk.IntVar(value=500)
        self.speed_slider = ctk.CTkSlider(self.sidebar, from_=50, to=2000, variable=self.speed_var, command=self.update_speed)
        self.speed_slider.pack(fill="x", padx=15, pady=5)
        self.speed_lbl = ctk.CTkLabel(self.sidebar, text="Скорость: 500 мс/день", font=ctk.CTkFont(size=11))
        self.speed_lbl.pack(pady=(0, 10))
        
        self.btn_realtime = ctk.CTkButton(self.sidebar, text="▶ Реал-тайм Симуляция", command=self.toggle_realtime, state="disabled", fg_color="#8e44ad", hover_color="#9b59b6")
        self.btn_realtime.pack(fill="x", padx=15, pady=5)
        
        self.speed_var = ctk.IntVar(value=500)
        self.speed_slider = ctk.CTkSlider(self.sidebar, from_=50, to=2000, variable=self.speed_var, command=self.update_speed)
        self.speed_slider.pack(fill="x", padx=15, pady=5)
        self.speed_lbl = ctk.CTkLabel(self.sidebar, text="Скорость: 500 мс/день", font=ctk.CTkFont(size=11))
        self.speed_lbl.pack(pady=(0, 10))
        

        self.btn_stop = ctk.CTkButton(self.sidebar, text="Остановить Движок", command=self.stop_engine, fg_color="#c0392b", hover_color="#e74c3c")
        self.btn_stop.pack(fill="x", padx=15, pady=20)
        
        # Статус
        self.status_lbl = ctk.CTkLabel(self.sidebar, text="Движок остановлен", text_color="#e74c3c")
        self.status_lbl.pack(side="bottom", pady=10)
        
        self.progress_bar = ctk.CTkProgressBar(self.sidebar)
        self.progress_bar.set(0)
        self.progress_bar.pack(side="bottom", fill="x", padx=15, pady=5)
        
        # Правая панель (Летопись и Карты)
        self.main_area = ctk.CTkFrame(self)
        self.main_area.pack(side="right", fill="both", expand=True, padx=10, pady=10)
        
        # Фильтры
        filter_frame = ctk.CTkFrame(self.main_area, fg_color="transparent")
        filter_frame.pack(fill="x", pady=(0, 10))
        
        ctk.CTkLabel(filter_frame, text="Вид:", font=ctk.CTkFont(weight="bold")).pack(side="left", padx=(0, 10))
        
        self.filters = {
            "all": ctk.CTkButton(filter_frame, text="Все", width=60, command=lambda: self.set_filter("all"), fg_color="#34495e"),
            "war": ctk.CTkButton(filter_frame, text="⚔️ Войны", width=80, command=lambda: self.set_filter("war"), fg_color="#c0392b"),
            "trade": ctk.CTkButton(filter_frame, text="💰 Торговля", width=80, command=lambda: self.set_filter("trade"), fg_color="#f39c12", text_color="black"),
            "disaster": ctk.CTkButton(filter_frame, text="🌪️ Бедствия", width=80, command=lambda: self.set_filter("disaster"), fg_color="#d35400"),
            "business": ctk.CTkButton(filter_frame, text="🏭 Бизнес", width=80, command=lambda: self.set_filter("business"), fg_color="#9b59b6"),
            "market": ctk.CTkButton(filter_frame, text="⚖️ Рынок", width=80, command=lambda: self.set_filter("market"), fg_color="#1abc9c"),
            "logistics": ctk.CTkButton(filter_frame, text="📦 Логистика", width=90, command=lambda: self.set_filter("logistics"), fg_color="#34495e"),
            "stats": ctk.CTkButton(filter_frame, text="📊 Статистика", width=90, command=lambda: self.set_filter("stats"), fg_color="#2980b9"),
            "cities": ctk.CTkButton(filter_frame, text="🏢 Города", width=80, command=lambda: self.set_filter("cities"), fg_color="#8e44ad"),
            "map": ctk.CTkButton(filter_frame, text="🗺️ Карта Мира", width=100, command=lambda: self.set_filter("map"), fg_color="#27ae60")
        }
        for btn in self.filters.values():
            btn.pack(side="left", padx=2)
        self.filters["all"].configure(border_width=2, border_color="#5dade2")
        
        # Текстовое поле с тегами
        self.textbox = ctk.CTkTextbox(self.main_area, font=("Consolas", 13), wrap="word")
        self.textbox.pack(fill="both", expand=True)
        
        self.textbox.tag_config("header", foreground="#5dade2", justify="center")
        self.textbox.tag_config("date", foreground="#f1c40f")
        self.textbox.tag_config("location", foreground="#2ecc71")
        self.textbox.tag_config("war", foreground="#e74c3c")
        self.textbox.tag_config("trade", foreground="#f1c40f")
        self.textbox.tag_config("disaster", foreground="#e67e22")
        self.textbox.tag_config("business", foreground="#9b59b6")
        self.textbox.tag_config("market", foreground="#1abc9c")
        self.textbox.tag_config("logistics", foreground="#34495e")
        self.textbox.tag_config("misc", foreground="#bdc3c7")
        self.textbox.tag_config("city_map", foreground="#ecf0f1")
        
        self.textbox.insert("1.0", "Добро пожаловать в панель симуляции Nexus Engine.\nНастройте параметры слева и нажмите '1. Инициализация и Генерация'.")
        self.textbox.configure(state="disabled")

        # Фрейм для карты
        self.map_frame = ctk.CTkFrame(self.main_area, fg_color="transparent")
        
        self.map_tools = ctk.CTkFrame(self.map_frame, fg_color="transparent")
        self.map_tools.pack(fill="x", pady=5)
        ctk.CTkButton(self.map_tools, text="🔄 Запросить карту", command=self.request_map).pack(side="left", padx=5)
        ctk.CTkButton(self.map_tools, text="☄️ Тест: Метеорит (Руины)", command=self.test_meteorite, fg_color="#e74c3c").pack(side="left", padx=5)
        ctk.CTkButton(self.map_tools, text="🏗️ Тест: Построить Бизнес (Новая дорога)", command=self.test_business, fg_color="#f39c12", text_color="black").pack(side="left", padx=5)

        self.map_canvas = tk.Canvas(self.map_frame, bg="#000")
        self.map_canvas.pack(fill="both", expand=True)
        
        self.map_canvas.bind("<ButtonPress-1>", self.on_drag_start)
        self.map_canvas.bind("<B1-Motion>", self.on_drag_motion)

    def on_drag_start(self, event):
        self.drag_data["x"] = event.x
        self.drag_data["y"] = event.y

    def on_drag_motion(self, event):
        dx = event.x - self.drag_data["x"]
        dy = event.y - self.drag_data["y"]
        self.map_canvas.move("all", dx, dy)
        self.drag_data["x"] = event.x
        self.drag_data["y"] = event.y

    def set_filter(self, cat):
        self.current_filter = cat
        for k, btn in self.filters.items():
            if k == cat:
                btn.configure(border_width=2, border_color="#5dade2")
            else:
                btn.configure(border_width=0)
        
        if cat == "map":
            self.textbox.pack_forget()
            self.map_frame.pack(fill="both", expand=True)
            self.render_map()
        else:
            self.map_frame.pack_forget()
            self.textbox.pack(fill="both", expand=True)
            if self.world_data:
                self.render_content()

    def start_generation(self):
        if not self.engine.is_running:
            if not self.engine.start():
                return
            self.engine.send({"command": "init"})
            
        self.status_lbl.configure(text="Генерация мира...", text_color="#f1c40f")
        self.progress_bar.configure(mode="indeterminate")
        self.progress_bar.start()
        self.btn_init.configure(state="disabled")
        
        self.pending_bootstrap = True # Флаг для запуска балансировки после генерации
        
        cmd = {
            "command": "buildWorld",
            "player_id": "sim_admin",
            "era": self.era_var.get(),
            "initial_agents": self.agents_var.get()
        }
        self.engine.send(cmd)

    def update_speed(self, val):
        speed = int(val)
        self.speed_lbl.configure(text=f"Скорость: {speed} мс/день")
        if getattr(self, 'realtime_active', False):
            self.engine.send({"command": "startRealtime", "interval": speed})

    def toggle_realtime(self):
        if not getattr(self, 'realtime_active', False):
            self.realtime_active = True
            self.btn_realtime.configure(text="⏸ Остановить Реал-тайм", fg_color="#e74c3c", hover_color="#c0392b")
            self.btn_sim.configure(state="disabled")
            self.btn_init.configure(state="disabled")
            self.engine.send({"command": "startRealtime", "interval": int(self.speed_var.get())})
        else:
            self.realtime_active = False
            self.btn_realtime.configure(text="▶ Реал-тайм Симуляция", fg_color="#8e44ad", hover_color="#9b59b6")
            self.btn_sim.configure(state="normal")
            self.btn_init.configure(state="normal")
            self.engine.send({"command": "stopRealtime"})
            self.status_lbl.configure(text="Реал-тайм остановлен", text_color="#f39c12")


    def start_simulation(self):
        time_map = {
            "1 месяц": 720,
            "6 месяцев": 4320,
            "1 год": 8640,
            "2 года": 17280,
            "5 лет": 43200,
            "10 лет": 86400
        }
        ticks = time_map.get(self.time_var.get(), 8640)
        
        self.status_lbl.configure(text=f"Симуляция ({self.time_var.get()})...", text_color="#3498db")
        self.progress_bar.configure(mode="indeterminate")
        self.progress_bar.start()
        self.btn_sim.configure(state="disabled")
        
        self.engine.send({"command": "simulateTicks", "ticks": ticks})

    def update_speed(self, val):
        speed = int(val)
        self.speed_lbl.configure(text=f"Скорость: {speed} мс/день")
        if getattr(self, 'realtime_active', False):
            self.engine.send({"command": "startRealtime", "interval": speed})

    def toggle_realtime(self):
        if not getattr(self, 'realtime_active', False):
            self.realtime_active = True
            self.btn_realtime.configure(text="⏸ Остановить Реал-тайм", fg_color="#e74c3c", hover_color="#c0392b")
            self.btn_sim.configure(state="disabled")
            self.btn_init.configure(state="disabled")
            self.engine.send({"command": "startRealtime", "interval": int(self.speed_var.get())})
        else:
            self.realtime_active = False
            self.btn_realtime.configure(text="▶ Реал-тайм Симуляция", fg_color="#8e44ad", hover_color="#9b59b6")
            self.btn_sim.configure(state="normal")
            self.btn_init.configure(state="normal")
            self.engine.send({"command": "stopRealtime"})
            self.status_lbl.configure(text="Реал-тайм остановлен", text_color="#f39c12")

    def request_map(self):
        self.engine.send({"command": "getWorldMap"})

    def test_meteorite(self):
        if not self.world_data or "regions" not in self.world_data:
            return
        regions = list(self.world_data["regions"].keys())
        if not regions: return
        target = random.choice(regions)
        self.engine.send({
            "command": "gmModifyTerrain",
            "args": {
                "regionId": target,
                "radius": 10,
                "newType": 9 # RUINS
            }
        })
        self.request_map()

    def test_business(self):
        if not self.world_data or "regions" not in self.world_data:
            return
        regions = list(self.world_data["regions"].keys())
        if not regions: return
        target = random.choice(regions)
        self.engine.send({
            "command": "playerManageBusiness",
            "action": "create",
            "args": {
                "regionId": target,
                "facilityType": "farms",
                "name": "Тестовая Ферма"
            }
        })
        self.request_map()

    def stop_engine(self):
        self.engine.stop()

        self.realtime_active = False
        self.realtime_active = False
        self.status_lbl.configure(text="Движок остановлен", text_color="#e74c3c")
        self.progress_bar.stop()
        self.progress_bar.set(0)
        self.btn_init.configure(state="normal")
        self.btn_sim.configure(state="disabled")

        if hasattr(self, 'btn_realtime'):
            self.btn_realtime.configure(state="disabled", text="▶ Реал-тайм Симуляция", fg_color="#8e44ad")
        if hasattr(self, 'btn_realtime'):
            self.btn_realtime.configure(state="disabled", text="▶ Реал-тайм Симуляция", fg_color="#8e44ad")

    def handle_progress(self, msg):
        self.after(0, lambda: self.status_lbl.configure(text=msg))

    def handle_result(self, data):
        def update_ui():
            self.progress_bar.stop()
            self.progress_bar.set(1)

            
            if data.get("status") == "realtime_update":
                self.world_data = data.get("world", self.world_data)
                self.status_lbl.configure(text=f"Реал-тайм: Тик {self.world_data.get('tick', 0)}", text_color="#8e44ad")
                if self.current_filter == "map":
                    self.render_map()
                else:
                    self.render_content()
                return
            
            # Если пришла только карта (от getWorldMap)
            if "map" in data and "world" not in data:
                if self.world_data is None:
                    self.world_data = {}
                self.world_data["map"] = data["map"]
                if self.current_filter == "map":
                    self.render_map()
                return

            if data.get("status") == "realtime_update":
                self.world_data = data["world"]
                self.status_lbl.configure(text=f"Реал-тайм: Тик {self.world_data.get('tick', 0)}", text_color="#8e44ad")
                if self.current_filter == "map":
                    self.render_map()
                else:
                    self.render_content()
                return

            if "world" in data:
                self.world_data = data["world"]
                
                if not hasattr(self, 'containers_data'): self.containers_data = {}
                if not hasattr(self, 'items_data'): self.items_data = {}
                for c in data.get("containers", []): self.containers_data[c[0]] = c[1]
                for i in data.get("items", []): self.items_data[i[0]] = i[1]
                for c_id in data.get("deleted_containers", []): self.containers_data.pop(c_id, None)
                for i_id in data.get("deleted_items", []): self.items_data.pop(i_id, None)
                
                if getattr(self, 'pending_bootstrap', False):
                    self.pending_bootstrap = False
                    self.status_lbl.configure(text="Экономическая балансировка...", text_color="#f39c12")
                    total_pop = sum(r.get("population", 0) for r in self.world_data.get("regions", {}).values())
                    days = max(30, 30 + total_pop // 10000)
                    self.engine.send({"command": "bootstrapWorld", "days": days})
                    return # Ждем ответа от bootstrapWorld
                    
                self.status_lbl.configure(text=f"Готово! Тик: {self.world_data.get('tick', 0)}", text_color="#2ecc71")
                self.btn_sim.configure(state="normal")
                self.btn_init.configure(state="normal")

                if hasattr(self, 'btn_realtime'):
                    self.btn_realtime.configure(state="normal")
                if hasattr(self, 'btn_realtime'):
                    self.btn_realtime.configure(state="normal")
                if self.current_filter == "map":
                    self.render_map()
                else:
                    self.render_content()
            elif data.get("message") == "Engine initialized":
                self.status_lbl.configure(text="Движок инициализирован", text_color="#2ecc71")
                
        self.after(0, update_ui)

    def handle_error(self, msg):
        self.after(0, lambda: messagebox.showerror("Ошибка Движка", msg))
        self.after(0, self.stop_engine)

    def render_map(self):
        if not self.world_data or "map" not in self.world_data:
            self.request_map()
            return
            
        map_data = self.world_data["map"]
        w = map_data.get("width", 256)
        h = map_data.get("height", 256)
        
        grid = map_data.get("grid", [])
        tiles = [cell[0] for cell in grid] if grid else map_data.get("tiles", [])
        
        roads = map_data.get("roads", [])
        locations = map_data.get("locations", {})
        disasters = map_data.get("disasters", [])

        if not tiles:
            return

        # Цвета тайлов (Синхронизировано с JS-клиентом)
        COLORS = {
            0: (26, 59, 92),    # OCEAN
            1: (41, 128, 185),  # SHALLOW_WATER
            2: (46, 204, 113),  # PLAINS
            3: (39, 174, 96),   # FOREST
            4: (127, 140, 141), # MOUNTAINS
            5: (243, 156, 18),  # HILLS
            6: (230, 126, 34),  # DESERT
            7: (142, 68, 173),  # SWAMP
            8: (236, 240, 241), # TUNDRA
            9: (52, 73, 94),    # RUINS
            10: (155, 89, 182), # ANOMALY
            11: (52, 152, 219), # RIVER
            12: (192, 57, 43),  # VOLCANO
            13: (60, 176, 67),  # RIVERBANK (Изумрудный)
            14: (31, 97, 141),  # LAKE (Глубокий синий)
            15: (88, 214, 141), # FLOODPLAIN (Светло-зеленый)
            16: (211, 84, 0),   # LAVA
            17: (85, 85, 85)    # ASH
        }

        # Создаем базовое изображение
        img = Image.new('RGB', (w, h))
        pixel_data = [COLORS.get(t, (0,0,0)) for t in tiles]
        img.putdata(pixel_data)

        # Масштабируем
        img = img.resize((w * self.map_zoom, h * self.map_zoom), Image.NEAREST)
        draw = ImageDraw.Draw(img)

        # 1. Рисуем дороги по waypoints (как в JS)
        if roads:
            for road in roads:
                condition = road.get("condition", "dirt")
                waypoints = road.get("waypoints", [])
                if not waypoints or len(waypoints) < 2: continue
                
                if condition == 'paved' or road.get("type") == "paved" or road.get("type") == "highway":
                    color = (149, 165, 166) # #95a5a6
                    width = max(2, int(self.map_zoom * 0.6))
                elif road.get("type") == "bridge" or road.get("type") == "ferry":
                    color = (139, 69, 19)
                    width = max(3, int(self.map_zoom * 0.8))
                elif road.get("type") == "tunnel":
                    color = (85, 85, 85)
                    width = max(2, int(self.map_zoom * 0.6))
                elif road.get("type") == "sea_route":
                    color = (52, 152, 219) # #3498db
                    width = max(2, int(self.map_zoom * 0.6))
                elif condition == 'ruined':
                    color = (231, 76, 60) # #e74c3c
                    width = max(2, int(self.map_zoom * 0.4))
                else:
                    color = (139, 69, 19) # #8b4513
                    width = max(2, int(self.map_zoom * 0.4))
                
                points = []
                for wp in waypoints:
                    px = wp[0] * self.map_zoom + self.map_zoom // 2
                    py = wp[1] * self.map_zoom + self.map_zoom // 2
                    points.append((px, py))
                
                draw.line(points, fill=color, width=width, joint="curve")

        # 2. Рисуем локации с обводкой текста
        def draw_text_with_outline(draw_obj, x, y, text, fill, outline):
            draw_obj.text((x-1, y-1), text, fill=outline)
            draw_obj.text((x+1, y-1), text, fill=outline)
            draw_obj.text((x-1, y+1), text, fill=outline)
            draw_obj.text((x+1, y+1), text, fill=outline)
            draw_obj.text((x, y), text, fill=fill)

        for loc in locations.values():
            lx = loc.get("x", 0) * self.map_zoom + self.map_zoom//2
            ly = loc.get("y", 0) * self.map_zoom + self.map_zoom//2
            ltype = loc.get("type", "village")
            
            if ltype == "city":
                radius = max(4, int(self.map_zoom * 1.2))
                draw.rectangle([lx-radius, ly-radius, lx+radius, ly+radius], fill=(241, 196, 15), outline=(0,0,0), width=2)
            elif ltype == "pirate_base":
                radius = max(4, int(self.map_zoom * 1.0))
                draw.rectangle([lx-radius, ly-radius, lx+radius, ly+radius], fill=(0, 0, 0), outline=(231, 76, 60), width=2)
            elif ltype == "ruins":
                radius = max(4, int(self.map_zoom * 1.0))
                draw.polygon([(lx, ly-radius), (lx-radius, ly+radius), (lx+radius, ly+radius)], fill=(127, 140, 141), outline=(0,0,0))
            else:
                radius = max(3, int(self.map_zoom * 0.8))
                draw.ellipse([lx-radius, ly-radius, lx+radius, ly+radius], fill=(189, 195, 199), outline=(0,0,0), width=1)
            
            name = loc.get("name", "").split('(')[0].strip()
            draw_text_with_outline(draw, lx + radius + 4, ly - radius - 4, name, fill=(255, 255, 255), outline=(0, 0, 0))

        # 3. Рисуем движущиеся объекты (Караваны и Армии)
        regions = self.world_data.get("regions", {})
        factions = self.world_data.get("factions", {})

        for r_id, r in regions.items():
            for c in r.get("caravans", []):
                if "x" in c and "y" in c:
                    px = c["x"] * self.map_zoom + self.map_zoom // 2
                    py = c["y"] * self.map_zoom + self.map_zoom // 2
                    radius = max(2, int(self.map_zoom * 0.6))
                    draw.ellipse([px-radius, py-radius, px+radius, py+radius], fill=(241, 196, 15), outline=(0,0,0))

        # 4. Рисуем зоны катастроф
        if disasters:
            for d in disasters:
                dx = d.get("epicenter_x", 0) * self.map_zoom + self.map_zoom // 2
                dy = d.get("epicenter_y", 0) * self.map_zoom + self.map_zoom // 2
                radius = d.get("radius", 0) * self.map_zoom
                
                dtype = d.get("type", "")
                color = (255, 255, 255)
                if dtype == "flood": color = (41, 128, 185)
                elif dtype in ["wildfire", "volcano"]: color = (231, 76, 60)
                elif dtype == "earthquake": color = (139, 69, 19)
                elif dtype == "plague": color = (142, 68, 173)
                elif dtype == "aether_storm": color = (155, 89, 182)
                elif dtype == "monster_invasion": color = (192, 57, 43)
                elif dtype == "drought": color = (243, 156, 18)
                elif dtype == "storm": color = (52, 152, 219)
                elif dtype == "tsunami": color = (31, 97, 141)
                elif dtype == "sea_monster": color = (26, 188, 156)
                
                draw.ellipse([dx-radius, dy-radius, dx+radius, dy+radius], outline=color, width=2)

        # Отрисовка кораблей и флотов
        ships = self.world_data.get("ships", [])
        fleets = self.world_data.get("fleets", [])
        
        for ship in ships:
            if "x" in ship and "y" in ship:
                px = ship["x"] * self.map_zoom + self.map_zoom // 2
                py = ship["y"] * self.map_zoom + self.map_zoom // 2
                radius = max(4, int(self.map_zoom * 1.2))
                color = (52, 152, 219) if ship.get("type") == "MERCHANT" else (231, 76, 60)
                if ship.get("type") == "PIRATE": color = (0, 0, 0)
                if ship.get("type") == "SEA_MONSTER": color = (26, 188, 156)
                draw.polygon([(px, py-radius), (px-radius, py+radius), (px+radius, py+radius)], fill=color, outline=(255,255,255))

        for fleet in fleets:
            if "x" in fleet and "y" in fleet:
                px = fleet["x"] * self.map_zoom + self.map_zoom // 2
                py = fleet["y"] * self.map_zoom + self.map_zoom // 2
                radius = max(3, int(self.map_zoom * 0.8))
                draw.rectangle([px-radius, py-radius, px+radius, py+radius], fill=(142, 68, 173), outline=(255,255,255))


        for f_id, f in factions.items():
            for a in f.get("armies", []):
                if a.get("siegeDays", -1) > 0 or a.get("current_phase", "march") != "march":
                    # Армия в осаде/бою - рисуем над городом
                    dest_id = a.get("destination")
                    if dest_id in locations:
                        end = locations[dest_id]
                        px = end.get("x", 0) * self.map_zoom + self.map_zoom // 2
                        py = end.get("y", 0) * self.map_zoom + self.map_zoom // 2
                        radius = max(3, int(self.map_zoom * 0.8))
                        draw.ellipse([px+5, py-5, px+5+radius*2, py-5+radius*2], fill=(231, 76, 60), outline=(255,255,255))
                elif "x" in a and "y" in a:
                    px = a["x"] * self.map_zoom + self.map_zoom // 2
                    py = a["y"] * self.map_zoom + self.map_zoom // 2
                    radius = max(2, int(self.map_zoom * 0.6))
                    draw.ellipse([px-radius, py-radius, px+radius, py+radius], fill=(231, 76, 60), outline=(0,0,0))

        self.map_photo = ImageTk.PhotoImage(img)
        self.map_canvas.delete("all")
        # Центрируем карту при первой отрисовке
        cx = (self.map_canvas.winfo_width() - w * self.map_zoom) // 2
        cy = (self.map_canvas.winfo_height() - h * self.map_zoom) // 2
        self.map_canvas.create_image(cx, cy, image=self.map_photo, anchor="nw", tags="map")

    def render_content(self):
        if not self.world_data:
            return
            
        self.textbox.configure(state="normal")
        self.textbox.delete("1.0", tk.END)
        
        # --- РЕЖИМ СТАТИСТИКИ ---
        if self.current_filter == "stats":
            w = self.world_data
            self.textbox.insert(tk.END, f"\n{'='*15} ГЛОБАЛЬНАЯ СТАТИСТИКА {'='*15}\n\n", "header")
            
            def count_items(cont_id, good_type):
                if not hasattr(self, 'containers_data') or cont_id not in self.containers_data: return 0
                cont = self.containers_data[cont_id]
                total = 0
                for item_id in cont.get("items", []):
                    if item_id in self.items_data:
                        item = self.items_data[item_id]
                        if item.get("prototype_id") == good_type:
                            total += item.get("stack_size", 0)
                return total

            self.textbox.insert(tk.END, "--- ФРАКЦИИ ---\n", "header")
            for fid, f in w.get("factions", {}).items():
                cap_id = f.get("regions", [""])[0] if f.get("regions") else ""
                vault_id = w.get("regions", {}).get(cap_id, {}).get("vault_id", "") if cap_id else ""
                gold = count_items(vault_id, "gold_ingot")
                weapons = count_items(vault_id, "weapons")
                
                self.textbox.insert(tk.END, f"👑 {f.get('name', fid)}:\n", "location")
                self.textbox.insert(tk.END, f"   Стабильность: {f.get('stability', 0)} | Легитимность: {f.get('legitimacy', 0)} | Усталость от войны: {f.get('warExhaustion', 0)}\n", "misc")
                
                faction_ships = [s for s in w.get("ships", []) if s.get("owner_id") == fid]
                faction_fleets = [fl for fl in w.get("fleets", []) if fl.get("owner_id") == fid]
                
                self.textbox.insert(tk.END, f"   Состояние: {f.get('warType', 'PEACE')} | Армий: {len(f.get('armies', []))} | Флотов: {len(faction_fleets)} (Кораблей: {len(faction_ships)})\n", "misc")
                self.textbox.insert(tk.END, f"   Золото в столице: {gold} | Оружие: {weapons}\n", "misc")
                
                wars = [k for k, v in f.get("diplomacy", {}).items() if v == "war"]
                if wars:
                    self.textbox.insert(tk.END, f"   Воюет с: {', '.join(wars)}\n", "war")
                self.textbox.insert(tk.END, "\n")

            self.textbox.insert(tk.END, "--- РЕГИОНЫ ---\n", "header")
            for rid, r in w.get("regions", {}).items():
                vault_id = r.get("vault_id", "")
                food = count_items(vault_id, "bread") + count_items(vault_id, "meat") + count_items(vault_id, "fish") + count_items(vault_id, "wheat") + count_items(vault_id, "smoked_meat")
                gold = count_items(vault_id, "gold_ingot")
                
                self.textbox.insert(tk.END, f"🏙️ {r.get('name', rid)} (Фракция: {r.get('factionId', 'Нет')}):\n", "location")
                self.textbox.insert(tk.END, f"   Население: {r.get('population', 0)} | Рабочих: {r.get('labor_force', 0)} | Безработица: {int(r.get('unemployment_rate', 0)*100)}%\n", "misc")
                self.textbox.insert(tk.END, f"   Угроза: {r.get('threat_level', 0)}/100 | Зарплата: {r.get('average_wage', 0)} | Денежная масса: {int(r.get('moneySupply', 0))}\n", "misc")
                self.textbox.insert(tk.END, f"   Склад (Еда: {food}, Золото: {gold})\n", "misc")
                
                facs = []
                for fname, fac in r.get("facilities", {}).items():
                    if fac.get("level", 0) > 0:
                        facs.append(f"{fname} (ур.{fac.get('level')})")
                if facs:
                    self.textbox.insert(tk.END, f"   Предприятия: {', '.join(facs)}\n", "business")
                
                port = w.get("port_facilities", {}).get(rid)
                if port:
                    shipyard_str = " + Верфь" if port.get("has_shipyard") else ""
                    blockade_str = " [БЛОКАДА!]" if port.get("is_blockaded") else ""
                    self.textbox.insert(tk.END, f"   ⚓ Порт (Ур.{port.get('level')}): {port.get('type')}{shipyard_str}{blockade_str}\n", "trade")
                    
                if r.get("isOccupied"):
                    self.textbox.insert(tk.END, f"   ⚠️ ОККУПИРОВАНО фракцией {r.get('occupierFactionId')} ({r.get('daysUnderOccupation')} дн.)\n", "war")
                self.textbox.insert(tk.END, "\n")
                
            self.textbox.configure(state="disabled")
            return

        # --- РЕЖИМ ОТРИСОВКИ ГОРОДОВ ---
        if self.current_filter == "cities":
            regions = self.world_data.get("regions", {})
            has_cities = False
            
            for rid, r in regions.items():
                layout = r.get("cityLayout", [])
                w = r.get("layoutWidth", 0)
                h = r.get("layoutHeight", 0)
                
                if not layout or w == 0 or h == 0:
                    continue
                
                has_cities = True
                self.textbox.insert(tk.END, f"\n{'='*15} {r.get('name', rid)} ({w}x{h}) {'='*15}\n\n", "header")
                
                grid = [['.' for _ in range(w)] for _ in range(h)]
                for block in layout:
                    bx, by = block.get("x", 0), block.get("y", 0)
                    btype = block.get("type", "empty")
                    
                    ch = '.'
                    if btype == 'road': ch = '#'
                    elif btype == 'house': ch = 'H'
                    elif btype == 'tavern': ch = 'T'
                    elif btype == 'forge': ch = 'F'
                    elif btype == 'market': ch = 'M'
                    elif btype == 'office': ch = 'O'
                    elif btype == 'temple': ch = '+'
                    
                    if 0 <= by < h and 0 <= bx < w:
                        grid[by][bx] = ch
                        
                for row in grid:
                    self.textbox.insert(tk.END, "    " + " ".join(row) + "\n", "city_map")
                
                self.textbox.insert(tk.END, "\n  Список зданий:\n", "location")
                for block in layout:
                    if block.get("type") not in ["empty", "road"]:
                        self.textbox.insert(tk.END, f"  - {block.get('name')} [{block.get('sublocation_id')}] (Тип: {block.get('type')})\n", "misc")
                        
            if not has_cities:
                self.textbox.insert(tk.END, "\nНет данных о застройке городов.\n", "misc")
                
            self.textbox.configure(state="disabled")
            return

        # --- РЕЖИМ ОТРИСОВКИ НОВОСТЕЙ ---
        news_list = self.world_data.get("news", [])
        
        if self.current_filter != "all":
            news_list = [n for n in news_list if n.get("category", "misc") == self.current_filter]
            
        if not news_list:
            self.textbox.insert(tk.END, "\nВ этой категории нет событий.\n", "misc")
            self.textbox.configure(state="disabled")
            return

        news_list.sort(key=lambda x: x.get("day", 0))
        
        current_year = -1
        current_month = -1
        
        for news in news_list:
            day_total = news.get("day", 0)
            year = (day_total // 360) + 1
            month = ((day_total % 360) // 30) + 1
            day_of_month = (day_total % 30) + 1
            
            if year != current_year or month != current_month:
                self.textbox.insert(tk.END, f"\n{'='*20} ГОД {year}, МЕСЯЦ {month} {'='*20}\n\n", "header")
                current_year = year
                current_month = month
                
            cat = news.get("category", "misc").lower()
            loc = news.get("location", "Неизвестно")
            text = news.get("text", "")
            
            tag = "misc"
            if cat == "war": tag = "war"
            elif cat == "trade": tag = "trade"
            elif cat == "disaster": tag = "disaster"
            elif cat == "business": tag = "business"
            elif cat == "market": tag = "market"
            elif cat == "logistics": tag = "logistics"
            
            self.textbox.insert(tk.END, f"[День {day_of_month:02d}] ", "date")
            self.textbox.insert(tk.END, f"[{loc}] ", "location")
            self.textbox.insert(tk.END, f"{text}\n", tag)
            
        self.textbox.configure(state="disabled")
        self.textbox.see(tk.END)

    def on_closing(self):
        self.engine.stop()
        self.destroy()

if __name__ == "__main__":
    app = SimulationApp()
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()