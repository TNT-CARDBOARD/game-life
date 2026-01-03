import ctypes
import sys
import os
import pygame
import math
import numpy as np
import time
import csv
import json
from collections import deque

# === SETUP ===
DLL_PATH = "./necrosphere.dll"
if not os.path.exists(DLL_PATH):
    print("❌ DLL НЕ НАЙДЕНА. Скомпилируй C++ сначала!")
    sys.exit()

lib = ctypes.CDLL(DLL_PATH)

# Константы мира и окна
WORLD_W, WORLD_H = 10000, 10000
MAX_AGENTS = 1000000
GENOME_SIZE = 64
MEMORY_SIZE = 16
WIN_W, WIN_H = 1600, 1000  # Размер окна из старой версии
VIEW_W, VIEW_H = 1300, 1000 # Область симуляции


# === C++ STRUCTURES (из старой, более подробной версии) ===
class Registers(ctypes.Structure):
    _fields_ = [("r", ctypes.c_int32 * 4), ("sp", ctypes.c_uint8), ("ip", ctypes.c_uint8), ("flags", ctypes.c_uint8)]

class Agent(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32), ("active", ctypes.c_bool),
        ("x", ctypes.c_float), ("y", ctypes.c_float), ("angle", ctypes.c_float), ("energy", ctypes.c_float),
        ("age", ctypes.c_uint32), ("generation", ctypes.c_uint32),
        ("code", ctypes.c_uint8 * GENOME_SIZE), ("stack", ctypes.c_int32 * 8), ("cpu", Registers),
        ("stat_attacks", ctypes.c_uint32), ("stat_signals", ctypes.c_uint32),
        ("stat_shared", ctypes.c_uint32), ("stat_writes", ctypes.c_uint32),
        ("species_id", ctypes.c_uint32),
        ("is_joined", ctypes.c_bool), ("is_hardened", ctypes.c_bool),
        ("attr_strength", ctypes.c_float),
        ("attr_speed", ctypes.c_float),
        ("memory", ctypes.c_int32 * MEMORY_SIZE),
        ("fear", ctypes.c_float),
        ("curiosity", ctypes.c_float),
        ("shout_buffer", ctypes.c_int32),
        ("shout_ttl", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 1),
        ("drive_fear", ctypes.c_float),
        ("drive_curiosity", ctypes.c_float),
        ("drive_social", ctypes.c_float),
        ("pad", ctypes.c_uint8 * 3) # Паддинг возможно придется поменять для выравнивания
    ]

class Daemon(ctypes.Structure):
    _fields_ = [("active", ctypes.c_bool), ("x", ctypes.c_float), ("y", ctypes.c_float), ("energy", ctypes.c_float), ("state", ctypes.c_int), ("kills", ctypes.c_uint32)]

class WorldStats(ctypes.Structure):
    _fields_ = [("ticks", ctypes.c_uint64), ("agents", ctypes.c_int), ("daemons", ctypes.c_int), ("nrg", ctypes.c_double), ("gen", ctypes.c_int), ("deaths", ctypes.c_int)]

# === БИНДИНГ ФУНКЦИЙ (полный, как в старой версии) ===
lib.init_simulation.restype = None
lib.set_paused.argtypes = [ctypes.c_bool]
lib.set_speed_delay.argtypes = [ctypes.c_int]
lib.render_to_buffer.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_int, ctypes.c_int, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_int]
lib.get_agents.restype = ctypes.POINTER(Agent)
lib.get_daemons.restype = ctypes.POINTER(Daemon)
lib.get_stats.restype = WorldStats
lib.get_tick.restype = ctypes.c_int

# === СИСТЕМЫ ИЗ СТАРОЙ ВЕРСИИ (ЛОГГЕР, ГРАФИКИ) ===
class Logger:
    def __init__(self):
        if not os.path.exists("logs"): os.makedirs("logs")
        self.csv_file = open("logs/stats.csv", "w", newline='')
        self.writer = csv.writer(self.csv_file)
        self.writer.writerow(["Tick", "Agents", "Daemons", "MaxGen"])
        self.trace_file = open("logs/trace.txt", "w")

    def log_stats(self, tick, stats):
        if tick % 100 == 0:
            self.writer.writerow([tick, stats.agents, stats.daemons, stats.gen])
            self.csv_file.flush()
    
    def log_trace(self, tick, agent):
        op_code = agent.code[agent.cpu.ip % GENOME_SIZE]
        op_name = OP_NAMES.get(op_code, f"unk_{op_code}")
        line = f"[{tick}] ID:{agent.id} | IP:{agent.cpu.ip} | OP:{op_name:<10} | NRG:{agent.energy:.1f}\n"
        self.trace_file.write(line)
        self.trace_file.flush()

    def close(self):
        self.csv_file.close()
        self.trace_file.close()

