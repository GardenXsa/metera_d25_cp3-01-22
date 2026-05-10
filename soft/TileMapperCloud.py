import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog
import customtkinter as ctk
from PIL import Image, ImageTk
import json
import requests
import io
import base64
import threading
import os
import re
import time

# ========== НАСТРОЙКИ ==========
LLMOST_API_KEY = "YOUR_API_KEY_HERE"
LLMOST_BASE_URL = "https://llmost.ru/api/v1"
VISION_MODEL = "openai/gpt-4o"
BATCH_SIZE = 10
# ================================

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")

class TileMapperUltimate(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Meterea TileMapper Ultimate — Cloud AI (с панорамированием)")
        self.geometry("1200x700")
        self.minsize(900, 500)

        # Данные
        self.tile_size = 16
        self.spacing = 1
        self.mappings = {}
        self.img_source = None
        self.photo = None
        self.selected_coords = (0, 0)
        self.current_tile_img = None
        self.zoom = 4
        self.project_path = None
        self.is_batch_processing = False
        self.is_processing_selected = False

        # Множественное выделение
        self.selected_tiles = []
        self.preview_cache = {}

        # ID элементов на канвасе
        self.mapping_rects = {}
        self.selection_rects = {}
        self.current_highlight = None

        # Переменные для drag&pan
        self.drag_start_x = 0
        self.drag_start_y = 0

        self.setup_ui()
        self.update_status("Готов", "#7f8c8d")

        # Горячие клавиши
        self.bind("<Return>", lambda e: self.add_mapping())
        self.bind("<Control-s>", lambda e: self.save_project())
        self.bind("<Control-o>", lambda e: self.load_project())
        self.bind("<Control-b>", lambda e: self.batch_recognize())

    # ------------------------------------------------------------
    #  UI
    # ------------------------------------------------------------
    def setup_ui(self):
        main_frame = ctk.CTkFrame(self)
        main_frame.pack(fill="both", expand=True, padx=5, pady=5)

        # Левая панель
        self.left_panel = ctk.CTkFrame(main_frame, width=340)
        self.left_panel.pack(side="left", fill="y", padx=(0,5))
        self.left_panel.pack_propagate(False)

        self.left_scroll = ctk.CTkScrollableFrame(self.left_panel, fg_color="transparent")
        self.left_scroll.pack(fill="both", expand=True)

        ctk.CTkLabel(self.left_scroll, text="🎮 TILE MAPPER", font=ctk.CTkFont(size=18, weight="bold"), text_color="#e67e22").pack(pady=(5,0))
        ctk.CTkLabel(self.left_scroll, text="cloud AI · smart workflow", font=ctk.CTkFont(size=11)).pack()

        # Статус API
        self.status_frame = ctk.CTkFrame(self.left_scroll)
        self.status_frame.pack(fill="x", pady=8, padx=10)
        self.status_icon = ctk.CTkLabel(self.status_frame, text="●", text_color="#2ecc71", font=ctk.CTkFont(size=14))
        self.status_icon.pack(side="left", padx=5)
        self.status_label = ctk.CTkLabel(self.status_frame, text="API ready", font=ctk.CTkFont(size=12))
        self.status_label.pack(side="left")

        # Кнопки
        btn_frame = ctk.CTkFrame(self.left_scroll)
        btn_frame.pack(fill="x", pady=5, padx=10)
        ctk.CTkButton(btn_frame, text="📂 Загрузить спрайт-лист", command=self.load_image).pack(fill="x", pady=2)
        ctk.CTkButton(btn_frame, text="💾 Сохранить проект (Ctrl+S)", command=self.save_project).pack(fill="x", pady=2)
        ctk.CTkButton(btn_frame, text="📁 Загрузить проект (Ctrl+O)", command=self.load_project).pack(fill="x", pady=2)

        # Настройки сетки
        grid_frame = ctk.CTkFrame(self.left_scroll)
        grid_frame.pack(fill="x", pady=5, padx=10)
        ctk.CTkLabel(grid_frame, text="Настройки сетки", font=ctk.CTkFont(weight="bold")).pack(anchor="w")
        row1 = ctk.CTkFrame(grid_frame)
        row1.pack(fill="x", pady=2)
        ctk.CTkLabel(row1, text="Размер тайла:").pack(side="left", padx=5)
        self.tile_size_var = ctk.IntVar(value=16)
        ctk.CTkEntry(row1, textvariable=self.tile_size_var, width=60).pack(side="right", padx=5)
        row2 = ctk.CTkFrame(grid_frame)
        row2.pack(fill="x", pady=2)
        ctk.CTkLabel(row2, text="Отступ:").pack(side="left", padx=5)
        self.spacing_var = ctk.IntVar(value=1)
        ctk.CTkEntry(row2, textvariable=self.spacing_var, width=60).pack(side="right", padx=5)
        ctk.CTkButton(grid_frame, text="Применить", command=self.update_grid_settings, width=100).pack(pady=5)

        # Превью
        preview_frame = ctk.CTkFrame(self.left_scroll)
        preview_frame.pack(fill="x", pady=5, padx=10)
        ctk.CTkLabel(preview_frame, text="Текущий тайл", font=ctk.CTkFont(weight="bold")).pack(anchor="w")
        self.preview_canvas = tk.Canvas(preview_frame, width=128, height=128, bg="#1e1e1e", highlightthickness=1, highlightbackground="#555")
        self.preview_canvas.pack(pady=5)

        # ID
        ctk.CTkLabel(self.left_scroll, text="ID тайла (snake_case):", anchor="w").pack(fill="x", padx=10)
        self.id_entry = ctk.CTkEntry(self.left_scroll, font=("Consolas", 12))
        self.id_entry.pack(fill="x", padx=10, pady=2)

        # Действия
        actions_frame = ctk.CTkFrame(self.left_scroll)
        actions_frame.pack(fill="x", pady=5, padx=10)
        ctk.CTkButton(actions_frame, text="🤖 Распознать AI", command=self.ask_cloud_ai, fg_color="#8e44ad").pack(fill="x", pady=2)
        ctk.CTkButton(actions_frame, text="🚀 ПАКЕТНО: Всё", command=self.batch_recognize, fg_color="#e67e22").pack(fill="x", pady=2)
        ctk.CTkButton(actions_frame, text="➕ Добавить вручную", command=self.add_mapping, fg_color="#f39c12").pack(fill="x", pady=2)
        ctk.CTkButton(actions_frame, text="⏩ Следующий неразмеченный", command=self.jump_to_next_unmapped, fg_color="#16a085").pack(fill="x", pady=2)

        # Список маппингов
        list_frame = ctk.CTkFrame(self.left_scroll)
        list_frame.pack(fill="both", expand=True, pady=5, padx=10)
        ctk.CTkLabel(list_frame, text="Карта тайлов", font=ctk.CTkFont(weight="bold")).pack(anchor="w")
        self.listbox = tk.Listbox(list_frame, bg="#1e1e1e", fg="#d4d4d4", font=("Consolas", 9), height=8)
        self.listbox.pack(fill="both", expand=True, pady=2)
        self.listbox.bind("<Delete>", self.delete_mapping)
        self.listbox.bind("<Double-Button-1>", self.edit_mapping)

        bottom_frame = ctk.CTkFrame(self.left_scroll)
        bottom_frame.pack(fill="x", pady=5, padx=10)
        ctk.CTkButton(bottom_frame, text="📋 Копировать JSON", command=self.copy_json, fg_color="#2c3e50").pack(side="left", fill="x", expand=True, padx=2)
        ctk.CTkButton(bottom_frame, text="↩️ Отменить последний", command=self.undo_last, fg_color="#c0392b").pack(side="right", fill="x", expand=True, padx=2)

        # Правая панель (канвас) с прокруткой
        self.right_panel = ctk.CTkFrame(main_frame)
        self.right_panel.pack(side="right", fill="both", expand=True)

        self.canvas_frame = ctk.CTkFrame(self.right_panel)
        self.canvas_frame.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(self.canvas_frame, bg="#0f0f0f", highlightthickness=0)
        self.v_scroll = tk.Scrollbar(self.canvas_frame, orient="vertical", command=self.canvas.yview)
        self.h_scroll = tk.Scrollbar(self.canvas_frame, orient="horizontal", command=self.canvas.xview)
        self.canvas.configure(yscrollcommand=self.v_scroll.set, xscrollcommand=self.h_scroll.set)
        self.v_scroll.pack(side="right", fill="y")
        self.h_scroll.pack(side="bottom", fill="x")
        self.canvas.pack(side="left", fill="both", expand=True)

        # События мыши для прокрутки и панорамирования
        self.canvas.bind("<Button-1>", self.on_canvas_click)
        self.canvas.bind("<Button-3>", self.on_canvas_right_click)
        self.canvas.bind("<ButtonPress-2>", self.drag_start)        # средняя кнопка
        self.canvas.bind("<B2-Motion>", self.drag_move)             # перемещение с зажатой средней
        self.canvas.bind("<MouseWheel>", self.on_mousewheel)        # Windows / Linux
        self.canvas.bind("<Button-4>", self.on_mousewheel)          # Linux (scroll up)
        self.canvas.bind("<Button-5>", self.on_mousewheel)          # Linux (scroll down)

        # Статус
        self.bottom_status = ctk.CTkLabel(self, text="Ready", font=ctk.CTkFont(size=11), anchor="w")
        self.bottom_status.pack(side="bottom", fill="x", padx=10, pady=2)

        self.progress_label = ctk.CTkLabel(self.left_scroll, text="", font=ctk.CTkFont(size=10))
        self.progress_label.pack(pady=2)

    # ------------------------------------------------------------
    #  Прокрутка и панорамирование
    # ------------------------------------------------------------
    def update_scrollregion(self):
        """Обновить область прокрутки после изменения содержимого канваса"""
        if not self.img_source:
            return
        w, h = self.img_source.size
        display_w = w * self.zoom
        display_h = h * self.zoom
        self.canvas.config(scrollregion=(0, 0, display_w, display_h))

    def on_mousewheel(self, event):
        """Обработка колесика мыши"""
        if event.delta:
            # Windows / Mac
            delta = int(event.delta / 120)
        else:
            # Linux (Button-4 = up, Button-5 = down)
            delta = 1 if event.num == 4 else -1
        if event.state & 0x0001:  # Shift зажат -> горизонтальная прокрутка
            self.canvas.xview_scroll(-delta, "units")
        else:
            self.canvas.yview_scroll(-delta, "units")

    def drag_start(self, event):
        self.drag_start_x = self.canvas.canvasx(event.x)
        self.drag_start_y = self.canvas.canvasy(event.y)

    def drag_move(self, event):
        dx = self.canvas.canvasx(event.x) - self.drag_start_x
        dy = self.canvas.canvasy(event.y) - self.drag_start_y
        self.canvas.move("all", dx, dy)
        self.drag_start_x = self.canvas.canvasx(event.x)
        self.drag_start_y = self.canvas.canvasy(event.y)
        self.update_scrollregion()

    # ------------------------------------------------------------
    #  Отрисовка (с обновлением scrollregion)
    # ------------------------------------------------------------
    def redraw_canvas_full(self):
        if not self.img_source:
            return
        self.canvas.delete("all")
        w, h = self.img_source.size
        display_w = w * self.zoom
        display_h = h * self.zoom
        self.display_img = self.img_source.resize((display_w, display_h), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(self.display_img)
        self.canvas.create_image(0, 0, image=self.photo, anchor="nw")

        step = (self.tile_size + self.spacing) * self.zoom
        for x in range(0, display_w, step):
            self.canvas.create_line(x, 0, x, display_h, fill="#444", width=1, tags="grid")
        for y in range(0, display_h, step):
            self.canvas.create_line(0, y, display_w, y, fill="#444", width=1, tags="grid")

        self.mapping_rects.clear()
        for tid, pos in self.mappings.items():
            tx, ty = pos["x"], pos["y"]
            self._draw_mapping_rect(tx, ty)

        self._redraw_all_selections()
        self._draw_current_highlight()
        self.update_scrollregion()

    def _draw_mapping_rect(self, tx, ty):
        x0 = tx * (self.tile_size + self.spacing) * self.zoom
        y0 = ty * (self.tile_size + self.spacing) * self.zoom
        size = self.tile_size * self.zoom
        rect_id = self.canvas.create_rectangle(x0, y0, x0+size, y0+size, outline="#2ecc71", width=2, tags="mapping")
        self.mapping_rects[(tx, ty)] = rect_id
        return rect_id

    def _remove_mapping_rect(self, tx, ty):
        if (tx, ty) in self.mapping_rects:
            self.canvas.delete(self.mapping_rects[(tx, ty)])
            del self.mapping_rects[(tx, ty)]

    def _draw_selection_rect(self, tx, ty):
        x0 = tx * (self.tile_size + self.spacing) * self.zoom
        y0 = ty * (self.tile_size + self.spacing) * self.zoom
        size = self.tile_size * self.zoom
        rect_id = self.canvas.create_rectangle(x0, y0, x0+size, y0+size, outline="#f1c40f", width=3, tags="selection")
        self.selection_rects[(tx, ty)] = rect_id
        return rect_id

    def _remove_selection_rect(self, tx, ty):
        if (tx, ty) in self.selection_rects:
            self.canvas.delete(self.selection_rects[(tx, ty)])
            del self.selection_rects[(tx, ty)]

    def _redraw_all_selections(self):
        for rect_id in self.selection_rects.values():
            self.canvas.delete(rect_id)
        self.selection_rects.clear()
        for (tx, ty) in self.selected_tiles:
            self._draw_selection_rect(tx, ty)

    def _draw_current_highlight(self):
        if self.current_highlight:
            self.canvas.delete(self.current_highlight)
        tx, ty = self.selected_coords
        x0 = tx * (self.tile_size + self.spacing) * self.zoom
        y0 = ty * (self.tile_size + self.spacing) * self.zoom
        size = self.tile_size * self.zoom
        self.current_highlight = self.canvas.create_rectangle(x0, y0, x0+size, y0+size, outline="#00ffff", width=3, tags="current")

    def add_mapping_visual(self, tile_id, x, y):
        self._draw_mapping_rect(x, y)
        if (x, y) in self.selected_tiles:
            self.selected_tiles.remove((x, y))
            self._remove_selection_rect(x, y)

    def remove_mapping_visual(self, x, y):
        self._remove_mapping_rect(x, y)

    # ------------------------------------------------------------
    #  Выделение
    # ------------------------------------------------------------
    def clear_selection(self):
        for (x, y) in self.selected_tiles:
            self._remove_selection_rect(x, y)
        self.selected_tiles.clear()

    def toggle_selection(self, x, y):
        if self.is_tile_mapped(x, y):
            self.update_status(f"Тайл ({x},{y}) уже размечен", "#e74c3c")
            return
        if (x, y) in self.selected_tiles:
            self.selected_tiles.remove((x, y))
            self._remove_selection_rect(x, y)
            self.update_status(f"Тайл ({x},{y}) убран из выделения", "#f39c12")
        else:
            self.selected_tiles.append((x, y))
            self._draw_selection_rect(x, y)
            self.update_status(f"Тайл ({x},{y}) добавлен (всего {len(self.selected_tiles)})", "#2ecc71")

    # ------------------------------------------------------------
    #  Обработчики кликов
    # ------------------------------------------------------------
    def on_canvas_click(self, event):
        if not self.img_source:
            return
        cx = self.canvas.canvasx(event.x)
        cy = self.canvas.canvasy(event.y)
        step = (self.tile_size + self.spacing) * self.zoom
        tx = int(cx // step)
        ty = int(cy // step)
        max_x = self.img_source.width // (self.tile_size + self.spacing)
        max_y = self.img_source.height // (self.tile_size + self.spacing)
        if 0 <= tx < max_x and 0 <= ty < max_y:
            if event.state & 0x0001:  # Shift
                self.toggle_selection(tx, ty)
            else:
                self.clear_selection()
                self.selected_coords = (tx, ty)
                self._draw_current_highlight()
                self.update_preview()
                existing_id = self.get_id_at(tx, ty)
                if existing_id:
                    self.id_entry.delete(0, tk.END)
                    self.id_entry.insert(0, existing_id)
                else:
                    self.id_entry.delete(0, tk.END)

    def on_canvas_right_click(self, event):
        if not self.img_source:
            return
        cx = self.canvas.canvasx(event.x)
        cy = self.canvas.canvasy(event.y)
        step = (self.tile_size + self.spacing) * self.zoom
        tx = int(cx // step)
        ty = int(cy // step)
        existing_id = self.get_id_at(tx, ty)
        if existing_id and messagebox.askyesno("Удалить", f"Удалить '{existing_id}'?"):
            del self.mappings[existing_id]
            self.remove_mapping_visual(tx, ty)
            self.refresh_listbox()
            self.update_status(f"Удалён {existing_id}", "#e67e22")

    # ------------------------------------------------------------
    #  Вспомогательные методы
    # ------------------------------------------------------------
    def update_preview(self):
        if not self.img_source:
            return
        tx, ty = self.selected_coords
        cache_key = (tx, ty, self.tile_size, self.spacing)
        if cache_key in self.preview_cache:
            disp = self.preview_cache[cache_key]
        else:
            x0 = tx * (self.tile_size + self.spacing)
            y0 = ty * (self.tile_size + self.spacing)
            self.current_tile_img = self.img_source.crop((x0, y0, x0+self.tile_size, y0+self.tile_size))
            disp = self.current_tile_img.resize((128, 128), Image.NEAREST)
            self.preview_cache[cache_key] = disp
        self.preview_photo = ImageTk.PhotoImage(disp)
        self.preview_canvas.delete("all")
        self.preview_canvas.create_image(0, 0, image=self.preview_photo, anchor="nw")

    def get_id_at(self, tx, ty):
        for tid, pos in self.mappings.items():
            if pos["x"] == tx and pos["y"] == ty:
                return tid
        return None

    def is_tile_mapped(self, x, y):
        return any(pos["x"] == x and pos["y"] == y for pos in self.mappings.values())

    def add_mapping(self):
        tile_id = self.id_entry.get().strip()
        if not tile_id:
            messagebox.showwarning("Пустой ID", "Введите ID")
            return
        if tile_id in self.mappings and not messagebox.askyesno("Заменить?", f"ID '{tile_id}' уже существует. Заменить?"):
            return
        tx, ty = self.selected_coords
        old_id = self.get_id_at(tx, ty)
        if old_id:
            del self.mappings[old_id]
            self.remove_mapping_visual(tx, ty)
        self.mappings[tile_id] = {"x": tx, "y": ty}
        self.add_mapping_visual(tile_id, tx, ty)
        self.refresh_listbox()
        self.id_entry.delete(0, tk.END)
        self.update_status(f"Добавлен {tile_id}", "#2ecc71")
        self.jump_to_next_unmapped()

    def refresh_listbox(self):
        self.listbox.delete(0, tk.END)
        for tid, pos in self.mappings.items():
            self.listbox.insert(tk.END, f"{tid}  [{pos['x']},{pos['y']}]")

    def delete_mapping(self, event):
        sel = self.listbox.curselection()
        if sel:
            line = self.listbox.get(sel[0])
            tid = line.split("[")[0].strip()
            if tid in self.mappings:
                pos = self.mappings[tid]
                self.remove_mapping_visual(pos["x"], pos["y"])
                del self.mappings[tid]
                self.refresh_listbox()
                self.update_status(f"Удалён {tid}", "#e67e22")

    def edit_mapping(self, event):
        sel = self.listbox.curselection()
        if sel:
            line = self.listbox.get(sel[0])
            old = line.split("[")[0].strip()
            new = simpledialog.askstring("Редактировать", "Новое имя:", initialvalue=old)
            if new and new != old:
                pos = self.mappings.pop(old)
                self.mappings[new] = pos
                self.refresh_listbox()
                self.update_status(f"Переименован {old} → {new}", "#f39c12")

    def undo_last(self):
        if self.listbox.size() > 0:
            last = self.listbox.get(tk.END)
            tid = last.split("[")[0].strip()
            if tid in self.mappings:
                pos = self.mappings[tid]
                self.remove_mapping_visual(pos["x"], pos["y"])
                del self.mappings[tid]
                self.refresh_listbox()
                self.update_status(f"Отменён {tid}", "#e67e22")

    def jump_to_next_unmapped(self):
        if not self.img_source:
            return
        max_x = self.img_source.width // (self.tile_size + self.spacing)
        max_y = self.img_source.height // (self.tile_size + self.spacing)
        cx, cy = self.selected_coords
        for x in range(cx+1, max_x):
            if not self.is_tile_mapped(x, cy):
                self.selected_coords = (x, cy)
                self._draw_current_highlight()
                self.update_preview()
                self.id_entry.delete(0, tk.END)
                return
        for y in range(cy+1, max_y):
            for x in range(max_x):
                if not self.is_tile_mapped(x, y):
                    self.selected_coords = (x, y)
                    self._draw_current_highlight()
                    self.update_preview()
                    self.id_entry.delete(0, tk.END)
                    return
        messagebox.showinfo("Готово", "Все тайлы размечены!")

    # ------------------------------------------------------------
    #  Загрузка / сохранение
    # ------------------------------------------------------------
    def load_image(self):
        path = filedialog.askopenfilename(filetypes=[("PNG images", "*.png")])
        if not path:
            return
        self.img_source = Image.open(path).convert("RGBA")
        self.mappings.clear()
        self.listbox.delete(0, tk.END)
        self.project_path = None
        self.clear_selection()
        self.selected_coords = (0, 0)
        self.redraw_canvas_full()
        self.update_status(f"Загружен {os.path.basename(path)}", "#2ecc71")

    def update_grid_settings(self):
        self.tile_size = self.tile_size_var.get()
        self.spacing = self.spacing_var.get()
        if self.img_source:
            self.redraw_canvas_full()

    def save_project(self):
        if not self.img_source:
            messagebox.showwarning("Нет спрайт-листа", "Сначала загрузите изображение")
            return
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("Meterea project", "*.json")])
        if not path:
            return
        data = {
            "version": "1.0",
            "image_path": getattr(self.img_source, 'filename', ''),
            "tile_size": self.tile_size,
            "spacing": self.spacing,
            "zoom": self.zoom,
            "mappings": self.mappings
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        self.project_path = path
        self.update_status(f"Сохранён {os.path.basename(path)}", "#2ecc71")

    def load_project(self):
        path = filedialog.askopenfilename(filetypes=[("Meterea project", "*.json")])
        if not path:
            return
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.tile_size = data.get("tile_size", 16)
        self.spacing = data.get("spacing", 1)
        self.tile_size_var.set(self.tile_size)
        self.spacing_var.set(self.spacing)
        img_path = data.get("image_path")
        if img_path and os.path.exists(img_path):
            self.img_source = Image.open(img_path).convert("RGBA")
            self.img_source.filename = img_path
        else:
            self.load_image()
            if not self.img_source:
                return
        self.mappings = data.get("mappings", {})
        self.refresh_listbox()
        self.clear_selection()
        self.selected_coords = (0, 0)
        self.redraw_canvas_full()
        if self.mappings:
            first = next(iter(self.mappings.values()))
            self.selected_coords = (first["x"], first["y"])
            self._draw_current_highlight()
            self.update_preview()
        self.update_status(f"Загружен {os.path.basename(path)}", "#2ecc71")

    def copy_json(self):
        if not self.mappings:
            messagebox.showwarning("Нет данных", "Нечего копировать")
            return
        js = "const TILE_SPRITE_MAP = " + json.dumps(self.mappings, indent=4) + ";"
        self.clipboard_clear()
        self.clipboard_append(js)
        self.update_status("JSON скопирован", "#27ae60")

    # ------------------------------------------------------------
    #  AI методы (одиночный и пакетные) – без изменений
    # ------------------------------------------------------------
    def ask_cloud_ai(self):
        if self.selected_tiles:
            self.batch_recognize_selected()
        else:
            if self.current_tile_img is None:
                messagebox.showwarning("Нет тайла", "Кликните на тайл или выделите несколько с Shift")
                return
            self.update_status("Распознаю через AI...", "#f1c40f")
            threading.Thread(target=self.call_llmost_api, daemon=True).start()

    def call_llmost_api(self):
        try:
            img = self.current_tile_img.convert("RGB").resize((512, 512), Image.LANCZOS)
            buffered = io.BytesIO()
            img.save(buffered, format="JPEG", quality=85)
            img_b64 = base64.b64encode(buffered.getvalue()).decode('utf-8')
            payload = {
                "model": VISION_MODEL,
                "messages": [{
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "Describe this game tile in ONE snake_case word. Example: 'dark_purple_tile' or 'stone_wall'. Just the word."},
                        {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{img_b64}"}}
                    ]
                }],
                "max_tokens": 30,
                "temperature": 0.2
            }
            headers = {"Authorization": f"Bearer {LLMOST_API_KEY}", "Content-Type": "application/json"}
            resp = requests.post(f"{LLMOST_BASE_URL}/chat/completions", json=payload, headers=headers, timeout=30)
            if resp.status_code == 200:
                data = resp.json()
                ai_text = data['choices'][0]['message']['content'].strip().lower()
                ai_text = re.sub(r'[^a-z0-9_]', '_', ai_text).strip('_')
                if not ai_text:
                    ai_text = "unknown_tile"
                self.after(0, lambda: self.id_entry.delete(0, tk.END))
                self.after(0, lambda: self.id_entry.insert(0, ai_text))
                self.after(0, lambda: self.update_status(f"AI: {ai_text}", "#2ecc71"))
            else:
                self.after(0, lambda: self.update_status(f"Ошибка API {resp.status_code}", "#e74c3c"))
        except Exception as e:
            self.after(0, lambda: self.update_status(f"Сбой: {str(e)[:30]}", "#e74c3c"))

    def batch_recognize_selected(self):
        if self.is_processing_selected:
            return
        if not self.img_source:
            messagebox.showwarning("Нет спрайт-листа", "Загрузите изображение")
            return
        unmapped_selected = [(x, y) for (x, y) in self.selected_tiles if not self.is_tile_mapped(x, y)]
        if not unmapped_selected:
            messagebox.showinfo("Нет неразмеченных", "Все выделенные тайлы уже размечены")
            self.clear_selection()
            return
        total = len(unmapped_selected)
        batches = (total + BATCH_SIZE - 1) // BATCH_SIZE
        approx_cost = batches * 0.02
        if messagebox.askyesno("Пакетное распознавание (выделенные)",
                               f"Выделено неразмеченных тайлов: {total}\n"
                               f"Будет отправлено {batches} запросов (по {BATCH_SIZE} тайлов).\n"
                               f"Примерная стоимость: ${approx_cost:.2f}\nПродолжить?"):
            self.is_processing_selected = True
            self.progress_label.configure(text="⏳ Обработка...")
            self.update_status(f"Обработка выделенных: 0/{total}", "#f1c40f")
            threading.Thread(target=self._process_selected_tiles, args=(unmapped_selected,), daemon=True).start()

    def _process_selected_tiles(self, tiles):
        total = len(tiles)
        processed = 0
        for i in range(0, total, BATCH_SIZE):
            chunk = tiles[i:i+BATCH_SIZE]
            self.after(0, lambda c=chunk, idx=i: self.update_status(f"Пакет {idx//BATCH_SIZE+1}: {len(c)} тайлов", "#f1c40f"))
            results = self.send_batch_request(chunk)
            added = 0
            for (x, y, tile_id) in results:
                if tile_id and not self.is_tile_mapped(x, y):
                    final_id = tile_id
                    cnt = 1
                    while final_id in self.mappings:
                        final_id = f"{tile_id}_{cnt}"
                        cnt += 1
                    self.mappings[final_id] = {"x": x, "y": y}
                    self.after(0, lambda tid=final_id, xx=x, yy=y: self.add_mapping_visual(tid, xx, yy))
                    added += 1
                processed += 1
            self.after(0, lambda: self.refresh_listbox())
            time.sleep(0.5)
        self.is_processing_selected = False
        self.after(0, lambda: self.progress_label.configure(text=""))
        self.after(0, lambda: self.clear_selection())
        self.after(0, lambda: self.update_status(f"Выделенные обработаны. Добавлено {processed} тайлов", "#2ecc71"))

    def batch_recognize(self):
        if self.is_batch_processing:
            return
        if not self.img_source:
            messagebox.showwarning("Нет спрайт-листа", "Загрузите изображение")
            return
        max_x = self.img_source.width // (self.tile_size + self.spacing)
        max_y = self.img_source.height // (self.tile_size + self.spacing)
        unmapped = [(x,y) for y in range(max_y) for x in range(max_x) if not self.is_tile_mapped(x,y)]
        if not unmapped:
            messagebox.showinfo("Готово", "Все тайлы уже размечены!")
            return
        batches = (len(unmapped) + BATCH_SIZE - 1) // BATCH_SIZE
        approx_cost = batches * 0.02
        if messagebox.askyesno("Пакетное распознавание",
                               f"Найдено {len(unmapped)} тайлов.\n"
                               f"Будет отправлено {batches} запросов (по {BATCH_SIZE} тайлов).\n"
                               f"Примерная стоимость: ${approx_cost:.2f}\nПродолжить?"):
            self.is_batch_processing = True
            self.progress_label.configure(text="⏳ Пакетная обработка...")
            self.update_status(f"Пакет: 0/{len(unmapped)}", "#f1c40f")
            threading.Thread(target=self.process_batch_grouped, args=(unmapped,), daemon=True).start()

    def process_batch_grouped(self, tiles):
        total = len(tiles)
        processed = 0
        for i in range(0, total, BATCH_SIZE):
            chunk = tiles[i:i+BATCH_SIZE]
            self.after(0, lambda: self.update_status(f"Пакет: {i}/{total} (отправка {len(chunk)} тайлов)", "#f1c40f"))
            results = self.send_batch_request(chunk)
            for (x, y, tile_id) in results:
                if tile_id and not self.is_tile_mapped(x, y):
                    final_id = tile_id
                    cnt = 1
                    while final_id in self.mappings:
                        final_id = f"{tile_id}_{cnt}"
                        cnt += 1
                    self.mappings[final_id] = {"x": x, "y": y}
                    self.after(0, lambda tid=final_id, xx=x, yy=y: self.add_mapping_visual(tid, xx, yy))
                processed += 1
            self.after(0, lambda: self.refresh_listbox())
            time.sleep(0.5)
        self.is_batch_processing = False
        self.after(0, lambda: self.progress_label.configure(text=""))
        self.after(0, lambda: self.update_status(f"Пакет завершён. Обработано {processed} тайлов", "#2ecc71"))

    def send_batch_request(self, chunk):
        content = [{"type": "text", "text": f"You will receive {len(chunk)} tile images. For each image, output a JSON array of strings in snake_case describing the tile. Example: [\"dark_purple_tile\", \"stone_wall\", \"grass_center\"]. Order must match the order of images."}]
        for (x, y) in chunk:
            x0 = x*(self.tile_size+self.spacing)
            y0 = y*(self.tile_size+self.spacing)
            tile_img = self.img_source.crop((x0, y0, x0+self.tile_size, y0+self.tile_size))
            img = tile_img.convert("RGB").resize((256, 256), Image.LANCZOS)
            buffered = io.BytesIO()
            img.save(buffered, format="JPEG", quality=75)
            img_b64 = base64.b64encode(buffered.getvalue()).decode('utf-8')
            content.append({"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{img_b64}"}})

        payload = {
            "model": VISION_MODEL,
            "messages": [{"role": "user", "content": content}],
            "max_tokens": 200,
            "temperature": 0.2,
            "response_format": {"type": "json_object"}
        }
        headers = {"Authorization": f"Bearer {LLMOST_API_KEY}", "Content-Type": "application/json"}
        try:
            resp = requests.post(f"{LLMOST_BASE_URL}/chat/completions", json=payload, headers=headers, timeout=60)
            if resp.status_code == 200:
                data = resp.json()
                answer = data['choices'][0]['message']['content'].strip()
                match = re.search(r'\[.*\]', answer, re.DOTALL)
                if match:
                    names = json.loads(match.group(0))
                else:
                    names = []
                results = []
                for idx, (x, y) in enumerate(chunk):
                    name = names[idx] if idx < len(names) else "unknown"
                    name = re.sub(r'[^a-z0-9_]', '_', name).strip('_')
                    if not name:
                        name = "unknown"
                    results.append((x, y, name))
                return results
            else:
                return [(x, y, None) for (x, y) in chunk]
        except Exception as e:
            return [(x, y, None) for (x, y) in chunk]

    def update_status(self, msg, color="#7f8c8d"):
        self.bottom_status.configure(text=msg, text_color=color)
        self.status_label.configure(text=msg[:40], text_color=color)

if __name__ == "__main__":
    app = TileMapperUltimate()
    app.mainloop()