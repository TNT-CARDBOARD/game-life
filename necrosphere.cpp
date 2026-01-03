// necrosphere.cpp - MYTHOLOGY ENGINE (v3.1 FIX)
// Исправлена ошибка двойного определения 'pad' в структуре Agent.
// Компиляция: cl /std:c++20 /O2 /EHsc /LD necrosphere.cpp

#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>
#include <barrier>
#include <random>
#include <cstring>
#include <chrono>
#include <ctime>

#define DLL_EXPORT __declspec(dllexport)

// ==========================================
// CONFIG
// ==========================================
const int WORLD_W = 10000;
const int WORLD_H = 10000;
const int MAX_AGENTS = 1000000;
const int MAX_DAEMONS = 500;
const int CELL_SIZE = 4;
const int GRID_W = WORLD_W / CELL_SIZE;
const int GRID_H = WORLD_H / CELL_SIZE;
const int GENOME_SIZE = 64;
const int MEMORY_SIZE = 16;

// БАЛАНС
const float ENERGY_START = 200.0f;
const float COST_OP = 0.01f;
const float COST_EXIST = 0.02f;
const float COST_MOVE = 0.3f;
const float COST_ACTION = 1.0f;

enum OpCode : uint8_t {
    NOP = 0, MOV_FWD, ROT_L, ROT_R,
    SENSE_DIST, SENSE_PHEROMONE, SENSE_ENG,
    EAT, ATTACK, GIVE, MARK_PHEROMONE, SPAWN,
    LOAD, STORE, JMP, JZ, JNZ, JGT,
    READ_SELF, WRITE_SELF, RANDOM, JOIN, HARDEN,
    MEM_WRITE, MEM_READ,
    SENSE_FEAR, SENSE_CURIOSITY,
    MARK_PERMANENT, SENSE_MARK,
    SHOUT, LISTEN,
    SENSE_DRIVE,
    HALT = 255
};

struct Registers { int32_t r[4]; uint8_t sp, ip, flags; };

// ===== ИСПРАВЛЕННАЯ СТРУКТУРА AGENT =====
struct Agent {
    uint32_t id;
    bool active;
    float x, y, angle, energy;
    uint32_t age, generation;
    uint8_t code[GENOME_SIZE];
    int32_t stack[8];
    Registers cpu;
    uint32_t stat_attacks, stat_signals, stat_shared, stat_writes, species_id;
    bool is_joined, is_hardened;
    int32_t memory[MEMORY_SIZE];
    float fear;
    float curiosity;
    int32_t shout_buffer;
    uint8_t shout_ttl;
    float drive_fear;
    float drive_curiosity;
    float drive_social;
    uint8_t pad[3]; // Один pad в конце для выравнивания
};

struct Daemon { bool active; float x, y, energy; int state; uint32_t kills; };

struct Cell {
    std::atomic<float> food;
    std::atomic<int> pheromone;
    std::atomic<float> hazard;
    std::atomic<int> occupant_id;
    std::atomic<int32_t> permanent_mark;
};

struct WorldStats {
    uint64_t total_ticks;
    int alive_agents;
    int alive_daemons;
    double total_energy;
    int max_gen;
    int deaths_purge;
};

// ... (остальной код остается БЕЗ ИЗМЕНЕНИЙ) ...
// GLOBAL DATA
Agent agents[MAX_AGENTS];
Daemon daemons[MAX_DAEMONS];
Cell grid[GRID_W * GRID_H];
WorldStats stats;

const int NUM_THREADS = 16;
std::vector<std::thread> workers;
std::atomic<bool> running{ true };
std::atomic<bool> paused{ false };
std::atomic<int> tick_delay{ 0 };
std::atomic<int> tick_counter{ 0 };
std::barrier<>* sync_point = nullptr;

struct XorShift {
    uint32_t state;
    XorShift(uint32_t seed) : state(seed ? seed : 123456789) {}
    inline uint32_t next() {
        uint32_t x = state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        state = x;
        return x;
    }
    inline float next_float() { return (next() & 0xFFFFFF) / 16777216.0f; }
};

uint32_t calc_species(uint8_t* code) {
    uint32_t h = 5381;
    for (int i = 0; i < GENOME_SIZE; i++) h = ((h << 5) + h) + code[i];
    return h;
}

inline int grid_idx(float x, float y) {
    while (x < 0) x += WORLD_W; while (x >= WORLD_W) x -= WORLD_W;
    while (y < 0) y += WORLD_H; while (y >= WORLD_H) y -= WORLD_H;
    return ((int)y / CELL_SIZE) * GRID_W + ((int)x / CELL_SIZE);
}