class Graph:
    def __init__(self, x, y, w, h, title, color):
        self.rect = pygame.Rect(x, y, w, h)
        self.title = title
        self.color = color
        self.data = deque(maxlen=w) 
        self.max_val = 1
        
    def add(self, value):
        self.data.append(value)
        self.max_val = max(self.max_val, value)
        if len(self.data) == self.data.maxlen and value < self.max_val * 0.7:
             self.max_val = max(1, int(max(self.data) * 1.1))

    def draw(self, surf, font):
        pygame.draw.rect(surf, (20, 20, 30), self.rect)
        pygame.draw.rect(surf, (50, 50, 60), self.rect, 1)
        val = self.data[-1] if self.data else 0
        txt = font.render(f"{self.title}: {val}", True, self.color)
        surf.blit(txt, (self.rect.x + 5, self.rect.y + 5))
        if len(self.data) < 2: return
        
        points = [(self.rect.x + i, self.rect.bottom - 5 - (v / self.max_val) * (self.rect.height - 20)) for i, v in enumerate(self.data)]
        pygame.draw.lines(surf, self.color, False, points, 2)


# === ИНИЦИАЛИЗАЦИЯ PYGAME ===
pygame.init()
screen = pygame.display.set_mode((WIN_W, WIN_H))
pygame.display.set_caption("NECROSPHERE | HYBRID ENGINE")
clock = pygame.time.Clock()
font = pygame.font.SysFont("Consolas", 14)
font_small = pygame.font.SysFont("Consolas", 12)

print("🚀 Запуск гибридной симуляции...")
lib.init_simulation()
agents_ptr = lib.get_agents()
daemons_ptr = lib.get_daemons() # Получаем указатель на демонов

# Настройка рендера на основе numpy из новой версии
pixel_data = np.zeros((VIEW_H, VIEW_W), dtype=np.uint32)
pixel_ptr = pixel_data.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32))
sim_surface = pygame.Surface((VIEW_W, VIEW_H))

# Переменные состояния из старой версии
camera_x, camera_y, zoom = WORLD_W//2, WORLD_H//2, 0.1
view_mode, selected_id, paused, speed_delay = 1, -1, False, 0
running = True

logger = Logger()
graph_pop = Graph(VIEW_W + 10, WIN_H - 160, 280, 70, "POPULATION", (0, 255, 0))
graph_gen = Graph(VIEW_W + 10, WIN_H - 80, 280, 70, "MAX GEN", (0, 200, 255))


# Полный словарь опкодов из старой версии
OP_NAMES = {
    0: "NOP", 1: "MOVE", 2: "ROT_L", 3: "ROT_R", 4: "SENSE_D", 5: "SENSE_P", 6: "SENSE_E",
    7: "EAT", 8: "ATK", 9: "GIVE", 10: "MARK", 11: "SPAWN", 12: "LOAD", 13: "STORE",
    14: "JMP", 15: "JZ", 16: "JNZ", 17: "JGT", 18: "READ_S", 19: "WRITE_S", 20: "RND", 
    21: "JOIN", 22: "HARDEN",
    23: "MEM_WRITE", 24: "MEM_READ",
    25: "SENSE_FEAR", 26: "SENSE_CUR",
    27: "MARK_PERM", 28: "SENSE_MARK",
    29: "SHOUT", 30: "LISTEN",
    31: "SENSE_DRIVE",
    255: "HALT"
}


