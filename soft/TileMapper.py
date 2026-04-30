import tkinter as tk
from tkinter import filedialog, messagebox
from PIL import Image, ImageTk
import json

class TileMapper:
    def __init__(self, root):
        self.root = root
        self.root.title("Chronicles of Meterea - Tile Mapper Pro v1.0")
        self.root.geometry("1100x800")
        self.root.configure(bg="#2b2b2b")

        # Настройки Kenney 1-bit
        self.tile_size = 16
        self.spacing = 1
        
        self.mappings = {}
        self.img_source = None
        self.canvas_img = None
        self.selected_coords = (0, 0)

        self.setup_ui()

    def setup_ui(self):
        # Сайдбар
        self.sidebar = tk.Frame(self.root, width=300, bg="#3c3f41", bd=0)
        self.sidebar.pack(side="right", fill="y")

        tk.Label(self.sidebar, text="РЕДАКТОР ТАЙЛОВ", bg="#3c3f41", fg="#a9b7c6", font=("Arial", 12, "bold")).pack(pady=10)

        # Поле ввода имени
        tk.Label(self.sidebar, text="ID тайла (например, wall_stone):", bg="#3c3f41", fg="#fff").pack()
        self.id_entry = tk.Entry(self.sidebar, bg="#2b2b2b", fg="#fff", insertbackground="white")
        self.id_entry.pack(pady=5, padx=10, fill="x")
        self.id_entry.bind("<Return>", lambda e: self.add_mapping())

        tk.Button(self.sidebar, text="ДОБАВИТЬ / ОБНОВИТЬ", command=self.add_mapping, bg="#4b6eaf", fg="white").pack(pady=10)

        # Список маппингов
        self.listbox = tk.Listbox(self.sidebar, bg="#2b2b2b", fg="#a9b7c6", height=20)
        self.listbox.pack(pady=10, padx=10, fill="both", expand=True)

        tk.Button(self.sidebar, text="КОПИРОВАТЬ JSON", command=self.copy_json, bg="#315131", fg="white").pack(pady=10, padx=10, fill="x")
        tk.Button(self.sidebar, text="ЗАГРУЗИТЬ СПРАЙТ-ШИТ", command=self.load_image).pack(pady=5)

        # Зона превью
        self.preview_canvas = tk.Canvas(self.sidebar, width=64, height=64, bg="#000", highlightthickness=0)
        self.preview_canvas.pack(pady=20)
        tk.Label(self.sidebar, text="Превью (4x зум)", bg="#3c3f41", fg="#888").pack()

        # Основная зона скролла
        self.main_frame = tk.Frame(self.root, bg="#2b2b2b")
        self.main_frame.pack(side="left", fill="both", expand=True)

        self.canvas = tk.Canvas(self.main_frame, bg="#1e1e1e", cursor="cross")
        self.v_bar = tk.Scrollbar(self.main_frame, orient="vertical", command=self.canvas.yview)
        self.h_bar = tk.Scrollbar(self.main_frame, orient="horizontal", command=self.canvas.xview)
        
        self.canvas.configure(yscrollcommand=self.v_bar.set, xscrollcommand=self.h_bar.set)
        
        self.v_bar.pack(side="right", fill="y")
        self.h_bar.pack(side="bottom", fill="x")
        self.canvas.pack(side="left", fill="both", expand=True)

        self.canvas.bind("<Button-1>", self.on_canvas_click)

    def load_image(self):
        path = filedialog.askopenfilename(filetypes=[("PNG", "*.png")])
        if not path: return
        
        self.img_source = Image.open(path).convert("RGBA")
        # Увеличиваем для удобства клика (визуально)
        display_scale = 2
        w, h = self.img_source.size
        self.display_img = self.img_source.resize((w*display_scale, h*display_scale), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(self.display_img)
        
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.photo, anchor="nw")
        self.canvas.config(scrollregion=(0, 0, w*display_scale, h*display_scale))
        self.scale_factor = display_scale

    def on_canvas_click(self, event):
        if not self.img_source: return
        
        # Пересчитываем клик в реальные пиксели картинки
        real_x = self.canvas.canvasx(event.x) / self.scale_factor
        real_y = self.canvas.canvasy(event.y) / self.scale_factor

        # Вычисляем колонку и строку (x тайла и y тайла)
        # Учитываем отступ 1px между тайлами
        tile_x = int(real_x // (self.tile_size + self.spacing))
        tile_y = int(real_y // (self.tile_size + self.spacing))

        self.selected_coords = (tile_x, tile_y)
        
        # Рисуем рамку выбора
        self.canvas.delete("selector")
        sel_x = tile_x * (self.tile_size + self.spacing) * self.scale_factor
        sel_y = tile_y * (self.tile_size + self.spacing) * self.scale_factor
        size = self.tile_size * self.scale_factor
        self.canvas.create_rectangle(sel_x, sel_y, sel_x+size, sel_y+size, outline="cyan", width=2, tags="selector")

        self.update_preview(tile_x, tile_y)

    def update_preview(self, tx, ty):
        # Вырезаем тайл из оригинала
        x0 = tx * (self.tile_size + self.spacing)
        y0 = ty * (self.tile_size + self.spacing)
        tile_img = self.img_source.crop((x0, y0, x0+self.tile_size, y0+self.tile_size))
        tile_img = tile_img.resize((64, 64), Image.NEAREST)
        self.preview_photo = ImageTk.PhotoImage(tile_img)
        self.preview_canvas.create_image(0, 0, image=self.preview_photo, anchor="nw")

    def add_mapping(self):
        tile_id = self.id_entry.get().strip()
        if not tile_id:
            messagebox.showwarning("Внимание", "Введите ID тайла!")
            return
        
        self.mappings[tile_id] = {"x": self.selected_coords[0], "y": self.selected_coords[1]}
        self.refresh_list()
        self.id_entry.delete(0, tk.END)

    def refresh_list(self):
        self.listbox.delete(0, tk.END)
        for k, v in sorted(self.mappings.items()):
            self.listbox.insert(tk.END, f"{k}: x={v['x']}, y={v['y']}")

    def copy_json(self):
        output = "const TILE_SPRITE_MAP = " + json.dumps(self.mappings, indent=4) + ";"
        self.root.clipboard_clear()
        self.root.clipboard_append(output)
        messagebox.showinfo("Успех", "JSON скопирован в буфер обмена!")

if __name__ == "__main__":
    root = tk.Tk()
    app = TileMapper(root)
    root.mainloop()