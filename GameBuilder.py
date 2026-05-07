import os
import json
import subprocess
import re
import tkinter as tk
from tkinter import filedialog, messagebox
import customtkinter as ctk
from PIL import Image

# Настройка темы
ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

class GameBuilderApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("Chronicles of Meterea - Builder v1.2")
        self.geometry("600x580")
        self.resizable(False, False)

        self.project_path = os.getcwd()
        self.icon_path = ctk.StringVar(value="")
        self.current_version = self.get_current_version()

        self.setup_ui()

    def get_current_version(self):
        try:
            with open("package.json", "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("version", "1.0.0")
        except:
            return "1.0.0"

    def setup_ui(self):
        self.label_title = ctk.CTkLabel(self, text="BUILDER: METEREA", font=("MedievalSharp", 24, "bold"), text_color="#5dade2")
        self.label_title.pack(pady=20)

        # Инфо-блок про версию
        self.info_label = ctk.CTkLabel(self, text="Формат версии: X.Y.Z (например, 0.0.6)", font=("Arial", 11), text_color="#7f8c8d")
        self.info_label.pack()

        # Поле Версии
        self.version_frame = ctk.CTkFrame(self)
        self.version_frame.pack(pady=5, padx=40, fill="x")
        
        self.label_version = ctk.CTkLabel(self.version_frame, text="Версия игры:", font=("Arial", 14))
        self.label_version.pack(side="left", padx=10, pady=10)
        
        self.entry_version = ctk.CTkEntry(self.version_frame, width=150)
        self.entry_version.insert(0, self.current_version)
        self.entry_version.pack(side="right", padx=10, pady=10)

        # Выбор иконки
        self.icon_frame = ctk.CTkFrame(self)
        self.icon_frame.pack(pady=10, padx=40, fill="x")

        self.btn_browse_icon = ctk.CTkButton(self.icon_frame, text="Выбрать иконку", command=self.browse_icon)
        self.btn_browse_icon.pack(side="left", padx=10, pady=10)

        self.label_icon_path = ctk.CTkLabel(self.icon_frame, textvariable=self.icon_path, font=("Arial", 10), wraplength=250)
        self.label_icon_path.pack(side="right", padx=10, pady=10)

        # Переключатель DEBUG режима
        self.debug_frame = ctk.CTkFrame(self)
        self.debug_frame.pack(pady=5, padx=40, fill="x")
        
        self.debug_var = tk.BooleanVar(value=False)
        self.check_debug = ctk.CTkCheckBox(self.debug_frame, text="Режим отладки (DEBUG_MODE)", 
                                          variable=self.debug_var, font=("Arial", 13))
        self.check_debug.pack(side="left", padx=10, pady=10)

        # Прогресс
        self.status_label = ctk.CTkLabel(self, text="Готов к сборке", text_color="gray")
        self.status_label.pack(pady=(20,0))

        self.progress_bar = ctk.CTkProgressBar(self)
        self.progress_bar.set(0)
        self.progress_bar.pack(pady=10, padx=40, fill="x")

        # Кнопка Сборки
        self.btn_build = ctk.CTkButton(self, text="🚀 НАЧАТЬ СБОРКУ (.EXE)", 
                                       font=("Arial", 16, "bold"), 
                                       height=50, 
                                       fg_color="#2e7d32", 
                                       hover_color="#1b5e20",
                                       command=self.start_build)
        self.btn_build.pack(pady=20, padx=40, fill="x")

    def browse_icon(self):
        file = filedialog.askopenfilename(filetypes=[("Images", "*.png *.jpg *.jpeg *.webp *.bmp *.tga")])
        if file:
            self.icon_path.set(file)

    def is_valid_version(self, version):
        # Регулярка для проверки SemVer (X.Y.Z)
        pattern = r"^\d+\.\d+\.\d+(-[a-zA-Z0-9.]+)?$"
        return re.match(pattern, version) is not None

    def convert_icon(self, source_path):
        target_path = os.path.join(self.project_path, "assets", "icon.ico")
        os.makedirs(os.path.dirname(target_path), exist_ok=True)
        img = Image.open(source_path)
        img = img.resize((256, 256), Image.Resampling.LANCZOS)
        img.save(target_path, format='ICO', sizes=[(256, 256)])
        return target_path

    def update_package_json(self, version):
        path = "package.json"
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        
        data["version"] = version
        if "build" not in data: data["build"] = {}
        data["build"]["win"] = {"target": "nsis", "icon": "assets/icon.ico"}

        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    def update_script_debug_mode(self, is_debug):
        """Автоматически меняет константу DEBUG_MODE в script.js"""
        path = os.path.join(self.project_path, "script.js")
        if not os.path.exists(path):
            print("[Builder] ОШИБКА: script.js не найден!")
            return
        
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
        
        new_val = "true" if is_debug else "false"
        # Ищем строку const DEBUG_MODE = true; или const DEBUG_MODE = false;
        pattern = r"const DEBUG_MODE = (true|false);"
        
        if re.search(pattern, content):
            new_content = re.sub(pattern, f"const DEBUG_MODE = {new_val};", content)
            with open(path, "w", encoding="utf-8") as f:
                f.write(new_content)
            print(f"[Builder] script.js обновлен: DEBUG_MODE = {new_val}")
        else:
            print("[Builder] ПРЕДУПРЕЖДЕНИЕ: Не удалось найти строку 'const DEBUG_MODE' в script.js")

    def compile_engine(self):
        """Компилирует C++ ядро с флагом -static перед сборкой игры"""
        engine_dir = os.path.join(self.project_path, "engine")
        source_file = os.path.join(engine_dir, "meterea_engine.cpp")
        
        if not os.path.exists(source_file):
            raise Exception(f"Исходный код движка не найден: {source_file}")
            
        exe_name = "meterea_engine.exe" if os.name == 'nt' else "meterea_engine"
        output_file = os.path.join(engine_dir, exe_name)
        
        compile_cmd = [
            "g++", 
            "-std=c++17", 
            "-O2", 
            "-static", 
            "-o", output_file, 
            source_file
        ]
        
        try:
            print(f"[Builder] Запуск компиляции: {' '.join(compile_cmd)}")
            result = subprocess.run(compile_cmd, capture_output=True, text=True, check=True)
            print(f"[Builder] Движок успешно скомпилирован: {output_file}")
        except FileNotFoundError:
            raise Exception("Компилятор g++ не найден! Убедитесь, что MinGW/GCC установлен и добавлен в PATH.")
        except subprocess.CalledProcessError as e:
            error_msg = f"Ошибка компиляции C++ движка!\nКод: {e.returncode}\nВывод:\n{e.stderr}"
            print(error_msg)
            raise Exception(error_msg)

    def start_build(self):
        version = self.entry_version.get().strip()
        icon = self.icon_path.get()

        if not self.is_valid_version(version):
            messagebox.showerror("Ошибка версии", f"Версия '{version}' недопустима!\n\nИспользуйте формат X.Y.Z (например 0.0.6 или 1.2.0).")
            return

        if not icon and not os.path.exists("assets/icon.ico"):
            messagebox.showerror("Ошибка", "Выберите изображение для иконки!")
            return

        try:
            self.btn_build.configure(state="disabled")
            
            if icon:
                self.status_label.configure(text="📦 Конвертация иконки...", text_color="white")
                self.update()
                self.convert_icon(icon)
            
            self.status_label.configure(text="📝 Обновление package.json...", text_color="white")
            self.progress_bar.set(0.2)
            self.update()
            self.update_package_json(version)

            self.status_label.configure(text="⚙️ Настройка DEBUG_MODE...", text_color="white")
            self.progress_bar.set(0.3)
            self.update()
            self.update_script_debug_mode(self.debug_var.get())

            self.status_label.configure(text="🔨 Компиляция C++ ядра (Nexus Engine)...", text_color="#3498db")
            self.progress_bar.set(0.4)
            self.update()
            self.compile_engine()

            self.status_label.configure(text="🏗️ Сборка пошла (может занять 2-5 минут)...", text_color="#f1c40f")
            self.progress_bar.set(0.6)
            self.update()

            # Запуск npm run dist
            process = subprocess.Popen("npm run dist", shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            
            for line in process.stdout:
                print(line, end="") # Вывод в консоль для отладки
            
            process.wait()

            if process.returncode == 0:
                self.progress_bar.set(1.0)
                self.status_label.configure(text="✅ ГОТОВО!", text_color="#2ecc71")
                messagebox.showinfo("Успех", f"Сборка завершена успешно!\nРежим отладки: {'ВКЛ' if self.debug_var.get() else 'ВЫКЛ'}")
                os.startfile(os.path.join(self.project_path, "dist"))
            else:
                raise Exception("Electron-builder завершился с ошибкой. Проверь терминал.")

        except Exception as e:
            self.status_label.configure(text="❌ ОШИБКА", text_color="#e74c3c")
            messagebox.showerror("Ошибка билда", str(e))
        finally:
            self.btn_build.configure(state="normal")

if __name__ == "__main__":
    app = GameBuilderApp()
    app.mainloop()