void apply_degradation(Agent& ag, XorShift& rng) {
    bool starving = (ag.energy < 20.0f);
    bool old = (ag.age > 1000);
    if (starving || old) {
        float chance = (starving ? 0.05f : 0.0f) + (old ? 0.0001f * (ag.age - 1000) : 0.0f);
        if (rng.next_float() < chance) {
            int idx = rng.next() % GENOME_SIZE;
            ag.code[idx] = rng.next() % 256;
            if (rng.next_float() < 0.1f) ag.cpu.ip = rng.next() % GENOME_SIZE;
            ag.species_id = calc_species(ag.code);
        }
    }
}

void run_vm(Agent& ag, XorShift& rng) {
    int max_cycles = 30; int cycles = 0;
    while (cycles < max_cycles && ag.energy > 0) {
        uint8_t op = ag.code[ag.cpu.ip % GENOME_SIZE];
        ag.cpu.ip++; ag.energy -= COST_OP; cycles++;

        switch (op) {
        case NOP: break;
        case MOV_FWD: {
            if (ag.is_hardened) break;
            float speed = ag.is_joined ? 0.5f : 2.5f;
            ag.x += cos(ag.angle) * speed; ag.y += sin(ag.angle) * speed;
            if (ag.x < 0) ag.x += WORLD_W; else if (ag.x >= WORLD_W) ag.x -= WORLD_W;
            if (ag.y < 0) ag.y += WORLD_H; else if (ag.y >= WORLD_H) ag.y -= WORLD_H;
            ag.energy -= COST_MOVE;
            break;
        }
        case ROT_L: ag.angle -= 0.2f; break;
        case ROT_R: ag.angle += 0.2f; break;
        case SENSE_DIST: {
            float tx = ag.x + cos(ag.angle) * 40.0f;
            float ty = ag.y + sin(ag.angle) * 40.0f;
            ag.cpu.r[0] = (int32_t)grid[grid_idx(tx, ty)].food.load();
            break;
        }
        case SENSE_PHEROMONE: ag.cpu.r[0] = grid[grid_idx(ag.x, ag.y)].pheromone.load(); break;
        case SENSE_ENG: ag.cpu.r[0] = (int32_t)ag.energy; break;
        case EAT: {
            int idx = grid_idx(ag.x, ag.y);
            float v = grid[idx].food.exchange(0.0f);
            if (v > 0) ag.energy += v; else ag.energy -= 0.2f;
            break;
        }
        case ATTACK: {
            grid[grid_idx(ag.x, ag.y)].hazard.store(15.0f);
            ag.energy -= COST_ACTION; ag.stat_attacks++;
            break;
        }
        case GIVE: {
            if (ag.energy > 30.0f) {
                grid[grid_idx(ag.x, ag.y)].food.fetch_add(20.0f);
                ag.energy -= 20.0f; ag.stat_shared++;
            } break;
        }
        case MARK_PHEROMONE: {
            grid[grid_idx(ag.x, ag.y)].pheromone.store(ag.cpu.r[0]);
            ag.energy -= COST_ACTION; ag.stat_signals++;
            break;
        }
        case SPAWN: if (ag.energy > ENERGY_START * 2.0f) ag.cpu.flags |= 0x80; break;
        case JOIN: ag.is_joined = !ag.is_joined; break;
        case HARDEN: ag.is_hardened = !ag.is_hardened; break;
        case LOAD: { uint8_t addr = ag.code[ag.cpu.ip++ % GENOME_SIZE]; ag.cpu.r[0] = ag.code[addr % GENOME_SIZE]; break; }
        case STORE: { uint8_t addr = ag.code[ag.cpu.ip++ % GENOME_SIZE]; ag.code[addr % GENOME_SIZE] = (uint8_t)ag.cpu.r[0]; ag.stat_writes++; break; }
        case JMP: ag.cpu.ip += ag.code[ag.cpu.ip % GENOME_SIZE]; break;
        case JZ: if (ag.cpu.r[0] == 0) ag.cpu.ip += ag.code[ag.cpu.ip % GENOME_SIZE]; else ag.cpu.ip++; break;
        case JNZ: if (ag.cpu.r[0] != 0) ag.cpu.ip += ag.code[ag.cpu.ip % GENOME_SIZE]; else ag.cpu.ip++; break;
        case JGT: if (ag.cpu.r[0] > 0) ag.cpu.ip += ag.code[ag.cpu.ip % GENOME_SIZE]; else ag.cpu.ip++; break;
        case READ_SELF: ag.cpu.r[0] = ag.code[ag.cpu.r[1] % GENOME_SIZE]; break;
        case WRITE_SELF: {
            ag.code[ag.cpu.r[1] % GENOME_SIZE] = (uint8_t)ag.cpu.r[0];
            ag.species_id = calc_species(ag.code);
            ag.energy -= COST_ACTION * 2; ag.stat_writes++;
            break;
        }
        case RANDOM: ag.cpu.r[0] = rng.next() % 256; break;
        case MEM_WRITE: { ag.memory[ag.cpu.r[1] % MEMORY_SIZE] = ag.cpu.r[0]; ag.energy -= COST_OP * 2; break; }
        case MEM_READ: { ag.cpu.r[0] = ag.memory[ag.cpu.r[1] % MEMORY_SIZE]; break; }
        case SENSE_FEAR: { ag.cpu.r[0] = (int32_t)(ag.fear * 255.0f); break; }
        case SENSE_CURIOSITY: { ag.cpu.r[0] = (int32_t)(ag.curiosity * 255.0f); break; }
        case MARK_PERMANENT: { grid[grid_idx(ag.x, ag.y)].permanent_mark.store(ag.cpu.r[0]); ag.energy -= 50.0f; break; }
        case SENSE_MARK: {
            int val = grid[grid_idx(ag.x, ag.y)].permanent_mark.load();
            if (rng.next_float() < ag.fear * 0.2f) {
                val = rng.next();
            }
            ag.cpu.r[0] = val;
            if (val != 0 && val != ag.memory[MEMORY_SIZE - 1]) {
                ag.curiosity = std::min(1.0f, ag.curiosity + 0.5f);
                ag.memory[MEMORY_SIZE - 1] = val;
            }
            break;
        }
        case SHOUT: { ag.shout_buffer = ag.cpu.r[0]; ag.shout_ttl = 5; ag.energy -= 2.0f; break; }
        case LISTEN: {
            ag.cpu.r[0] = 0;
            int max_ttl = 0;
            int cx = (int)ag.x / CELL_SIZE; int cy = (int)ag.y / CELL_SIZE;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int ni = ((cy + dy + GRID_H) % GRID_H) * GRID_W + ((cx + dx + GRID_W) % GRID_W);
                int occupant_id = grid[ni].occupant_id.load();
                if (occupant_id > 0 && occupant_id < MAX_AGENTS && agents[occupant_id].active && agents[occupant_id].shout_ttl > max_ttl) {
                    ag.cpu.r[0] = agents[occupant_id].shout_buffer;
                    max_ttl = agents[occupant_id].shout_ttl;
                }
            }
            break;
        }
        case SENSE_DRIVE: {
            int dominant_drive = 0;
            float max_drive = ag.drive_fear;
            if (ag.drive_curiosity > max_drive) {
                max_drive = ag.drive_curiosity;
                dominant_drive = 1;
            }
            if (ag.drive_social > max_drive) {
                dominant_drive = 2;
            }
            ag.cpu.r[0] = dominant_drive;
            break;
        }
        }
    }

    int idx = grid_idx(ag.x, ag.y);
    grid[idx].occupant_id.store(ag.id);
    float dmg = grid[idx].hazard.exchange(0.0f);
    if (dmg > 0) {
        if (ag.is_hardened) dmg *= 0.1f;
        ag.energy -= dmg;
        ag.fear = std::min(1.0f, ag.fear + 0.5f);
    }
}

