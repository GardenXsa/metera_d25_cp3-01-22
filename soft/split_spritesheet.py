import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from PIL import Image, ImageTk, ImageDraw   # ← добавили ImageDraw
import os
import uuid

class SpriteSplitter:
    def __init__(self, root):
        self.root = root
        self.root.title("Хроники Метеры — Разделитель спрайтов")
        self.root.geometry("720x620")          # ← меньше для твоего экрана
        self.root.resizable(True, True)        # можно тянуть
        self.root.minsize(650, 500)
        
        self.image_path = None
        self.image = None
        self.photo = None
        
        # UI
        tk.Label(root, text="Загрузи спрайт-шит с мечами:", font=("Arial", 11)).pack(pady=8)
        tk.Button(root, text="Выбрать PNG-файл", command=self.load_image, width=20).pack()
        
        # Превью с сеткой
        self.preview_label = tk.Label(root, text="Превью + сетка появится здесь")
        self.preview_label.pack(pady=10)
        
        # Настройки (компактнее)
        frame = ttk.LabelFrame(root, text="Настройки сетки")
        frame.pack(padx=15, pady=5, fill="x")
        
        row = 0
        ttk.Label(frame, text="Колонок:").grid(row=row, column=0, padx=5, pady=4, sticky="e")
        self.cols_var = tk.IntVar(value=6)
        ttk.Spinbox(frame, from_=1, to=20, textvariable=self.cols_var, width=5).grid(row=row, column=1)
        
        ttk.Label(frame, text="Строк:").grid(row=row, column=2, padx=5, pady=4, sticky="e")
        self.rows_var = tk.IntVar(value=5)
        ttk.Spinbox(frame, from_=1, to=20, textvariable=self.rows_var, width=5).grid(row=row, column=3)
        row += 1
        
        ttk.Label(frame, text="Ширина ячейки:").grid(row=row, column=0, padx=5, pady=4, sticky="e")
        self.cell_w_var = tk.IntVar(value=64)
        ttk.Spinbox(frame, from_=32, to=512, textvariable=self.cell_w_var, width=5).grid(row=row, column=1)
        
        ttk.Label(frame, text="Высота ячейки:").grid(row=row, column=2, padx=5, pady=4, sticky="e")
        self.cell_h_var = tk.IntVar(value=64)
        ttk.Spinbox(frame, from_=32, to=512, textvariable=self.cell_h_var, width=5).grid(row=row, column=3)
        row += 1
        
        ttk.Label(frame, text="Отступ между ячейками:").grid(row=row, column=0, padx=5, pady=4, sticky="e")
        self.h_pad_var = tk.IntVar(value=2)
        ttk.Spinbox(frame, from_=0, to=20, textvariable=self.h_pad_var, width=5).grid(row=row, column=1)
        ttk.Label(frame, text="Смещение X:").grid(row=row, column=2, padx=5, pady=4, sticky="e")
        self.offset_x_var = tk.IntVar(value=0)
        ttk.Spinbox(frame, from_=0, to=100, textvariable=self.offset_x_var, width=5).grid(row=row, column=3)
        row += 1
        
        ttk.Label(frame, text="Смещение Y:").grid(row=row, column=2, padx=5, pady=4, sticky="e")
        self.offset_y_var = tk.IntVar(value=0)
        ttk.Spinbox(frame, from_=0, to=100, textvariable=self.offset_y_var, width=5).grid(row=row, column=3)
        
        # Кнопки
        btn_frame = tk.Frame(root)
        btn_frame.pack(pady=12)
        tk.Button(btn_frame, text="Показать сетку на превью", command=self.show_grid_preview,
                  bg="#2196F3", fg="white", font=("Arial", 10, "bold")).pack(side="left", padx=8)
        tk.Button(btn_frame, text="РАЗДЕЛИТЬ + УДАЛИТЬ ФОН", command=self.split,
                  bg="#4CAF50", fg="white", font=("Arial", 12, "bold")).pack(side="left", padx=8)
        
        self.status = tk.Label(root, text="Готов. Загрузи файл и жми «Показать сетку»", fg="gray")
        self.status.pack(pady=5)

    def load_image(self):
        self.image_path = filedialog.askopenfilename(filetypes=[("PNG files", "*.png")])
        if not self.image_path: return
        self.image = Image.open(self.image_path).convert("RGBA")
        
        preview = self.image.copy()
        preview.thumbnail((420, 420))
        self.photo = ImageTk.PhotoImage(preview)
        self.preview_label.config(image=self.photo)
        self.status.config(text=f"Загружено: {os.path.basename(self.image_path)}", fg="green")

    def show_grid_preview(self):
        if not self.image:
            messagebox.showerror("Ошибка", "Сначала загрузи файл!")
            return
        
        # Создаём превью
        preview = self.image.copy()
        preview.thumbnail((420, 420))
        w, h = preview.size
        orig_w, orig_h = self.image.size
        scale = w / orig_w
        
        draw = ImageDraw.Draw(preview)
        
        cols = self.cols_var.get()
        rows = self.rows_var.get()
        cell_w = self.cell_w_var.get() * scale
        cell_h = self.cell_h_var.get() * scale
        h_pad = self.h_pad_var.get() * scale
        offset_x = self.offset_x_var.get() * scale
        offset_y = self.offset_y_var.get() * scale
        
        # Вертикальные линии
        for j in range(1, cols):
            x = offset_x + j * (cell_w + h_pad)
            draw.line([(x, 0), (x, h)], fill="red", width=3)
        
        # Горизонтальные линии
        for i in range(1, rows):
            y = offset_y + i * (cell_h + h_pad)
            draw.line([(0, y), (w, y)], fill="red", width=3)
        
        # Рамка вокруг
        draw.rectangle([offset_x, offset_y, w-1, h-1], outline="red", width=3)
        
        self.photo = ImageTk.PhotoImage(preview)
        self.preview_label.config(image=self.photo)
        self.status.config(text="Сетка наложена! Подгоняй параметры и жми кнопку снова", fg="#FF9800")

    def split(self):
        if not self.image:
            messagebox.showerror("Ошибка", "Загрузи файл!")
            return
        
        # тот же код разделения, что раньше (с удалением белого фона)
        cols = self.cols_var.get()
        rows = self.rows_var.get()
        cell_w = self.cell_w_var.get()
        cell_h = self.cell_h_var.get()
        h_pad = self.h_pad_var.get()
        offset_x = self.offset_x_var.get()
        offset_y = self.offset_y_var.get()
        
        output_dir = "output_swords"
        os.makedirs(output_dir, exist_ok=True)
        
        count = 0
        for i in range(rows):
            for j in range(cols):
                left = offset_x + j * (cell_w + h_pad)
                top = offset_y + i * (cell_h + h_pad)
                right = left + cell_w
                bottom = top + cell_h
                
                cell = self.image.crop((left, top, right, bottom))
                
                # Удаляем белый фон
                data = cell.getdata()
                new_data = [(r, g, b, 0) if (r, g, b) == (255, 255, 255) else (r, g, b, a) for r, g, b, a in data]
                cell.putdata(new_data)
                
                random_name = f"item_{uuid.uuid4().hex[:8]}.png"
                cell.save(os.path.join(output_dir, random_name))
                count += 1
        
        messagebox.showinfo("ГОТОВО!", f"Готово! {count} предметов с прозрачным фоном\nПапка: {os.path.abspath(output_dir)}")

if __name__ == "__main__":
    root = tk.Tk()
    app = SpriteSplitter(root)
    root.mainloop()