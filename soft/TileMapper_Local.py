import tkinter as tk
from tkinter import filedialog, messagebox
from PIL import Image, ImageTk
import json
import requests
import io
import base64
import threading

# Базовый URL
BASE_URL = "http://127.0.0.1:1234/v1"

class TileMapperLocal:
    def __init__(self, root):
        self.root = root
        self.root.title("Meterea Smart Mapper - FIXING CHANNEL ERROR")
        self.root.geometry("1100x850")
        self.root.configure(bg="#1e1e1e")

        self.tile_size = 16
        self.spacing = 1
        self.mappings = {}
        self.img_source = None
        self.selected_coords = (0, 0)
        self.current_tile = None
        self.zoom = 3

        self.setup_ui()

    def setup_ui(self):
        self.sidebar = tk.Frame(self.root, width=300, bg="#252526")
        self.sidebar.pack(side="right", fill="y")
        
        tk.Label(self.sidebar, text="LOCAL AI STATUS", bg="#252526", fg="#5dade2", font=("Arial", 10, "bold")).pack(pady=5)
        self.status_lbl = tk.Label(self.sidebar, text="Ready", bg="#252526", fg="#7f8c8d")
        self.status_lbl.pack(pady=5)

        self.preview_canvas = tk.Canvas(self.sidebar, width=128, height=128, bg="#000", highlightthickness=1)
        self.preview_canvas.pack(pady=10)

        self.btn_ai = tk.Button(self.sidebar, text="🤖 РАСПОЗНАТЬ (LLAVA)", command=self.ask_local_ai, bg="#8e44ad", fg="white", font=("Arial", 10, "bold"))
        self.btn_ai.pack(pady=5, padx=20, fill="x")

        tk.Label(self.sidebar, text="ID ТАЙЛА:", bg="#252526", fg="#fff").pack()
        self.id_entry = tk.Entry(self.sidebar, bg="#1e1e1e", fg="#fff", font=("Consolas", 12), insertbackground="white")
        self.id_entry.pack(pady=5, padx=10, fill="x")
        self.id_entry.bind("<Return>", lambda e: self.add_mapping())

        self.listbox = tk.Listbox(self.sidebar, bg="#1e1e1e", fg="#d4d4d4", font=("Consolas", 10))
        self.listbox.pack(pady=10, padx=10, fill="both", expand=True)

        tk.Button(self.sidebar, text="КОПИРОВАТЬ JSON", command=self.copy_json, bg="#27ae60", fg="white").pack(pady=5, padx=10, fill="x")
        tk.Button(self.sidebar, text="ЗАГРУЗИТЬ АССЕТ", command=self.load_image).pack(pady=5)

        self.container = tk.Frame(self.root, bg="#0f0f0f")
        self.container.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(self.container, bg="#0f0f0f", cursor="cross")
        self.v_scroll = tk.Scrollbar(self.container, orient="vertical", command=self.canvas.yview)
        self.h_scroll = tk.Scrollbar(self.container, orient="horizontal", command=self.canvas.xview)
        self.canvas.configure(yscrollcommand=self.v_scroll.set, xscrollcommand=self.h_scroll.set)
        self.v_scroll.pack(side="right", fill="y")
        self.h_scroll.pack(side="bottom", fill="x")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.canvas.bind("<Button-1>", self.on_canvas_click)

    def load_image(self):
        path = filedialog.askopenfilename(filetypes=[("PNG", "*.png")])
        if not path: return
        self.img_source = Image.open(path).convert("RGBA")
        w, h = self.img_source.size
        self.display_img = self.img_source.resize((w*self.zoom, h*self.zoom), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(self.display_img)
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.photo, anchor="nw")
        self.canvas.config(scrollregion=(0, 0, w*self.zoom, h*self.zoom))

    def on_canvas_click(self, event):
        if not self.img_source: return
        cx, cy = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        tx = int((cx / self.zoom) // (self.tile_size + self.spacing))
        ty = int((cy / self.zoom) // (self.tile_size + self.spacing))
        self.selected_coords = (tx, ty)
        self.canvas.delete("sel")
        x0, y0 = tx * (self.tile_size + self.spacing) * self.zoom, ty * (self.tile_size + self.spacing) * self.zoom
        size = self.tile_size * self.zoom
        self.canvas.create_rectangle(x0, y0, x0+size, y0+size, outline="cyan", width=2, tags="sel")
        self.update_preview(tx, ty)

    def update_preview(self, tx, ty):
        x0, y0 = tx * (self.tile_size + self.spacing), ty * (self.tile_size + self.spacing)
        self.current_tile = self.img_source.crop((x0, y0, x0+self.tile_size, y0+self.tile_size))
        disp = self.current_tile.resize((128, 128), Image.NEAREST)
        self.preview_photo = ImageTk.PhotoImage(disp)
        self.preview_canvas.delete("all")
        self.preview_canvas.create_image(0, 0, image=self.preview_photo, anchor="nw")

    def get_vision_model(self):
        """Ищет модель llva в списке"""
        try:
            resp = requests.get(f"{BASE_URL}/models", timeout=5)
            if resp.status_code == 200:
                models = resp.json()['data']
                for m in models:
                    if "llava" in m['id'].lower():
                        return m['id']
                return models[0]['id'] # Если не нашли llava, берем первую
        except:
            return "llava-v1.5-7b-llamafile"
        return "llava-v1.5-7b-llamafile"

    def ask_local_ai(self):
        if self.current_tile is None: return
        self.status_lbl.config(text="⌛ Analyzing...", fg="#f1c40f")
        threading.Thread(target=self.call_lm_studio).start()

    def call_lm_studio(self):
        try:
            model_id = self.get_vision_model()
            
            # Подготовка картинки (LLaVA любит 336x336)
            temp_img = self.current_tile.convert("RGB").resize((336, 336), Image.LANCZOS)
            buffered = io.BytesIO()
            temp_img.save(buffered, format="JPEG")
            img_base64 = base64.b64encode(buffered.getvalue()).decode('utf-8')

            # УПРОЩЕННЫЙ ПАЙЛОАД (БЕЗ EXTRA KEYS)
            payload = {
                "model": model_id,
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": "Describe this RPG tile. Give me a single snake_case name. Example: cave_wall"},
                            {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{img_base64}"}}
                        ]
                    }
                ],
                "max_tokens": 20,
                "temperature": 0
            }

            response = requests.post(f"{BASE_URL}/chat/completions", json=payload, timeout=60)
            
            if response.status_code == 200:
                result = response.json()
                ai_text = result['choices'][0]['message']['content'].strip().lower()
                # Удаляем лишние знаки
                ai_text = ai_text.replace('"', '').replace("'", "").split('\n')[0].replace(" ", "_")
                
                self.id_entry.delete(0, tk.END)
                self.id_entry.insert(0, ai_text)
                self.status_lbl.config(text=f"✅ OK: {model_id[:10]}", fg="#2ecc71")
            else:
                self.status_lbl.config(text=f"❌ HTTP {response.status_code}", fg="#e74c3c")
                print(f"Error Detail: {response.text}")

        except Exception as e:
            self.status_lbl.config(text="❌ Connection Error", fg="#e74c3c")
            print(f"Exception: {e}")

    def add_mapping(self):
        tid = self.id_entry.get().strip()
        if tid:
            self.mappings[tid] = {"x": self.selected_coords[0], "y": self.selected_coords[1]}
            self.listbox.insert(tk.END, f"{tid}: {self.selected_coords[0]},{self.selected_coords[1]}")
            self.id_entry.delete(0, tk.END)

    def copy_json(self):
        js = "const TILE_SPRITE_MAP = " + json.dumps(self.mappings, indent=4) + ";"
        self.root.clipboard_clear()
        self.root.clipboard_append(js)
        messagebox.showinfo("OK", "JSON copied!")

if __name__ == "__main__":
    root = tk.Tk()
    app = TileMapperLocal(root)
    root.mainloop()