void worker_thread(int thread_id) {
    XorShift rng(thread_id * 888 + (uint32_t)time(0));
    while (running) {
        if (paused) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        sync_point->arrive_and_wait();

        int start_idx = (MAX_AGENTS / NUM_THREADS) * thread_id;
        int end_idx = start_idx + (MAX_AGENTS / NUM_THREADS);

        for (int i = start_idx; i < end_idx; ++i) {
            Agent& a = agents[i];
            if (!a.active) continue;

            apply_degradation(a, rng);
            run_vm(a, rng);

            a.age++;
            a.energy -= COST_EXIST;
            a.fear *= 0.99f;
            a.curiosity *= 0.995f;
            if (a.shout_ttl > 0) a.shout_ttl--;
            if (a.fear > 0.7f) a.drive_fear += 0.1f;
            if (a.curiosity > 0.7f) a.drive_curiosity += 0.1f;
            if (a.is_joined) a.drive_social += 0.05f;
            a.drive_fear *= 0.998f;
            a.drive_curiosity *= 0.998f;
            a.drive_social *= 0.998f;

            if (a.energy <= 0) {
                a.active = false;
                std::atomic_fetch_add((std::atomic<int>*) & stats.alive_agents, -1);
                grid[grid_idx(a.x, a.y)].food.fetch_add(40.0f);
                continue;
            }

            if (a.cpu.flags & 0x80) {
                a.cpu.flags &= ~0x80; a.energy /= 2.2f;
                for (int att = 0; att < 50; att++) {
                    int t = rng.next() % MAX_AGENTS;
                    if (!agents[t].active) {
                        agents[t] = a; agents[t].id = rng.next(); agents[t].active = true;
                        agents[t].age = 0; agents[t].generation++;
                        agents[t].stat_attacks = 0; agents[t].stat_signals = 0; agents[t].stat_shared = 0; agents[t].stat_writes = 0;
                        agents[t].fear = 0; agents[t].curiosity = 0.5f;
                        agents[t].drive_fear = 0; agents[t].drive_curiosity = 0; agents[t].drive_social = 0;
                        memset(agents[t].memory, 0, sizeof(agents[t].memory));

                        if (rng.next_float() < 0.01f) {
                            agents[t].memory[0] = a.memory[0];
                        }

                        agents[t].x += (rng.next_float() - 0.5f) * 10; agents[t].y += (rng.next_float() - 0.5f) * 10;
                        if (rng.next_float() < 0.05f) {
                            agents[t].code[rng.next() % GENOME_SIZE] = rng.next() % 256;
                            agents[t].species_id = calc_species(agents[t].code);
                        }
                        std::atomic_fetch_add((std::atomic<int>*) & stats.alive_agents, 1);
                        if (agents[t].generation > stats.max_gen) stats.max_gen = agents[t].generation;
                        break;
                    }
                }
            }
        }
        sync_point->arrive_and_wait();

        if (thread_id == 0) {
            if (stats.alive_agents < 1500) {
                for (int k = 0; k < 2000; k++) {
                    int idx = rng.next() % MAX_AGENTS;
                    if (!agents[idx].active) {
                        agents[idx].active = true; agents[idx].x = rng.next_float() * WORLD_W; agents[idx].y = rng.next_float() * WORLD_H;
                        agents[idx].energy = ENERGY_START; agents[idx].generation = 0;
                        for (int b = 0; b < GENOME_SIZE; b++) agents[idx].code[b] = rng.next() % 256;
                        agents[idx].species_id = calc_species(agents[idx].code);
                        stats.alive_agents++;
                    }
                }
            }
            if (rng.next_float() < 0.8f) {
                int clusters = (GRID_W * GRID_H) / 8000;
                for (int c = 0; c < clusters; c++) {
                    int cx = rng.next() % GRID_W;
                    int cy = rng.next() % GRID_H;
                    grid[cy * GRID_W + cx].food.store(800.0f);
                }
            }

            int d_active = 0;
            for (int d = 0; d < MAX_DAEMONS; d++) {
                if (daemons[d].active) {
                    daemons[d].energy -= 1.0f;
                    if (daemons[d].energy <= 0) daemons[d].active = false;
                    else d_active++;
                }
                else if (rng.next_float() < 0.005f && stats.alive_agents > 5000) {
                    daemons[d].active = true;
                    daemons[d].x = rng.next_float() * WORLD_W;
                    daemons[d].y = rng.next_float() * WORLD_H;
                    daemons[d].energy = 500;
                }
            }
            stats.alive_daemons = d_active;

            int erosion_attempts = 10;
            for (int i = 0; i < erosion_attempts; ++i) {
                int idx = rng.next() % (GRID_W * GRID_H);
                if (grid[idx].permanent_mark.load() != 0) {
                    if (rng.next_float() < 0.0001f) {
                        grid[idx].permanent_mark.store(0);
                    }
                }
            }

            tick_counter++;
        }
        sync_point->arrive_and_wait();
    }
}