# === ГЛАВНЫЙ ЦИКЛ ===
frame_cnt = 0
while running:
    clock.tick(60)
    frame_cnt += 1

    for event in pygame.event.get():
        if event.type == pygame.QUIT: 
            running = False
        
        # ===== ИСПРАВЛЕННЫЙ БЛОК ОБРАБОТКИ НАЖАТИЙ =====
        if event.type == pygame.KEYDOWN:
            if event.key == pygame.K_SPACE:
                paused = not paused
                lib.set_paused(paused)
            elif event.key == pygame.K_1:
                view_mode = 1
            elif event.key == pygame.K_2:
                view_mode = 2
            elif event.key == pygame.K_3:
                view_mode = 3
            elif event.key == pygame.K_UP:
                speed_delay = max(0, speed_delay - 10) # Увеличил шаг для скорости
                lib.set_speed_delay(speed_delay)
            elif event.key == pygame.K_DOWN:
                speed_delay = min(100, speed_delay + 10) # Увеличил шаг для скорости
                lib.set_speed_delay(speed_delay)
        # ===============================================

        if event.type == pygame.MOUSEBUTTONDOWN:
            mx, my = pygame.mouse.get_pos()
            if mx > VIEW_W: continue # Не выбирать в UI панели
            wx, wy = (mx - VIEW_W/2)/zoom + camera_x, (my - VIEW_H/2)/zoom + camera_y
            min_d, found_id = 100.0/zoom, -1
            # Умный поиск из старой версии
            step = 1 if zoom > 0.8 else 50 
            for i in range(0, MAX_AGENTS, step):
                if not agents_ptr[i].active: continue
                d = math.hypot(agents_ptr[i].x - wx, agents_ptr[i].y - wy)
                if d < min_d: min_d, found_id = d, i
            selected_id = found_id

    # Управление камерой
    keys = pygame.key.get_pressed()
    spd = 50 / zoom
    if keys[pygame.K_w]: camera_y -= spd
    if keys[pygame.K_s]: camera_y += spd
    if keys[pygame.K_a]: camera_x -= spd
    if keys[pygame.K_d]: camera_x += spd
    if keys[pygame.K_q]: zoom = max(0.01, zoom * 0.95)
    if keys[pygame.K_e]: zoom = min(10.0, zoom * 1.05)

    # === РЕНДЕР (быстрый, через C++) ===
    lib.render_to_buffer(pixel_ptr, VIEW_W, VIEW_H, camera_x, camera_y, zoom, view_mode, selected_id)
    pygame.surfarray.blit_array(sim_surface, pixel_data.T)
    
    screen.fill((10, 10, 20))
    screen.blit(sim_surface, (0, 0))
    
    # === UI и АНАЛИТИКА (из старой версии) ===
    stats = lib.get_stats()
    tick = lib.get_tick()

    if frame_cnt % 10 == 0 and not paused:
        logger.log_stats(tick, stats)
        graph_pop.add(stats.agents)
        graph_gen.add(stats.gen)

    if selected_id != -1 and agents_ptr[selected_id].active and not paused:
        logger.log_trace(tick, agents_ptr[selected_id])
    
    # --- Левая инфо-панель ---
    view_names = ["ROLES", "SPECIES", "ENERGY"]
    lines = [
        f"TICK: {tick}", f"AGENTS: {stats.agents}", f"DAEMONS: {stats.daemons}", f"MAX GEN: {stats.gen}",
        f"FPS: {clock.get_fps():.1f}", f"ZOOM: {zoom:.3f}", f"VIEW: {view_names[view_mode-1]} (1-3)",
        f"SPEED: {'PAUSED' if paused else f'DELAY {speed_delay}ms'} (UP/DOWN)"
    ]
    for i, l in enumerate(lines): screen.blit(font.render(l, True, (200, 200, 200)), (10, 10 + i*20))

    # --- Правая панель для выбранного агента ---
    if selected_id != -1 and agents_ptr[selected_id].active:
        a = agents_ptr[selected_id]
        panel_x = VIEW_W
        pygame.draw.rect(screen, (15, 15, 25), (panel_x, 0, WIN_W - panel_x, WIN_H))
        
        y = 10
        info_lines = [
            f"AGENT ID: {a.id}", f"SPECIES: {a.species_id:X}",
            f"ENERGY: {a.energy:.1f}",
            f"FEAR: {a.fear:.2f} | CURIOSITY: {a.curiosity:.2f}",
            # === Новая информация о драйвах ===
            f"D_FEAR: {a.drive_fear:.2f}",
            f"D_CUR:  {a.drive_curiosity:.2f}",
            f"D_SOC:  {a.drive_social:.2f}",
            "-"*20, "AGENT MEMORY (m0=cultural):"
        ]
        for l in info_lines:
            screen.blit(font.render(l, True, (200, 200, 200)), (panel_x + 10, y))
            y += 20
        
        # Рендер памяти агента
        for i in range(0, MEMORY_SIZE, 2):
            mem_str = f"m{i:02}: {a.memory[i]:<6} | m{i+1:02}: {a.memory[i+1]:<6}"
            screen.blit(font_small.render(mem_str, True, (120, 180, 255)), (panel_x + 15, y))
            y += 15

        screen.blit(font.render("-"*20, True, (200, 200, 200)), (panel_x + 10, y)); y+=20
        screen.blit(font.render("GENOME:", True, (200, 200, 200)), (panel_x + 10, y)); y+=20

        # Дамп генома (как и раньше)
        start_idx = max(0, a.cpu.ip - 8)
        end_idx = min(GENOME_SIZE, start_idx + 18)
        for i in range(start_idx, end_idx):
            op = a.code[i]
            op_name = OP_NAMES.get(op, f"0x{op:02X}")
            is_curr = (i == a.cpu.ip % GENOME_SIZE)
            col = (255, 255, 0) if is_curr else (100, 150, 100)
            prefix = ">> " if is_curr else f"{i:02}: "
            screen.blit(font_small.render(prefix + op_name, True, col), (panel_x + 15, y))
            y += 15
    
    # --- Нижняя панель с графиками ---
    graph_pop.draw(screen, font_small)
    graph_gen.draw(screen, font_small)

    pygame.display.flip()

logger.close()
pygame.quit()