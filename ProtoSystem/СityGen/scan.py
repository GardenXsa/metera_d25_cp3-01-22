import os
import threading
import customtkinter as ctk
from tkinter import filedialog, messagebox

# --- КОНФИГУРАЦИЯ ИНТЕРФЕЙСА ---
ctk.set_appearance_mode("System")
ctk.set_default_color_theme("blue")

class ProjectScannerApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        # Настройка главного окна
        self.title("Project Scanner Pro")
        self.geometry("650x700")
        self.minsize(600, 650)

        # --- Переменные состояния ---
        self.project_path = ctk.StringVar(value=os.getcwd())
        self.output_file = ctk.StringVar(value=os.path.join(os.getcwd(), "project_scan.txt"))
        
        # Режим сканирования: 1 - полный, 0 - только структура
        self.full_scan_mode = ctk.BooleanVar(value=True) 
        
        # Настройки
        self.skip_hidden = ctk.BooleanVar(value=True)
        self.skip_large_files = ctk.BooleanVar(value=True)
        self.max_file_size_mb = ctk.StringVar(value="1.0")

        # Базовые списки исключений
        self.default_ignored_dirs = [
            '.git', '.idea', '.vscode', '__pycache__', '.godot', 
            'node_modules', 'venv', '.import', 'dist', 'build', 'bin', 'obj', 'sound', '.ai_backups'
        ]
        
        self.default_ignored_exts = [
            '.png', '.jpg', '.jpeg', '.gif', '.ico', '.bmp', '.tga', '.svg',
            '.wav', '.mp3', '.ogg', '.flac',
            '.exe', '.dll', '.so', '.pck', '.zip', '.rar', '.pdf', '.docx', 
            '.ttr', '.ttf', '.otf', '.pyc',
            '.obj', '.gltf', '.glb', '.fbx', '.dae', '.stl', '.ply', 
            '.blend', '.blend1', '.3ds', '.max', '.ma', '.mb'
        ]

        self.create_widgets()

    def create_widgets(self):
        # === Вкладки (Tabs) ===
        self.tabview = ctk.CTkTabview(self)
        self.tabview.pack(fill="both", expand=True, padx=20, pady=(10, 0))

        self.tab_main = self.tabview.add("Основное")
        self.tab_ignore = self.tabview.add("Исключения")
        self.tab_settings = self.tabview.add("Настройки")

        self.setup_main_tab()
        self.setup_ignore_tab()
        self.setup_settings_tab()

        # === Блок управления (Всегда виден внизу) ===
        self.frame_actions = ctk.CTkFrame(self, fg_color="transparent")
        self.frame_actions.pack(pady=10, padx=20, fill="x", side="bottom")

        self.btn_start = ctk.CTkButton(
            self.frame_actions, text="НАЧАТЬ СКАНИРОВАНИЕ", 
            height=45, font=("Arial", 14, "bold"), command=self.start_scan_thread
        )
        self.btn_start.pack(fill="x", pady=(0, 10))

        # Прогресс бар и статус
        self.progress_bar = ctk.CTkProgressBar(self.frame_actions, mode='indeterminate')
        self.progress_bar.set(0)
        self.progress_bar.pack(fill="x", pady=(0, 5))
        
        self.status_label = ctk.CTkLabel(self.frame_actions, text="Готов к работе", text_color="gray", font=("Arial", 12))
        self.status_label.pack()

    def setup_main_tab(self):
        # Выбор папки проекта
        ctk.CTkLabel(self.tab_main, text="📁 Папка проекта:", font=("Arial", 13, "bold")).pack(anchor="w", padx=10, pady=(10, 0))
        input_row = ctk.CTkFrame(self.tab_main, fg_color="transparent")
        input_row.pack(fill="x", padx=10, pady=(5, 15))
        
        ctk.CTkEntry(input_row, textvariable=self.project_path).pack(side="left", fill="x", expand=True, padx=(0, 10))
        ctk.CTkButton(input_row, text="Обзор", width=80, command=self.select_directory).pack(side="right")

        # Выбор файла отчета
        ctk.CTkLabel(self.tab_main, text="📄 Файл отчета:", font=("Arial", 13, "bold")).pack(anchor="w", padx=10)
        output_row = ctk.CTkFrame(self.tab_main, fg_color="transparent")
        output_row.pack(fill="x", padx=10, pady=(5, 20))
        
        ctk.CTkEntry(output_row, textvariable=self.output_file).pack(side="left", fill="x", expand=True, padx=(0, 10))
        ctk.CTkButton(output_row, text="Сохранить как", width=80, command=self.select_output_file).pack(side="right")

        # Режим сканирования
        mode_frame = ctk.CTkFrame(self.tab_main)
        mode_frame.pack(fill="x", padx=10, pady=10)
        ctk.CTkLabel(mode_frame, text="Режим сканирования:", font=("Arial", 13, "bold")).pack(pady=(10, 5))
        
        ctk.CTkRadioButton(mode_frame, text="Полный скан (Структура + Содержимое файлов)", variable=self.full_scan_mode, value=True).pack(anchor="w", padx=20, pady=5)
        ctk.CTkRadioButton(mode_frame, text="Только архитектура (Дерево папок и файлов)", variable=self.full_scan_mode, value=False).pack(anchor="w", padx=20, pady=(5, 15))

    def setup_ignore_tab(self):
        self.tab_ignore.grid_columnconfigure(0, weight=1)
        self.tab_ignore.grid_columnconfigure(1, weight=1)

        # Папки
        lbl_dirs = ctk.CTkLabel(self.tab_ignore, text="Исключить папки:", font=("Arial", 12, "bold"))
        lbl_dirs.grid(row=0, column=0, padx=10, pady=5, sticky="w")
        
        self.txt_ignore_dirs = ctk.CTkTextbox(self.tab_ignore, height=350)
        self.txt_ignore_dirs.grid(row=1, column=0, padx=10, pady=(0, 10), sticky="nsew")
        self.txt_ignore_dirs.insert("0.0", "\n".join(self.default_ignored_dirs))

        # Форматы (Расширения)
        lbl_exts = ctk.CTkLabel(self.tab_ignore, text="Исключить расширения:", font=("Arial", 12, "bold"))
        lbl_exts.grid(row=0, column=1, padx=10, pady=5, sticky="w")
        
        self.txt_ignore_exts = ctk.CTkTextbox(self.tab_ignore, height=350)
        self.txt_ignore_exts.grid(row=1, column=1, padx=10, pady=(0, 10), sticky="nsew")
        self.txt_ignore_exts.insert("0.0", "\n".join(self.default_ignored_exts))

    def setup_settings_tab(self):
        # Скрытые файлы
        ctk.CTkSwitch(self.tab_settings, text="Игнорировать скрытые файлы/папки (начинаются с точки)", variable=self.skip_hidden).pack(anchor="w", padx=20, pady=15)
        
        # Лимит размера
        size_frame = ctk.CTkFrame(self.tab_settings, fg_color="transparent")
        size_frame.pack(fill="x", padx=20, pady=10)
        
        ctk.CTkSwitch(size_frame, text="Пропускать большие файлы", variable=self.skip_large_files).pack(side="left")
        
        ctk.CTkLabel(size_frame, text="Лимит (МБ):").pack(side="left", padx=(20, 5))
        entry_size = ctk.CTkEntry(size_frame, textvariable=self.max_file_size_mb, width=60)
        entry_size.pack(side="left")

    # --- ЛОГИКА ИНТЕРФЕЙСА ---
    def select_directory(self):
        path = filedialog.askdirectory()
        if path:
            self.project_path.set(path)

    def select_output_file(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt", filetypes=[("Text files", "*.txt"), ("All files", "*.*")])
        if path:
            self.output_file.set(path)

    def start_scan_thread(self):
        self.btn_start.configure(state="disabled", text="СКАНИРОВАНИЕ...")
        self.progress_bar.start()
        self.status_label.configure(text="Подготовка...", text_color=ctk.ThemeManager.theme["CTkLabel"]["text_color"])
        
        # Собираем настройки
        config = {
            "project_path": self.project_path.get(),
            "output_file": self.output_file.get(),
            "full_scan": self.full_scan_mode.get(),
            "skip_hidden": self.skip_hidden.get(),
            "skip_large": self.skip_large_files.get(),
            "max_mb": 1.0
        }

        # Обработка лимита МБ
        try:
            config["max_mb"] = float(self.max_file_size_mb.get())
        except ValueError:
            config["max_mb"] = 1.0
            self.max_file_size_mb.set("1.0")
        
        # Сбор игнорируемых данных (сразу приводим к нижнему регистру для расширений)
        config["ignore_dirs"] = {line.strip() for line in self.txt_ignore_dirs.get("0.0", "end").splitlines() if line.strip()}
        config["ignore_exts"] = {line.strip().lower() for line in self.txt_ignore_exts.get("0.0", "end").splitlines() if line.strip()}

        # Запуск потока
        thread = threading.Thread(target=self.run_scan, args=(config,))
        thread.start()

    def run_scan(self, config):
        try:
            abs_output_file = os.path.abspath(config["output_file"])
            project_path = config["project_path"]
            max_size_bytes = config["max_mb"] * 1024 * 1024
            
            if not os.path.exists(project_path):
                raise FileNotFoundError("Указанная папка проекта не существует.")

            with open(config["output_file"], 'w', encoding='utf-8') as f:
                # --- ЭТАП 1: АРХИТЕКТУРА ---
                f.write("========================================\n")
                f.write("        АРХИТЕКТУРА ПРОЕКТА\n")
                f.write("========================================\n\n")

                self.update_status(f"Анализ структуры папок...")

                for root, dirs, files in os.walk(project_path):
                    # Фильтрация папок (по списку исключений и скрытым)
                    dirs[:] = [d for d in dirs if d not in config["ignore_dirs"] and not (config["skip_hidden"] and d.startswith('.'))]
                    
                    level = root.replace(project_path, '').count(os.sep)
                    indent = ' ' * 4 * level
                    f.write(f"{indent}{os.path.basename(root)}/\n")
                    sub_indent = ' ' * 4 * (level + 1)
                    
                    # Запись файлов
                    for file_name in files:
                        if config["skip_hidden"] and file_name.startswith('.'):
                            continue
                        if os.path.abspath(os.path.join(root, file_name)) != abs_output_file:
                            f.write(f"{sub_indent}{file_name}\n")

                # Если выбран режим "Только архитектура", завершаем работу
                if not config["full_scan"]:
                    self.finish_scan(True, config["output_file"])
                    return

                # --- ЭТАП 2: ЧТЕНИЕ СОДЕРЖИМОГО ---
                f.write("\n\n========================================\n")
                f.write("     СОДЕРЖИМОЕ ТЕКСТОВЫХ ФАЙЛОВ\n")
                f.write("========================================\n\n")

                self.update_status(f"Чтение файлов...")
                
                for root, dirs, files in os.walk(project_path):
                    dirs[:] = [d for d in dirs if d not in config["ignore_dirs"] and not (config["skip_hidden"] and d.startswith('.'))]

                    for file_name in files:
                        if config["skip_hidden"] and file_name.startswith('.'):
                            continue

                        file_path = os.path.join(root, file_name)
                        
                        # Пропускаем сам файл отчета
                        if os.path.abspath(file_path) == abs_output_file:
                            continue
                        
                        # Проверка расширения
                        _, ext = os.path.splitext(file_name)
                        if ext.lower() in config["ignore_exts"]:
                            continue

                        self.update_status(f"Обработка: {file_name}")

                        try:
                            with open(file_path, 'r', encoding='utf-8') as content_file:
                                if config["skip_large"]:
                                    content_file.seek(0, 2)
                                    size = content_file.tell()
                                    content_file.seek(0)
                                    if size > max_size_bytes:
                                        f.write(f"--- Файл {file_name} пропущен (>{config['max_mb']}MB) ---\n\n")
                                        continue

                                content = content_file.read()
                                f.write(f"----------------------------------------\n")
                                f.write(f"Файл: {file_path}\n")
                                f.write(f"----------------------------------------\n\n")
                                f.write(content)
                                f.write("\n\n")
                        except Exception:
                            # Игнорируем бинарные файлы, которые попытались открыться как текст, или файлы без прав
                            pass

            self.finish_scan(True, config["output_file"])

        except Exception as e:
            self.finish_scan(False, str(e))

    def update_status(self, text):
        # Обрезаем длинный текст для статуса
        if len(text) > 50:
            text = text[:47] + "..."
        self.status_label.configure(text=text)

    def finish_scan(self, success, message):
        self.progress_bar.stop()
        self.btn_start.configure(state="normal", text="НАЧАТЬ СКАНИРОВАНИЕ")
        
        if success:
            self.status_label.configure(text="Готово!", text_color="green")
            if messagebox.askyesno("Успех", f"Сканирование завершено.\n\nОткрыть файл отчета?"):
                try:
                    os.startfile(message)  # Windows
                except AttributeError:
                    try:
                        os.system(f"open '{message}'")  # macOS
                    except:
                        os.system(f"xdg-open '{message}'") # Linux
        else:
            self.status_label.configure(text="Ошибка", text_color="red")
            messagebox.showerror("Ошибка", f"Произошла ошибка:\n{message}")

if __name__ == "__main__":
    app = ProjectScannerApp()
    app.mainloop()