extern "C" {
    DLL_EXPORT void init_simulation() {
        if (!sync_point) sync_point = new std::barrier<>(NUM_THREADS);
        for (int i = 0; i < MAX_AGENTS; i++) agents[i].active = false;
        for (int i = 0; i < MAX_DAEMONS; i++) daemons[i].active = false;

        for (int i = 0; i < GRID_W * GRID_H; ++i) grid[i].permanent_mark = 0;

        srand((unsigned)time(0));
        for (int i = 0; i < 20000; i++) {
            int k = rand() % MAX_AGENTS;
            if (agents[k].active) continue;
            agents[k].active = true; agents[k].x = rand() % WORLD_W; agents[k].y = rand() % WORLD_H;
            agents[k].energy = ENERGY_START; agents[k].generation = 0;
            for (int b = 0; b < GENOME_SIZE; b++) agents[k].code[b] = rand() % 256;
            agents[k].species_id = calc_species(agents[k].code);
            memset(agents[k].memory, 0, sizeof(agents[k].memory));
            agents[k].fear = 0;
            agents[k].curiosity = 0.5f;
            agents[k].drive_fear = 0; agents[k].drive_curiosity = 0; agents[k].drive_social = 0;
        }
        stats.alive_agents = 20000;
        if (workers.empty()) { for (int i = 0; i < NUM_THREADS; i++) workers.emplace_back(worker_thread, i); }
    }

    DLL_EXPORT void render_to_buffer(uint32_t* buffer, int bw, int bh, float cx, float cy, float zoom, int mode, int sel_id) {
        memset(buffer, 0, bw * bh * 4);
        float hw = bw * 0.5f; float hh = bh * 0.5f;

        if (zoom > 0.1) {
            for (int y = 0; y < bh; y += 4) {
                for (int x = 0; x < bw; x += 4) {
                    float wx = (x - hw) / zoom + cx;
                    float wy = (y - hh) / zoom + cy;
                    int mark = grid[grid_idx(wx, wy)].permanent_mark.load();
                    if (mark != 0) {
                        uint32_t h = mark;
                        uint8_t r = (h & 0xFF); uint8_t g = ((h >> 8) & 0xFF); uint8_t b = ((h >> 16) & 0xFF);
                        uint32_t color = ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
                        for (int ox = 0; ox < 4; ++ox) for (int oy = 0; oy < 4; ++oy) if ((y + oy) < bh && (x + ox) < bw) buffer[(y + oy) * bw + (x + ox)] = color;
                    }
                }
            }
        }

        for (int i = 0; i < MAX_AGENTS; i++) {
            if (!agents[i].active) continue;
            int sx = (int)((agents[i].x - cx) * zoom + hw);
            int sy = (int)((agents[i].y - cy) * zoom + hh);
            if (sx >= 0 && sx < bw && sy >= 0 && sy < bh) {
                uint8_t r = 0, g = 0, b = 0;
                if (mode == 1) {
                    if (agents[i].stat_writes > 2) { r = 255; g = 0; b = 255; }
                    else if (agents[i].stat_attacks > 5) { r = 255; g = 50; b = 50; }
                    else if (agents[i].stat_shared > 5) { r = 255; g = 255; b = 0; }
                    else if (agents[i].stat_signals > 5) { r = 0; g = 255; b = 255; }
                    else { g = (uint8_t)std::min(255.0f, agents[i].energy); b = 50; }
                }
                else if (mode == 2) {
                    uint32_t h = agents[i].species_id; r = (h & 0xFF); g = ((h >> 8) & 0xFF); b = ((h >> 16) & 0xFF);
                }
                else {
                    float nrg = std::max(0.0f, std::min(255.0f, agents[i].energy));
                    r = 255 - (uint8_t)nrg; g = (uint8_t)nrg; b = 0;
                }

                if (i == sel_id) { r = 255; g = 255; b = 255; }

                buffer[sy * bw + sx] |= (r << 16) | (g << 8) | b;
            }
        }
        for (int i = 0; i < MAX_DAEMONS; i++) {
            if (!daemons[i].active) continue;
            int sx = (int)((daemons[i].x - cx) * zoom + hw);
            int sy = (int)((daemons[i].y - cy) * zoom + hh);
            if (sx > 2 && sx < bw - 2 && sy > 2 && sy < bh - 2) {
                for (int ox = -2; ox <= 2; ox++)
                    for (int oy = -2; oy <= 2; oy++)
                        buffer[(sy + oy) * bw + (sx + ox)] = 0xFF0000;
            }
        }
    }

    DLL_EXPORT void set_paused(bool p) { paused = p; }
    DLL_EXPORT void set_speed_delay(int ms) { tick_delay = ms; }
    DLL_EXPORT Agent* get_agents() { return agents; }
    DLL_EXPORT Daemon* get_daemons() { return daemons; }
    DLL_EXPORT WorldStats get_stats() { return stats; }
    DLL_EXPORT int get_tick() { return tick_counter.load(); }
}