import os
import json
import shutil
import datetime
import difflib
import re
import customtkinter as ctk
from tkinter import messagebox

# --- НАСТРОЙКИ ИНТЕРФЕЙСА ---
BG_COLOR = "#1e1e1e"
SIDEBAR_COLOR = "#252526"
TEXT_COLOR = "#d4d4d4"
DIFF_ADD_BG = "#234b23" 
DIFF_DEL_BG = "#512020" 
ACCENT_COLOR = "#007acc" 

BACKUP_DIR = ".ai_backups"

class BackupManagerWindow(ctk.CTkToplevel):
    def __init__(self, parent):
        super().__init__(parent)
        self.title("Машина Времени")
        self.geometry("600x450")
        self.configure(fg_color=BG_COLOR)
        self.attributes("-topmost", True)
        
        lbl = ctk.CTkLabel(self, text="История патчей:", font=("Consolas", 14, "bold"), text_color=ACCENT_COLOR)
        lbl.pack(pady=10)

        self.scroll_frame = ctk.CTkScrollableFrame(self, fg_color=SIDEBAR_COLOR)
        self.scroll_frame.pack(fill="both", expand=True, padx=10, pady=10)
        self.load_backups()

    def load_backups(self):
        if not os.path.exists(BACKUP_DIR): return
        backups = sorted(os.listdir(BACKUP_DIR), reverse=True)
        for b_dir in backups:
            full_path = os.path.join(BACKUP_DIR, b_dir)
            if not os.path.isdir(full_path): continue
            
            meta_path = os.path.join(full_path, "patch_meta.json")
            name = b_dir
            if os.path.exists(meta_path):
                try:
                    with open(meta_path, 'r', encoding='utf-8') as f:
                        meta = json.load(f)
                        name = f"[{meta['timestamp']}] {meta['patch_name']}"
                except: pass

            frame = ctk.CTkFrame(self.scroll_frame, fg_color=BG_COLOR)
            frame.pack(fill="x", pady=2, padx=5)
            ctk.CTkLabel(frame, text=name, font=("Consolas", 11), anchor="w").pack(side="left", padx=10, pady=5)
            ctk.CTkButton(frame, text="ОТКАТ", width=60, fg_color="#c0392b", 
                          command=lambda p=full_path: self.restore(p)).pack(side="right", padx=5)

    def restore(self, path):
        if messagebox.askyesno("Подтверждение", "Откатить файлы к этой версии?"):
            for root, _, files in os.walk(path):
                for f in files:
                    if f == "patch_meta.json": continue
                    src = os.path.join(root, f)
                    rel = os.path.relpath(src, path)
                    dst = os.path.join(os.getcwd(), rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
            messagebox.showinfo("Успех", "Файлы восстановлены!")
            self.destroy()

class AIPatcherPro(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("AI Patcher Pro - Ultimate Edition")
        self.geometry("1100x700")
        self.configure(fg_color=BG_COLOR)
        
        self.memory_files = {} 
        self.current_patch_name = "Без имени"

        self.setup_ui()

    def setup_ui(self):
        # Сайдбар
        self.sidebar = ctk.CTkFrame(self, width=300, fg_color=SIDEBAR_COLOR, corner_radius=0)
        self.sidebar.pack(side="left", fill="y")

        ctk.CTkLabel(self.sidebar, text="AI PATCHER PRO", font=("Consolas", 18, "bold"), text_color=ACCENT_COLOR).pack(pady=10)

        # Кнопки управления буфером
        btn_box = ctk.CTkFrame(self.sidebar, fg_color="transparent")
        btn_box.pack(fill="x", padx=10)
        ctk.CTkButton(btn_box, text="📋 Вставить", width=130, height=28, command=self.paste).pack(side="left", padx=2)
        ctk.CTkButton(btn_box, text="🗑 Очистить", width=130, height=28, fg_color="#c0392b", command=self.clear).pack(side="right", padx=2)

        self.txt_json = ctk.CTkTextbox(self.sidebar, height=250, font=("Consolas", 11))
        self.txt_json.pack(padx=10, pady=10, fill="x")

        self.btn_analyze = ctk.CTkButton(self.sidebar, text="🔍 Анализировать", height=35, command=self.analyze)
        self.btn_analyze.pack(pady=5, padx=10, fill="x")

        self.lbl_status = ctk.CTkLabel(self.sidebar, text="Статус: Ожидание", font=("Consolas", 12), text_color="#f1c40f")
        self.lbl_status.pack()

        # Кнопка генерации промпта для ИИ
        self.btn_prompt = ctk.CTkButton(self.sidebar, text="🤖 Скопировать Промпт для ИИ", height=30, fg_color="#2980b9", command=self.copy_ai_prompt)
        self.btn_prompt.pack(pady=15, padx=10, fill="x", side="bottom")

        self.btn_apply = ctk.CTkButton(self.sidebar, text="✅ ПРИМЕНИТЬ", height=40, state="disabled", fg_color="#27ae60", command=self.apply)
        self.btn_apply.pack(pady=5, padx=10, fill="x", side="bottom")

        self.btn_history = ctk.CTkButton(self.sidebar, text="📜 История", height=30, fg_color="#8e44ad", command=self.open_history)
        self.btn_history.pack(pady=5, padx=10, fill="x", side="bottom")

        # Основная зона (Diff)
        self.main_area = ctk.CTkFrame(self, fg_color=BG_COLOR)
        self.main_area.pack(side="right", fill="both", expand=True, padx=5, pady=5)

        self.txt_diff = ctk.CTkTextbox(self.main_area, font=("Consolas", 12), wrap="none")
        self.txt_diff.pack(fill="both", expand=True)
        
        self.txt_diff.tag_config("add", background=DIFF_ADD_BG)
        self.txt_diff.tag_config("del", background=DIFF_DEL_BG)
        self.txt_diff.tag_config("info", foreground=ACCENT_COLOR)
        # ИСПРАВЛЕНО: Убран font, оставлен только цвет
        self.txt_diff.tag_config("error", foreground="#e74c3c")

    def paste(self):
        self.txt_json.delete("1.0", "end")
        self.txt_json.insert("end", self.clipboard_get())

    def clear(self):
        self.txt_json.delete("1.0", "end")
        self.txt_diff.configure(state="normal")
        self.txt_diff.delete("1.0", "end")
        self.txt_diff.configure(state="disabled")
        self.btn_apply.configure(state="disabled")
        self.lbl_status.configure(text="Статус: Ожидание", text_color="#f1c40f")

    def open_history(self):
        BackupManagerWindow(self)

    def copy_ai_prompt(self):
        prompt = """Ты — AI-ассистент программиста. Выдавай код ТОЛЬКО в формате JSON для автоматического патчера.
Правила:
1. Выдавай только один блок ```json ... ```.
2. Ты можешь использовать переносы строк прямо внутри строк JSON, парсер это поймет.
3. Доступные action: 
   - "create_file" (создать новый)
   - "replace" (заменить search на content)
   - "insert_after" (вставить content после search)
   - "insert_before" (вставить content перед search)
   - "append" (добавить content в самый конец файла)
   - "prepend" (добавить content в самое начало файла)
   - "delete" (удалить блок search)

Пример ответа:
```json
{
  "patch_name": "Добавление новой функции",
  "operations": [
    {
      "path": "js/script.js",
      "action": "insert_after",
      "search": "function init() {",
      "content": "    console.log('Init started');"
    },
    {
      "path": "css/style.css",
      "action": "append",
      "content": ".new-class { color: red; }"
    }
  ]
}
```"""
        self.clipboard_clear()
        self.clipboard_append(prompt)
        messagebox.showinfo("Скопировано", "Идеальный промпт для ИИ скопирован в буфер обмена!\nВставь его в ChatGPT/Claude перед тем как просить написать код.")

    def extract_brace_block(self, content, start_marker):
        # Умный поиск маркера игнорируя пробелы
        match = self.fuzzy_find(content, start_marker)
        if not match: return -1, -1
        
        start_idx = content.find(match)
        b_start = content.find('{', start_idx)
        if b_start == -1: return -1, -1
        
        count = 0
        for i in range(b_start, len(content)):
            if content[i] == '{': count += 1
            elif content[i] == '}': count -= 1
            if count == 0: return start_idx, i + 1
        return -1, -1

    def fuzzy_find(self, text, search_query):
        """
        Магия для ИИ: Ищет текст, полностью игнорируя разницу в пробелах, табах и переносах строк.
        Если ИИ ошибся с отступами, эта функция всё равно найдет нужный кусок кода.
        """
        if not search_query.strip(): return None
        
        # Экранируем спецсимволы regex, но разбиваем по пробельным символам
        parts = [re.escape(p) for p in search_query.split()]
        # Собираем регулярку, где между словами может быть любое количество пробелов/переносов
        regex_pattern = r'\s*'.join(parts)
        
        try:
            match = re.search(regex_pattern, text)
            if match:
                return match.group(0) # Возвращаем РЕАЛЬНЫЙ кусок текста из файла
        except: pass
        return None

    def smart_replace(self, old_text, search_text, new_text, action):
        """Умная замена с несколькими уровнями защиты от ошибок ИИ"""
        
        # 1. Попытка идеального совпадения
        if search_text in old_text:
            actual_search = search_text
        else:
            # 2. Нормализация переносов строк (Windows \r\n -> Linux \n)
            old_norm = old_text.replace('\r\n', '\n')
            search_norm = search_text.replace('\r\n', '\n')
            if search_norm in old_norm:
                actual_search = search_norm
                old_text = old_norm
            else:
                # 3. FUZZY SEARCH (Нечеткий поиск - спасает в 99% случаев)
                actual_search = self.fuzzy_find(old_text, search_text)
                if not actual_search:
                    raise ValueError("Текст для привязки (search) не найден в файле даже при нечетком поиске.")

        # Выполняем действие с найденным реальным куском текста
        if action == "replace":
            return old_text.replace(actual_search, new_text)
        elif action == "insert_after":
            return old_text.replace(actual_search, actual_search + "\n" + new_text)
        elif action == "insert_before":
            return old_text.replace(actual_search, new_text + "\n" + actual_search)
        elif action == "delete":
            return old_text.replace(actual_search, "")
            
        return old_text

    def analyze(self):
        self.txt_diff.configure(state="normal")
        self.txt_diff.delete("1.0", "end")
        self.memory_files = {}
        
        raw = self.txt_json.get("1.0", "end").strip()
        try:
            # Умный поиск JSON блока (даже если ИИ добавил текст до/после)
            match = re.search(r'(\{.*\})', raw, re.DOTALL)
            if not match: raise ValueError("JSON структура не найдена в тексте")
            
            js_str = match.group(1)
            # Фикс висячих запятых (ИИ часто ставит запятую перед ']')
            js_str = re.sub(r',(\s*[\]}])', r'\1', js_str) 
            
            # strict=False - КРИТИЧЕСКИ ВАЖНО! Позволяет ИИ не экранировать \n внутри строк
            data = json.loads(js_str, strict=False)
        except Exception as e:
            self.txt_diff.insert("end", f"❌ КРИТИЧЕСКАЯ ОШИБКА ПАРСИНГА JSON:\n{e}\n\n", "error")
            self.txt_diff.insert("end", "Совет: Нажмите кнопку 'Скопировать Промпт' и отправьте его нейросети, чтобы она выдала правильный формат.", "info")
            self.txt_diff.configure(state="disabled")
            return

        self.current_patch_name = data.get("patch_name", "Без имени")
        ops = data.get("operations", [])
        errs = False

        for op in ops:
            path = op.get("path")
            action = op.get("action")
            abs_p = os.path.abspath(path)
            
            self.txt_diff.insert("end", f"\nФайл: {path} [{action}]\n", "info")
            
            if action != "create_file" and not os.path.exists(abs_p):
                self.txt_diff.insert("end", f"❌ ОШИБКА: Файл не найден на диске\n", "error")
                errs = True; continue

            # Загрузка контента
            if abs_p in self.memory_files:
                old_c = self.memory_files[abs_p]
            else:
                old_c = "" if action == "create_file" else open(abs_p, 'r', encoding='utf-8').read()

            new_c = old_c
            try:
                content = op.get("content", "")
                search = op.get("search", "")

                if action == "create_file": 
                    new_c = content
                elif action in ["replace", "insert_after", "insert_before", "delete"]:
                    if not search and action != "delete": raise ValueError("Отсутствует поле 'search'")
                    new_c = self.smart_replace(old_c, search, content, action)
                elif action == "append":
                    new_c = old_c + "\n" + content
                elif action == "prepend":
                    new_c = content + "\n" + old_c
                elif action in ["replace_js_block", "delete_js_block"]:
                    s, e = self.extract_brace_block(old_c, op.get("start_marker", ""))
                    if s == -1: raise ValueError("Маркер начала блока не найден")
                    new_c = old_c[:s] + (content if action == "replace_js_block" else "") + old_c[e:]
                else:
                    raise ValueError(f"Неизвестное действие: {action}")

                # Генерация красивого Diff
                diff_lines = list(difflib.unified_diff(old_c.splitlines(), new_c.splitlines(), n=2, lineterm=''))
                if not diff_lines and action != "create_file":
                    self.txt_diff.insert("end", "⚠️ Предупреждение: Изменений не произошло (код уже такой)\n", "info")
                
                for line in diff_lines:
                    if line.startswith('---') or line.startswith('+++'): continue
                    if line.startswith('@@'): continue
                    
                    if line.startswith('+'): self.txt_diff.insert("end", line + "\n", "add")
                    elif line.startswith('-'): self.txt_diff.insert("end", line + "\n", "del")
                    else: self.txt_diff.insert("end", line + "\n")
                
                self.memory_files[abs_p] = new_c
            except Exception as e:
                self.txt_diff.insert("end", f"❌ ОШИБКА ОПЕРАЦИИ: {e}\n", "error")
                errs = True

        self.txt_diff.configure(state="disabled")
        if not errs and self.memory_files:
            self.lbl_status.configure(text="✅ Готово к применению", text_color="#2ecc71")
            self.btn_apply.configure(state="normal")
        elif not self.memory_files:
            self.lbl_status.configure(text="⚠️ Нет изменений", text_color="#f39c12")
        else:
            self.lbl_status.configure(text="❌ Найдены ошибки", text_color="#e74c3c")

    def apply(self):
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        b_path = os.path.join(BACKUP_DIR, f"backup_{ts}")
        os.makedirs(b_path, exist_ok=True)
        
        with open(os.path.join(b_path, "patch_meta.json"), "w", encoding="utf-8") as f:
            json.dump({"timestamp": ts, "patch_name": self.current_patch_name}, f, ensure_ascii=False)

        for p, content in self.memory_files.items():
            # Бэкап старого файла если он был
            if os.path.exists(p):
                rel = os.path.relpath(p, os.getcwd())
                bp = os.path.join(b_path, rel)
                os.makedirs(os.path.dirname(bp), exist_ok=True)
                shutil.copy2(p, bp)
            
            # Принудительно создаем древо папок для нового файла
            os.makedirs(os.path.dirname(p), exist_ok=True)
            
            # Запись нового контента
            with open(p, 'w', encoding='utf-8') as f: 
                f.write(content)
            
        messagebox.showinfo("Успех", f"Патч '{self.current_patch_name}' успешно применен!")
        self.clear()

if __name__ == "__main__":
    app = AIPatcherPro()
    app.mainloop()