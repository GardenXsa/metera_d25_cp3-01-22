#!/usr/bin/env python3
import customtkinter as ctk
import tkinter as tk
from tkinter import messagebox
import subprocess
import threading
import json
import sys
import os

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
        exe_name = './meterea_engine.exe' if sys.platform == 'win32' else './meterea_engine'
        if not os.path.exists(exe_name):
            self.on_error(f"Движок {exe_name} не найден. Скомпилируйте C++ код.")
            return False
        
        try:
            self.proc = subprocess.Popen(
                [exe_name],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding='utf-8'
            )
            self.is_running = True
            threading.Thread(target=self._read_loop, daemon=True).start()
            return True
        except Exception as e:
            self.on_error(f"Ошибка запуска: {e}")
            return False

    def send(self, cmd_data):
        if self.is_running and self.proc:
            try:
                self.proc.stdin.write(json.dumps(cmd_data, ensure_ascii=False) + '\n')
                self.proc.stdin.flush()
            except Exception as e:
                self.on_error(f"Ошибка отправки: {e}")

    def _read_loop(self):
        while self.is_running and self.proc:
            line = self.proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line: continue
            
            try:
                data = json.loads(line)
                if data.get("status") == "progress":
                    self.on_progress(data.get("message", ""))
                elif data.get("status") == "ok":
                    self.on_result(data)
                elif data.get("status") == "error":
                    self.on_error(data.get("message", "Неизвестная ошибка движка"))
            except json.JSONDecodeError:
                print(f"[RAW ENGINE OUTPUT] {line}")
        self.is_running = False

    def stop(self):
        self.is_running = False
        if self.proc:
            self.proc.terminate()
            self.proc = None

class SimulationApp(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Nexus Engine - Панель Симуляции")
        self.geometry("1200x800")
        self.minsize(900, 600)
        
        self.engine = EngineProcess(self.handle_progress, self.handle_result, self.handle_error)
        self.world_data = None
        self.current_filter = "all"
        
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
        
        self.btn_sim = ctk.CTkButton(self.sidebar, text="2. Запуск Симуляции", command=self.start_simulation, state="disabled", fg_color="#2980b9", hover_color="#3498db")
        self.btn_sim.pack(fill="x", padx=15, pady=5)
        
        self.btn_stop = ctk.CTkButton(self.sidebar, text="Остановить Движок", command=self.stop_engine, fg_color="#c0392b", hover_color="#e74c3c")
        self.btn_stop.pack(fill="x", padx=15, pady=20)
        
        # Статус
        self.status_lbl = ctk.CTkLabel(self.sidebar, text="Движок остановлен", text_color="#e74c3c")
        self.status_lbl.pack(side="bottom", pady=10)
        
        self.progress_bar = ctk.CTkProgressBar(self.sidebar)
        self.progress_bar.set(0)
        self.progress_bar.pack(side="bottom", fill="x", padx=15, pady=5)
        
        # Правая панель (Летопись)
        self.main_area = ctk.CTkFrame(self)
        self.main_area.pack(side="right", fill="both", expand=True, padx=10, pady=10)
        
        # Фильтры
        filter_frame = ctk.CTkFrame(self.main_area, fg_color="transparent")
        filter_frame.pack(fill="x", pady=(0, 10))
        
        ctk.CTkLabel(filter_frame, text="Фильтр событий:", font=ctk.CTkFont(weight="bold")).pack(side="left", padx=(0, 10))
        
        self.filters = {
            "all": ctk.CTkButton(filter_frame, text="Все", width=60, command=lambda: self.set_filter("all"), fg_color="#34495e"),
            "war": ctk.CTkButton(filter_frame, text="⚔️ Войны", width=80, command=lambda: self.set_filter("war"), fg_color="#c0392b"),
            "trade": ctk.CTkButton(filter_frame, text="💰 Торговля", width=80, command=lambda: self.set_filter("trade"), fg_color="#f39c12", text_color="black"),
            "disaster": ctk.CTkButton(filter_frame, text="🌪️ Бедствия", width=80, command=lambda: self.set_filter("disaster"), fg_color="#d35400"),
            "misc": ctk.CTkButton(filter_frame, text="📜 Прочее", width=80, command=lambda: self.set_filter("misc"), fg_color="#7f8c8d")
        }
        for btn in self.filters.values():
            btn.pack(side="left", padx=2)
        self.filters["all"].configure(border_width=2, border_color="#5dade2")
        
        # Текстовое поле с тегами
        self.textbox = ctk.CTkTextbox(self.main_area, font=("Consolas", 13), wrap="word")
        self.textbox.pack(fill="both", expand=True)
        
        # Настройка тегов для цветов
        self.textbox.tag_config("header", foreground="#5dade2", justify="center")
        self.textbox.tag_config("date", foreground="#f1c40f")
        self.textbox.tag_config("location", foreground="#2ecc71")
        self.textbox.tag_config("war", foreground="#e74c3c")
        self.textbox.tag_config("trade", foreground="#f1c40f")
        self.textbox.tag_config("disaster", foreground="#e67e22")
        self.textbox.tag_config("misc", foreground="#bdc3c7")
        
        self.textbox.insert("1.0", "Добро пожаловать в панель симуляции Nexus Engine.\nНастройте параметры слева и нажмите 'Инициализация'.")
        self.textbox.configure(state="disabled")

    def set_filter(self, cat):
        self.current_filter = cat
        for k, btn in self.filters.items():
            if k == cat:
                btn.configure(border_width=2, border_color="#5dade2")
            else:
                btn.configure(border_width=0)  # просто убираем рамку
        if self.world_data:
            self.render_news()

    def start_generation(self):
        if not self.engine.is_running:
            if not self.engine.start():
                return
            self.engine.send({"command": "init"})
            
        self.status_lbl.configure(text="Генерация мира...", text_color="#f1c40f")
        self.progress_bar.configure(mode="indeterminate")
        self.progress_bar.start()
        self.btn_init.configure(state="disabled")
        
        # Отправляем команду генерации
        cmd = {
            "command": "buildWorld",
            "player_id": "sim_admin",
            "era": self.era_var.get(),
            "initial_agents": self.agents_var.get()
        }
        self.engine.send(cmd)

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

    def stop_engine(self):
        self.engine.stop()
        self.status_lbl.configure(text="Движок остановлен", text_color="#e74c3c")
        self.progress_bar.stop()
        self.progress_bar.set(0)
        self.btn_init.configure(state="normal")
        self.btn_sim.configure(state="disabled")

    def handle_progress(self, msg):
        self.after(0, lambda: self.status_lbl.configure(text=msg))

    def handle_result(self, data):
        def update_ui():
            self.progress_bar.stop()
            self.progress_bar.set(1)
            
            if "world" in data:
                self.world_data = data["world"]
                self.status_lbl.configure(text=f"Готово! Тик: {self.world_data.get('tick', 0)}", text_color="#2ecc71")
                self.btn_sim.configure(state="normal")
                self.btn_init.configure(state="normal")
                self.render_news()
            elif data.get("message") == "Engine initialized":
                self.status_lbl.configure(text="Движок инициализирован", text_color="#2ecc71")
                
        self.after(0, update_ui)

    def handle_error(self, msg):
        self.after(0, lambda: messagebox.showerror("Ошибка Движка", msg))
        self.after(0, self.stop_engine)

    def render_news(self):
        if not self.world_data or "news" not in self.world_data:
            return
            
        self.textbox.configure(state="normal")
        self.textbox.delete("1.0", tk.END)
        
        news_list = self.world_data["news"]
        
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
