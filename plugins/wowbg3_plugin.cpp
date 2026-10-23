// WOW-BG3 native plugin for bg3le (design: "plug and play,
// like SkyCraft"). Dropped into ~/.local/share/bg3le/plugins, it replaces runtime
// patch 0003 (arena hooks) without changing bg3le's source:
//
//   A  WoW walkable/blocked tiles and heights stamped into BG3's AI grid, restorable;
//   B  WoW triangles added to the physics scenes' raycasts (optional arena "void");
//   C  the client's camera takes an outside pose, and can follow benilla's camera
//      streamed through the link directory every frame (FOV too).
//
// How it reaches the engine: bg3le exports its functions (dlsym), so the plugin
// finds the server level manager (bg3le_server_level_manager), the client one
// (bg3le::target::ClientLevelManager + load_bias) and the engine allocator
// (bg3se::GameAllocRaw/GameFree). Every lookup is checked; a missing one turns the
// feature off, never the game.
//
// How Lua reaches the plugin: a plugin cannot add Lua functions, so each physics
// scene gets a private vtable copy (as patch 0003 part B did) whose RaycastClosest
// recognises one reserved query (type 0, include 0, context kChannel): that call,
// made by the SE mod through Ext._Internal.PhysicsQuery, runs a plugin command
// synchronously on the calling context's own thread (server Lua on the server
// thread, client Lua on the client thread), with six float arguments and the
// opcode in the exclude mask. Results go to a per-side Out block whose address the
// plugin publishes as two int settings (Lua reads it with Ext._Internal.Peek).
// Bulk input (tiles, triangles, poses) comes from files in the mod's IO folder.
// Every other query passes straight to the engine.
//
// Fail-closed rules are the patch's: original vtables are kept and restored only
// while ours is still installed; tables and triangle sets are never freed; tiles
// are restored only while they still hold what was written.

#include <stdafx.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <GameDefinitions/Level.h>
#include <GameDefinitions/Physics.h>
#include <GameDefinitions/Ai.h>
#include <GameDefinitions/Components/Camera.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bg3le_plugin.h"

#define WOWBG3_VERSION "0.2.9"

// ---- what bg3le exports ----

namespace {

struct Api {
    void* (*ServerLevelManager)() = nullptr;
    bool (*AllocatorReady)() = nullptr;
    std::uintptr_t (*ClientLevelManagerOffset)() = nullptr;
    std::uintptr_t (*LoadBias)() = nullptr;
    void* (*Alloc)(std::size_t) = nullptr;
    void (*Free)(void*) = nullptr;
};
Api g_api;
const bg3le_host* g_host = nullptr;
bg3le_plugin* g_self = nullptr;

void logf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(char const* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_host != nullptr) g_host->log(g_self, "%s", buf);
}

bool allocator_ready() {
    return g_api.Alloc != nullptr && g_api.Free != nullptr && g_api.AllocatorReady != nullptr
        && g_api.AllocatorReady();
}

// A read that cannot fault, as bg3le's safe_read.
bool safe_read(void const* addr, void* out, std::size_t n) {
    if (addr == nullptr || reinterpret_cast<std::uintptr_t>(addr) < 0x1000) return false;
    iovec local{out, n};
    iovec remote{const_cast<void*>(addr), n};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

}  // namespace

// bg3se's containers grow through the engine's allocator; these forward to bg3le's.
namespace bg3se {
void* GameAllocRaw(std::size_t size) { return g_api.Alloc != nullptr ? g_api.Alloc(size) : nullptr; }
void GameFree(void* p) noexcept {
    if (g_api.Free != nullptr) g_api.Free(p);
}
// bg3se's container asserts: logged, never fatal here (bounds are checked before use).
void AssertionFailed(char const* what) { logf("bg3se assertion: %s", what != nullptr ? what : "?"); }
}  // namespace bg3se

// ---- level, physics scene and AI grid lookups (bg3le's src/vendor/level.cpp) ----

namespace {

bool has_vtable(void const* obj) {
    std::uint64_t vmt = 0;
    return obj != nullptr && safe_read(obj, &vmt, sizeof(vmt)) && vmt > 0x10000;
}

void* client_level_manager() {
    if (g_api.ClientLevelManagerOffset == nullptr || g_api.LoadBias == nullptr) return nullptr;
    const std::uintptr_t off = g_api.ClientLevelManagerOffset();
    if (off == 0) return nullptr;
    void* mgr = nullptr;
    auto const* slot = (void const*)(g_api.LoadBias() + off);
    if (!safe_read(slot, &mgr, sizeof(mgr)) || !has_vtable(mgr)) return nullptr;
    auto* m = static_cast<bg3se::LevelManager*>(mgr);
    bg3se::EoCLevel* level = nullptr;
    bg3se::LevelManager* owner = nullptr;
    if (!safe_read(&m->CurrentLevel, &level, sizeof(level)) || level == nullptr
        || !safe_read(&level->LevelManager, &owner, sizeof(owner)) || owner != m) {
        return nullptr;
    }
    return mgr;
}

void* level_manager(bool client) {
    if (client) return client_level_manager();
    return g_api.ServerLevelManager != nullptr ? g_api.ServerLevelManager() : nullptr;
}

bg3se::EoCLevel* current_level(bool client) {
    auto* mgr = static_cast<bg3se::LevelManager*>(level_manager(client));
    bg3se::EoCLevel* level = nullptr;
    if (mgr == nullptr || !safe_read(&mgr->CurrentLevel, &level, sizeof(level))) return nullptr;
    return level;
}

// bg3se's declaration puts both cylinder sweeps at slots 14 and 18 (`xor eax, eax; ret`).
bool scene_checks_out(void const* scene) {
    std::uint64_t vmt = 0, slots[19] = {};
    if (!safe_read(scene, &vmt, sizeof(vmt)) || !safe_read((void const*)vmt, slots, sizeof(slots))) {
        return false;
    }
    for (int slot : {14, 18}) {
        unsigned char code[3] = {};
        if (!safe_read((void const*)slots[slot], code, sizeof(code)) || code[0] != 0x31 || code[1] != 0xc0
            || code[2] != 0xc3) {
            return false;
        }
    }
    return true;
}

void* physics_scene_raw(bool client) {
    auto* level = current_level(client);
    bg3se::phx::PhysicsSceneBase* scene = nullptr;
    if (level == nullptr || !safe_read(&level->PhysicsScene, &scene, sizeof(scene)) || scene == nullptr) {
        return nullptr;
    }
    return scene;
}

bg3se::AiGrid* ai_grid(bool client) {
    auto* level = current_level(client);
    bg3se::AiGrid* grid = nullptr;
    if (level == nullptr || !safe_read(&level->AiGrid, &grid, sizeof(grid)) || !has_vtable(grid)) return nullptr;
    return grid;
}

bg3se::AiWorldPos to_world_pos(glm::vec3 pos) {
    if (pos.x > 9999999.0f || pos.x < -9999999.0f || pos.z > 9999999.0f || pos.z < -9999999.0f) {
        return bg3se::AiWorldPos{.Xglobal = 0x7fff, .Zglobal = 0x7fff, .Xlocal = .0f, .Zlocal = .0f,
                                 .Y = 3.40282347e+38f};
    }
    const float xG = floorf(pos.x + pos.x), zG = floorf(pos.z + pos.z);
    return bg3se::AiWorldPos{.Xglobal = (int)xG, .Zglobal = (int)zG, .Xlocal = pos.x - (xG * 0.5f),
                             .Zlocal = pos.z - (zG * 0.5f), .Y = pos.y};
}

bool world_to_tile(bg3se::AiSubgrid const& sg, bg3se::AiWorldPos const& pos, glm::ivec2& local) {
    int gX, gZ;
    if (fabs(pos.Xlocal) < 0.00000011920929f && fabs(pos.Zlocal) < 0.00000011920929f) {
        gX = pos.Xglobal - sg.WorldPos.Xglobal;
        gZ = pos.Zglobal - sg.WorldPos.Zglobal;
    } else {
        auto x = (int)::floor(((pos.Xglobal * sg.CellSize + pos.Xlocal) - sg.WorldPos.Xlocal) / sg.CellSize);
        auto z = (int)::floor(((pos.Zglobal * sg.CellSize + pos.Zlocal) - sg.WorldPos.Zlocal) / sg.CellSize);
        gX = x - sg.WorldPos.Xglobal;
        gZ = z - sg.WorldPos.Zglobal;
    }
    if (gX < 0 || gZ < 0 || gX >= sg.SizeX || gZ >= sg.SizeY) return false;
    local = glm::ivec2(gX, gZ);
    return true;
}

bg3se::AiGridTile const* tile_at(bg3se::AiSubgrid const& sg, glm::ivec2 local) {
    auto const* data = sg.TileGrid;
    if (data == nullptr || local.x >= data->Width || local.y >= data->Height) return nullptr;
    return &data->Tiles[local.x + local.y * data->Width];
}

template <class F>
void each_tile(bg3se::AiGrid const& grid, bg3se::AiWorldPos const& pos, F&& f) {
    const std::uint64_t key =
        (std::uint32_t)(std::int32_t)::floor(pos.Xglobal / bg3se::AiGrid::PatchSize)
        | ((std::uint64_t)(std::uint32_t)(std::int32_t)::floor(pos.Zglobal / bg3se::AiGrid::PatchSize) << 32);
    auto patch = grid.SubgridsAtPatch.find(key);
    if (patch == grid.SubgridsAtPatch.end()) return;
    for (auto id : patch.Value()) {
        auto* sg = grid.Subgrids.get_or_default(id);
        glm::ivec2 local;
        if (sg == nullptr || !world_to_tile(*sg, pos, local)) continue;
        auto const* tile = tile_at(*sg, local);
        if (tile != nullptr && !tile->Flags.IsBlocker()) f(id, *sg, local, *tile);
    }
}

// ---- files in the mod's IO folder ----

std::string io_dir() {
    char const* home = std::getenv("HOME");
    std::string dir = home != nullptr ? home : "";
    return dir + "/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3/";
}

bool read_file(std::string const& name, std::vector<unsigned char>& out, std::size_t cap) {
    const std::string path = io_dir() + name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (std::size_t)st.st_size <= cap;
    if (ok) {
        out.resize((std::size_t)st.st_size);
        std::size_t got = 0;
        while (got < out.size()) {
            const ssize_t r = ::read(fd, out.data() + got, out.size() - got);
            if (r <= 0) break;
            got += (std::size_t)r;
        }
        ok = got == out.size();
    }
    ::close(fd);
    return ok;
}

bool write_file(std::string const& name, std::string const& text) {
    const std::string path = io_dir() + name, tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = ::write(fd, text.data(), text.size()) == (ssize_t)text.size();
    ::close(fd);
    return ok && ::rename(tmp.c_str(), path.c_str()) == 0;
}

double mono_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---- results for Lua ----

struct Out {
    std::int32_t Err;        // 0 ok, else a code; Why holds the text
    std::int32_t R[4];
    float F[16];
    std::uint32_t Calls;
    char Why[220];
};
static_assert(sizeof(Out) == 4 + 16 + 64 + 4 + 220);
Out g_out[2];  // [0] server, [1] client

void out_reset(Out& o) {
    o.Err = 0;
    for (auto& r : o.R) r = 0;
    for (auto& f : o.F) f = 0;
    o.Why[0] = 0;
    ++o.Calls;
}
bool fail(Out& o, int code, char const* why) {
    o.Err = code;
    std::snprintf(o.Why, sizeof(o.Why), "%s", why);
    return false;
}

// Every vtable copy we ever installed, with the engine vtable it copied. Never freed: a
// call may still be running through one, and an engine object may outlive our tracking
// of it (the client's physics scene survives a level change), in which case its
// vtable pointer still names our copy and the engine's own is found here.
struct RetiredTable {
    std::unique_ptr<std::uint64_t[]> Table;
    void const* OldVmt;
};
std::vector<RetiredTable> g_retired_tables;
void retire(std::unique_ptr<std::uint64_t[]>& table, void const* oldVmt) {
    if (table) g_retired_tables.push_back({std::move(table), oldVmt});
}
// The engine's vtable behind `vmt`: itself, or the one an earlier copy of ours was made from.
void const* engine_vmt(void const* vmt) {
    for (auto const& t : g_retired_tables) {
        if (vmt == t.Table.get() + 2) return t.OldVmt;
    }
    return vmt;
}

}  // namespace

// ==== Part A: arena tiles (patch 0003 part A) ====

namespace {

constexpr std::uint64_t kArenaBlockBits = 0x2 | 0x4 | 0x4000 | 0x8000;  // WalkBlock, WalkBlock2, StaticBlock, GenericBlock
int g_arena_max_side = 80;                                             // setting max_side (tiles, 0.5 m)

// A void stamp also owns the ground and cloud surface bytes (bits 24..39: the out-of-bounds layer
// is Chasm, surface 25, which makes BG3 drop characters and refuse paths) and the extra flags
// Chasm and PaintedGroundSurface (AiExtraFlags start at bit 46). Lighting and material stay.
constexpr std::uint64_t kVoidOwnedBits = kArenaBlockBits | (0xffffull << 24) | (0x100ull << 46) | (0x4000ull << 46);

struct ArenaTile {
    bg3se::AiGridTile* Tile;
    std::uint64_t Mask;           // the bits this stamp owns on the tile
    std::uint64_t OldBits, NewBits;  // within Mask
    std::uint16_t OldMin, OldMax, NewMin, NewMax;
    std::uint16_t OldSurface, NewSurface;  // SurfaceMetaDataIndex (cleared in a void stamp)
};
// Bits a stamp owns: the static blocks, plus GenericBlock-only "out of bounds" tiles in a void
// stamp (the same bits; listed for clarity).
float g_anchor_tolerance = 4.0f;
float g_layer_y = 0.0f;       // void stamp: only subgrids whose Translate.y is this (within 0.5 m)
bool g_layer_only = false;

struct ArenaStamp {
    bg3se::AiGrid* Grid = nullptr;
    std::vector<std::pair<bg3se::AiGridTileData*, bg3se::AiGridTile*>> Data;
    std::vector<ArenaTile> Tiles;
};

ArenaStamp g_arena[2];

bg3se::AiGridTile* arena_tile(bg3se::AiGrid& grid, float x, float z, float anchorY, bg3se::AiSubgrid const*& subgrid,
                              bg3se::AiGridTileData*& data) {
    bg3se::AiGridTile* best = nullptr;
    float diff = 3.40282347e+38f;
    each_tile(grid, to_world_pos({x, anchorY, z}),
              [&](bg3se::AiSubgridId, bg3se::AiSubgrid const& sg, glm::ivec2, bg3se::AiGridTile const& tile) {
                  if (g_layer_only && fabs(sg.Translate.y - g_layer_y) > 0.5f) return;
                  const float d = fabs(anchorY - (sg.Translate.y + tile.GetLocalMinHeight()));
                  // A named layer has one tile per column: no tolerance, so the void arena can be
                  // stamped far above that layer (world mode lifts it clear of the level's sight).
                  if (d < diff && (g_layer_only || d <= g_anchor_tolerance)) {
                      diff = d;
                      best = const_cast<bg3se::AiGridTile*>(&tile);
                      subgrid = &sg;
                      data = sg.TileGrid;
                  }
              });
    return best;
}

std::uint16_t arena_height(float local, bool& clamped) {
    const float h = roundf(local * 50.0f);
    if (h < 0.0f || h > 65535.0f) {
        clamped = true;
        return 0;
    }
    return (std::uint16_t)h;
}

void arena_restore(bool client, int* counts) {
    auto& st = g_arena[client ? 1 : 0];
    counts[0] = counts[1] = counts[2] = 0;
    if (st.Tiles.empty()) {
        st = ArenaStamp{};
        return;
    }
    bool alive = st.Grid != nullptr && ai_grid(client) == st.Grid;
    for (auto const& [data, tiles] : st.Data) {
        bg3se::AiGridTile* now = nullptr;
        if (!alive || !safe_read(&data->Tiles, &now, sizeof(now)) || now != tiles) alive = false;
    }
    if (!alive) {
        counts[2] = (int)st.Tiles.size();
        logf("arena: %s grid changed, %d stamped tiles dropped", client ? "client" : "server", counts[2]);
        st = ArenaStamp{};
        return;
    }
    for (auto it = st.Tiles.rbegin(); it != st.Tiles.rend(); ++it) {
        auto& t = *it->Tile;
        if ((t.Flags.Flags & it->Mask) != it->NewBits || t.MinHeight != it->NewMin
            || t.MaxHeight != it->NewMax || t.SurfaceMetaDataIndex != it->NewSurface) {
            ++counts[1];
            // A void tile is out of bounds and only ever ours: what the fight left on it (a fire
            // surface, say) goes, and the chasm comes back, so nobody can stand there afterwards.
            if (it->Mask != kVoidOwnedBits) continue;
        }
        t.Flags.Flags = (t.Flags.Flags & ~it->Mask) | it->OldBits;
        t.MinHeight = it->OldMin;
        t.MaxHeight = it->OldMax;
        t.SurfaceMetaDataIndex = it->OldSurface;
        ++counts[0];
    }
    logf("arena: %s restored %d, changed %d", client ? "client" : "server", counts[0], counts[1]);
    st = ArenaStamp{};
}

bool arena_stamp(bool client, float minX, float minZ, float anchorY, int n, unsigned char const* walk,
                 float const* floors, float tolerance, bool voidStamp, float layerY, int* counts, char const** why) {
    int old[3];
    arena_restore(client, old);
    counts[0] = counts[1] = counts[2] = 0;
    if (n < 1 || n > g_arena_max_side) {
        *why = "arena side out of range";
        return false;
    }
    auto* grid = ai_grid(client);
    if (grid == nullptr) {
        *why = "no AI grid";
        return false;
    }
    // Void stamp: the tiles may lie far below the WoW floor (an out-of-bounds layer), so the
    // layer is chosen with a wider tolerance and its surface link is cleared while stamped.
    g_anchor_tolerance = std::clamp(tolerance > 0.0f ? tolerance : 4.0f, 0.5f, 60.0f);
    // A void stamp names its layer (the out-of-bounds subgrids' Translate.y): other layers in the
    // same columns are left alone, and must lie well away from the WoW floor (see collision-hooks).
    g_layer_only = voidStamp && layerY != 0.0f;
    g_layer_y = layerY;
    auto& st = g_arena[client ? 1 : 0];
    st.Grid = grid;
    st.Tiles.reserve((std::size_t)n * n);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            const int i = r * n + c;
            const float x = minX + (c + 0.5f) * 0.5f, z = minZ + (r + 0.5f) * 0.5f;
            bg3se::AiSubgrid const* sg = nullptr;
            bg3se::AiGridTileData* data = nullptr;
            auto* tile = arena_tile(*grid, x, z, anchorY, sg, data);
            if (tile == nullptr || data == nullptr) {
                ++counts[1];
                continue;
            }
            bool clamped = false;
            std::uint16_t span =
                tile->MaxHeight >= tile->MinHeight ? (std::uint16_t)(tile->MaxHeight - tile->MinHeight) : 0;
            if (voidStamp) span = 0;  // the out-of-bounds layer's 3 m slab means nothing here
            const std::uint16_t lo = arena_height(floors[i] - sg->Translate.y, clamped);
            const std::uint16_t hi = arena_height(floors[i] - sg->Translate.y + span / 50.0f, clamped);
            if (clamped) {
                ++counts[2];
                continue;
            }
            bool seen = false;
            for (auto const& d : st.Data) seen = seen || d.first == data;
            if (!seen) st.Data.emplace_back(data, data->Tiles);
            const std::uint16_t surface = voidStamp ? (std::uint16_t)0xffff : tile->SurfaceMetaDataIndex;
            const std::uint64_t mask = voidStamp ? kVoidOwnedBits : kArenaBlockBits;
            ArenaTile saved{tile, mask, tile->Flags.Flags & mask, walk[i] ? 0 : kArenaBlockBits,
                            tile->MinHeight, tile->MaxHeight, lo, hi, tile->SurfaceMetaDataIndex, surface};
            tile->Flags.Flags = (tile->Flags.Flags & ~mask) | saved.NewBits;
            tile->MinHeight = lo;
            tile->MaxHeight = hi;
            tile->SurfaceMetaDataIndex = surface;
            st.Tiles.push_back(saved);
            ++counts[0];
        }
    }
    logf("arena: %s stamped %d, no tile %d, out of range %d", client ? "client" : "server", counts[0], counts[1],
         counts[2]);
    if (counts[2] > 0) {
        int back[3];
        arena_restore(client, back);
        *why = "a floor height is outside the subgrid's range";
        return false;
    }
    return true;
}

}  // namespace

// ==== Part D: the treadmill (world mode: ground follows Tav anywhere) ====
//
// A pool of whole 128 x 128 subgrids of the out-of-bounds layer (64 m squares of plain
// GenericBlock tiles, nothing standing on them) is moved to a 64 m lattice around Tav and stamped
// with WoW ground; as she walks on, squares behind her are moved ahead. Ids, side tables and
// allocator ownership stay the engine's: only a subgrid's position, its tiles and its patch
// registration (AiGrid.SubgridsAtPatch, AiSubgrid.field_B0) change, the way the engine itself moves
// platform subgrids. Patch arrays change so that a concurrent reader sees the old or the new ids,
// never freed memory: a removal swaps the last id in, growth copies into a new buffer and leaks the
// old one, new map nodes are linked complete. PoolRestore puts every subgrid back from its snapshot.
// A subgrid with anything registered on its tiles (a character, an item) is never moved.

namespace {

template <class T>
struct RawArray {  // bg3se::Array<T>: buffer, capacity, size
    T* Buf;
    std::uint32_t Capacity;
    std::uint32_t Size;
};
struct RawPatchNode {  // bg3se::MapNode<uint64_t, Array<AiSubgridId>>
    RawPatchNode* Next;
    std::uint64_t Key;
    RawArray<bg3se::AiSubgridId> Ids;
};
struct RawPatchMap {  // bg3se::RefMapInternals: item count, hash size, buckets
    std::uint32_t ItemCount;
    std::uint32_t HashSize;
    RawPatchNode** HashTable;
};
static_assert(sizeof(RawArray<bg3se::AiSubgridId>) == sizeof(bg3se::Array<bg3se::AiSubgridId>));
static_assert(sizeof(RawArray<glm::ivec2>) == sizeof(bg3se::Array<glm::ivec2>));
static_assert(sizeof(RawPatchMap) == sizeof(bg3se::LegacyRefMap<std::uint64_t, bg3se::Array<bg3se::AiSubgridId>>));
static_assert(sizeof(RawPatchNode) == sizeof(bg3se::MapNode<std::uint64_t, bg3se::Array<bg3se::AiSubgridId>>));

constexpr int kCellTiles = 128;  // a pool subgrid's side in tiles (64 m)
// 12 at first, 48 in 0.2.8; 0.2.9 (treadmill per party member): BGO_Main_A has 48 + 36
// qualifying subgrids on two layers (a survey of the acts' levels), and Pool takes several layers at once.
constexpr int kPoolMax = 128;
constexpr int kPoolLayersMax = 16;
constexpr std::uint64_t kEdgeBit = 0x200ull << 46, kIntersectionBit = 0x800ull << 46, kLedgeBits = 0xf8ull << 46;
// A pool tile is all ours: blocks, the chasm surface, Blocker (a hole in the layer), Slope and
// SlopeBlock, ledges and seams.
constexpr std::uint64_t kSlopeBit = 0x800, kSlopeBlockBit = 0x1000;
constexpr std::uint64_t kPoolOwnedBits =
    kVoidOwnedBits | 0x1ull | kSlopeBit | kSlopeBlockBit | kLedgeBits | kEdgeBit | kIntersectionBit;

struct PoolSub {
    bg3se::AiSubgridId Id = 0;
    bg3se::AiSubgrid* Sg = nullptr;
    bg3se::AiGridTile* TilesAt = nullptr;   // the tile array at snapshot time (alive check)
    glm::vec3 Translate{};
    bg3se::AiWorldPos WorldPos{};
    std::vector<bg3se::AiGridTile> Tiles;   // snapshot
    std::vector<glm::ivec2> Patches;        // field_B0 at snapshot: the patches it was registered in
    int Blockers = 0;
    bool Moved = false;
    int Xg = 0, Zg = 0;                     // placed: min corner in tiles (0.5 m)
    float BaseY = 0;
    std::vector<glm::ivec2> Live;           // placed: the patches it is registered in now
};

struct PoolState {
    bg3se::AiGrid* Grid = nullptr;
    std::vector<PoolSub> Subs;
    bool BoundsOk = false;  // the grid's bounds were found where kBoundsOffset says (checked at Pool)
};
PoolState g_pool[2];
// The level's own AI grid box in metres (min x, min z, max x, max z) as Pool found it (0.2.9): the ray
// proxy (scene_proxy) looks for a static BG3 shape there too, not only under the squares and on the
// tutorial's chasm plane (BGO_Main_A has neither).
glm::vec4 g_level_box[2] = {};
bool g_level_box_set[2] = {};
// BG3's client-side copies of where each AI subgrid is (its AI grid visual and box, the sight
// overlay's record) follow a moved pool subgrid (defined with op 31 below).
void pool_sight_follow(PoolState& p);
// The pool subgrids' sight and surface textures again from their live tiles (op 50, below).
void pool_textures_follow(PoolState& p, int only);

// The grid's bounds (bg3se lists them only as comments: WorldBoundMin/Max, AiWorldMin), found at
// AiGrid+0x260: min and max corner over all subgrids in tiles (x, z; the max is
// the far edge), then the min corner in metres with the lowest Translate.y. BG3's cursor did not
// pick Tav on squares outside them. They are checked against the subgrids before any write.
constexpr std::size_t kBoundsOffset = 0x260;
struct GridBounds {
    glm::ivec2 Min, Max;
    glm::vec3 WorldMin;
};
static_assert(sizeof(GridBounds) == 28);

GridBounds* grid_bounds(bg3se::AiGrid* g) {
    return reinterpret_cast<GridBounds*>(reinterpret_cast<char*>(g) + kBoundsOffset);
}

GridBounds computed_bounds(bg3se::AiGrid* g) {
    GridBounds b{{INT32_MAX, INT32_MAX}, {INT32_MIN, INT32_MIN}, {3.4e38f, 3.4e38f, 3.4e38f}};
    for (auto it = g->Subgrids.begin(); it != g->Subgrids.end(); ++it) {
        bg3se::AiSubgrid const* sg = it.Value();
        if (sg == nullptr) continue;
        const int x0 = sg->WorldPos.Xglobal, z0 = sg->WorldPos.Zglobal;
        b.Min = glm::min(b.Min, glm::ivec2{x0, z0});
        b.Max = glm::max(b.Max, glm::ivec2{x0 + sg->SizeX, z0 + sg->SizeY});
        b.WorldMin = glm::min(b.WorldMin, glm::vec3{x0 * 0.5f, sg->Translate.y, z0 * 0.5f});
    }
    return b;
}

bool bounds_equal(GridBounds const& a, GridBounds const& b) {
    return a.Min == b.Min && a.Max == b.Max && fabsf(a.WorldMin.x - b.WorldMin.x) < 0.01f
        && fabsf(a.WorldMin.y - b.WorldMin.y) < 0.01f && fabsf(a.WorldMin.z - b.WorldMin.z) < 0.01f;
}

// After squares move: the bounds cover every subgrid where it now lies (only when Pool found them).
void pool_bounds(PoolState& p) {
    if (!p.BoundsOk) return;
    const GridBounds b = computed_bounds(p.Grid);
    GridBounds* at = grid_bounds(p.Grid);
    at->Min = b.Min;
    at->Max = b.Max;
    at->WorldMin = b.WorldMin;
}

RawPatchMap* patch_map(bg3se::AiGrid* grid) { return reinterpret_cast<RawPatchMap*>(&grid->SubgridsAtPatch); }

std::uint64_t patch_key(glm::ivec2 p) { return (std::uint32_t)p.x | ((std::uint64_t)(std::uint32_t)p.y << 32); }

int floor_div(int a, int b) { return (int)floorf((float)a / (float)b); }

std::vector<glm::ivec2> patches_of(int xg, int zg, int sx, int sz) {
    std::vector<glm::ivec2> out;
    const int p = (int)bg3se::AiGrid::PatchSize;
    for (int pz = floor_div(zg, p); pz <= floor_div(zg + sz - 1, p); ++pz) {
        for (int px = floor_div(xg, p); px <= floor_div(xg + sx - 1, p); ++px) out.push_back({px, pz});
    }
    return out;
}

RawPatchNode* patch_find(RawPatchMap* m, std::uint64_t key) {
    if (m->HashTable == nullptr || m->HashSize == 0) return nullptr;
    for (RawPatchNode* n = m->HashTable[key % m->HashSize]; n != nullptr; n = n->Next) {
        if (n->Key == key) return n;
    }
    return nullptr;
}

bool patch_has(RawPatchNode const* n, bg3se::AiSubgridId id) {
    for (std::uint32_t i = 0; n != nullptr && i < n->Ids.Size; ++i) {
        if (n->Ids.Buf[i] == id) return true;
    }
    return false;
}

void patch_remove(RawPatchMap* m, glm::ivec2 p, bg3se::AiSubgridId id) {
    RawPatchNode* n = patch_find(m, patch_key(p));
    if (n == nullptr) return;
    auto& a = n->Ids;
    for (std::uint32_t i = 0; i < a.Size; ++i) {
        if (a.Buf[i] != id) continue;
        const std::uint32_t last = a.Size - 1;
        __atomic_store_n(&a.Buf[i], a.Buf[last], __ATOMIC_RELEASE);
        __atomic_store_n(&a.Size, last, __ATOMIC_RELEASE);
        return;
    }
}

bool patch_add(RawPatchMap* m, glm::ivec2 p, bg3se::AiSubgridId id) {
    if (m->HashTable == nullptr || m->HashSize == 0 || g_api.Alloc == nullptr || !allocator_ready()) return false;
    const std::uint64_t key = patch_key(p);
    if (RawPatchNode* n = patch_find(m, key)) {
        auto& a = n->Ids;
        if (patch_has(n, id)) return true;
        if (a.Size < a.Capacity) {
            __atomic_store_n(&a.Buf[a.Size], id, __ATOMIC_RELEASE);
            __atomic_store_n(&a.Size, a.Size + 1, __ATOMIC_RELEASE);
            return true;
        }
        const std::uint32_t cap = std::max<std::uint32_t>(8, a.Capacity * 2);
        auto* buf = static_cast<bg3se::AiSubgridId*>(g_api.Alloc(cap * sizeof(bg3se::AiSubgridId)));
        if (buf == nullptr) return false;
        if (a.Size > 0) std::memcpy(buf, a.Buf, a.Size * sizeof(bg3se::AiSubgridId));
        buf[a.Size] = id;
        __atomic_store_n(&a.Buf, buf, __ATOMIC_RELEASE);  // the old buffer is leaked: a reader may hold it
        __atomic_store_n(&a.Capacity, cap, __ATOMIC_RELEASE);
        __atomic_store_n(&a.Size, a.Size + 1, __ATOMIC_RELEASE);
        return true;
    }
    auto* node = static_cast<RawPatchNode*>(g_api.Alloc(sizeof(RawPatchNode)));
    auto* buf = static_cast<bg3se::AiSubgridId*>(g_api.Alloc(8 * sizeof(bg3se::AiSubgridId)));
    if (node == nullptr || buf == nullptr) return false;
    node->Next = nullptr;
    node->Key = key;
    buf[0] = id;
    node->Ids = RawArray<bg3se::AiSubgridId>{buf, 8, 1};
    RawPatchNode** slot = &m->HashTable[key % m->HashSize];
    while (*slot != nullptr) slot = &(*slot)->Next;
    __atomic_store_n(slot, node, __ATOMIC_RELEASE);
    m->ItemCount++;
    return true;
}

// The engine's own lookup (bg3se's find, its hashing) sees the subgrid at that patch.
bool patch_check(bg3se::AiGrid* grid, glm::ivec2 p, bg3se::AiSubgridId id) {
    auto it = grid->SubgridsAtPatch.find(patch_key(p));
    if (it == grid->SubgridsAtPatch.end()) return false;
    for (auto v : it.Value()) {
        if (v == id) return true;
    }
    return false;
}

bool set_patch_list(bg3se::AiSubgrid* sg, std::vector<glm::ivec2> const& list) {
    auto* a = reinterpret_cast<RawArray<glm::ivec2>*>(&sg->field_B0);
    if (list.size() > a->Capacity) {
        auto* buf = static_cast<glm::ivec2*>(g_api.Alloc(list.size() * sizeof(glm::ivec2)));
        if (buf == nullptr) return false;
        std::memcpy(buf, list.data(), list.size() * sizeof(glm::ivec2));
        __atomic_store_n(&a->Buf, buf, __ATOMIC_RELEASE);  // old buffer leaked
        a->Capacity = (std::uint32_t)list.size();
    } else if (!list.empty()) {
        std::memcpy(a->Buf, list.data(), list.size() * sizeof(glm::ivec2));
    }
    __atomic_store_n(&a->Size, (std::uint32_t)list.size(), __ATOMIC_RELEASE);
    return true;
}

bool pool_alive(bool client) {
    auto& p = g_pool[client ? 1 : 0];
    if (p.Grid == nullptr || ai_grid(client) != p.Grid) return false;
    for (auto const& s : p.Subs) {
        if (p.Grid->Subgrids.get_or_default(s.Id) != s.Sg) return false;
        bg3se::AiGridTileData* td = nullptr;
        bg3se::AiGridTile* now = nullptr;
        if (!safe_read(&s.Sg->TileGrid, &td, sizeof(td)) || td == nullptr || !safe_read(&td->Tiles, &now, sizeof(now))
            || now != s.TilesAt) {
            return false;
        }
    }
    return true;
}

bool tiles_free(bg3se::AiSubgrid const* sg) {
    auto const* td = sg->TileGrid;
    for (int k = 0; k < td->Width * td->Height; ++k) {
        if (td->Tiles[k].MetaDataIndex != 0xffff) return false;
    }
    return true;
}

// What stands on a pool subgrid: the entities registered on its tiles (the AI grid's
// metadata a tile names: AiGrid::MetaData[MetaDataIndex]->Entities) and the objects attached to it.
// Place refuses a subgrid with either; WorldMode asks both sides first (op 61) so a square moves on both
// sides or on neither.
struct Occupants {
    int Tiles = 0, Attached = 0;
    std::vector<std::uint64_t> Entities;  // distinct, at most 8 (registered on tiles, then attached)
    int FirstX = -1, FirstY = -1;         // the first tile holding something
};
Occupants pool_occupants(bg3se::AiGrid const* grid, bg3se::AiSubgrid const* sg) {
    Occupants o;
    auto const* td = sg->TileGrid;
    auto note = [&](std::uint64_t h) {
        if (o.Entities.size() < 8 && std::find(o.Entities.begin(), o.Entities.end(), h) == o.Entities.end()) {
            o.Entities.push_back(h);
        }
    };
    for (int k = 0; k < td->Width * td->Height; ++k) {
        const auto idx = td->Tiles[k].MetaDataIndex;
        if (idx == 0xffff) continue;
        if (o.Tiles++ == 0) {
            o.FirstX = k % td->Width;
            o.FirstY = k / td->Width;
        }
        if (idx < grid->MetaData.size() && grid->MetaData[idx] != nullptr) {
            for (auto const& h : grid->MetaData[idx]->Entities) note(h.Handle);
        }
    }
    o.Attached = (int)sg->AttachedObjectRefCounts.size();
    for (auto const& h : sg->AttachedObjectRefCounts.keys()) note(h.Handle);
    return o;
}
std::string occupants_json(int k, PoolSub const& s, Occupants const& o) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "{\"K\":%d,\"Id\":%u,\"Moved\":%s,\"Tiles\":%d,\"Attached\":%d,\"First\":[%d,%d],\"Entities\":[", k,
                  (unsigned)s.Id, s.Moved ? "true" : "false", o.Tiles, o.Attached, o.FirstX, o.FirstY);
    std::string j = buf;
    for (std::size_t i = 0; i < o.Entities.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s\"%llu\"", i ? "," : "", (unsigned long long)o.Entities[i]);
        j += buf;
    }
    return j + "]}";
}

// Seam flags: every perimeter tile is an edge; a side that meets another placed pool subgrid
// (same lattice, touching) is an intersection, as the engine marks seams between subgrids.
void pool_seams(PoolState& p) {
    for (auto& a : p.Subs) {
        if (!a.Moved) continue;
        bool side[4] = {};  // -x, +x, -z, +z
        for (auto const& b : p.Subs) {
            if (!b.Moved || &b == &a) continue;
            if (b.Zg == a.Zg && b.Xg == a.Xg - kCellTiles) side[0] = true;
            if (b.Zg == a.Zg && b.Xg == a.Xg + kCellTiles) side[1] = true;
            if (b.Xg == a.Xg && b.Zg == a.Zg - kCellTiles) side[2] = true;
            if (b.Xg == a.Xg && b.Zg == a.Zg + kCellTiles) side[3] = true;
        }
        auto* tiles = a.Sg->TileGrid->Tiles;
        for (int r = 0; r < kCellTiles; ++r) {
            for (int c = 0; c < kCellTiles; ++c) {
                const bool w = c == 0, e = c == kCellTiles - 1, s = r == 0, n = r == kCellTiles - 1;
                if (!(w || e || s || n)) continue;
                auto& t = tiles[c + r * kCellTiles];
                std::uint64_t f = (t.Flags.Flags & ~kIntersectionBit) | kEdgeBit;
                if ((w && side[0]) || (e && side[1]) || (s && side[2]) || (n && side[3])) f |= kIntersectionBit;
                t.Flags.Flags = f;
            }
        }
    }
}

std::string pool_json(PoolState const& p) {
    std::string j = "[";
    char buf[256];
    for (std::size_t i = 0; i < p.Subs.size(); ++i) {
        auto const& s = p.Subs[i];
        std::snprintf(buf, sizeof(buf), "%s{\"K\":%zu,\"Id\":%u,\"Home\":[%.3f,%.3f,%.3f],\"Blockers\":%d,\"Moved\":%s,"
                      "\"At\":[%.1f,%.3f,%.1f],\"Patches\":%zu}", i ? "," : "", i, (unsigned)s.Id, s.Translate.x,
                      s.Translate.y, s.Translate.z, s.Blockers, s.Moved ? "true" : "false", s.Xg * 0.5f, s.BaseY,
                      s.Zg * 0.5f, s.Moved ? s.Live.size() : s.Patches.size());
        j += buf;
    }
    return j + "]";
}

// Pool: snapshot up to `want` whole subgrids of the layers at `layers` (fewest holes first). With
// `ids` (pool-ids.txt: the ids both sides qualify, chosen by WorldMode.lua so server and client
// move the same subgrids) exactly those, in that order.
// Several layers (0.2.9): the layer only says where to look for plain out-of-bounds
// subgrids. Nothing a moved subgrid does depends on it: Place sets its height (Translate.y = the
// square's base), clears every tile bit the pool owns (blocks, the chasm surface link, Blocker,
// slopes, ledges, seams), takes the subgrid out of every patch and back in where it lies, and the
// sight and surface textures are rebuilt per subgrid from its live tiles (ops 31 and 50). Its home
// tiles (the level's chasm surface object lists them) come back from the snapshot at PoolRestore.
// The void stamp's layer rule (one tile per column, g_layer_only) is the old arena's, not the pool's.
bool pool_init(bool client, std::vector<float> const& layers, int want, std::vector<bg3se::AiSubgridId> const* ids,
               int* counts, char const** why) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) {
        *why = "no AI grid";
        return false;
    }
    auto& p = g_pool[client ? 1 : 0];
    if (!p.Subs.empty() && pool_alive(client)) {
        bool moved = false;
        for (auto const& s : p.Subs) moved = moved || s.Moved;
        if (moved) {
            *why = "the pool is in use (restore it first)";
            return false;
        }
    }
    p = PoolState{};
    std::vector<PoolSub> found;
    auto* m = patch_map(grid);
    for (auto it = grid->Subgrids.begin(); it != grid->Subgrids.end(); ++it) {
        bg3se::AiSubgrid* sg = it.Value();
        bool inLayer = false;
        for (float y : layers) inLayer = inLayer || (sg != nullptr && fabsf(sg->Translate.y - y) <= 0.5f);
        if (sg == nullptr || !inLayer || sg->SizeX != kCellTiles || sg->SizeY != kCellTiles
            || sg->CellSize != 0.5f || sg->LoadedExternally || sg->PatchesDirty) {
            continue;
        }
        auto* td = sg->TileGrid;
        if (td == nullptr || td->Tiles == nullptr || td->Width != kCellTiles || td->Height != kCellTiles) continue;
        if (sg->AttachedObjectRefCounts.size() != 0) continue;
        if (sg->WorldPos.Xglobal != (int)lroundf(sg->Translate.x * 2) || sg->WorldPos.Zglobal != (int)lroundf(sg->Translate.z * 2)
            || sg->WorldPos.Xlocal != 0.0f || sg->WorldPos.Zlocal != 0.0f) {
            continue;
        }
        // Plain out-of-bounds ground only: every open tile blocked (GenericBlock) within the layer's
        // 3 m slab, and nothing registered on any tile.
        bool plain = true;
        int blockers = 0;
        for (int k = 0; k < kCellTiles * kCellTiles && plain; ++k) {
            auto const& t = td->Tiles[k];
            if (t.MetaDataIndex != 0xffff) plain = false;
            if (t.Flags.IsBlocker()) {
                ++blockers;
                continue;
            }
            if ((t.Flags.Flags & 0x8000) == 0 || t.MaxHeight > 200) plain = false;
        }
        if (!plain) continue;
        // Registered exactly where its position says (checked before we ever change it).
        const auto expect = patches_of(sg->WorldPos.Xglobal, sg->WorldPos.Zglobal, kCellTiles, kCellTiles);
        auto const* b0 = reinterpret_cast<RawArray<glm::ivec2> const*>(&sg->field_B0);
        if (b0->Size != expect.size()) continue;
        bool registered = true;
        for (auto q : expect) registered = registered && patch_has(patch_find(m, patch_key(q)), it.Key());
        if (!registered) continue;
        PoolSub s;
        s.Id = it.Key();
        s.Sg = sg;
        s.TilesAt = td->Tiles;
        s.Translate = sg->Translate;
        s.WorldPos = sg->WorldPos;
        s.Tiles.assign(td->Tiles, td->Tiles + kCellTiles * kCellTiles);
        s.Patches.assign(b0->Buf, b0->Buf + b0->Size);
        s.Blockers = blockers;
        found.push_back(std::move(s));
    }
    std::sort(found.begin(), found.end(), [](PoolSub const& a, PoolSub const& b) {
        return a.Blockers != b.Blockers ? a.Blockers < b.Blockers : a.Translate.x < b.Translate.x;
    });
    if (ids != nullptr) {
        std::vector<PoolSub> chosen;
        for (auto id : *ids) {
            auto it = std::find_if(found.begin(), found.end(), [id](PoolSub const& s) { return s.Id == id; });
            if (it == found.end()) {
                *why = "a chosen pool subgrid does not qualify on this side";
                return false;
            }
            chosen.push_back(std::move(*it));
            found.erase(it);
        }
        found = std::move(chosen);
    }
    if ((int)found.size() > std::clamp(want, 1, kPoolMax)) found.resize(std::clamp(want, 1, kPoolMax));
    if (found.empty()) {
        *why = "no whole, plain subgrid in that layer";
        return false;
    }
    p.Grid = grid;
    p.Subs = std::move(found);
    {
        GridBounds now{};
        p.BoundsOk = safe_read(grid_bounds(grid), &now, sizeof(now)) && bounds_equal(now, computed_bounds(grid));
        if (p.BoundsOk) {
            g_level_box[client ? 1 : 0] = {now.Min.x * 0.5f, now.Min.y * 0.5f, now.Max.x * 0.5f, now.Max.y * 0.5f};
            g_level_box_set[client ? 1 : 0] = true;
        }
        if (!p.BoundsOk) {
            logf("pool (%s): grid bounds not where expected (%d %d %d %d); left alone", client ? "client" : "server",
                 now.Min.x, now.Min.y, now.Max.x, now.Max.y);
        }
    }
    write_file(client ? "native-client-pool.json" : "native-server-pool.json", pool_json(p));
    counts[0] = (int)p.Subs.size();
    {
        std::string names;
        char buf[32];
        for (float y : layers) {
            std::snprintf(buf, sizeof(buf), "%s%.3f", names.empty() ? "" : " ", y);
            names += buf;
        }
        logf("pool (%s): %d subgrids of layer(s) %s", client ? "client" : "server", counts[0], names.c_str());
    }
    return true;
}

// ==== Ground surfaces on the pool squares (op 60) ====
//
// BG3's ground surfaces (fire, water, grease, ice, acid, clouds ...) are objects of the server's
// esv::SurfaceManager whose cells name tiles by (x, y, subgrid id); each AI grid tile names the surface
// on it per layer (tile +0xe, through the grid's surface metadata), and a cell's surface counts down
// there. Place keeps a pool subgrid's id when it moves the subgrid, so a surface left on a square (from
// a fight 30 m back) stayed attached to the subgrid while Place rewrote its tiles at the new spot: the
// tiles forgot it, the surface kept counting, and when it ran out the engine cleared its cells wherever
// the subgrid lay by then, wiping a surface made on that ground since (harness: grease left on
// a square that moved 130 m ahead removed a water surface made on those tiles 2 minutes later, while a
// control water elsewhere lived on). BG3 has the same case for its moving platforms and handles it in the
// server's SurfaceManager update: when a platform is destroyed it collects every tile of the platform's
// subgrids that has a ground surface (bg3+0x2b98fa0, layer 0) and gives them to its cell removal
// (bg3+0x389f310), then the same for clouds (layer 1) (bg3+0x21db37d..0x21dc565). Place and PoolRestore
// do exactly that for a pool subgrid that was placed before (its tiles are WoW ground), before its tiles
// are rewritten: the surfaces' cells there go the engine's own way (their client copies follow through
// the removal's replication), cells of the same surfaces on other squares stay. A subgrid at home (the
// level's out-of-bounds layer) is never touched: its tiles carry the level's own chasm surface. Server
// only: the server owns surfaces. If this build's pieces do not check out, Place goes on without it.
constexpr std::uintptr_t kSurfCellRemove = 0x389f310;  // void (esv::SurfaceManager*, u8 layer, {first, end}*)
constexpr std::uintptr_t kSurfAtTile = 0x2b98fa0;      // u16 (AiGrid*, AiTilePos const*, u8 layer, bool): 0xffff none
constexpr std::uintptr_t kSurfMgrVmt = 0x7a7bf38, kSurfMgrVmt2 = 0x7a7bf98;  // esv::SurfaceManager, its system base
constexpr std::size_t kLevelSurfaceManager = 0xa8;     // esv::Level -> SurfaceManager (bg3se: esv::Level)
constexpr std::size_t kLevelAiGrid = 0x80;             // esv::Level -> AiGrid, as the removal reads it
constexpr std::size_t kSurfMgrLevel = 0x170;           // SurfaceManager -> esv::Level
constexpr unsigned char kSurfCellRemoveCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xec, 0x78, 0xc7, 0x87, 0xd4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x22, 0x48, 0x8b, 0x6a,
    0x08};
constexpr unsigned char kSurfAtTileCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x8a, 0x05, 0x13, 0x0b, 0x02,
    0x05, 0x89, 0xcb, 0x84, 0xc0, 0x0f, 0x84, 0xf1, 0x00, 0x00, 0x00, 0x44, 0x8b, 0x46, 0x04, 0x66, 0xb9, 0xff, 0xff};

struct SurfTilePos {  // bg3se::AiTilePos
    std::int16_t X, Y;
    std::uint32_t Subgrid;
};
static_assert(sizeof(SurfTilePos) == 8);
struct SurfCellSpan {
    SurfTilePos const* First;
    SurfTilePos const* End;
};
using SurfAtTileFn = std::uint16_t (*)(void*, SurfTilePos const*, std::uint8_t, bool);
using SurfCellRemoveFn = void (*)(void*, std::uint8_t, SurfCellSpan*);

// The server's esv::SurfaceManager for this grid when this build's pieces check out, else nullptr.
void* surface_manager(void const* grid, char const** why) {
    const std::uintptr_t bias = g_api.LoadBias != nullptr ? g_api.LoadBias() : 0;
    unsigned char c1[sizeof(kSurfCellRemoveCode)] = {}, c2[sizeof(kSurfAtTileCode)] = {};
    if (bias == 0 || !safe_read((void const*)(bias + kSurfCellRemove), c1, sizeof(c1))
        || std::memcmp(c1, kSurfCellRemoveCode, sizeof(c1)) != 0
        || !safe_read((void const*)(bias + kSurfAtTile), c2, sizeof(c2))
        || std::memcmp(c2, kSurfAtTileCode, sizeof(c2)) != 0) {
        *why = "BG3's surface cell removal is not where this build has it";
        return nullptr;
    }
    void* level = current_level(false);
    void *sm = nullptr, *back = nullptr, *lgrid = nullptr;
    std::uint64_t v1 = 0, v2 = 0;
    if (level == nullptr || !safe_read((char const*)level + kLevelSurfaceManager, &sm, sizeof(sm)) || sm == nullptr
        || !safe_read(sm, &v1, sizeof(v1)) || v1 != bias + kSurfMgrVmt
        || !safe_read((char const*)sm + 0x60, &v2, sizeof(v2)) || v2 != bias + kSurfMgrVmt2
        || !safe_read((char const*)sm + kSurfMgrLevel, &back, sizeof(back)) || back != level
        || !safe_read((char const*)level + kLevelAiGrid, &lgrid, sizeof(lgrid)) || lgrid != grid) {
        *why = "no server SurfaceManager for the pool's AI grid";
        return nullptr;
    }
    return sm;
}

// found[0], found[1]: tiles of pool subgrid s with a ground / cloud surface. remove: BG3's own cell
// removal takes them (as for a destroyed platform). Server side only; false (why) when the engine's
// pieces do not check out.
bool pool_unsurface(PoolState& p, PoolSub& s, bool remove, int found[2], char const** why) {
    found[0] = found[1] = 0;
    void* sm = surface_manager(p.Grid, why);
    if (sm == nullptr) return false;
    const std::uintptr_t bias = g_api.LoadBias();
    auto at = reinterpret_cast<SurfAtTileFn>(bias + kSurfAtTile);
    auto removeCells = reinterpret_cast<SurfCellRemoveFn>(bias + kSurfCellRemove);
    auto const* td = s.Sg->TileGrid;
    if (td == nullptr || td->Tiles == nullptr || td->Width <= 0 || td->Height <= 0) {
        *why = "pool subgrid without tiles";
        return false;
    }
    std::vector<SurfTilePos> cells;
    for (std::uint8_t layer = 0; layer < 2; ++layer) {
        cells.clear();
        for (int y = 0; y < td->Height; ++y) {
            for (int x = 0; x < td->Width; ++x) {
                // The engine's own first test (no surface metadata, no surface on any layer), then its lookup.
                if (td->Tiles[y * td->Width + x].SurfaceMetaDataIndex == 0xffff) continue;
                const SurfTilePos t{(std::int16_t)x, (std::int16_t)y, (std::uint32_t)s.Id};
                if (at(p.Grid, &t, layer, false) != 0xffff) cells.push_back(t);
            }
        }
        found[layer] = (int)cells.size();
        if (remove && !cells.empty()) {
            SurfCellSpan span{cells.data(), cells.data() + cells.size()};
            removeCells(sm, layer, &span);
        }
    }
    return true;
}

// Before Place moves or PoolRestore sends home a placed pool subgrid (server): its surfaces go.
void pool_unsurface_before_move(PoolState& p, PoolSub& s, char const* what) {
    static bool warned = false;
    int found[2] = {};
    char const* why = nullptr;
    if (!pool_unsurface(p, s, true, found, &why)) {
        if (!warned) logf("pool (server): surfaces on squares are left to the engine: %s", why != nullptr ? why : "?");
        warned = true;
        return;
    }
    if (found[0] + found[1] > 0) {
        logf("pool (server): subgrid %u: %d ground and %d cloud surface cells removed before it %s", (unsigned)s.Id,
             found[0], found[1], what);
    }
}

// Place pool subgrid k with its min corner at (minX, minZ) and base height baseY; tiles from
// section-<k>.bin, rows along +Z, 128 x 128 records of either
//   <u8 walkable, f32 floor in BG3 metres>               (5 bytes: MinHeight = MaxHeight = floor), or
//   <u8 flags, f32 min, f32 max in BG3 metres>           (9 bytes: flags 1 walkable, 2 Slope; the
//                                                        tile's height range, MinHeight and MaxHeight)
bool pool_place(bool client, int k, float minX, float minZ, float baseY, std::vector<unsigned char> const& raw,
                int* counts, char const** why) {
    auto& p = g_pool[client ? 1 : 0];
    if (p.Subs.empty() || !pool_alive(client)) {
        *why = "no pool (or the level changed)";
        return false;
    }
    if (k < 0 || k >= (int)p.Subs.size()) {
        *why = "pool index out of range";
        return false;
    }
    const std::size_t rec = raw.size() / ((std::size_t)kCellTiles * kCellTiles);
    if ((rec != 5 && rec != 9) || raw.size() != (std::size_t)kCellTiles * kCellTiles * rec) {
        *why = "section tiles must be 128 x 128 records of 5 or 9 bytes";
        return false;
    }
    if (!std::isfinite(minX) || !std::isfinite(minZ) || !std::isfinite(baseY) || fabsf(minX * 2 - roundf(minX * 2)) > 1e-3f
        || fabsf(minZ * 2 - roundf(minZ * 2)) > 1e-3f || fabsf(minX) > 20000 || fabsf(minZ) > 20000) {
        *why = "section corner must be finite and on the 0.5 m grid";
        return false;
    }
    std::vector<std::uint16_t> heights(kCellTiles * kCellTiles), tops(kCellTiles * kCellTiles);
    for (int i = 0; i < kCellTiles * kCellTiles; ++i) {
        float f, top;
        std::memcpy(&f, raw.data() + i * rec + 1, 4);
        top = f;
        if (rec == 9) std::memcpy(&top, raw.data() + i * rec + 5, 4);
        const float h = roundf((f - baseY) * 50.0f), t = roundf((top - baseY) * 50.0f);
        if (!std::isfinite(f) || !std::isfinite(top) || h < 0.0f || h > 65535.0f || t < h || t > 65535.0f) {
            *why = "a floor height is outside the section's range above its base";
            return false;
        }
        heights[i] = (std::uint16_t)h;
        tops[i] = (std::uint16_t)t;
    }
    PoolSub& s = p.Subs[k];
    bg3se::AiSubgrid* sg = s.Sg;
    if (!tiles_free(sg) || sg->AttachedObjectRefCounts.size() != 0) {
        const Occupants occ = pool_occupants(p.Grid, sg);
        logf("pool (%s): subgrid %u not moved: %d tiles hold something, %d attached, entities %llx %llx (first tile %d %d)",
             client ? "client" : "server", (unsigned)s.Id, occ.Tiles, occ.Attached,
             occ.Entities.size() > 0 ? (unsigned long long)occ.Entities[0] : 0ull,
             occ.Entities.size() > 1 ? (unsigned long long)occ.Entities[1] : 0ull, occ.FirstX, occ.FirstY);
        *why = "something stands on that section";
        return false;
    }
    const int xg = (int)lroundf(minX * 2), zg = (int)lroundf(minZ * 2);
    for (auto const& o : p.Subs) {
        if (&o != &s && o.Moved && o.Xg == xg && o.Zg == zg) {
            *why = "another pool subgrid is placed there";
            return false;
        }
    }
    // Its ground surfaces stay with the ground it leaves (above), before its tiles are rewritten.
    if (!client && s.Moved) pool_unsurface_before_move(p, s, "moved");
    auto* m = patch_map(p.Grid);
    // 1. Out of every lookup first, 2. rewritten, 3. registered where it now lies.
    for (auto q : s.Moved ? s.Live : s.Patches) patch_remove(m, q, s.Id);
    auto* tiles = sg->TileGrid->Tiles;
    for (int i = 0; i < kCellTiles * kCellTiles; ++i) {
        auto& t = tiles[i];
        const unsigned char fl = raw[i * rec];
        const bool walk = rec == 5 ? fl != 0 : (fl & 1) != 0;
        const bool slope = rec == 9 && (fl & 2) != 0;
        t.Flags.Flags = (s.Tiles[i].Flags.Flags & ~kPoolOwnedBits) | (walk ? 0 : kArenaBlockBits) | (slope ? kSlopeBit : 0);
        t.MinHeight = heights[i];
        t.MaxHeight = tops[i];
        t.SurfaceMetaDataIndex = 0xffff;
    }
    sg->Translate = glm::vec3{xg * 0.5f, baseY, zg * 0.5f};
    sg->WorldPos = bg3se::AiWorldPos{.Xglobal = xg, .Zglobal = zg, .Xlocal = 0.0f, .Zlocal = 0.0f, .Y = baseY};
    const auto list = patches_of(xg, zg, kCellTiles, kCellTiles);
    int added = 0;
    for (auto q : list) {
        if (patch_add(m, q, s.Id) && patch_check(p.Grid, q, s.Id)) ++added;
    }
    set_patch_list(sg, list);
    s.Moved = true;
    s.Xg = xg;
    s.Zg = zg;
    s.BaseY = baseY;
    s.Live = list;
    pool_seams(p);
    pool_bounds(p);
    if (client) {
        pool_sight_follow(p);
        pool_textures_follow(p, k);
    }
    counts[0] = added;
    counts[1] = (int)list.size();
    if (added != (int)list.size()) {
        logf("pool (%s): subgrid %u registered in %d of %zu patches", client ? "client" : "server", (unsigned)s.Id, added,
             list.size());
        *why = "the subgrid could not be registered in every patch";
        return false;
    }
    return true;
}

// Everything back: every moved pool subgrid's tiles, position and patches from its snapshot.
void pool_restore(bool client, int* counts) {
    auto& p = g_pool[client ? 1 : 0];
    counts[0] = counts[1] = counts[2] = 0;
    if (p.Subs.empty()) {
        p = PoolState{};
        return;
    }
    if (!pool_alive(client)) {
        counts[2] = (int)p.Subs.size();
        logf("pool (%s): grid changed, %d subgrids dropped", client ? "client" : "server", counts[2]);
        p = PoolState{};
        return;
    }
    auto* m = patch_map(p.Grid);
    for (auto& s : p.Subs) {
        if (!s.Moved) continue;
        if (!client) pool_unsurface_before_move(p, s, "went home");  // Not onto the level's chasm
        if (!tiles_free(s.Sg)) ++counts[1];  // something still stood there: it goes back regardless
        for (auto q : s.Live) patch_remove(m, q, s.Id);
        std::memcpy(s.Sg->TileGrid->Tiles, s.Tiles.data(), s.Tiles.size() * sizeof(bg3se::AiGridTile));
        s.Sg->Translate = s.Translate;
        s.Sg->WorldPos = s.WorldPos;
        for (auto q : s.Patches) patch_add(m, q, s.Id);
        set_patch_list(s.Sg, s.Patches);
        s.Moved = false;
        s.Live.clear();
        ++counts[0];
    }
    pool_bounds(p);
    if (client) {
        pool_sight_follow(p);
        pool_textures_follow(p, -1);
    }
    logf("pool (%s): %d subgrids back home (%d had something on them)", client ? "client" : "server", counts[0], counts[1]);
    p = PoolState{};
}

}  // namespace

// ==== Part B: WoW triangles in raycasts, plus the Lua channel (patch 0003 part B) ====

namespace {

using phx_hit = bg3se::phx::PhysicsHit;
using phx_hits = bg3se::phx::PhysicsHitAll;
using ShapeFilter = bg3se::Function<bool(bg3se::phx::PhysicsShape const*)>;
using ReadLock = std::optional<bg3se::phx::PhysicsSceneScopedReadLock*>;
using RayClosestProc = bool (*)(void const*, glm::vec3 const&, glm::vec3 const&, phx_hit&, bg3se::PhysicsType,
                                bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int, ShapeFilter*);
using RayAllProc = bool (*)(void const*, glm::vec3 const&, glm::vec3 const&, phx_hits&, bg3se::PhysicsType,
                            bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int, ReadLock);
using RayAnyProc = bool (*)(void const*, glm::vec3 const&, glm::vec3 const&, bg3se::PhysicsType,
                            bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int, ReadLock);
constexpr int kRayClosestSlot = 8, kRayAllSlot = 9, kRayAnySlot = 10;  // Itanium: two destructor slots first
constexpr int kSphereClosestSlot = 11, kCapsuleClosestSlot = 12, kSphereAllSlot = 15, kCapsuleAllSlot = 16;
using SphereClosestProc = bool (*)(void const*, float, glm::vec3 const&, glm::vec3 const&, phx_hit&, bg3se::PhysicsType,
                                   bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int);
using CapsuleClosestProc = bool (*)(void const*, float, float, glm::vec3 const&, glm::vec3 const&, phx_hit&,
                                    bg3se::PhysicsType, bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int);
using SphereAllProc = bool (*)(void const*, float, glm::vec3 const&, glm::vec3 const&, phx_hits&, bg3se::PhysicsType,
                               bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int);
using CapsuleAllProc = bool (*)(void const*, float, float, glm::vec3 const&, glm::vec3 const&, phx_hits&,
                                bg3se::PhysicsType, bg3se::PhysicsGroupFlags, bg3se::PhysicsGroupFlags, int, int, int);
constexpr int kVmtCopy = 64;
// Character, Item and Dead: never voided. Dead (0x4000): a dead stand-in's shape carries
// it instead of Character, so the void dropped every corpse from BG3's cursor (its query includes 0x4000)
// and a click on one walked onto it instead of opening BG3's loot window.
constexpr std::uint32_t kKeepGroups = 0x200 | 0x400 | 0x4000;
constexpr std::uint32_t kWowGroups = 0x10 | 0x800;    // what a WoW hit reports: Terrain, Scenery
// 400000 until 0.2.8; 0.2.9: up to 128 squares (a treadmill per party member) at 4k to 50k each.
constexpr std::size_t kMaxTris = 2000000;
constexpr float kCell = 1.0f;
constexpr int kChannel = 0x57423301;  // "WB3" 1: a query with this context, type 0 and include 0 is a plugin call
float g_max_span = 60.0f;             // setting max_span_m: largest side of one cluster of triangles (below)
// Clusters (0.2.9, a treadmill per party member): the squares of members far apart lie far
// apart, and one dense 1 m cell grid over all of them would be kilometres wide (a 2 km box: 4 million
// cells). The triangles are grouped by 64 m blocks; touching blocks (8 neighbours) form one cluster
// with its own cell grid, so the span limit and the grid's size are per cluster and members can be any
// distance apart. A query walks every cluster its segment touches; the nearest hit wins.
constexpr float kClusterBlock = 64.0f;
constexpr float kMaxSpanLimit = 1024.0f;  // Limits accepts up to this per cluster

struct WowTri {
    glm::vec3 A, E1, E2, N;
};

// What a triangle is (arena-triangles.kinds, one byte each, from wow_arena.py via world_bridge.py):
// 0 WoW's terrain mesh, 1 a synthetic wall along walkable/blocked tile edges, 2 a WMO (building),
// 3 an M2 doodad (tree, fence), 4 a flat floor WoW draws nothing at (deep water's surface, an ADT
// hole). Every query sees them all; BG3's camera clearance only the kinds WoW's camera collides
// with (terrain and WMOs).
constexpr std::uint32_t kKindAll = ~0u, kCameraKinds = (1u << 0) | (1u << 2);

// One cluster's cell grid: CellTris holds indices into ArenaRays::Tris.
struct RayGrid {
    std::vector<std::uint32_t> CellStart, CellTris;
    glm::vec3 Min{}, Max{};
    int Nx = 0, Nz = 0;
};

struct ArenaRays {
    std::vector<WowTri> Tris;
    std::vector<std::uint8_t> Kind;  // per entry of Tris; empty: unknown (all count as terrain)
    std::vector<RayGrid> Grids;      // one per cluster (0.2.9)
    glm::vec3 Min{}, Max{};          // every cluster
    bool Void = false;
    std::uint32_t Mask = kWowGroups;
    std::uint32_t SkipContexts = 0;  // bit c: queries with context c pass straight to the engine
    bg3se::phx::PhysicsShape* Proxy = nullptr;
};

// The arena's rays for a query, or null when the query's context is left to BG3 alone
// (the client's cursor picking, context 1: WoW does the picking in the real flow).
ArenaRays const* for_context(ArenaRays const* r, int context) {
    if (r != nullptr && context >= 0 && context < 32 && ((r->SkipContexts >> context) & 1u) != 0) return nullptr;
    return r;
}

struct RayStats {
    std::atomic<std::uint64_t> Calls[3]{}, Touch[3]{}, Wow[3]{}, Voided[3]{};
    std::mutex Lock;
    struct Seen {
        int Slot;
        std::uint32_t Type, In, Ex;
        int Context;
        std::uint64_t Count;
        glm::vec3 A{}, B{};    // the last such query's segment
        int Hits = -1, Chars = -1;  // RaycastAll: hits it returned, of them characters (after our changes)
    };
    std::vector<Seen> Queries;
};

// One per side: the scene whose vtable is ours (channel always, arena rays when set).
struct SceneSwap {
    void* Scene = nullptr;
    void const* OldVmt = nullptr;
    std::unique_ptr<std::uint64_t[]> Table;
    RayClosestProc Closest = nullptr;
    RayAllProc All = nullptr;
    RayAnyProc Any = nullptr;
    SphereClosestProc SphereClosest = nullptr;
    CapsuleClosestProc CapsuleClosest = nullptr;
    SphereAllProc SphereAll = nullptr;
    CapsuleAllProc CapsuleAll = nullptr;
    std::atomic<std::uint64_t> SweepCalls[4]{}, SweepWow[4]{}, SweepVoided[4]{};
    std::atomic<ArenaRays const*> Rays{nullptr};
    double RaysAt = 0;
    RayStats Stats;
    std::uint64_t* SlotCounts = nullptr;  // calls per vtable slot (thunks), the raycast slots excepted
};

SceneSwap g_scene[2];
// Replaced triangle sets, freed only well after their swap (a query in flight may still read one;
// queries take microseconds, a set lives on for kRetireSeconds). The live set of each side is
// g_live_rays[side]; it is retired when the next one is swapped in or rays go off.
struct RetiredRays {
    std::unique_ptr<ArenaRays> Rays;
    double At;
};
std::vector<RetiredRays> g_retired_rays;
std::unique_ptr<ArenaRays> g_live_rays[2];
constexpr double kRetireSeconds = 30.0;
std::mutex g_rays_lock;                                  // the two sides install from their own threads
std::mutex g_install_lock;                               // scene installs (client frame thread) vs commands

struct KnownScene {
    std::atomic<void const*> Scene{nullptr};
    RayClosestProc Closest = nullptr;
    RayAllProc All = nullptr;
    RayAnyProc Any = nullptr;
    SphereClosestProc SphereClosest = nullptr;
    CapsuleClosestProc CapsuleClosest = nullptr;
    SphereAllProc SphereAll = nullptr;
    CapsuleAllProc CapsuleAll = nullptr;
    int Side = 0;
};
constexpr int kKnownScenes = 64;
KnownScene g_known[kKnownScenes];
std::atomic<int> g_known_count{0};

KnownScene const* known_of(void const* scene) {
    const int n = g_known_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_known[i].Scene.load(std::memory_order_acquire) == scene) return &g_known[i];
    }
    return nullptr;
}

struct Hooked {
    KnownScene const* K;
    SceneSwap* S;
    ArenaRays const* R;
};
Hooked hooked_of(void const* scene) {
    Hooked h{known_of(scene), nullptr, nullptr};
    if (h.K == nullptr) {
        logf("query on an unknown hooked scene %p", scene);
        std::abort();  // cannot happen: only registered scenes get the copied table
    }
    SceneSwap& s = g_scene[h.K->Side];
    h.S = &s;
    if (s.Scene == scene) h.R = s.Rays.load(std::memory_order_acquire);
    return h;
}

bool segment_touches(ArenaRays const& r, glm::vec3 const& a, glm::vec3 const& b) {
    for (int k = 0; k < 3; ++k) {
        if (std::max(a[k], b[k]) < r.Min[k] || std::min(a[k], b[k]) > r.Max[k]) return false;
    }
    return true;
}

// Inside one cluster's columns (0.2.9: not the box round all of them, which spans the void between
// members far apart).
bool inside_square(ArenaRays const& r, glm::vec3 const& p) {
    if (!(p.x >= r.Min.x && p.x <= r.Max.x && p.z >= r.Min.z && p.z <= r.Max.z)) return false;
    for (auto const& g : r.Grids) {
        if (p.x >= g.Min.x && p.x <= g.Max.x && p.z >= g.Min.z && p.z <= g.Max.z) return true;
    }
    return false;
}

bool grid_touches(RayGrid const& g, glm::vec3 const& a, glm::vec3 const& b) {
    for (int k = 0; k < 3; ++k) {
        if (std::max(a[k], b[k]) < g.Min[k] || std::min(a[k], b[k]) > g.Max[k]) return false;
    }
    return true;
}

bool tri_hit(WowTri const& t, glm::vec3 const& a, glm::vec3 const& d, float& out) {
    const glm::vec3 p = glm::cross(d, t.E2);
    const float det = glm::dot(t.E1, p);
    if (fabsf(det) < 1e-9f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = a - t.A;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(s, t.E1);
    const float v = glm::dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = glm::dot(t.E2, q) * inv;
    if (tt <= 1e-5f || tt > 1.0f) return false;
    out = tt;
    return true;
}

bool wow_closest(ArenaRays const& r, glm::vec3 const& a, glm::vec3 const& b, float& best, glm::vec3& normal,
                 std::uint32_t kinds = kKindAll) {
    if (r.Tris.empty() || !segment_touches(r, a, b)) return false;
    const glm::vec3 d = b - a;
    thread_local std::vector<std::uint32_t> mark;
    thread_local std::uint32_t generation = 0;
    if (mark.size() != r.Tris.size()) {
        mark.assign(r.Tris.size(), 0);
        generation = 0;
    }
    if (++generation == 0) {
        std::fill(mark.begin(), mark.end(), 0);
        generation = 1;
    }
    best = 2.0f;
    for (auto const& g : r.Grids) {
        if (!grid_touches(g, a, b)) continue;
        auto test_cell = [&](int cx, int cz) {
            if (cx < 0 || cz < 0 || cx >= g.Nx || cz >= g.Nz) return;
            const int c = cz * g.Nx + cx;
            for (std::uint32_t k = g.CellStart[c]; k < g.CellStart[c + 1]; ++k) {
                const std::uint32_t i = g.CellTris[k];
                if (mark[i] == generation) continue;
                mark[i] = generation;
                if (kinds != kKindAll && ((kinds >> (r.Kind.empty() ? 0 : r.Kind[i])) & 1u) == 0) continue;
                float t;
                if (tri_hit(r.Tris[i], a, d, t) && t < best) {
                    best = t;
                    normal = r.Tris[i].N;
                }
            }
        };
        float t0 = 0.0f, t1 = 1.0f;
        bool miss = false;
        for (int k : {0, 2}) {
            if (fabsf(d[k]) < 1e-9f) {
                if (a[k] < g.Min[k] || a[k] > g.Max[k]) miss = true;
                continue;
            }
            float ta = (g.Min[k] - a[k]) / d[k], tb = (g.Max[k] - a[k]) / d[k];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (miss || t0 > t1 || t0 >= best) continue;  // a nearer hit in another cluster already
        const glm::vec3 p0 = a + d * t0;
        int cx = std::clamp((int)floorf((p0.x - g.Min.x) / kCell), 0, g.Nx - 1);
        int cz = std::clamp((int)floorf((p0.z - g.Min.z) / kCell), 0, g.Nz - 1);
        const int sx = d.x > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
        const float dtx = fabsf(d.x) > 1e-9f ? kCell / fabsf(d.x) : 1e30f;
        const float dtz = fabsf(d.z) > 1e-9f ? kCell / fabsf(d.z) : 1e30f;
        float ntx = fabsf(d.x) > 1e-9f ? ((g.Min.x + (cx + (sx > 0 ? 1 : 0)) * kCell) - a.x) / d.x : 1e30f;
        float ntz = fabsf(d.z) > 1e-9f ? ((g.Min.z + (cz + (sz > 0 ? 1 : 0)) * kCell) - a.z) / d.z : 1e30f;
        for (int steps = 0; steps < g.Nx + g.Nz + 2; ++steps) {
            test_cell(cx, cz);
            const float leave = std::min(ntx, ntz);
            if (best <= leave || leave > t1) break;
            if (ntx < ntz) {
                cx += sx;
                ntx += dtx;
            } else {
                cz += sz;
                ntz += dtz;
            }
            if (cx < 0 || cz < 0 || cx >= g.Nx || cz >= g.Nz) break;
        }
    }
    return best <= 1.0f;
}

bool wants_wow(ArenaRays const& r, bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex) {
    return ((std::uint32_t)type & 1u) != 0 && ((std::uint32_t)in & r.Mask) != 0
        && ((std::uint32_t)ex & r.Mask) != r.Mask;
}

void note_query(SceneSwap& s, int slot, bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in,
                bg3se::PhysicsGroupFlags ex, int context, glm::vec3 const& a, glm::vec3 const& b, int hits = -1,
                int chars = -1) {
    std::lock_guard<std::mutex> lock(s.Stats.Lock);
    for (auto& q : s.Stats.Queries) {
        if (q.Slot == slot && q.Type == (std::uint32_t)type && q.In == (std::uint32_t)in
            && q.Ex == (std::uint32_t)ex && q.Context == context) {
            ++q.Count;
            q.A = a;
            q.B = b;
            q.Hits = hits;
            q.Chars = chars;
            return;
        }
    }
    if (s.Stats.Queries.size() < 48) {
        RayStats::Seen q{slot, (std::uint32_t)type, (std::uint32_t)in, (std::uint32_t)ex, context, 1};
        q.A = a;
        q.B = b;
        q.Hits = hits;
        q.Chars = chars;
        s.Stats.Queries.push_back(q);
    }
}

bool bg3_closest_outside(KnownScene const& k, ArenaRays const& r, void const* scene, glm::vec3 const& a,
                         glm::vec3 const& b, bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in,
                         bg3se::PhysicsGroupFlags ex, int context, int index, int exIndex, phx_hit& hit,
                         bool& voided) {
    alignas(phx_hits) thread_local unsigned char raw[sizeof(phx_hits)];
    thread_local bool made = false;
    auto* all = reinterpret_cast<phx_hits*>(raw);
    if (!made) {
        new (raw) phx_hits{};
        made = true;
    }
    all->Normals.clear();
    all->Positions.clear();
    all->Distances.clear();
    all->PhysicsGroup.clear();
    all->PhysicsExtraFlags.clear();
    all->Shapes.clear();
    k.All(scene, a, b, *all, type, in, ex, context, index, exIndex, std::nullopt);
    int pick = -1;
    voided = false;
    const std::uint32_t n = all->Distances.size();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (i >= all->Positions.size() || i >= all->PhysicsGroup.size()) break;
        if (inside_square(r, all->Positions[i]) && ((std::uint32_t)all->PhysicsGroup[i] & kKeepGroups) == 0) {
            voided = true;
            continue;
        }
        if (pick < 0 || all->Distances[i] < all->Distances[pick]) pick = (int)i;
    }
    if (pick < 0) return false;
    hit.Distance = all->Distances[pick];
    hit.Position = all->Positions[pick];
    hit.Normal = pick < (int)all->Normals.size() ? all->Normals[pick] : glm::vec3{0, 1, 0};
    hit.PhysicsGroup = all->PhysicsGroup[pick];
    hit.PhysicsExtraFlags =
        pick < (int)all->PhysicsExtraFlags.size() ? all->PhysicsExtraFlags[pick] : (bg3se::PhysicsExtraFlags)0;
    hit.Shape = pick < (int)all->Shapes.size() ? all->Shapes[pick] : r.Proxy;
    return true;
}

void fill_wow_hit(ArenaRays const& r, glm::vec3 const& a, glm::vec3 const& b, float t, glm::vec3 n, phx_hit& hit) {
    const glm::vec3 d = b - a;
    if (glm::dot(n, d) > 0.0f) n = -n;
    hit.Normal = n;
    hit.Position = a + d * t;
    hit.Distance = t;  // BG3 reports hit distance as a fraction of the segment (measured r41)
    hit.PhysicsGroup = (bg3se::PhysicsGroupFlags)kWowGroups;
    hit.PhysicsExtraFlags = (bg3se::PhysicsExtraFlags)0;
    hit.Shape = r.Proxy;
}

bool channel_call(int side, std::uint32_t op, glm::vec3 const& a, glm::vec3 const& b);

bool hook_closest(void const* scene, glm::vec3 const& a, glm::vec3 const& b, phx_hit& hit, bg3se::PhysicsType type,
                  bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context, int index, int exIndex,
                  ShapeFilter* filter) {
    const Hooked h = hooked_of(scene);
    if (context == kChannel && (std::uint32_t)type == 0 && (std::uint32_t)in == 0) {
        hit = phx_hit{};
        return channel_call(h.K->Side, (std::uint32_t)ex, a, b);
    }
    SceneSwap* s = h.S;
    ArenaRays const* r = for_context(h.R, context);
    s->Stats.Calls[0].fetch_add(1, std::memory_order_relaxed);
    if (r == nullptr || !segment_touches(*r, a, b)) {
        return h.K->Closest(scene, a, b, hit, type, in, ex, context, index, exIndex, filter);
    }
    s->Stats.Touch[0].fetch_add(1, std::memory_order_relaxed);
    note_query(*s, kRayClosestSlot, type, in, ex, context, a, b);
    bool got;
    if (r->Void) {
        bool voided = false;
        got = bg3_closest_outside(*h.K, *r, scene, a, b, type, in, ex, context, index, exIndex, hit, voided);
        if (voided) s->Stats.Voided[0].fetch_add(1, std::memory_order_relaxed);
    } else {
        got = h.K->Closest(scene, a, b, hit, type, in, ex, context, index, exIndex, filter);
    }
    float t;
    glm::vec3 n;
    if (wants_wow(*r, type, in, ex) && wow_closest(*r, a, b, t, n) && (!got || t < hit.Distance)) {
        fill_wow_hit(*r, a, b, t, n, hit);
        s->Stats.Wow[0].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return got;
}

// BG3's cursor character query: RaycastAll type 15, include 0x4ca20, exclude 0x210080, context 1, from
// BG3's camera through the cursor to the far plane (333 m to 10 km). The picking job keeps a character
// only if it is the query's first hit. WoW's geometry here is a collision proxy seen from BG3's camera,
// not what the player sees (WoW draws from its own eye, slid clear of hills, and composites BG3's
// characters by WoW's depth): the arena's 4 m walls round blocked tiles and hill crests next to BG3's
// camera hid Tav from the cursor (0/9 at (-1399.8, 120.2, -350.3) and (-1333.6, 169.5,
// -384.0), 2/9 at (-1401.8, 120.0, -352.8)). That query gets no WoW hits; every other query
// (movement, the cursor's ground point, sight, casts) keeps them.
bool cursor_character_query(bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex,
                            int context, glm::vec3 const& a, glm::vec3 const& b) {
    return context == 1 && (std::uint32_t)type == 15 && (std::uint32_t)in == 0x4ca20 && (std::uint32_t)ex == 0x210080
        && glm::length(b - a) > 100.0f;
}

bool hook_all(void const* scene, glm::vec3 const& a, glm::vec3 const& b, phx_hits& hits, bg3se::PhysicsType type,
              bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context, int index, int exIndex,
              ReadLock lock) {
    const Hooked h = hooked_of(scene);
    SceneSwap* s = h.S;
    ArenaRays const* r = for_context(h.R, context);
    s->Stats.Calls[1].fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t before = hits.Distances.size();
    const bool got = h.K->All(scene, a, b, hits, type, in, ex, context, index, exIndex, lock);
    auto chars = [&]() {
        int c = 0;
        for (std::uint32_t i = before; i < hits.PhysicsGroup.size(); ++i) c += ((std::uint32_t)hits.PhysicsGroup[i] & 0x200) != 0;
        return c;
    };
    if (r == nullptr || !segment_touches(*r, a, b)) {
        // The cursor's queries (context 1) are noted even away from the arena (picking research).
        if (context == 1) note_query(*s, kRayAllSlot, type, in, ex, context, a, b, (int)(hits.Distances.size() - before), chars());
        return got;
    }
    s->Stats.Touch[1].fetch_add(1, std::memory_order_relaxed);
    if (r->Void) {
        std::uint32_t w = before;
        const std::uint32_t n = std::min({hits.Distances.size(), hits.Positions.size(), hits.Normals.size(),
                                          hits.PhysicsGroup.size(), hits.PhysicsExtraFlags.size(),
                                          hits.Shapes.size()});
        for (std::uint32_t i = before; i < n; ++i) {
            if (inside_square(*r, hits.Positions[i]) && ((std::uint32_t)hits.PhysicsGroup[i] & kKeepGroups) == 0) {
                s->Stats.Voided[1].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (w != i) {
                hits.Distances[w] = hits.Distances[i];
                hits.Positions[w] = hits.Positions[i];
                hits.Normals[w] = hits.Normals[i];
                hits.PhysicsGroup[w] = hits.PhysicsGroup[i];
                hits.PhysicsExtraFlags[w] = hits.PhysicsExtraFlags[i];
                hits.Shapes[w] = hits.Shapes[i];
            }
            ++w;
        }
        if (w < n) {
            hits.Distances.resize(w);
            hits.Positions.resize(w);
            hits.Normals.resize(w);
            hits.PhysicsGroup.resize(w);
            hits.PhysicsExtraFlags.resize(w);
            hits.Shapes.resize(w);
        }
    }
    float t;
    glm::vec3 n;
    if (!cursor_character_query(type, in, ex, context, a, b) && wants_wow(*r, type, in, ex) && wow_closest(*r, a, b, t, n)
        && allocator_ready()) {
        phx_hit wh{};
        fill_wow_hit(*r, a, b, t, n, wh);
        hits.Normals.push_back(wh.Normal);
        hits.Positions.push_back(wh.Position);
        hits.Distances.push_back(wh.Distance);
        hits.PhysicsGroup.push_back(wh.PhysicsGroup);
        hits.PhysicsExtraFlags.push_back(wh.PhysicsExtraFlags);
        hits.Shapes.push_back(wh.Shape);
        s->Stats.Wow[1].fetch_add(1, std::memory_order_relaxed);
    }
    note_query(*s, kRayAllSlot, type, in, ex, context, a, b, (int)(hits.Distances.size() - before), chars());
    return hits.Distances.size() > 0;
}

bool hook_any(void const* scene, glm::vec3 const& a, glm::vec3 const& b, bg3se::PhysicsType type,
              bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context, int index, int exIndex,
              ReadLock lock) {
    const Hooked h = hooked_of(scene);
    SceneSwap* s = h.S;
    ArenaRays const* r = for_context(h.R, context);
    s->Stats.Calls[2].fetch_add(1, std::memory_order_relaxed);
    if (r == nullptr || !segment_touches(*r, a, b)) {
        return h.K->Any(scene, a, b, type, in, ex, context, index, exIndex, lock);
    }
    s->Stats.Touch[2].fetch_add(1, std::memory_order_relaxed);
    note_query(*s, kRayAnySlot, type, in, ex, context, a, b);
    float t;
    glm::vec3 n;
    if (wants_wow(*r, type, in, ex) && wow_closest(*r, a, b, t, n)) {
        s->Stats.Wow[2].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!r->Void) return h.K->Any(scene, a, b, type, in, ex, context, index, exIndex, lock);
    phx_hit hit{};
    bool voided = false;
    const bool got = bg3_closest_outside(*h.K, *r, scene, a, b, type, in, ex, context, index, exIndex, hit, voided);
    if (voided) s->Stats.Voided[2].fetch_add(1, std::memory_order_relaxed);
    return got;
}

// ---- sweeps (void step 5): ground snapping and character physics sweep spheres and capsules ----
//
// WoW triangles are tested with rays from the shape's centre and from its lowest point (a
// vertical capsule's foot), which is what keeps a character standing on WoW ground and
// stops it at WoW walls; BG3's own hits inside the arena are dropped in void mode.

struct SweepShape {
    float Radius, HalfHeight;  // HalfHeight 0 for a sphere
};

bool wow_sweep(ArenaRays const& r, SweepShape sh, glm::vec3 const& a, glm::vec3 const& b, float& t, glm::vec3& n) {
    const glm::vec3 foot{0.0f, -(sh.HalfHeight + sh.Radius), 0.0f};
    float tc = 2.0f, tf = 2.0f;
    glm::vec3 nc{}, nf{};
    const bool hc = wow_closest(r, a, b, tc, nc);
    const bool hf = wow_closest(r, a + foot, b + foot, tf, nf);
    if (!hc && !hf) return false;
    if (hf && (!hc || tf <= tc)) {
        t = tf;
        n = nf;
    } else {
        t = tc;
        n = nc;
    }
    return true;
}

bool sweep_touches(ArenaRays const& r, SweepShape sh, glm::vec3 const& a, glm::vec3 const& b) {
    const glm::vec3 foot{0.0f, -(sh.HalfHeight + sh.Radius), 0.0f};
    return segment_touches(r, a, b) || segment_touches(r, a + foot, b + foot);
}

template <class Engine>
bool sweep_closest_common(int slot, void const* scene, SweepShape sh, glm::vec3 const& a, glm::vec3 const& b, phx_hit& hit,
                          bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context,
                          int index, int exIndex, Engine&& closest, bool canVoid,
                          std::function<void(phx_hits&)> const& all) {
    const Hooked h = hooked_of(scene);
    SceneSwap* s = h.S;
    ArenaRays const* r = for_context(h.R, context);
    s->SweepCalls[slot].fetch_add(1, std::memory_order_relaxed);
    if (r == nullptr || !sweep_touches(*r, sh, a, b)) return closest();
    note_query(*s, slot == 0 ? kSphereClosestSlot : kCapsuleClosestSlot, type, in, ex, context, a, b);
    bool got = false;
    if (r->Void && canVoid) {
        alignas(phx_hits) thread_local unsigned char raw[sizeof(phx_hits)];
        thread_local bool made = false;
        auto* list = reinterpret_cast<phx_hits*>(raw);
        if (!made) {
            new (raw) phx_hits{};
            made = true;
        }
        list->Normals.clear();
        list->Positions.clear();
        list->Distances.clear();
        list->PhysicsGroup.clear();
        list->PhysicsExtraFlags.clear();
        list->Shapes.clear();
        all(*list);
        int pick = -1;
        const std::uint32_t n = std::min(list->Distances.size(), std::min(list->Positions.size(), list->PhysicsGroup.size()));
        for (std::uint32_t i = 0; i < n; ++i) {
            if (inside_square(*r, list->Positions[i]) && ((std::uint32_t)list->PhysicsGroup[i] & kKeepGroups) == 0) {
                s->SweepVoided[slot].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (pick < 0 || list->Distances[i] < list->Distances[pick]) pick = (int)i;
        }
        if (pick >= 0) {
            hit.Distance = list->Distances[pick];
            hit.Position = list->Positions[pick];
            hit.Normal = pick < (int)list->Normals.size() ? list->Normals[pick] : glm::vec3{0, 1, 0};
            hit.PhysicsGroup = list->PhysicsGroup[pick];
            hit.PhysicsExtraFlags = pick < (int)list->PhysicsExtraFlags.size() ? list->PhysicsExtraFlags[pick]
                                                                               : (bg3se::PhysicsExtraFlags)0;
            hit.Shape = pick < (int)list->Shapes.size() ? list->Shapes[pick] : r->Proxy;
            got = true;
        }
    } else {
        got = closest();
    }
    float t;
    glm::vec3 nrm;
    if (wants_wow(*r, type, in, ex) && wow_sweep(*r, sh, a, b, t, nrm) && (!got || t < hit.Distance)) {
        fill_wow_hit(*r, a, b, t, nrm, hit);
        s->SweepWow[slot].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return got;
}

bool hook_sphere_closest(void const* scene, float radius, glm::vec3 const& a, glm::vec3 const& b, phx_hit& hit,
                         bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context,
                         int index, int exIndex) {
    KnownScene const* k = known_of(scene);
    return sweep_closest_common(
        0, scene, {radius, 0.0f}, a, b, hit, type, in, ex, context, index, exIndex,
        [&] { return k->SphereClosest(scene, radius, a, b, hit, type, in, ex, context, index, exIndex); },
        k->SphereAll != nullptr,
        [&](phx_hits& l) { k->SphereAll(scene, radius, a, b, l, type, in, ex, context, index, exIndex); });
}

bool hook_capsule_closest(void const* scene, float radius, float halfHeight, glm::vec3 const& a, glm::vec3 const& b,
                          phx_hit& hit, bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex,
                          int context, int index, int exIndex) {
    KnownScene const* k = known_of(scene);
    return sweep_closest_common(
        1, scene, {radius, halfHeight}, a, b, hit, type, in, ex, context, index, exIndex,
        [&] { return k->CapsuleClosest(scene, radius, halfHeight, a, b, hit, type, in, ex, context, index, exIndex); },
        k->CapsuleAll != nullptr,
        [&](phx_hits& l) { k->CapsuleAll(scene, radius, halfHeight, a, b, l, type, in, ex, context, index, exIndex); });
}

template <class Engine>
bool sweep_all_common(int slot, void const* scene, SweepShape sh, glm::vec3 const& a, glm::vec3 const& b, phx_hits& hits,
                      bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context,
                      Engine&& engine) {
    const Hooked h = hooked_of(scene);
    SceneSwap* s = h.S;
    ArenaRays const* r = for_context(h.R, context);
    s->SweepCalls[slot].fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t before = hits.Distances.size();
    const bool got = engine();
    if (r == nullptr || !sweep_touches(*r, sh, a, b)) return got;
    note_query(*s, slot == 2 ? kSphereAllSlot : kCapsuleAllSlot, type, in, ex, context, a, b);
    if (r->Void) {
        std::uint32_t w = before;
        const std::uint32_t n = std::min({hits.Distances.size(), hits.Positions.size(), hits.Normals.size(),
                                          hits.PhysicsGroup.size(), hits.PhysicsExtraFlags.size(), hits.Shapes.size()});
        for (std::uint32_t i = before; i < n; ++i) {
            if (inside_square(*r, hits.Positions[i]) && ((std::uint32_t)hits.PhysicsGroup[i] & kKeepGroups) == 0) {
                s->SweepVoided[slot].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (w != i) {
                hits.Distances[w] = hits.Distances[i];
                hits.Positions[w] = hits.Positions[i];
                hits.Normals[w] = hits.Normals[i];
                hits.PhysicsGroup[w] = hits.PhysicsGroup[i];
                hits.PhysicsExtraFlags[w] = hits.PhysicsExtraFlags[i];
                hits.Shapes[w] = hits.Shapes[i];
            }
            ++w;
        }
        if (w < n) {
            hits.Distances.resize(w);
            hits.Positions.resize(w);
            hits.Normals.resize(w);
            hits.PhysicsGroup.resize(w);
            hits.PhysicsExtraFlags.resize(w);
            hits.Shapes.resize(w);
        }
    }
    float t;
    glm::vec3 nrm;
    if (wants_wow(*r, type, in, ex) && wow_sweep(*r, sh, a, b, t, nrm) && allocator_ready()) {
        phx_hit wh{};
        fill_wow_hit(*r, a, b, t, nrm, wh);
        hits.Normals.push_back(wh.Normal);
        hits.Positions.push_back(wh.Position);
        hits.Distances.push_back(wh.Distance);
        hits.PhysicsGroup.push_back(wh.PhysicsGroup);
        hits.PhysicsExtraFlags.push_back(wh.PhysicsExtraFlags);
        hits.Shapes.push_back(wh.Shape);
        s->SweepWow[slot].fetch_add(1, std::memory_order_relaxed);
    }
    return hits.Distances.size() > 0;
}

bool hook_sphere_all(void const* scene, float radius, glm::vec3 const& a, glm::vec3 const& b, phx_hits& hits,
                     bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex, int context,
                     int index, int exIndex) {
    KnownScene const* k = known_of(scene);
    return sweep_all_common(2, scene, {radius, 0.0f}, a, b, hits, type, in, ex, context,
                            [&] { return k->SphereAll(scene, radius, a, b, hits, type, in, ex, context, index, exIndex); });
}

bool hook_capsule_all(void const* scene, float radius, float halfHeight, glm::vec3 const& a, glm::vec3 const& b,
                      phx_hits& hits, bg3se::PhysicsType type, bg3se::PhysicsGroupFlags in, bg3se::PhysicsGroupFlags ex,
                      int context, int index, int exIndex) {
    KnownScene const* k = known_of(scene);
    return sweep_all_common(3, scene, {radius, halfHeight}, a, b, hits, type, in, ex, context, [&] {
        return k->CapsuleAll(scene, radius, halfHeight, a, b, hits, type, in, ex, context, index, exIndex);
    });
}

// Points slots [from, to) of `slots` (except `skip`) at counting thunks; *counts gets the
// counter array (indexed by slot). Two pages per call, never unmapped. Null when unavailable.
unsigned char* count_thunks(std::uint64_t* slots, int from, int to, std::initializer_list<int> skip,
                            std::uint64_t** counts) {
    static int pages = 0;
    if (pages >= 64 || to > 128) return nullptr;
    const std::size_t page = 4096;
    void* mem = mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    auto* base = static_cast<unsigned char*>(mem);
    auto* originals = reinterpret_cast<std::uint64_t*>(base + page);
    auto* counters = reinterpret_cast<std::uint64_t*>(base + 2 * page);
    for (int i = from; i < to; ++i) {
        bool skipped = false;
        for (int k : skip) skipped = skipped || k == i;
        originals[i] = slots[i];
        counters[i] = 0;
        if (skipped) continue;
        unsigned char* t = base + i * 16;
        const std::int32_t relCount = (std::int32_t)(reinterpret_cast<unsigned char*>(&counters[i]) - (t + 8));
        t[0] = 0xF0; t[1] = 0x48; t[2] = 0xFF; t[3] = 0x05;
        std::memcpy(t + 4, &relCount, 4);
        const std::int32_t relJump = (std::int32_t)(reinterpret_cast<unsigned char*>(&originals[i]) - (t + 14));
        t[8] = 0xFF; t[9] = 0x25;
        std::memcpy(t + 10, &relJump, 4);
        t[14] = 0xCC; t[15] = 0xCC;
    }
    if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0) return nullptr;  // leaked on purpose
    ++pages;
    for (int i = from; i < to; ++i) {
        bool skipped = false;
        for (int k : skip) skipped = skipped || k == i;
        if (!skipped) slots[i] = reinterpret_cast<std::uint64_t>(base + i * 16);
    }
    *counts = counters;
    return base;
}

// Puts the scene's own vtable back (only while ours is still in place).
void scene_unhook(int side) {
    auto& s = g_scene[side];
    s.Rays.store(nullptr, std::memory_order_release);
    if (s.Scene != nullptr && s.OldVmt != nullptr) {
        void const* now = nullptr;
        safe_read(s.Scene, &now, sizeof(now));
        if (now == s.Table.get() + 2) {
            __atomic_store_n(reinterpret_cast<void const**>(s.Scene), s.OldVmt, __ATOMIC_RELEASE);
            logf("%s physics scene vtable restored", side ? "client" : "server");
        } else {
            logf("%s physics scene vtable changed since; left alone", side ? "client" : "server");
        }
    }
    retire(s.Table, s.OldVmt);
    s.Scene = nullptr;
    s.OldVmt = nullptr;
}

// Gives this side's current physics scene our vtable copy (pass-through until rays
// are set). Called from the client frame thread; the swap is one atomic store of a
// complete table, so either side's thread may be querying meanwhile.
bool scene_hook(int side, char const** why) {
    void* scene = physics_scene_raw(side == 1);
    if (scene == nullptr) {
        *why = "no physics scene";
        return false;
    }
    auto& s = g_scene[side];
    if (s.Scene == scene) {
        void const* now = nullptr;
        if (safe_read(scene, &now, sizeof(now)) && s.Table && now == s.Table.get() + 2) return true;
    }
    if (!scene_checks_out(scene)) {
        *why = "the scene's vtable is not the one bg3se declares";
        return false;
    }
    if (s.Scene != nullptr && s.Scene != scene) scene_unhook(side);  // an older scene (level change)
    void const* vmt = nullptr;
    if (!safe_read(scene, &vmt, sizeof(vmt)) || vmt == nullptr) {
        *why = "unreadable scene vtable";
        return false;
    }
    if (s.Table && vmt == s.Table.get() + 2) vmt = s.OldVmt;  // ours, but no longer tracked as live
    retire(s.Table, s.OldVmt);
    vmt = engine_vmt(vmt);  // a scene that kept an earlier copy of ours: start from the engine's
    auto table = std::make_unique<std::uint64_t[]>(kVmtCopy + 2);
    if (!safe_read(static_cast<std::uint64_t const*>(vmt) - 2, table.get(), (kVmtCopy + 2) * 8)) {
        *why = "unreadable scene vtable";
        return false;
    }
    auto closest = reinterpret_cast<RayClosestProc>(table[2 + kRayClosestSlot]);
    auto all = reinterpret_cast<RayAllProc>(table[2 + kRayAllSlot]);
    auto any = reinterpret_cast<RayAnyProc>(table[2 + kRayAnySlot]);
    auto sphereClosest = reinterpret_cast<SphereClosestProc>(table[2 + kSphereClosestSlot]);
    auto capsuleClosest = reinterpret_cast<CapsuleClosestProc>(table[2 + kCapsuleClosestSlot]);
    auto sphereAll = reinterpret_cast<SphereAllProc>(table[2 + kSphereAllSlot]);
    auto capsuleAll = reinterpret_cast<CapsuleAllProc>(table[2 + kCapsuleAllSlot]);
    if (auto const* k = known_of(scene)) {
        if (k->Closest != closest || k->All != all || k->Any != any || k->Side != side) {
            *why = "physics scene functions changed since it was first hooked";
            return false;
        }
    } else {
        const int slot = g_known_count.load(std::memory_order_acquire);
        if (slot >= kKnownScenes) {
            *why = "too many physics scenes hooked in this process";
            return false;
        }
        g_known[slot].Closest = closest;
        g_known[slot].All = all;
        g_known[slot].Any = any;
        g_known[slot].SphereClosest = sphereClosest;
        g_known[slot].CapsuleClosest = capsuleClosest;
        g_known[slot].SphereAll = sphereAll;
        g_known[slot].CapsuleAll = capsuleAll;
        g_known[slot].Side = side;
        g_known[slot].Scene.store(scene, std::memory_order_release);
        g_known_count.store(slot + 1, std::memory_order_release);
    }
    s.Closest = closest;
    s.All = all;
    s.Any = any;
    s.SphereClosest = sphereClosest;
    s.CapsuleClosest = capsuleClosest;
    s.SphereAll = sphereAll;
    s.CapsuleAll = capsuleAll;
    table[2 + kSphereClosestSlot] = reinterpret_cast<std::uint64_t>(&hook_sphere_closest);
    table[2 + kCapsuleClosestSlot] = reinterpret_cast<std::uint64_t>(&hook_capsule_closest);
    table[2 + kSphereAllSlot] = reinterpret_cast<std::uint64_t>(&hook_sphere_all);
    table[2 + kCapsuleAllSlot] = reinterpret_cast<std::uint64_t>(&hook_capsule_all);
    table[2 + kRayClosestSlot] = reinterpret_cast<std::uint64_t>(&hook_closest);
    table[2 + kRayAllSlot] = reinterpret_cast<std::uint64_t>(&hook_all);
    table[2 + kRayAnySlot] = reinterpret_cast<std::uint64_t>(&hook_any);
    // Read-only call counting on the other slots (2..39; 14 and 18 stay, bg3le checks their code):
    // which queries ground snapping, sweeps and falling use. Pages are never unmapped.
    s.SlotCounts = nullptr;
    if (unsigned char* page = count_thunks(table.get() + 2, 2, 40,
                                           {kRayClosestSlot, kRayAllSlot, kRayAnySlot, 14, 18, kSphereClosestSlot,
                                            kCapsuleClosestSlot, kSphereAllSlot, kCapsuleAllSlot},
                                           &s.SlotCounts)) {
        (void)page;
    }
    s.Rays.store(nullptr, std::memory_order_release);
    s.Table = std::move(table);
    s.Scene = scene;
    s.OldVmt = vmt;
    __atomic_store_n(reinterpret_cast<void const**>(scene), (void const*)(s.Table.get() + 2), __ATOMIC_RELEASE);
    logf("%s physics scene %p hooked (channel; rays off)", side ? "client" : "server", scene);
    return true;
}

// Moves this side's live set (if any) to the retired list and frees sets retired long ago.
// Called with g_rays_lock held, after the swap that took the set out of use.
void retire_live(int side) {
    const double now = mono_seconds();
    if (g_live_rays[side]) g_retired_rays.push_back({std::move(g_live_rays[side]), now});
    std::size_t w = 0;
    for (std::size_t i = 0; i < g_retired_rays.size(); ++i) {
        if (now - g_retired_rays[i].At > kRetireSeconds) continue;  // freed here
        if (w != i) g_retired_rays[w] = std::move(g_retired_rays[i]);
        ++w;
    }
    g_retired_rays.resize(w);
}

void rays_off(int side) {
    auto& s = g_scene[side];
    std::lock_guard<std::mutex> held(g_rays_lock);
    if (s.Rays.exchange(nullptr, std::memory_order_acq_rel) != nullptr) {
        logf("%s arena rays off", side ? "client" : "server");
    }
    retire_live(side);
}

// The shape every synthetic WoW hit names. The engine reads a hit's Shape as an identity (shape ->
// physics object -> entity), never as geometry (world mode stood on WoW ground 1 km above the
// chasm plane it named, r60), so one static BG3 shape per scene serves anywhere: found once, at the
// arena or failing that at known static ground (the tutorial's chasm plane), and checked again
// before each install and twice a second (frame) in case the level dropped it.
struct ProxyPick {
    void const* Scene = nullptr;
    bg3se::phx::PhysicsShape* Shape = nullptr;
    glm::vec2 Column{};  // where it was found: a vertical ray there must still hit it
};
ProxyPick g_proxy[2];
constexpr std::uint32_t kProxyRefuse = 0x200 | 0x400 | 0x4000000 | 0x8000000;  // Character, Item, PlatformOwner, Falling

bool proxy_probe(int side, void* scene, glm::vec2 col, std::uint32_t groups, phx_hit& probe) {
    auto& s = g_scene[side];
    alignas(16) unsigned char noFilter[64] = {};
    probe = phx_hit{};
    return s.Closest(scene, {col.x, 3000.0f, col.y}, {col.x, -3000.0f, col.y}, probe, (bg3se::PhysicsType)1,
                     (bg3se::PhysicsGroupFlags)groups, (bg3se::PhysicsGroupFlags)kProxyRefuse, 0, -1, -1,
                     reinterpret_cast<ShapeFilter*>(noFilter))
        && probe.Shape != nullptr && ((std::uint32_t)probe.PhysicsGroup & kProxyRefuse) == 0;
}

// Still the same shape at the same column (the level keeps it)?
bool proxy_still(int side, void* scene) {
    auto& p = g_proxy[side];
    if (p.Scene != scene || p.Shape == nullptr) return false;
    phx_hit probe{};
    for (std::uint32_t groups : {0x10u, 0x20u, 0x800u, ~kKeepGroups}) {
        if (proxy_probe(side, scene, p.Column, groups, probe) && probe.Shape == p.Shape) return true;
    }
    return false;
}

bg3se::phx::PhysicsShape* scene_proxy(int side, void* scene, glm::vec3 const& lo, glm::vec3 const& hi) {
    auto& p = g_proxy[side];
    if (proxy_still(side, scene)) return p.Shape;
    if (p.Shape != nullptr) logf("%s proxy shape %p gone; picking another", side ? "client" : "server", (void*)p.Shape);
    p = ProxyPick{};
    std::vector<glm::vec2> cols;
    for (float fx : {0.5f, 0.25f, 0.75f}) {
        for (float fz : {0.5f, 0.25f, 0.75f}) cols.push_back({lo.x + (hi.x - lo.x) * fx, lo.z + (hi.z - lo.z) * fz});
    }
    // The level's own grid (0.2.9): a 7 x 7 lattice of columns over it, middle first.
    if (g_level_box_set[side]) {
        const glm::vec4 b = g_level_box[side];
        std::vector<glm::vec2> lv;
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j) lv.push_back({b.x + (b.z - b.x) * (i + 0.5f) / 7.0f, b.y + (b.w - b.y) * (j + 0.5f) / 7.0f});
        const glm::vec2 mid{(b.x + b.z) * 0.5f, (b.y + b.w) * 0.5f};
        std::sort(lv.begin(), lv.end(), [&](glm::vec2 a, glm::vec2 c) { return glm::length(a - mid) < glm::length(c - mid); });
        cols.insert(cols.end(), lv.begin(), lv.end());
    }
    // The tutorial's chasm plane (x -252..-100, z -464..-304), measured.
    for (glm::vec2 c : {glm::vec2{-136.0f, -444.0f}, glm::vec2{-180.0f, -400.0f}, glm::vec2{-220.0f, -350.0f},
                        glm::vec2{-120.0f, -320.0f}}) {
        cols.push_back(c);
    }
    for (std::uint32_t groups : {0x10u, 0x20u, 0x800u, ~kKeepGroups}) {
        for (glm::vec2 c : cols) {
            phx_hit probe{};
            if (!proxy_probe(side, scene, c, groups, probe)) continue;
            p.Scene = scene;
            p.Shape = probe.Shape;
            p.Column = c;
            logf("%s proxy shape %p at %.1f %.1f (y %.1f, groups 0x%x, hit groups 0x%x)", side ? "client" : "server",
                 (void*)probe.Shape, c.x, c.y, probe.Position.y, groups, (unsigned)probe.PhysicsGroup);
            return p.Shape;
        }
    }
    return nullptr;
}

// Proxy check (op ProxyCheck, on this side's own thread, which owns its scene's queries; Arena.lua
// calls it every two seconds while rays are on): a proxy the level dropped is replaced in the live
// set, so no synthetic hit names a freed shape for long. Returns 0 kept, 1 replaced, 2 rays off.
int proxy_check(int side) {
    auto& s = g_scene[side];
    void* scene = physics_scene_raw(side == 1);
    if (scene == nullptr || s.Scene != scene) return 0;
    std::lock_guard<std::mutex> held(g_rays_lock);
    ArenaRays* live = g_live_rays[side].get();
    if (live == nullptr || s.Rays.load(std::memory_order_acquire) != live) return 0;
    if (proxy_still(side, scene)) return 0;
    bg3se::phx::PhysicsShape* next = scene_proxy(side, scene, live->Min, live->Max);
    if (next != nullptr) {
        __atomic_store_n(&live->Proxy, next, __ATOMIC_RELEASE);
        return 1;
    }
    s.Rays.store(nullptr, std::memory_order_release);
    logf("%s no proxy shape left: arena rays off", side ? "client" : "server");
    retire_live(side);
    return 2;
}

bool rays_on(int side, float const* tris, std::size_t n, std::uint8_t const* kinds, bool voidStatic, std::uint32_t mask,
             std::uint32_t skipContexts, char const** why) {
    // Everything is checked and built first; the live set is swapped only at the end, so a refused
    // install leaves the ground Tav stands on in place.
    auto& s = g_scene[side];
    void* scene = physics_scene_raw(side == 1);
    if (s.Scene == nullptr || scene != s.Scene) {
        *why = "this side's physics scene is not hooked";
        return false;
    }
    if (n == 0 || n > kMaxTris) {
        *why = "triangle count out of range";
        return false;
    }
    auto r = std::make_unique<ArenaRays>();
    r->Void = voidStatic;
    r->Mask = mask != 0 ? mask : kWowGroups;
    r->SkipContexts = skipContexts;
    r->Min = glm::vec3(3.4e38f);
    r->Max = glm::vec3(-3.4e38f);
    r->Tris.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        glm::vec3 p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = {tris[i * 9 + k * 3], tris[i * 9 + k * 3 + 1], tris[i * 9 + k * 3 + 2]};
            if (!std::isfinite(p[k].x) || !std::isfinite(p[k].y) || !std::isfinite(p[k].z)) {
                *why = "triangle vertex is not finite";
                return false;
            }
            r->Min = glm::min(r->Min, p[k]);
            r->Max = glm::max(r->Max, p[k]);
        }
        WowTri t{p[0], p[1] - p[0], p[2] - p[0], {}};
        const glm::vec3 c = glm::cross(t.E1, t.E2);
        const float len = glm::length(c);
        if (len < 1e-9f) continue;
        t.N = c / len;
        r->Tris.push_back(t);
        if (kinds != nullptr) r->Kind.push_back(kinds[i]);
    }
    // Clusters (see kClusterBlock): each triangle's 64 m block by its centre; blocks that touch (8
    // neighbours) are one cluster with its own grid. Union-find over the blocks.
    if (r->Tris.empty()) {
        *why = "no triangle with an area";
        return false;
    }
    auto blockOf = [](WowTri const& t) {
        const glm::vec3 c = t.A + (t.E1 + t.E2) / 3.0f;
        return glm::ivec2{(int)floorf(c.x / kClusterBlock), (int)floorf(c.z / kClusterBlock)};
    };
    std::vector<glm::ivec2> blocks;
    std::vector<std::uint32_t> triBlock(r->Tris.size());
    {
        std::unordered_map<std::uint64_t, std::uint32_t> index;
        for (std::uint32_t i = 0; i < r->Tris.size(); ++i) {
            const glm::ivec2 bk = blockOf(r->Tris[i]);
            const std::uint64_t key = (std::uint32_t)bk.x | ((std::uint64_t)(std::uint32_t)bk.y << 32);
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, (std::uint32_t)blocks.size()).first;
                blocks.push_back(bk);
            }
            triBlock[i] = it->second;
        }
    }
    std::vector<std::uint32_t> parent(blocks.size());
    for (std::uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    auto root = [&](std::uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (std::uint32_t i = 0; i < blocks.size(); ++i) {
        for (std::uint32_t j = i + 1; j < blocks.size(); ++j) {
            if (std::abs(blocks[i].x - blocks[j].x) <= 1 && std::abs(blocks[i].y - blocks[j].y) <= 1) parent[root(i)] = root(j);
        }
    }
    std::unordered_map<std::uint32_t, std::uint32_t> gridOf;  // root block -> grid index
    std::vector<std::uint32_t> triGrid(r->Tris.size());
    for (std::uint32_t i = 0; i < r->Tris.size(); ++i) {
        const std::uint32_t rb = root(triBlock[i]);
        auto it = gridOf.find(rb);
        if (it == gridOf.end()) {
            it = gridOf.emplace(rb, (std::uint32_t)r->Grids.size()).first;
            RayGrid g;
            g.Min = glm::vec3(3.4e38f);
            g.Max = glm::vec3(-3.4e38f);
            r->Grids.push_back(std::move(g));
        }
        triGrid[i] = it->second;
        auto& g = r->Grids[it->second];
        auto const& t = r->Tris[i];
        for (glm::vec3 v : {t.A, t.A + t.E1, t.A + t.E2}) {
            g.Min = glm::min(g.Min, v);
            g.Max = glm::max(g.Max, v);
        }
    }
    for (auto& g : r->Grids) {
        if (g.Max.x - g.Min.x > g_max_span || g.Max.z - g.Min.z > g_max_span || g.Max.y - g.Min.y > 400.0f) {
            *why = "triangles span more than the arena limit (max_span_m)";
            return false;
        }
        g.Min -= glm::vec3(0.05f);
        g.Max += glm::vec3(0.05f);
        g.Nx = std::max(1, (int)ceilf((g.Max.x - g.Min.x) / kCell));
        g.Nz = std::max(1, (int)ceilf((g.Max.z - g.Min.z) / kCell));
        g.CellStart.assign((std::size_t)g.Nx * g.Nz + 1, 0);
    }
    // Two passes (count, then fill): no vector per cell, so a big cluster costs 4 bytes a cell.
    auto cellsOf = [&](std::uint32_t i, auto&& fn) {
        auto const& g = r->Grids[triGrid[i]];
        auto const& t = r->Tris[i];
        const glm::vec3 b = t.A + t.E1, c = t.A + t.E2;
        const float x0 = std::min({t.A.x, b.x, c.x}), x1 = std::max({t.A.x, b.x, c.x});
        const float z0 = std::min({t.A.z, b.z, c.z}), z1 = std::max({t.A.z, b.z, c.z});
        const int cx0 = std::clamp((int)floorf((x0 - g.Min.x) / kCell), 0, g.Nx - 1);
        const int cx1 = std::clamp((int)floorf((x1 - g.Min.x) / kCell), 0, g.Nx - 1);
        const int cz0 = std::clamp((int)floorf((z0 - g.Min.z) / kCell), 0, g.Nz - 1);
        const int cz1 = std::clamp((int)floorf((z1 - g.Min.z) / kCell), 0, g.Nz - 1);
        for (int z = cz0; z <= cz1; ++z)
            for (int x = cx0; x <= cx1; ++x) fn((std::size_t)z * g.Nx + x);
    };
    for (std::uint32_t i = 0; i < r->Tris.size(); ++i) {
        auto& g = r->Grids[triGrid[i]];
        cellsOf(i, [&](std::size_t c) { ++g.CellStart[c + 1]; });
    }
    for (auto& g : r->Grids) {
        for (std::size_t c = 1; c < g.CellStart.size(); ++c) g.CellStart[c] += g.CellStart[c - 1];
        g.CellTris.resize(g.CellStart.back());
    }
    {
        std::vector<std::vector<std::uint32_t>> fill(r->Grids.size());
        for (std::size_t k = 0; k < r->Grids.size(); ++k) fill[k].assign(r->Grids[k].CellStart.begin(), r->Grids[k].CellStart.end() - 1);
        for (std::uint32_t i = 0; i < r->Tris.size(); ++i) {
            auto& g = r->Grids[triGrid[i]];
            auto& f = fill[triGrid[i]];
            cellsOf(i, [&](std::size_t c) { g.CellTris[f[c]++] = i; });
        }
    }
    r->Min -= glm::vec3(0.05f);
    r->Max += glm::vec3(0.05f);
    std::lock_guard<std::mutex> held(g_rays_lock);
    r->Proxy = scene_proxy(side, scene, r->Min, r->Max);
    if (r->Proxy == nullptr) {
        *why = "no static BG3 shape in the level to stand in for WoW hits";
        return false;
    }
    for (auto& c : s.Stats.Calls) c = 0;
    for (auto& c : s.Stats.Touch) c = 0;
    for (auto& c : s.Stats.Wow) c = 0;
    for (auto& c : s.Stats.Voided) c = 0;
    {
        std::lock_guard<std::mutex> lock(s.Stats.Lock);
        s.Stats.Queries.clear();
    }
    s.RaysAt = mono_seconds();
    s.Rays.store(r.get(), std::memory_order_release);
    logf("%s arena rays on, %zu triangles in %zu cluster(s), void %d", side ? "client" : "server", r->Tris.size(),
         r->Grids.size(), voidStatic ? 1 : 0);
    retire_live(side);
    g_live_rays[side] = std::move(r);
    return true;
}

std::string rays_stats_json(int side) {
    auto& s = g_scene[side];
    std::string j = "{\"Hooked\":";
    j += s.Scene != nullptr ? "true" : "false";
    j += ",\"Rays\":";
    j += s.Rays.load() != nullptr ? "true" : "false";
    char const* names[3] = {"Closest", "All", "Any"};
    for (int k = 0; k < 3; ++k) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), ",\"%s\":[%llu,%llu,%llu,%llu]", names[k],
                      (unsigned long long)s.Stats.Calls[k], (unsigned long long)s.Stats.Touch[k],
                      (unsigned long long)s.Stats.Wow[k], (unsigned long long)s.Stats.Voided[k]);
        j += buf;
    }
    {
        char buf[640];
        std::snprintf(buf, sizeof(buf), ",\"Sweeps\":{\"SphereClosest\":[%llu,%llu,%llu],\"CapsuleClosest\":[%llu,%llu,%llu],"
                      "\"SphereAll\":[%llu,%llu,%llu],\"CapsuleAll\":[%llu,%llu,%llu]}",
                      (unsigned long long)s.SweepCalls[0], (unsigned long long)s.SweepWow[0], (unsigned long long)s.SweepVoided[0],
                      (unsigned long long)s.SweepCalls[1], (unsigned long long)s.SweepWow[1], (unsigned long long)s.SweepVoided[1],
                      (unsigned long long)s.SweepCalls[2], (unsigned long long)s.SweepWow[2], (unsigned long long)s.SweepVoided[2],
                      (unsigned long long)s.SweepCalls[3], (unsigned long long)s.SweepWow[3], (unsigned long long)s.SweepVoided[3]);
        j += buf;
    }
    j += ",\"Queries\":[";
    std::lock_guard<std::mutex> lock(s.Stats.Lock);
    bool first = true;
    for (auto const& q : s.Stats.Queries) {
        char buf[320];
        std::snprintf(buf, sizeof(buf), "%s[%d,%u,%u,%u,%d,%llu,[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],%d,%d]", first ? "" : ",",
                      q.Slot, q.Type, q.In, q.Ex, q.Context, (unsigned long long)q.Count, q.A.x, q.A.y, q.A.z, q.B.x, q.B.y,
                      q.B.z, q.Hits, q.Chars);
        j += buf;
        first = false;
    }
    j += "]}";
    return j;
}

}  // namespace

// ==== Part C: the camera (patch 0003 part C), FOV, and following benilla ====

namespace {

using CamVec3Proc = glm::vec3 const* (*)(void const*);
using CamQuatProc = float const* (*)(void const*);
using CamXformProc = bg3se::Transform const* (*)(void const*);
using CamFloatProc = float (*)(void*);
constexpr int kCamFovSlot = 4, kCamAspectSlot = 5;
constexpr int kCamTranslateSlot = 10, kCamRotateSlot = 12, kCamTransformSlot = 14, kCamVmtCopy = 40;
constexpr std::ptrdiff_t kCamShift = -16;
constexpr int kPoseRing = 8;

struct CamPose {
    glm::vec3 Translate{};
    float Rotate[4]{};
    bg3se::Transform Xform{};
};

struct CameraSwap {
    void* Controller = nullptr;
    void const* OldVmt = nullptr;
    std::unique_ptr<std::uint64_t[]> Table;
    CamPose Ring[kPoseRing];
    std::atomic<int> Current{-1};
    int Next = 0;
    int QuatOrder = 0;
    float OwnFov = 0;  // what BG3's GetFov returned at install (its unit)
    std::atomic<float> Fov{0.0f};  // > 0: served by GetFov
    std::atomic<bool> Hold{false};  // BG3's own camera for a moment (a harness click picks with it)
    double HoldUntil = 0;
    std::atomic<std::uint64_t> Calls[5]{}, Served[5]{};
};

CameraSwap g_cam;
struct KnownCamera {
    std::atomic<void const*> Controller{nullptr};
    CamVec3Proc Translate = nullptr;
    CamQuatProc Rotate = nullptr;
    CamXformProc Transform = nullptr;
    CamFloatProc Fov = nullptr, Aspect = nullptr;
};
KnownCamera g_known_cams[16];
std::atomic<int> g_known_cam_count{0};

glm::mat4* cam_inv_view(void* controller) {
    auto* c = static_cast<bg3se::rf::CameraController*>(controller);
    return reinterpret_cast<glm::mat4*>(reinterpret_cast<char*>(&c->Camera.InvViewMatrix) + kCamShift);
}

KnownCamera const& known_cam(void const* self) {
    const int n = g_known_cam_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_known_cams[i].Controller.load(std::memory_order_acquire) == self) return g_known_cams[i];
    }
    logf("camera query on an unknown controller %p", self);
    std::abort();
}

CamPose const* live_pose(void const* self) {
    if (self != g_cam.Controller || g_cam.Hold.load(std::memory_order_relaxed)) return nullptr;
    const int i = g_cam.Current.load(std::memory_order_acquire);
    return i < 0 ? nullptr : &g_cam.Ring[i];
}

CamPose const* clear_pose(void const* self, bool* tested = nullptr);  // leading: BG3's own pose, eye out of WoW's ground

glm::vec3 const* cam_translate_hook(void const* self) {
    g_cam.Calls[0].fetch_add(1, std::memory_order_relaxed);
    if (auto const* p = live_pose(self)) {
        g_cam.Served[0].fetch_add(1, std::memory_order_relaxed);
        return &p->Translate;
    }
    if (auto const* p = clear_pose(self)) return &p->Translate;
    return known_cam(self).Translate(self);
}
float const* cam_rotate_hook(void const* self) {
    g_cam.Calls[1].fetch_add(1, std::memory_order_relaxed);
    if (auto const* p = live_pose(self)) {
        g_cam.Served[1].fetch_add(1, std::memory_order_relaxed);
        return p->Rotate;
    }
    return known_cam(self).Rotate(self);
}
bg3se::Transform const* cam_transform_hook(void const* self) {
    g_cam.Calls[2].fetch_add(1, std::memory_order_relaxed);
    if (auto const* p = live_pose(self)) {
        g_cam.Served[2].fetch_add(1, std::memory_order_relaxed);
        return &p->Xform;
    }
    if (auto const* p = clear_pose(self)) return &p->Xform;
    return known_cam(self).Transform(self);
}
float cam_fov_hook(void* self) {
    g_cam.Calls[3].fetch_add(1, std::memory_order_relaxed);
    const float f = g_cam.Fov.load(std::memory_order_acquire);
    if (self == g_cam.Controller && f > 0.0f) {
        g_cam.Served[3].fetch_add(1, std::memory_order_relaxed);
        return f;
    }
    return known_cam(self).Fov(self);
}
float cam_aspect_hook(void* self) {
    g_cam.Calls[4].fetch_add(1, std::memory_order_relaxed);
    return known_cam(self).Aspect(self);
}

void quat_raw(glm::mat3 const& camToWorld, int order, float* out) {
    glm::quat q = glm::quat_cast(order >= 2 ? glm::transpose(camToWorld) : camToWorld);
    if ((order & 1) == 0) {
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    } else {
        out[0] = q.w; out[1] = q.x; out[2] = q.y; out[3] = q.z;
    }
}
float quat_err(float const* a, float const* b) {
    float same = 0, flip = 0;
    for (int i = 0; i < 4; ++i) {
        same += fabsf(a[i] - b[i]);
        flip += fabsf(a[i] + b[i]);
    }
    return std::min(same, flip);
}

void cam_remove() {
    g_cam.Current.store(-1, std::memory_order_release);
    g_cam.Fov.store(0.0f, std::memory_order_release);
    if (g_cam.Controller != nullptr && g_cam.OldVmt != nullptr) {
        void const* now = nullptr;
        safe_read(g_cam.Controller, &now, sizeof(now));
        if (now == g_cam.Table.get() + 2) {
            __atomic_store_n(reinterpret_cast<void const**>(g_cam.Controller), g_cam.OldVmt, __ATOMIC_RELEASE);
            logf("camera controller vtable restored");
        } else {
            logf("camera controller vtable changed since; left alone");
        }
    }
    retire(g_cam.Table, g_cam.OldVmt);
    g_cam.Controller = nullptr;
    g_cam.OldVmt = nullptr;
}

bool cam_install(void* controller, char const** why) {
    cam_remove();
    void const* vmt = nullptr;
    if (controller == nullptr || !safe_read(controller, &vmt, sizeof(vmt)) || vmt == nullptr) {
        *why = "unreadable camera controller";
        return false;
    }
    vmt = engine_vmt(vmt);  // a controller that kept an earlier copy of ours
    auto table = std::make_unique<std::uint64_t[]>(kCamVmtCopy + 2);
    if (!safe_read(static_cast<std::uint64_t const*>(vmt) - 2, table.get(), (kCamVmtCopy + 2) * 8)) {
        *why = "unreadable camera controller vtable";
        return false;
    }
    glm::mat4 inv{};
    if (!safe_read(cam_inv_view(controller), &inv, sizeof(inv))) {
        *why = "unreadable camera matrices";
        return false;
    }
    const auto translate = reinterpret_cast<CamVec3Proc>(table[2 + kCamTranslateSlot]);
    const auto rotate = reinterpret_cast<CamQuatProc>(table[2 + kCamRotateSlot]);
    const auto transform = reinterpret_cast<CamXformProc>(table[2 + kCamTransformSlot]);
    const auto fov = reinterpret_cast<CamFloatProc>(table[2 + kCamFovSlot]);
    const auto aspect = reinterpret_cast<CamFloatProc>(table[2 + kCamAspectSlot]);
    const glm::vec3 t = *translate(controller);
    float q[4];
    std::memcpy(q, rotate(controller), sizeof(q));
    const bg3se::Transform x = *transform(controller);
    const glm::vec3 eye{inv[3]};
    if (glm::length(t - eye) > 0.05f || glm::length(x.Translate - t) > 1e-3f) {
        logf("camera getters disagree: translate %.2f %.2f %.2f eye %.2f %.2f %.2f", t.x, t.y, t.z, eye.x, eye.y,
             eye.z);
        *why = "camera translate getter does not match the controller's eye (slots 10, 14)";
        return false;
    }
    int order = -1;
    for (int o = 0; o < 4 && order < 0; ++o) {
        float mine[4];
        quat_raw(glm::mat3(inv), o, mine);
        if (quat_err(mine, q) < 1e-2f) order = o;
    }
    if (order < 0 || quat_err(reinterpret_cast<float const*>(&x.RotationQuat), q) > 1e-3f) {
        logf("camera rotate getter %.3f %.3f %.3f %.3f matches no convention", q[0], q[1], q[2], q[3]);
        *why = "camera rotate getter does not match the controller's view (slots 12, 14)";
        return false;
    }
    const float ownFov = fov(controller);
    if (!std::isfinite(ownFov) || ownFov <= 0.0f || ownFov >= 180.0f) {
        *why = "camera GetFov (slot 4) does not return a field of view";
        return false;
    }
    bool known = false;
    const int n = g_known_cam_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_known_cams[i].Controller.load(std::memory_order_acquire) == controller) {
            if (g_known_cams[i].Translate != translate || g_known_cams[i].Rotate != rotate
                || g_known_cams[i].Transform != transform || g_known_cams[i].Fov != fov
                || g_known_cams[i].Aspect != aspect) {
                *why = "camera controller functions changed since it was first hooked";
                return false;
            }
            known = true;
        }
    }
    if (!known) {
        if (n >= 16) {
            *why = "too many camera controllers hooked in this process";
            return false;
        }
        g_known_cams[n].Translate = translate;
        g_known_cams[n].Rotate = rotate;
        g_known_cams[n].Transform = transform;
        g_known_cams[n].Fov = fov;
        g_known_cams[n].Aspect = aspect;
        g_known_cams[n].Controller.store(controller, std::memory_order_release);
        g_known_cam_count.store(n + 1, std::memory_order_release);
    }
    for (int i = 0; i < kPoseRing; ++i) g_cam.Ring[i].Xform = x;
    g_cam.QuatOrder = order;
    g_cam.OwnFov = ownFov;
    g_cam.Current.store(-1, std::memory_order_release);
    g_cam.Fov.store(0.0f, std::memory_order_release);
    for (auto& c : g_cam.Calls) c = 0;
    for (auto& c : g_cam.Served) c = 0;
    table[2 + kCamTranslateSlot] = reinterpret_cast<std::uint64_t>(&cam_translate_hook);
    table[2 + kCamRotateSlot] = reinterpret_cast<std::uint64_t>(&cam_rotate_hook);
    table[2 + kCamTransformSlot] = reinterpret_cast<std::uint64_t>(&cam_transform_hook);
    table[2 + kCamFovSlot] = reinterpret_cast<std::uint64_t>(&cam_fov_hook);
    table[2 + kCamAspectSlot] = reinterpret_cast<std::uint64_t>(&cam_aspect_hook);
    g_cam.Table = std::move(table);
    g_cam.Controller = controller;
    g_cam.OldVmt = vmt;
    __atomic_store_n(reinterpret_cast<void const**>(controller), (void const*)(g_cam.Table.get() + 2),
                     __ATOMIC_RELEASE);
    logf("camera controller %p hooked (rotation order %d, own fov %.4f)", controller, order, ownFov);
    return true;
}

void cam_pose(float const* view) {
    if (view == nullptr || g_cam.Controller == nullptr) {
        g_cam.Current.store(-1, std::memory_order_release);
        return;
    }
    glm::mat4 v;
    std::memcpy(&v, view, sizeof(v));
    const glm::mat4 inv = glm::inverse(v);
    CamPose& p = g_cam.Ring[g_cam.Next];
    p.Translate = glm::vec3(inv[3]);
    quat_raw(glm::mat3(inv), g_cam.QuatOrder, p.Rotate);
    std::memcpy(&p.Xform.RotationQuat, p.Rotate, sizeof(p.Rotate));
    p.Xform.Translate = p.Translate;
    g_cam.Current.store(g_cam.Next, std::memory_order_release);
    g_cam.Next = (g_cam.Next + 1) % kPoseRing;
}

bool cam_view(float* view16) {
    if (g_cam.Controller == nullptr) return false;
    auto* c = static_cast<bg3se::rf::CameraController*>(g_cam.Controller);
    return safe_read(reinterpret_cast<char*>(&c->Camera.ViewMatrix) + kCamShift, view16, 64);
}

// ---- following benilla's camera (the visible game owns the camera) ----
//
// benilla writes wow-bg3/link/camera.bin in place every frame (see link/README.md,
// "Camera stream"): WoW world coordinates, so the bridge basis set by Lua maps them.

struct WowCamRecord {
    char Magic[4];            // "WBC1"
    std::uint32_t Seq;
    float Eye[3], Forward[3], Up[3];  // WoW world (yards)
    float FovY;               // radians, vertical
    float Aspect;
    std::uint32_t Pad0;
    double Time;              // benilla's monotonic seconds
    std::uint32_t Seq2;       // equals Seq when the record is whole
    std::uint32_t Pad1;
};
static_assert(sizeof(WowCamRecord) == 72 && offsetof(WowCamRecord, Time) == 56 && offsetof(WowCamRecord, Seq2) == 64);

struct Basis {
    double Wow[3]{}, Bg3[3]{};
    double Yaw = 0;
    bool Mirror = false;  // bridge x negated (handedness), setting "mirror"
    bool Set = false;
};
Basis g_basis;

struct Follow {
    bool On = false;
    bool UseFov = false;
    int Fd = -1;
    int Retry = 0;
    std::uint32_t LastSeq = 0;
    double LastAt = 0;
    std::uint64_t Frames = 0, Stale = 0;
};
Follow g_follow;

glm::dvec3 bridge_vec(double x, double y, double z) {
    double bx = -y * 0.9144, by = z * 0.9144, bz = -x * 0.9144;
    if (g_basis.Mirror) bx = -bx;
    const double s = std::sin(g_basis.Yaw), c = std::cos(g_basis.Yaw);
    return {c * bx + s * bz, by, -s * bx + c * bz};
}
glm::dvec3 bridge_point(float const* p) {
    return bridge_vec(p[0] - g_basis.Wow[0], p[1] - g_basis.Wow[1], p[2] - g_basis.Wow[2])
        + glm::dvec3(g_basis.Bg3[0], g_basis.Bg3[1], g_basis.Bg3[2]);
}

// BG3's view convention (r48): world to view rows s, u, f with f forward and s = up x f.
void view_from(glm::dvec3 eye, glm::dvec3 f, glm::dvec3 upHint, float* v) {
    f = glm::normalize(f);
    glm::dvec3 s = glm::cross(upHint, f);
    if (glm::length(s) < 1e-6) s = glm::cross(glm::dvec3(0, 0, 1), f);
    s = glm::normalize(s);
    const glm::dvec3 u = glm::cross(f, s);
    const double m[16] = {s.x, u.x, f.x, 0, s.y, u.y, f.y, 0, s.z, u.z, f.z, 0,
                          -glm::dot(s, eye), -glm::dot(u, eye), -glm::dot(f, eye), 1};
    for (int i = 0; i < 16; ++i) v[i] = (float)m[i];
}

void follow_frame() {
    if (!g_follow.On || !g_basis.Set || g_cam.Controller == nullptr) return;
    if (g_follow.Fd < 0) {
        if (g_follow.Retry-- > 0) return;
        g_follow.Retry = 60;
        const std::string path = io_dir() + "link/camera.bin";
        g_follow.Fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (g_follow.Fd < 0) return;
        logf("follow: reading %s", path.c_str());
    }
    WowCamRecord rec{};
    if (::pread(g_follow.Fd, &rec, sizeof(rec), 0) != (ssize_t)sizeof(rec)) return;
    if (std::memcmp(rec.Magic, "WBC1", 4) != 0 || rec.Seq != rec.Seq2 || rec.Seq == g_follow.LastSeq) {
        ++g_follow.Stale;
        return;
    }
    for (float f : {rec.Eye[0], rec.Eye[1], rec.Eye[2], rec.Forward[0], rec.Forward[1], rec.Forward[2], rec.Up[0],
                    rec.Up[1], rec.Up[2], rec.FovY}) {
        if (!std::isfinite(f)) return;
    }
    g_follow.LastSeq = rec.Seq;
    g_follow.LastAt = mono_seconds();
    const glm::dvec3 eye = bridge_point(rec.Eye);
    const glm::dvec3 f = bridge_vec(rec.Forward[0], rec.Forward[1], rec.Forward[2]);
    const glm::dvec3 up = bridge_vec(rec.Up[0], rec.Up[1], rec.Up[2]);
    if (glm::length(f) < 1e-6) return;
    float view[16];
    view_from(eye, f, up, view);
    cam_pose(view);
    if (g_follow.UseFov && rec.FovY > 0.05f && rec.FovY < 3.0f) {
        // GetFov's unit is BG3's own: degrees if what it returned at install looks like degrees.
        const float fov = g_cam.OwnFov > 3.2f ? rec.FovY * 57.29577951f : rec.FovY;
        g_cam.Fov.store(fov, std::memory_order_release);
    } else {
        g_cam.Fov.store(0.0f, std::memory_order_release);
    }
    ++g_follow.Frames;
}

// ---- leading: BG3 owns the camera and benilla follows (BG3 window is the game) ----
//
// Every frame BG3's own camera pose (the original getters, never a served pose; with the eye
// clearance below applied, as BG3 renders it) is written,
// mapped back to WoW world coordinates with the same basis, to wow-bg3/link/bg3cam.bin:
// 96 bytes, "BGC1", seq, eye[3], forward[3], up[3] (WoW yards), vertical fov (radians),
// aspect, flags u32 (1: the eye clearance tested this eye), time f64, seq again, pad, then BG3's projection terms P00, P11, P22, P32, P23,
// P33 (glm column-major [col][row]) for the compositor's depth comparison. Readers check
// seq == seq2.

struct Lead {
    bool On = false;
    int Fd = -1;
    std::uint32_t Seq = 0;
    std::uint64_t Frames = 0;
};
Lead g_lead;

// ---- eye clearance while leading: BG3's camera knows nothing of WoW's hills ----
//
// On a hillside BG3's orbit camera can sit inside WoW's ground or a building, and WoW's own
// clearance (benilla) then moved WoW's eye alone, so BG3's overlays no longer lined up. Here BG3's
// eye itself moves out, before BG3 uses it: the camera getters (slots 10 and 14) serve BG3's own
// pose with the eye slid toward Tav's chest along the line from her chest to the eye, to just before
// the first WoW terrain or WMO face on it (what WoW's camera collides with; M2 trees and fences do
// not count, nor the synthetic walls), then on until it is 0.75 yd above WoW's ground under it (at
// most 90% of the way, as benilla did). The rotation stays BG3's, so the cursor's picking (served eye,
// the controller's own rotation) agrees, and lead_frame streams the same eye to benilla, whose own
// clearance (0.6 yd) then never fires. Computed once per new BG3 eye (or Tav position) and cached in
// a ring like the served poses. Tav's position comes from link/tav.bin (WorldMode.lua), read once a
// frame. Off with WOWBG3_CAM_CLEAR=0 in BG3's environment.

constexpr float kChestM = 1.37f;      // 1.5 yd above Tav's feet: where the eye slides toward
constexpr float kEyeClearM = 0.69f;   // 0.75 yd above WoW's ground under the eye
constexpr float kEyeMarginM = 0.3f;   // stays this far in front of the face it would cross
// Indoors ("going indoors, the camera tries to move above the
// building"). BG3's orbit eye sits 7 to 10 m over her; inside a WoW building one thin line from her chest
// found a way out through a doorway, a window, an arch or a seam between a room's ceiling and the next
// room's, so the eye popped out above the room (harness: in Northshire Abbey's side rooms the eye stood
// 1 m over the room's ceiling with nothing on the line) and back in a step later. Now: the line is thick
// (four more lines kEyeRadiusM to its sides, the eye's own size), the eye stays kCeilingMarginM under the
// ceiling over her (a WoW face within kCeilingReachM over her chest, the middle of five rays), and the eye
// comes back out at most kEyeEaseOutMps (pulled in at once, as WoW's own camera does).
// What eases is the pull-in, not the eye's distance: easing the distance held the eye in
// whenever it grew, and it grows on its own at every Lua tick while she walks (link/tav.bin moves her
// chest 30 times a second, the eye moves every frame) and when the player zooms out. In the
// 0.1.0 playtest that held 48517 eyes with nothing in the way, a shake while walking. And the line starts
// where BG3's camera looks (its forward line's first WoW ground, raised to chest height along that line),
// Tav's chest only when that ground is not there: in a fight the camera follows whoever's turn it is, and
// a line from Tav's chest to that eye crossed hills and pulled the camera toward her (26 m in his session).
constexpr float kEyeRadiusM = 0.25f;
constexpr float kCeilingReachM = 20.0f;
constexpr float kCeilingMarginM = 0.45f;
constexpr float kEyeEaseOutMps = 3.0f;
constexpr float kLookReachM = 80.0f;     // how far along BG3's forward line its look point is searched
constexpr float kAnchorJumpM = 4.0f;     // a look point this far from the last: a new target, no ease

struct CamClear {
    bool On = true;
    std::atomic<float> TavX{0.0f}, TavY{0.0f}, TavZ{0.0f};
    std::atomic<std::uint32_t> TavGen{0};  // 0: no position yet
    std::uint32_t TavSeq = 0;
    struct Entry {
        void const* Controller = nullptr;
        glm::vec3 Key{};          // that controller's own eye this was computed for
        std::uint32_t Gen = 0;    // and Tav's position generation
        bool Moved = false;
        bool Tested = false;      // WoW's ground was there to test it against
        bool Easing = false;      // held in by the ease out: computed again next frame
        std::uint64_t Frame = 0;  // g_clear.Frame when computed
        CamPose Pose;
    };
    // Per controller: 0 the game camera, 1 the lead source (the action camera while it renders).
    Entry Ring[2][kPoseRing];
    std::atomic<int> Current[2]{-1, -1};
    int Next[2]{0, 0};
    std::mutex Lock;
    std::atomic<std::uint64_t> Computed{0}, Moved{0};
    std::atomic<float> LastDist{0.0f}, MaxDist{0.0f};
    // Indoors: eyes with a ceiling over her, eyes the ceiling or the thick line pulled in
    // further than the one line did, eyes held in by the ease out.
    std::atomic<std::uint64_t> Indoor{0}, Capped{0}, Eased{0};
    std::atomic<std::uint64_t> Looked{0};  // lines started where BG3's camera looks, not at Tav's chest
    std::atomic<float> Ceiling{0.0f};  // the last ceiling's height over her chest (m), 0 outdoors
    // The ease out per controller (under Lock): how far the last served eye was pulled in along the line
    // (m), from which anchor, and when; Frame: lead_frame's count, so an easing entry is computed again
    // once a frame.
    float EaseIn[2]{0.0f, 0.0f};
    glm::vec3 EaseAnchor[2]{};
    double EaseAt[2]{0.0, 0.0};
    bool EaseHave[2]{false, false};
    std::atomic<std::uint64_t> Frame{0};
};
CamClear g_clear;

// ---- the lead source: BG3's camera that renders (WoW's camera mimics BG3's
// action camera during casts and attacks) ----
//
// During a cast or an attack BG3 renders from its action camera, another Camera entity with its own
// controller, while the game camera keeps its own pose. Arena.lua finds the camera that renders each
// tick and names its controller with LeadSource; the plugin then hooks that controller's eye getters
// too (a private vtable copy, as cam_install does: the eye clearance applies to what BG3 renders) and
// lead_frame streams its pose, field of view and projection. LeadSource 0 goes back to the game
// camera and unhooks the action camera (only while its vtable is still ours).
// Its hooks find the engine's functions in the table copy itself (after its kCamVmtCopy slots: a
// marker, then the original GetWorldTranslate and GetWorldTransform), or, once the object has its own
// table back, in that table: an action camera lives for one cast, so its hooks may still be running
// for an old object after the next one took over. Such cameras are not registered in g_known_cams.
constexpr std::uint64_t kSrcMark = 0x5742334c45414431ull;  // "WB3LEAD1"
constexpr int kSrcTable = kCamVmtCopy + 2 + 3;

struct LeadSource {
    std::atomic<void*> Controller{nullptr};  // null: the game camera leads
    void const* OldVmt = nullptr;
    std::unique_ptr<std::uint64_t[]> Table;
    CamXformProc Transform = nullptr;
    CamFloatProc Fov = nullptr, Aspect = nullptr;
    int QuatOrder = 0;
    bool Degrees = false;        // its GetFov reports degrees
    std::uint64_t Switches = 0;
};
LeadSource g_src;
std::recursive_mutex g_src_lock;  // LeadSource (client Lua) vs lead_frame (frame handler); hooks never take it

std::uint64_t src_original(void const* self, int slot, int extra) {
    auto const* vt = *reinterpret_cast<std::uint64_t const* const*>(self);
    return vt[kCamVmtCopy] == kSrcMark ? vt[kCamVmtCopy + extra] : vt[slot];
}

glm::vec3 const* src_translate_hook(void const* self) {
    if (auto const* p = clear_pose(self)) return &p->Translate;
    return reinterpret_cast<CamVec3Proc>(src_original(self, kCamTranslateSlot, 1))(self);
}
bg3se::Transform const* src_transform_hook(void const* self) {
    if (auto const* p = clear_pose(self)) return &p->Xform;
    return reinterpret_cast<CamXformProc>(src_original(self, kCamTransformSlot, 2))(self);
}

void src_remove(char const* why) {
    std::lock_guard<std::recursive_mutex> held(g_src_lock);
    void* c = g_src.Controller.exchange(nullptr);  // lead_frame and the eye clearance stop using it first
    if (c == nullptr) return;
    void const* now = nullptr;
    if (safe_read(c, &now, sizeof(now)) && now == g_src.Table.get() + 2) {
        __atomic_store_n(reinterpret_cast<void const**>(c), g_src.OldVmt, __ATOMIC_RELEASE);
    }
    retire(g_src.Table, g_src.OldVmt);
    g_src.OldVmt = nullptr;
    logf("lead source back to the game camera (%s)", why);
}

bool src_install(void* controller, char const** why) {
    std::lock_guard<std::recursive_mutex> held(g_src_lock);
    src_remove("another camera renders");
    if (controller == nullptr || controller == g_cam.Controller) return true;
    void const* vmt = nullptr;
    if (!safe_read(controller, &vmt, sizeof(vmt)) || vmt == nullptr) {
        *why = "unreadable camera controller";
        return false;
    }
    vmt = engine_vmt(vmt);
    auto table = std::make_unique<std::uint64_t[]>(kSrcTable);
    glm::mat4 inv{};
    if (!safe_read(static_cast<std::uint64_t const*>(vmt) - 2, table.get(), (kCamVmtCopy + 2) * 8)
        || !safe_read(cam_inv_view(controller), &inv, sizeof(inv))) {
        *why = "unreadable camera controller vtable or matrices";
        return false;
    }
    const auto translate = reinterpret_cast<CamVec3Proc>(table[2 + kCamTranslateSlot]);
    const auto rotate = reinterpret_cast<CamQuatProc>(table[2 + kCamRotateSlot]);
    const auto transform = reinterpret_cast<CamXformProc>(table[2 + kCamTransformSlot]);
    const auto fov = reinterpret_cast<CamFloatProc>(table[2 + kCamFovSlot]);
    const auto aspect = reinterpret_cast<CamFloatProc>(table[2 + kCamAspectSlot]);
    const glm::vec3 t = *translate(controller);
    const bg3se::Transform x = *transform(controller);
    float q[4];
    std::memcpy(q, rotate(controller), sizeof(q));
    if (glm::length(x.Translate - t) > 1e-3f || glm::length(t - glm::vec3(inv[3])) > 0.05f) {
        *why = "the camera's eye getters do not match its own view";
        return false;
    }
    int order = -1;
    for (int o = 0; o < 4 && order < 0; ++o) {
        float mine[4];
        quat_raw(glm::mat3(inv), o, mine);
        if (quat_err(mine, q) < 1e-2f) order = o;
    }
    const float f = fov(controller);
    if (order < 0 || !std::isfinite(f) || f <= 0.0f || f >= 180.0f) {
        *why = "the camera's rotation or field of view getter matches no convention";
        return false;
    }
    table[2 + kCamVmtCopy] = kSrcMark;
    table[2 + kCamVmtCopy + 1] = reinterpret_cast<std::uint64_t>(translate);
    table[2 + kCamVmtCopy + 2] = reinterpret_cast<std::uint64_t>(transform);
    table[2 + kCamTranslateSlot] = reinterpret_cast<std::uint64_t>(&src_translate_hook);
    table[2 + kCamTransformSlot] = reinterpret_cast<std::uint64_t>(&src_transform_hook);
    g_src.Table = std::move(table);
    g_src.OldVmt = vmt;
    g_src.Transform = transform;
    g_src.Fov = fov;
    g_src.Aspect = aspect;
    g_src.QuatOrder = order;
    g_src.Degrees = f > 3.2f;
    ++g_src.Switches;
    g_src.Controller.store(controller, std::memory_order_release);  // before the swap: its hooks find it
    __atomic_store_n(reinterpret_cast<void const**>(controller), (void const*)(g_src.Table.get() + 2), __ATOMIC_RELEASE);
    logf("lead source: camera %p renders (rotation order %d, fov %.3f)", controller, order, f);
    return true;
}

// The controller BG3 renders from now: the lead source while its vtable is still ours, else the
// game camera.
void* lead_controller() {
    if (void* c = g_src.Controller.load(std::memory_order_acquire); c != nullptr) {
        void const* now = nullptr;
        if (safe_read(c, &now, sizeof(now)) && now == g_src.Table.get() + 2) return c;
        src_remove("its vtable changed (camera gone)");
    }
    return g_cam.Controller;
}

// Tav's BG3 position from link/tav.bin ("TAV1", seq, WoW xyz, BG3 xyz, yaw, flags, seq again):
// opened and read whole every frame (WorldMode.lua rewrites the file).
void clear_read_tav() {
    const std::string path = io_dir() + "link/tav.bin";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    unsigned char rec[44];
    const ssize_t got = ::pread(fd, rec, sizeof(rec), 0);
    ::close(fd);
    std::uint32_t seq = 0, seq2 = 0;
    float p[3];
    if (got != (ssize_t)sizeof(rec) || std::memcmp(rec, "TAV1", 4) != 0) return;
    std::memcpy(&seq, rec + 4, 4);
    std::memcpy(&seq2, rec + 40, 4);
    std::memcpy(p, rec + 20, 12);
    if (seq == 0 || seq != seq2 || seq == g_clear.TavSeq || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        return;
    }
    g_clear.TavSeq = seq;
    g_clear.TavX.store(p[0], std::memory_order_relaxed);
    g_clear.TavY.store(p[1], std::memory_order_relaxed);
    g_clear.TavZ.store(p[2], std::memory_order_relaxed);
    g_clear.TavGen.fetch_add(1, std::memory_order_release);
}

// The point the clearance line starts from: where BG3's camera looks (the first WoW ground on its forward
// line, found from the far end back so an eye inside a hill still finds it), kChestM above that ground
// along the same line; Tav's chest when the line meets no ground (looking level or up, or off the
// triangles).
glm::vec3 clear_anchor(ArenaRays const& r, glm::vec3 const& eye0, glm::vec3 const& fwd, glm::vec3 const& tav) {
    const glm::vec3 chest = tav + glm::vec3(0.0f, kChestM, 0.0f);
    if (!(fwd.y < -0.15f)) return chest;
    const glm::vec3 deep = eye0 + fwd * kLookReachM;
    float t = 2.0f;
    glm::vec3 n{};
    if (!wow_closest(r, deep, eye0, t, n, kCameraKinds)) return chest;
    const glm::vec3 look = deep + (eye0 - deep) * t;
    g_clear.Looked.fetch_add(1, std::memory_order_relaxed);
    return look - fwd * (kChestM / -fwd.y);
}

// The eye moved out of WoW's ground and buildings, or false when it is clear already or there is
// nothing to test against (no triangles on the client or none under the eye, no Tav position);
// *tested says which, *anchor where the line starts.
bool clear_eye(glm::vec3 const& eye0, glm::vec3 const& fwd, glm::vec3 const& tav, float* target, bool* tested,
               glm::vec3* anchor) {
    *tested = false;
    *target = 1.0f;
    ArenaRays const* r = g_scene[1].Rays.load(std::memory_order_acquire);
    if (r == nullptr || r->Tris.empty() || !inside_square(*r, eye0)) return false;
    const glm::vec3 chest = clear_anchor(*r, eye0, fwd, tav);
    *anchor = chest;
    const glm::vec3 d = eye0 - chest;
    const float len = glm::length(d);
    // An eye 60 m or more from the anchor is BG3's camera before it caught up with her (a world mode
    // start teleports her 1.2 km): left alone.
    if (!(len > 0.5f) || len > 60.0f) return false;
    *tested = true;
    float f = 1.0f, t = 2.0f;
    glm::vec3 n{};
    if (wow_closest(*r, chest, eye0, t, n, kCameraKinds)) f = std::max(0.0f, t - kEyeMarginM / len);
    const float thin = f;
    // The thick line: four lines kEyeRadiusM to the sides of the first. A hit within the
    // first kEyeRadiusM * 2 of a side line is a wall she stands against, not one between her and the eye.
    const glm::vec3 dn = d / len;
    glm::vec3 side = glm::cross(dn, glm::vec3(0.0f, 1.0f, 0.0f));
    side = glm::length(side) > 1e-3f ? glm::normalize(side) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up2 = glm::normalize(glm::cross(side, dn));
    for (glm::vec3 const& o : {side, -side, up2, -up2}) {
        const glm::vec3 off = o * kEyeRadiusM;
        t = 2.0f;  // each ray its own nearest hit (a shared t only took hits nearer than the last ray's)
        if (wow_closest(*r, chest + off, eye0 + off, t, n, kCameraKinds) && t * len > kEyeRadiusM * 2.0f) {
            f = std::min(f, std::max(0.0f, t - kEyeMarginM / len));
        }
    }
    // The ceiling over her: the middle of five rays up from her chest (one beam or one hole does not
    // decide); none or two hits of five: outdoors.
    float hits[5];
    int nh = 0;
    for (glm::vec2 const& o : {glm::vec2(0.0f), glm::vec2(0.5f, 0.0f), glm::vec2(-0.5f, 0.0f), glm::vec2(0.0f, 0.5f),
                               glm::vec2(0.0f, -0.5f)}) {
        const glm::vec3 a = chest + glm::vec3(o.x, 0.0f, o.y);
        t = 2.0f;
        if (wow_closest(*r, a, a + glm::vec3(0.0f, kCeilingReachM, 0.0f), t, n, kCameraKinds)) hits[nh++] = t * kCeilingReachM;
    }
    if (nh >= 3) {
        std::sort(hits, hits + nh);
        const float ceiling = hits[nh / 2];
        g_clear.Indoor.fetch_add(1, std::memory_order_relaxed);
        g_clear.Ceiling.store(ceiling, std::memory_order_relaxed);
        if (d.y > 1e-3f) f = std::min(f, std::max(0.0f, (ceiling - kCeilingMarginM) / d.y));
    } else {
        g_clear.Ceiling.store(0.0f, std::memory_order_relaxed);
    }
    if (f < thin - 1e-3f) g_clear.Capped.fetch_add(1, std::memory_order_relaxed);
    auto clear = [&](glm::vec3 const& q) {
        float tt = 2.0f;
        glm::vec3 nn{};
        return !wow_closest(*r, q + glm::vec3(0.0f, 0.05f, 0.0f), q - glm::vec3(0.0f, kEyeClearM, 0.0f), tt, nn,
                            kCameraKinds);
    };
    if (f >= 1.0f && clear(eye0)) return true;
    f = std::min(f, 1.0f);
    while (f > 0.1f && !clear(chest + d * f)) f -= 0.05f;
    *target = std::max(f, 0.1f);
    return true;
}

CamPose const* clear_pose(void const* self, bool* tested) {
    if (tested != nullptr) *tested = false;
    if (!g_clear.On || !g_lead.On || self == nullptr) return nullptr;
    const int slot = self == g_cam.Controller ? 0 : self == g_src.Controller.load(std::memory_order_acquire) ? 1 : -1;
    if (slot < 0) return nullptr;
    const std::uint32_t gen = g_clear.TavGen.load(std::memory_order_acquire);
    if (gen == 0) return nullptr;
    bg3se::Transform const* x = slot == 0 ? known_cam(self).Transform(self)
        : reinterpret_cast<CamXformProc>(src_original(self, kCamTransformSlot, 2))(self);
    if (x == nullptr) return nullptr;
    const glm::vec3 eye0 = x->Translate;
    auto& ring = g_clear.Ring[slot];
    const std::uint64_t frame = g_clear.Frame.load(std::memory_order_acquire);
    auto cached = [&](int i) {
        return i >= 0 && ring[i].Controller == self && ring[i].Gen == gen && ring[i].Key == eye0
            && (!ring[i].Easing || ring[i].Frame == frame);
    };
    auto found = [&](int i) -> CamPose const* {
        if (tested != nullptr) *tested = ring[i].Tested;
        return ring[i].Moved ? &ring[i].Pose : nullptr;
    };
    int i = g_clear.Current[slot].load(std::memory_order_acquire);
    if (cached(i)) return found(i);
    std::lock_guard<std::mutex> held(g_clear.Lock);
    i = g_clear.Current[slot].load(std::memory_order_acquire);
    if (cached(i)) return found(i);
    const glm::vec3 tav{g_clear.TavX.load(std::memory_order_relaxed), g_clear.TavY.load(std::memory_order_relaxed),
                        g_clear.TavZ.load(std::memory_order_relaxed)};
    // BG3's forward (the third column of the camera-to-world rotation, as lead_frame reads it).
    float raw[4];
    std::memcpy(raw, &x->RotationQuat, sizeof(raw));
    const int order = slot == 0 ? g_cam.QuatOrder : g_src.QuatOrder;
    const glm::quat q = (order & 1) == 0 ? glm::quat(raw[3], raw[0], raw[1], raw[2]) : glm::quat(raw[0], raw[1], raw[2], raw[3]);
    glm::mat3 rot = glm::mat3_cast(q);
    if (order >= 2) rot = glm::transpose(rot);
    const glm::vec3 fwd = glm::normalize(rot[2]);
    bool wasTested = false;
    float target = 1.0f;
    glm::vec3 chest = tav + glm::vec3(0.0f, kChestM, 0.0f);
    clear_eye(eye0, fwd, tav, &target, &wasTested, &chest);
    // The ease out: pulled in at once, the pull-in shrinks at most kEyeEaseOutMps.
    const float len = glm::length(eye0 - chest);
    float served = target;
    bool easing = false;
    const double now = mono_seconds();
    if (!wasTested || !(len > 0.5f)) {
        g_clear.EaseHave[slot] = false;
    } else {
        float in = (1.0f - target) * len;
        if (g_clear.EaseHave[slot] && in < g_clear.EaseIn[slot]
            && glm::length(chest - g_clear.EaseAnchor[slot]) < kAnchorJumpM) {
            const double dt = std::clamp(now - g_clear.EaseAt[slot], 0.0, 0.1);
            const float least = g_clear.EaseIn[slot] - kEyeEaseOutMps * (float)dt;
            if (least > in + 1e-3f) {
                in = least;
                easing = true;
                g_clear.Eased.fetch_add(1, std::memory_order_relaxed);
            }
        }
        in = std::min(in, 0.9f * len);
        g_clear.EaseIn[slot] = in;
        g_clear.EaseAnchor[slot] = chest;
        g_clear.EaseAt[slot] = now;
        g_clear.EaseHave[slot] = true;
        served = 1.0f - in / len;
    }
    const bool moved = wasTested && served < 0.999f;
    const glm::vec3 eye = chest + (eye0 - chest) * served;
    CamClear::Entry& e = ring[g_clear.Next[slot]];
    e.Controller = self;
    e.Key = eye0;
    e.Gen = gen;
    e.Moved = moved;
    e.Tested = wasTested;
    e.Easing = easing;
    e.Frame = frame;
    if (tested != nullptr) *tested = wasTested;
    if (moved) {
        e.Pose.Xform = *x;
        e.Pose.Xform.Translate = eye;
        e.Pose.Translate = eye;
        std::memcpy(e.Pose.Rotate, &x->RotationQuat, sizeof(e.Pose.Rotate));
        const float dist = glm::length(eye - eye0);
        g_clear.Moved.fetch_add(1, std::memory_order_relaxed);
        g_clear.LastDist.store(dist, std::memory_order_relaxed);
        if (dist > g_clear.MaxDist.load(std::memory_order_relaxed)) g_clear.MaxDist.store(dist, std::memory_order_relaxed);
    } else {
        g_clear.LastDist.store(0.0f, std::memory_order_relaxed);
    }
    g_clear.Computed.fetch_add(1, std::memory_order_relaxed);
    g_clear.Current[slot].store(g_clear.Next[slot], std::memory_order_release);
    g_clear.Next[slot] = (g_clear.Next[slot] + 1) % kPoseRing;
    return moved ? &e.Pose : nullptr;
}

glm::dvec3 wow_vec(glm::dvec3 b) {
    const double s = std::sin(g_basis.Yaw), c = std::cos(g_basis.Yaw);
    double bx = c * b.x - s * b.z, bz = s * b.x + c * b.z;  // undo the yaw
    if (g_basis.Mirror) bx = -bx;
    return {-bz / 0.9144, -bx / 0.9144, b.y / 0.9144};
}
glm::dvec3 wow_point(glm::dvec3 p) {
    return wow_vec(p - glm::dvec3(g_basis.Bg3[0], g_basis.Bg3[1], g_basis.Bg3[2]))
        + glm::dvec3(g_basis.Wow[0], g_basis.Wow[1], g_basis.Wow[2]);
}

void lead_frame() {
    if (!g_lead.On || !g_basis.Set || g_cam.Controller == nullptr) return;
    std::lock_guard<std::recursive_mutex> held(g_src_lock);
    void* ctl = lead_controller();  // the game camera, or its action camera while that renders
    const bool src = ctl != g_cam.Controller;
    const int order = src ? g_src.QuatOrder : g_cam.QuatOrder;
    KnownCamera const& k = known_cam(g_cam.Controller);
    const CamXformProc getTransform = src ? g_src.Transform : k.Transform;
    const CamFloatProc getFov = src ? g_src.Fov : k.Fov, getAspect = src ? g_src.Aspect : k.Aspect;
    bg3se::Transform const* x = getTransform(ctl);
    if (x == nullptr) return;
    bool tested = false;
    if (g_clear.On) {
        clear_read_tav();
        if (auto const* p = clear_pose(ctl, &tested)) x = &p->Xform;  // the eye BG3 renders from
    }
    float raw[4];
    std::memcpy(raw, &x->RotationQuat, sizeof(raw));
    glm::quat q = (order & 1) == 0 ? glm::quat(raw[3], raw[0], raw[1], raw[2]) : glm::quat(raw[0], raw[1], raw[2], raw[3]);
    glm::mat3 m = glm::mat3_cast(q);
    if (order >= 2) m = glm::transpose(m);
    // Columns of the camera-to-world rotation are BG3's s, u, f (see view_from).
    const glm::dvec3 eye = wow_point(glm::dvec3(x->Translate));
    const glm::dvec3 f = wow_vec(glm::dvec3(m[2])), u = wow_vec(glm::dvec3(m[1]));
    float fov = getFov(ctl);
    if (src ? g_src.Degrees : g_cam.OwnFov > 3.2f) fov *= 0.01745329252f;  // BG3 reports degrees
    const float aspect = getAspect(ctl);
    unsigned char rec[96] = {};
    const std::uint32_t seq = ++g_lead.Seq;
    const float vals[11] = {(float)eye.x, (float)eye.y, (float)eye.z, (float)f.x, (float)f.y, (float)f.z,
                            (float)u.x, (float)u.y, (float)u.z, fov, aspect};
    for (float v : vals) {
        if (!std::isfinite(v)) return;
    }
    std::memcpy(rec, "BGC1", 4);
    std::memcpy(rec + 4, &seq, 4);
    std::memcpy(rec + 8, vals, sizeof(vals));
    // Flags at 52 (was padding): 1 = the eye clearance tested this eye against WoW's ground (benilla
    // then leaves it alone: its own clearance is only for eyes the plugin could not test).
    const std::uint32_t flags = tested ? 1u : 0u;
    std::memcpy(rec + 52, &flags, 4);
    const double now = mono_seconds();
    std::memcpy(rec + 56, &now, 8);
    std::memcpy(rec + 64, &seq, 4);
    glm::mat4 proj{};
    auto* cc = static_cast<bg3se::rf::CameraController*>(ctl);
    if (safe_read(reinterpret_cast<char*>(&cc->Camera.ProjectionMatrix) + kCamShift, &proj, sizeof(proj))) {
        const float p[6] = {proj[0][0], proj[1][1], proj[2][2], proj[3][2], proj[2][3], proj[3][3]};
        std::memcpy(rec + 72, p, sizeof(p));
    }
    if (g_lead.Fd < 0) {
        const std::string path = io_dir() + "link/bg3cam.bin";
        g_lead.Fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (g_lead.Fd < 0) return;
        logf("lead: writing %s", path.c_str());
    }
    if (::pwrite(g_lead.Fd, rec, sizeof(rec), 0) != (ssize_t)sizeof(rec)) {
        ::close(g_lead.Fd);
        g_lead.Fd = -1;
        return;
    }
    ++g_lead.Frames;
    g_clear.Frame.fetch_add(1, std::memory_order_release);
    if (g_lead.Frames % 600 == 1) {
        logf("lead: eye clearance moved %llu of %llu eyes (last %.2f m, max %.2f m); indoors %llu (ceiling %.2f m "
             "over her chest), pulled in by the ceiling or the thick line %llu, held by the ease out %llu; "
             "started where BG3 looks %llu",
             (unsigned long long)g_clear.Moved.load(), (unsigned long long)g_clear.Computed.load(),
             g_clear.LastDist.load(), g_clear.MaxDist.load(), (unsigned long long)g_clear.Indoor.load(),
             g_clear.Ceiling.load(), (unsigned long long)g_clear.Capped.load(), (unsigned long long)g_clear.Eased.load(),
             (unsigned long long)g_clear.Looked.load());
    }
}

}  // namespace

// ==== Call counting (patch 0003 diagnosis) ====

namespace {

struct CountSwap {
    void* Object = nullptr;
    void const* OldVmt = nullptr;
    std::unique_ptr<std::uint64_t[]> Table;
    unsigned char* Page = nullptr;
    int Slots = 0;
};
CountSwap g_count;
constexpr int kCountMaxSlots = 48;
constexpr std::size_t kThunkSize = 16;
int g_count_pages = 0;

void count_remove() {
    if (g_count.Object != nullptr && g_count.OldVmt != nullptr) {
        void const* now = nullptr;
        safe_read(g_count.Object, &now, sizeof(now));
        if (now == g_count.Table.get() + 2) {
            __atomic_store_n(reinterpret_cast<void const**>(g_count.Object), g_count.OldVmt, __ATOMIC_RELEASE);
        }
    }
    retire(g_count.Table, g_count.OldVmt);
    g_count.Object = nullptr;
    g_count.OldVmt = nullptr;
    g_count.Page = nullptr;
    g_count.Slots = 0;
}

bool count_install(void* object, int slots, char const** why) {
    count_remove();
    if (slots < 1 || slots > kCountMaxSlots) {
        *why = "slot count out of range";
        return false;
    }
    if (g_count_pages >= 32) {
        *why = "too many counting installs in this process";
        return false;
    }
    void const* vmt = nullptr;
    if (object == nullptr || !safe_read(object, &vmt, sizeof(vmt)) || vmt == nullptr) {
        *why = "unreadable object";
        return false;
    }
    for (auto const& t : g_retired_tables) {
        if (vmt == t.Table.get() + 2) {
            *why = "the object still uses an earlier table";
            return false;
        }
    }
    if (g_cam.Table && vmt == g_cam.Table.get() + 2) {
        *why = "the object is hooked for the camera";
        return false;
    }
    for (auto const& s : g_scene) {
        if (s.Table && vmt == s.Table.get() + 2) {
            *why = "the object is a hooked physics scene";
            return false;
        }
    }
    auto table = std::make_unique<std::uint64_t[]>(kCountMaxSlots + 2);
    if (!safe_read(static_cast<std::uint64_t const*>(vmt) - 2, table.get(), (kCountMaxSlots + 2) * 8)) {
        *why = "unreadable vtable";
        return false;
    }
    const std::size_t page = 4096;
    void* mem = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        *why = "no memory for thunks";
        return false;
    }
    auto* base = static_cast<unsigned char*>(mem);
    unsigned char* thunks = base;
    auto* originals = reinterpret_cast<std::uint64_t*>(base + page);
    auto* counters = reinterpret_cast<std::uint64_t*>(base + page + 1024);
    for (int i = 0; i < slots; ++i) {
        unsigned char* t = thunks + i * kThunkSize;
        originals[i] = table[2 + i];
        counters[i] = 0;
        const std::int32_t relCount = (std::int32_t)(reinterpret_cast<unsigned char*>(&counters[i]) - (t + 8));
        t[0] = 0xF0; t[1] = 0x48; t[2] = 0xFF; t[3] = 0x05;
        std::memcpy(t + 4, &relCount, 4);
        const std::int32_t relJump = (std::int32_t)(reinterpret_cast<unsigned char*>(&originals[i]) - (t + 14));
        t[8] = 0xFF; t[9] = 0x25;
        std::memcpy(t + 10, &relJump, 4);
        t[14] = 0xCC; t[15] = 0xCC;
    }
    if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0) {
        *why = "thunk page cannot be made executable";
        return false;
    }
    ++g_count_pages;
    for (int i = 0; i < slots; ++i) table[2 + i] = reinterpret_cast<std::uint64_t>(thunks + i * kThunkSize);
    g_count.Table = std::move(table);
    g_count.Object = object;
    g_count.OldVmt = vmt;
    g_count.Page = base;
    g_count.Slots = slots;
    __atomic_store_n(reinterpret_cast<void const**>(object), (void const*)(g_count.Table.get() + 2),
                     __ATOMIC_RELEASE);
    logf("counting %d slots of %p", slots, object);
    return true;
}

}  // namespace

// ==== Read-only AI grid survey (void arena research) ====

namespace {

std::uintptr_t bias() { return g_api.LoadBias != nullptr ? g_api.LoadBias() : 0; }

// Writes native-<side>-grid.json (subgrids, patches, grid counts) and native-<side>-grid.bin
// (per subgrid: <u32 id, u32 w, u32 h, f32 translateY, i32 xg, i32 zg, f32 xl, f32 zl> then
// w*h tiles <u64 flags, u16 min, u16 max, u16 meta, u16 surface meta, u64 internal tile>). Reads only.
bool grid_survey(bool client, int* counts, char const** why) {
    auto* grid = ai_grid(client);
    if (grid == nullptr) {
        *why = "no AI grid";
        return false;
    }
    std::string j = "{";
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "\"Grid\":\"%p\",\"Vmt\":\"%#lx\",\"MetaData\":%u,\"SurfaceMetaData\":%u,\"Objects\":%u,"
                  "\"Portals\":%u,\"Layers\":%u,\"FullTiles\":%u,\"Version\":%d,\"BoundsDirty\":%d,\"Subgrids\":[",
                  (void*)grid, (unsigned long)((std::uintptr_t)(*(void**)grid) - bias()), (unsigned)grid->MetaData.size(),
                  (unsigned)grid->SurfaceMetaData.size(), (unsigned)grid->Objects.size(), (unsigned)grid->Portals.size(), (unsigned)grid->Layers.size(),
                  (unsigned)grid->FullTiles.size(), grid->Version, grid->SubgridBoundsDirty ? 1 : 0);
    j += buf;
    std::string bin;
    int nsub = 0;
    std::size_t tiles = 0;
    for (auto it = grid->Subgrids.begin(); it != grid->Subgrids.end(); ++it) {
        const auto id = it.Key();
        bg3se::AiSubgrid const* sg = it.Value();
        if (sg == nullptr) continue;
        auto const* td = sg->TileGrid;
        auto const* itd = sg->InternalTileGrid;
        int blockers = 0, open = 0, staticBlocked = 0;
        float lo = 3.4e38f, hi = -3.4e38f;
        std::uint64_t orFlags = 0;
        int internalNonZero = 0;
        const int w = td != nullptr ? td->Width : 0, h = td != nullptr ? td->Height : 0;
        if (td != nullptr && td->Tiles != nullptr && (std::size_t)w * h < (1u << 24)) {
            std::uint32_t head[8];
            head[0] = (std::uint32_t)id;
            head[1] = (std::uint32_t)w;
            head[2] = (std::uint32_t)h;
            std::memcpy(&head[3], &sg->Translate.y, 4);
            std::memcpy(&head[4], &sg->WorldPos.Xglobal, 4);
            std::memcpy(&head[5], &sg->WorldPos.Zglobal, 4);
            std::memcpy(&head[6], &sg->WorldPos.Xlocal, 4);
            std::memcpy(&head[7], &sg->WorldPos.Zlocal, 4);
            bin.append(reinterpret_cast<char const*>(head), sizeof(head));
            for (int k = 0; k < w * h; ++k) {
                auto const& t = td->Tiles[k];
                const std::uint64_t f = t.Flags.Flags;
                orFlags |= f;
                if (t.Flags.IsBlocker()) {
                    ++blockers;
                } else {
                    ++open;
                    if ((f & kArenaBlockBits) != 0) ++staticBlocked;
                    lo = std::min(lo, sg->Translate.y + t.GetLocalMinHeight());
                    hi = std::max(hi, sg->Translate.y + t.GetLocalMinHeight());
                }
                char rec[24];
                std::memcpy(rec, &f, 8);
                std::memcpy(rec + 8, &t.MinHeight, 2);
                std::memcpy(rec + 10, &t.MaxHeight, 2);
                std::memcpy(rec + 12, &t.MetaDataIndex, 2);
                std::memcpy(rec + 14, &t.SurfaceMetaDataIndex, 2);
                // The internal tile (pathfinding node data), when the internal grid has the same shape.
                std::uint64_t internal = 0;
                if (itd != nullptr && itd->Tiles != nullptr && itd->Width == w && itd->Height == h) {
                    std::memcpy(&internal, &itd->Tiles[k], 8);
                }
                std::memcpy(rec + 16, &internal, 8);
                bin.append(rec, 24);
            }
            tiles += (std::size_t)w * h;
        }
        if (itd != nullptr && itd->Tiles != nullptr) {
            const int n = std::min(itd->Width * itd->Height, 1 << 22);
            for (int k = 0; k < n; ++k) internalNonZero += itd->Tiles[k].NodeId != 0 || itd->Tiles[k].Flags != 0;
        }
        std::snprintf(buf, sizeof(buf),
                      "%s{\"Id\":%u,\"Vmt\":\"%#lx\",\"TileVmt\":\"%#lx\",\"InternalVmt\":\"%#lx\",\"At\":\"%p\",\"Translate\":[%.3f,%.3f,%.3f],"
                      "\"CellSize\":%.3f,\"Size\":[%d,%d],\"Tiles\":[%d,%d],\"Internal\":[%d,%d,%d],"
                      "\"WorldPos\":[%d,%d,%.3f,%.3f,%.3f],\"Active\":%d,\"External\":%d,\"PatchesDirty\":%d,"
                      "\"Platform\":[%.3f,%.3f,%.3f],\"B0\":%u,\"Open\":%d,\"Blockers\":%d,"
                      "\"StaticBlocked\":%d,\"FloorRange\":[%.2f,%.2f],\"OrFlags\":\"%#llx\"}",
                      nsub ? "," : "", (unsigned)id, (unsigned long)((std::uintptr_t)(*(void* const*)sg) - bias()),
                      td ? (unsigned long)((std::uintptr_t)(*(void* const*)td) - bias()) : 0ul,
                      itd ? (unsigned long)((std::uintptr_t)(*(void* const*)itd) - bias()) : 0ul, (void const*)sg,
                      sg->Translate.x, sg->Translate.y, sg->Translate.z, sg->CellSize, sg->SizeX, sg->SizeY, w, h,
                      itd ? itd->Width : 0, itd ? itd->Height : 0, internalNonZero, sg->WorldPos.Xglobal,
                      sg->WorldPos.Zglobal, sg->WorldPos.Xlocal, sg->WorldPos.Zlocal, sg->WorldPos.Y,
                      sg->IsActive ? 1 : 0, sg->LoadedExternally ? 1 : 0, sg->PatchesDirty ? 1 : 0,
                      sg->RelativePlatformPosition.x, sg->RelativePlatformPosition.y, sg->RelativePlatformPosition.z,
                      (unsigned)sg->field_B0.size(), open, blockers, staticBlocked,
                      lo, hi, (unsigned long long)orFlags);
        j += buf;
        ++nsub;
    }
    j += "],\"Patches\":[";
    int npatch = 0;
    for (auto it = grid->SubgridsAtPatch.begin(); it != grid->SubgridsAtPatch.end(); ++it) {
        const std::uint64_t key = it.Key();
        std::snprintf(buf, sizeof(buf), "%s[%d,%d,[", npatch ? "," : "", (int)(std::int32_t)(key & 0xffffffffu),
                      (int)(std::int32_t)(key >> 32));
        j += buf;
        bool first = true;
        for (auto id : it.Value()) {
            j += (first ? "" : ",") + std::to_string((unsigned)id);
            first = false;
        }
        j += "]]";
        ++npatch;
    }
    j += "]}";
    write_file(client ? "native-client-grid.json" : "native-server-grid.json", j);
    {
        const std::string path = io_dir() + (client ? "native-client-grid.bin" : "native-server-grid.bin");
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            if (::write(fd, bin.data(), bin.size()) != (ssize_t)bin.size()) logf("grid survey: short write");
            ::close(fd);
        }
    }
    counts[0] = nsub;
    counts[1] = npatch;
    counts[2] = (int)std::min<std::size_t>(tiles, 0x7fffffff);
    logf("grid survey (%s): %d subgrids, %d patches, %zu tiles", client ? "client" : "server", nsub, npatch, tiles);
    return true;
}

}  // namespace

// ==== Read-only physics probes (void arena research) ====

namespace {

// Vertical rays (the engine's own RaycastAll, no arena hooks) on a grid of columns:
// writes native-<side>-columns.json [[x, z, [[y, group], ...]], ...] (at most 8 hits each).
bool column_probe(int side, float x0, float z0, float x1, float z1, float step, float top, int* counts,
                  char const** why) {
    auto& s = g_scene[side];
    void* scene = physics_scene_raw(side == 1);
    if (s.Scene == nullptr || scene != s.Scene || s.All == nullptr) {
        *why = "this side's physics scene is not hooked";
        return false;
    }
    if (!(step >= 0.5f) || x1 < x0 || z1 < z0 || (x1 - x0) / step > 200 || (z1 - z0) / step > 200) {
        *why = "column grid out of range (at most 200 x 200, step >= 0.5 m)";
        return false;
    }
    alignas(phx_hits) unsigned char raw[sizeof(phx_hits)];
    auto* hits = new (raw) phx_hits{};
    std::string j = "[";
    int columns = 0, hitColumns = 0;
    for (float z = z0; z <= z1 + 1e-3f; z += step) {
        for (float x = x0; x <= x1 + 1e-3f; x += step) {
            hits->Normals.clear();
            hits->Positions.clear();
            hits->Distances.clear();
            hits->PhysicsGroup.clear();
            hits->PhysicsExtraFlags.clear();
            hits->Shapes.clear();
            s.All(scene, {x, top, z}, {x, top - 400.0f, z}, *hits, (bg3se::PhysicsType)15,
                  (bg3se::PhysicsGroupFlags)0xffffffffu, (bg3se::PhysicsGroupFlags)0, 0, -1, -1, std::nullopt);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s[%.1f,%.1f,[", columns ? "," : "", x, z);
            j += buf;
            const std::uint32_t n = std::min<std::uint32_t>(
                std::min(hits->Positions.size(), hits->PhysicsGroup.size()), 8u);
            for (std::uint32_t k = 0; k < n; ++k) {
                std::snprintf(buf, sizeof(buf), "%s[%.2f,%u]", k ? "," : "", hits->Positions[k].y,
                              (unsigned)hits->PhysicsGroup[k]);
                j += buf;
            }
            j += "]]";
            hitColumns += n > 0;
            ++columns;
        }
    }
    j += "]";
    // The engine allocated the arrays' buffers; they are left to leak (a few KB), as bg3le does.
    write_file(side ? "native-client-columns.json" : "native-server-columns.json", j);
    counts[0] = columns;
    counts[1] = hitColumns;
    return true;
}

std::string scene_counts_json(int side) {
    auto const& s = g_scene[side];
    std::string j = "{\"Hooked\":";
    j += s.Scene != nullptr ? "true" : "false";
    j += ",\"Slots\":{";
    bool first = true;
    if (s.SlotCounts != nullptr) {
        for (int i = 2; i < 40; ++i) {
            const std::uint64_t c = __atomic_load_n(&s.SlotCounts[i], __ATOMIC_RELAXED);
            if (c == 0) continue;
            j += (first ? "\"" : ",\"") + std::to_string(i) + "\":" + std::to_string(c);
            first = false;
        }
    }
    j += "},\"Raycast\":[" + std::to_string(s.Stats.Calls[0].load()) + "," + std::to_string(s.Stats.Calls[1].load()) +
         "," + std::to_string(s.Stats.Calls[2].load()) + "]}";
    return j;
}

// ---- BG3's picking region check (world mode picks characters anywhere) ----
//
// Found in bg3 4.8.400.7143220: the client's cursor keeps a
// character it hit only if the level's region map byte at the character's position equals the
// player's current region (bg3+0x2472060, called by the cursor's character query job bg3+0x246a6b0
// and the other cursor checks with the owner's "enabled" bool, which the engine sets again every
// frame). The map is a 4 m grid over the AI grid's original bounds (client level+0xb8 = the region
// owner, +0x48 -> +0x40 a hash map, key -1: cell scale at +0xc, bytes at +0x10, cell count at +0x18,
// row width at +0x20, origin in tiles at +0x28), indexed row * width + column with only that linear
// index bounds-checked: west or east of the map a position wraps into another row's cells, and out
// of range the byte reads 0. The player's region (per-player object +0x2be0, from the region trigger
// the player's character stands in, 0 for none) is 3D (the trigger box's height counts). So Tav in
// the void was picked or not by an unrelated cell of the tutorial ship's map, and above the box's
// top (y 155.7) not at all. World mode empties the map (cell count 0): every lookup reads 0, which
// matches Tav's region in the void (none). Restored by count; a level change drops it unwritten.
// Fails closed: this build's code and object layout only.
constexpr std::uintptr_t kRegionCheckFn = 0x2472060;
constexpr unsigned char kRegionCheckCode[] = {0x40, 0x84, 0xf6, 0x0f, 0x84};  // test sil, sil; je
constexpr std::uintptr_t kRegionOwnerVmt = 0x79e9830;
constexpr std::uintptr_t kPlayerObjects = 0x7d5c0b8;  // per-player objects by player id (+0x2be0 region)

struct RegionMap {
    void* Level = nullptr;
    void* Owner = nullptr;
    char* Map = nullptr;
    std::int32_t Cells = 0, Width = 0;
    float Scale = 0;
    std::int32_t Origin[2] = {};
};

struct PickRegion {
    bool On = false;
    void* Level = nullptr;
    char* Map = nullptr;
    std::int32_t Cells = 0, Width = 0;
};
PickRegion g_pick;
std::mutex g_pick_lock;

template <class T>
bool rd(void const* p, T& v) {
    return safe_read(p, &v, sizeof(T));
}

bool region_map(RegionMap& m, char const** why) {
    auto* level = current_level(true);
    if (level == nullptr) return (*why = "no client level"), false;
    const std::uintptr_t bias = g_api.LoadBias();
    unsigned char code[sizeof(kRegionCheckCode)] = {};
    if (!safe_read((void const*)(bias + kRegionCheckFn), code, sizeof(code))
        || std::memcmp(code, kRegionCheckCode, sizeof(code)) != 0) {
        return (*why = "BG3's picking region check is not where this build has it"), false;
    }
    void* owner = nullptr;
    std::uintptr_t vmt = 0;
    if (!rd((char const*)level + 0xb8, owner) || !rd(owner, vmt) || vmt != bias + kRegionOwnerVmt) {
        return (*why = "the level's region owner is not where this build has it"), false;
    }
    char* holder = nullptr;
    char* table = nullptr;
    if (!rd((char const*)owner + 0x48, holder) || !rd(holder + 0x40, table) || table == nullptr) {
        return (*why = "no region map table"), false;
    }
    std::int32_t* buckets = nullptr;
    std::int32_t* next = nullptr;
    std::int32_t* keys = nullptr;
    char** values = nullptr;
    std::int32_t count = 0;
    if (!rd(table, buckets) || !rd(table + 8, count) || !rd(table + 0x10, next) || !rd(table + 0x20, keys)
        || !rd(table + 0x30, values) || count <= 0 || count > 4096) {
        return (*why = "region map table unreadable"), false;
    }
    std::int32_t idx = -1;
    if (!rd(buckets + (std::uint64_t)0xffffffffu % (std::uint64_t)count, idx)) idx = -1;
    char* map = nullptr;
    for (int guard = 0; idx >= 0 && guard < 64; ++guard) {
        std::int32_t key = 0;
        if (!rd(keys + idx, key)) break;
        if (key == -1) {
            if (!rd(values + idx, map)) map = nullptr;
            break;
        }
        if (!rd(next + idx, idx)) break;
    }
    if (map == nullptr) return (*why = "no region map"), false;
    char* data = nullptr;
    if (!rd(map + 0xc, m.Scale) || !rd(map + 0x10, data) || !rd(map + 0x18, m.Cells) || !rd(map + 0x20, m.Width)
        || !rd(map + 0x28, m.Origin) || data == nullptr || m.Width <= 0 || m.Cells < 0
        || (m.Cells > 0 && m.Cells % m.Width != 0) || !(m.Scale > 0.0f && m.Scale <= 1.0f)) {
        return (*why = "region map does not look like this build's"), false;
    }
    m.Level = level;
    m.Owner = owner;
    m.Map = map;
    return true;
}

// The region player 1 is in (0 none), or -1 if it cannot be read.
int player_region() {
    char* objects = nullptr;
    std::int32_t count = 0;
    char** buckets = nullptr;
    if (!rd((void const*)(g_api.LoadBias() + kPlayerObjects), objects) || !rd(objects + 0x34, count)
        || !rd(objects + 0x38, buckets) || count <= 0) {
        return -1;
    }
    char* node = nullptr;
    if (!rd(buckets + 1 % count, node)) return -1;
    for (int guard = 0; node != nullptr && guard < 64; ++guard) {
        std::int16_t key = 0;
        if (!rd(node + 8, key)) return -1;
        if (key == 1) {
            char* player = nullptr;
            std::int32_t region = -1;
            return rd(node + 0x18, player) && rd(player + 0x2be0, region) ? region : -1;
        }
        if (!rd(node, node)) return -1;
    }
    return -1;
}

void pick_report(Out& o, RegionMap const& m) {
    o.R[0] = g_pick.On ? 1 : 0;
    o.R[1] = g_pick.Cells;
    o.R[2] = m.Width;
    o.R[3] = player_region();
    o.F[0] = m.Scale;
    o.F[1] = (float)m.Origin[0];
    o.F[2] = (float)m.Origin[1];
    o.F[3] = (float)m.Cells;
}

// mode 1: empty the map (again, if the engine refilled it); 0: put the count back; 2: report.
bool pick_region(Out& o, int mode) {
    std::lock_guard<std::mutex> held(g_pick_lock);
    RegionMap m;
    char const* why = nullptr;
    const bool found = region_map(m, &why);
    if (mode == 0) {
        if (!g_pick.On) return true;
        std::int32_t now = -1;
        const bool same = found && m.Level == g_pick.Level && m.Map == g_pick.Map && m.Width == g_pick.Width
            && rd(m.Map + 0x18, now) && now == 0;
        if (same) *reinterpret_cast<volatile std::int32_t*>(m.Map + 0x18) = g_pick.Cells;
        logf("picking region map %s (%d cells)", same ? "back" : "dropped (level or map changed)", g_pick.Cells);
        o.R[0] = same ? 0 : 2;
        o.R[1] = g_pick.Cells;
        g_pick = PickRegion{};
        return true;
    }
    if (!found) return fail(o, 30, why);
    if (mode == 2) {
        pick_report(o, m);
        return true;
    }
    if (g_pick.On && (m.Level != g_pick.Level || m.Map != g_pick.Map)) {
        logf("picking region map dropped (level or map changed)");
        g_pick = PickRegion{};
    }
    if (!g_pick.On) {
        if (m.Cells == 0) return fail(o, 31, "region map already empty");
        g_pick = PickRegion{true, m.Level, m.Map, m.Cells, m.Width};
        logf("picking region map emptied (%d cells, %d wide)", m.Cells, m.Width);
    } else if (m.Cells != 0) {
        // The engine filled it again: take its count as the one to put back.
        g_pick.Cells = m.Cells;
        g_pick.Width = m.Width;
        logf("picking region map refilled by the engine (%d cells): emptied again", m.Cells);
    }
    *reinterpret_cast<volatile std::int32_t*>(m.Map + 0x18) = 0;
    m.Cells = 0;
    pick_report(o, m);
    return true;
}

// Frame handler: a client level change frees the map; forget it without writing.
void pick_level_check() {
    std::lock_guard<std::mutex> held(g_pick_lock);
    if (g_pick.On && current_level(true) != g_pick.Level) {
        logf("picking region map dropped: client level changed");
        g_pick = PickRegion{};
    }
}

}  // namespace

namespace {

// ---- BG3's spatial grid far from the level (world mode) ----
//
// BG3 answers "who is near X" from a spatial grid of 12.73 m cells (eoc::spatial_grid::DataComponent,
// one per side, kept by esv/ecl::spatial_grid::UpdateSystem). Its square (MinPos +0x80/+0x88, Size
// +0x8c cells) comes from the level's AI grid bounds (AiGrid+0x260, through a provider object) when
// the level loads. Inserts are not clamped, but the range query (bg3+0x2855cd0) clamps its cells to
// the square and finds nothing for a circle wholly outside it. Tav's void is 1.1 km west of
// TUT_Avernus_C, so there the server's activation (esv::ActivationManager looks for characters round
// each player through that query) never activates a stand-in: no eoc::ActiveComponent, so the client
// never adds ecl::level::PresenceComponent and builds no visual, sight or picking, and the server's
// AI never runs for it. The system keeps three flags the engine sets when a level loads
// (bg3+0x3e7c030): clear the grid, take the bounds from the AI grid again, insert every character and
// item again. World mode's Place keeps the AI grid's bounds round the placed squares, so setting the
// three flags makes the engine rebuild its own grid over the level and Tav's squares on its next
// update; after Restore the same flags shrink it back. Nothing else is written.
constexpr std::uintptr_t kGridSystemVmt[2] = {0x7a84e10, 0x79f00a0};  // esv, ecl spatial_grid::UpdateSystem
constexpr std::uintptr_t kGridSystemVmt2[2] = {0x7a84e70, 0x79f0100}; // their second base (+0x10)
constexpr std::uintptr_t kGridProviderVmt = 0x7a4a958;               // bounds from the level's AI grid
constexpr std::uintptr_t kGridFlagTest = 0x2b2f6cc;                  // the update's three flag tests
constexpr unsigned char kGridFlagCode[] = {0x80, 0x7b, 0x10, 0x00, 0x0f, 0x85, 0x82, 0x0b, 0x00, 0x00,
    0x80, 0x7b, 0x11, 0x00, 0x0f, 0x85, 0x91, 0x0b, 0x00, 0x00, 0x80, 0x7b, 0x12, 0x00, 0x0f, 0x85};
constexpr std::uintptr_t kGridProviderRead = 0x5ecc27b;              // the provider reads AiGrid+0x260..+0x26c
constexpr unsigned char kGridProviderCode[] = {0x8b, 0x88, 0x60, 0x02, 0x00, 0x00, 0x3b, 0x88, 0x68, 0x02,
    0x00, 0x00};
constexpr std::uintptr_t kGridFlags = 0x28;  // in the system: +0x28 clear, +0x29 bounds, +0x2a reinsert

// mode 1: ask the engine to rebuild this side's grid; 2: report. system: the side's
// spatial_grid::UpdateSystem (Lua: Ext._Internal.SystemProbe). R0 flags now (1 clear, 2 bounds,
// 4 reinsert), R1 1 when set by this call; F0..F3 the AI grid's bounds in metres (min x, z, max x, z)
// that the engine will take.
bool spatial_grid(Out& o, int side, void* system, int mode) {
    const std::uintptr_t bias = g_api.LoadBias();
    unsigned char code[sizeof(kGridFlagCode)] = {};
    unsigned char code2[sizeof(kGridProviderCode)] = {};
    if (!safe_read((void const*)(bias + kGridFlagTest), code, sizeof(code))
        || std::memcmp(code, kGridFlagCode, sizeof(code)) != 0
        || !safe_read((void const*)(bias + kGridProviderRead), code2, sizeof(code2))
        || std::memcmp(code2, kGridProviderCode, sizeof(code2)) != 0) {
        return fail(o, 40, "BG3's spatial grid update is not where this build has it");
    }
    std::uintptr_t vmt = 0, provider = 0, pvmt = 0;
    if (system != nullptr && rd(system, vmt) && vmt == bias + kGridSystemVmt2[side]) {
        system = (char*)system - 0x10;  // the registry may name the second base
        if (!rd(system, vmt)) vmt = 0;
    }
    if (system == nullptr || vmt != bias + kGridSystemVmt[side]) {
        return fail(o, 41, "not this side's spatial_grid::UpdateSystem");
    }
    if (!rd((char const*)system + 0x18, provider) || !rd((void const*)provider, pvmt) || pvmt != bias + kGridProviderVmt) {
        return fail(o, 42, "the spatial grid's bounds provider is not the AI grid one");
    }
    std::uint32_t flags = 0;
    if (!rd((char const*)system + kGridFlags, flags)) return fail(o, 43, "spatial grid flags unreadable");
    o.R[0] = (int)((flags & 0xff ? 1 : 0) | (flags & 0xff00 ? 2 : 0) | (flags & 0xff0000 ? 4 : 0));
    auto* grid = ai_grid(side == 1);
    GridBounds now{};
    if (grid == nullptr || !safe_read(grid_bounds(grid), &now, sizeof(now))) return fail(o, 44, "no AI grid");
    o.F[0] = std::floor(now.Min.x * 0.5f);
    o.F[1] = std::floor(now.Min.y * 0.5f);
    o.F[2] = std::ceil(now.Max.x * 0.5f);
    o.F[3] = std::ceil(now.Max.y * 0.5f);
    if (mode != 1) return true;
    // One aligned 32-bit store, as the engine's own level load sets them (byte +0x2b kept).
    auto* word = reinterpret_cast<std::atomic<std::uint32_t>*>((char*)system + kGridFlags);
    std::uint32_t old = word->load(std::memory_order_relaxed);
    while (!word->compare_exchange_weak(old, (old & 0xff000000u) | 0x010101u, std::memory_order_acq_rel)) {}
    o.R[1] = 1;
    logf("%s spatial grid: rebuild asked (AI grid bounds %.0f %.0f .. %.0f %.0f m)", side == 1 ? "client" : "server",
         o.F[0], o.F[1], o.F[2], o.F[3]);
    return true;
}

}  // namespace

// ==== BG3's AI grid visuals follow the pool (op 31) ====
//
// Enemy vision cones and ground surfaces with BG3's own logic and visuals. BG3's client
// keeps a visual per AI subgrid (the level's AI grid visual manager, ecl level + 0xa8; its map at
// +0x2b8 names each subgrid's visual by id), built when the level loads: the per-tile textures its
// sight overlay (EoCSight.bshd: enemies' vision cones, darkness) and its ground surfaces are drawn
// from, placed at the subgrid's Translate of that time (visual +0x404), and each subgrid's box with
// its 32 m chunk boxes (list at +0x2c8, 0x38-byte boxes) in a quadtree whose root is the AI grid's
// bounds of that time. BG3 works out cones and surfaces on the live grid (tile coordinates of the
// subgrid under a position), so after Place moved a pool subgrid it computed Tav's cones into that
// subgrid's textures and drew them where the subgrid had been, 1.1 km away (research,
// on the client). This op moves each pool subgrid's visual and boxes to where the subgrid is
// now (home again after Restore) and has the engine rebuild its box tree as at level load: a root
// from the grid's bounds now (bg3+0x38f0ef0; Place keeps them round the squares), every box
// inserted again (bg3+0x38f4c00). The old tree is left as it is (never freed: a reader may be in it).
//
// The sight overlay itself is drawn from records in BG3's EoCSight pass (one per player: the client's
// per-player render data, global bg3+0x7d5c0b8 by player index, + 0x2bc8), one record per AI subgrid
// near the player, made with the subgrid's Translate of that moment (bg3+0x6507230 -> 0x6557210,
// origin at +0x98). BG3 refreshes that origin every frame only for subgrids a platform owns
// (eoc::platform::PlatformComponent; the ShroudManager's lambda bg3+0x6507800), the engine's own
// moving subgrids. Pool subgrids move too, so their records follow them the same way here: without
// it BG3 drew the overlay of the squares round Tav (vision cones computed on the live grid, research
// traces) at the subgrids' old places. Pool and PoolRestore call this after every move.

namespace {

constexpr std::uintptr_t kVisLevelVmt = 0x79e57e8;  // the client level
constexpr std::size_t kVisManager = 0xa8;           // level -> AI grid visual manager
constexpr std::size_t kVisBack = 0x2b0, kVisMap = 0x2b8, kVisList = 0x2c8;
constexpr std::size_t kVisOrigin = 0x404;           // visual: its subgrid's Translate as built
constexpr std::uintptr_t kVisTreeInit = 0x38f0ef0, kVisTreeInsert = 0x38f4c00;
constexpr unsigned char kVisTreeInitCode[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0x8b, 0x5e, 0x68};
constexpr unsigned char kVisTreeInsertCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};

struct VisNode {
    VisNode* Next;
    std::uint32_t Key;
    std::uint32_t Pad;
    char* Value;
};
struct VisMap {  // LegacyRefMap<AiSubgridId, visual*>
    std::uint32_t Count, HashSize;
    VisNode** Buckets;
};
struct VisBox {  // a subgrid's box; its chunk boxes are 0x30 bytes, min at +0x10, max at +0x1c
    char* Chunks;
    std::uint32_t ChunkCap, ChunkCount;
    float Min[3], Max[3];
    std::uint32_t Id;
    std::int32_t Nx, Nz;
};
static_assert(sizeof(VisBox) == 0x38);
struct VisList {
    VisBox** Boxes;
    std::uint32_t Cap, Count;
    void* Root;
};
using VisTreeInitFn = void (*)(VisList*, void*);

constexpr std::uintptr_t kSightPlayers = 0x7d5c0b8;  // global: the client's per-player render data
constexpr std::size_t kSightPass = 0x2bc8;            // player data -> its EoCSight pass
constexpr std::uintptr_t kSightPassVmt = 0x79eda08;
constexpr std::size_t kSightRecs = 0xe8, kSightRecSize = 0xa8, kSightRecOrigin = 0x98;
struct SightPlayerNode {  // the global's map: player index -> render data
    SightPlayerNode* Next;
    std::int16_t Player;
    char Pad[14];
    char* Data;
};
static_assert(sizeof(SightPlayerNode) == 0x20);
struct SightRecMap {  // pass + 0xe8: subgrid id -> record index, the records
    std::int32_t* Buckets;
    std::int32_t HashSize, Pad0;
    std::int32_t* Next;
    void* Pad1;
    std::uint32_t* Keys;
    std::int32_t Cap, Count;
    char* Records;
};
static_assert(sizeof(SightRecMap) == 0x38);

// Each player's sight record of a pool subgrid to where the subgrid is now; F4 moved, F5 found.
void sight_records_follow(Out& o, PoolState const& p) {
    const std::uintptr_t bias = g_api.LoadBias();
    char* reg = nullptr;
    std::uint32_t hs = 0;
    SightPlayerNode** buckets = nullptr;
    if (!safe_read((void const*)(bias + kSightPlayers), &reg, sizeof(reg)) || reg == nullptr || !safe_read(reg + 0x34, &hs, sizeof(hs))
        || !safe_read(reg + 0x38, &buckets, sizeof(buckets)) || hs == 0 || hs > 4096 || buckets == nullptr) {
        return;
    }
    for (std::uint32_t b = 0; b < hs; ++b) {
        SightPlayerNode* n = nullptr;
        if (!safe_read(buckets + b, &n, sizeof(n))) return;
        for (int guard = 0; n != nullptr && guard < 64; ++guard) {
            SightPlayerNode node{};
            if (!safe_read(n, &node, sizeof(node))) break;
            n = node.Next;
            char* pass = nullptr;
            std::uintptr_t vmt = 0;
            SightRecMap r{};
            if (node.Data == nullptr || !safe_read(node.Data + kSightPass, &pass, sizeof(pass)) || pass == nullptr
                || !safe_read(pass, &vmt, sizeof(vmt)) || vmt != bias + kSightPassVmt || !safe_read(pass + kSightRecs, &r, sizeof(r))
                || r.HashSize <= 0 || r.HashSize > 65536 || r.Count < 0 || r.Count > 65536 || r.Buckets == nullptr || r.Records == nullptr) {
                continue;
            }
            for (auto const& s : p.Subs) {
                if (s.Sg == nullptr) continue;
                const std::uint32_t id = (std::uint32_t)s.Id;
                std::int32_t e = -1;
                if (!safe_read(r.Buckets + id % (std::uint32_t)r.HashSize, &e, sizeof(e))) continue;
                for (int g = 0; e >= 0 && e < r.Count && g < 65536; ++g) {
                    std::uint32_t k = 0;
                    if (!safe_read(r.Keys + e, &k, sizeof(k))) { e = -1; break; }
                    if (k == id) break;
                    if (!safe_read(r.Next + e, &e, sizeof(e))) { e = -1; break; }
                }
                if (e < 0 || e >= r.Count) continue;
                o.F[5] += 1.0f;
                auto* org = reinterpret_cast<float*>(r.Records + (std::size_t)e * kSightRecSize + kSightRecOrigin);
                glm::vec3 now{}, was{};
                if (!safe_read(&s.Sg->Translate, &now, sizeof(now)) || !safe_read(org, &was, sizeof(was))) continue;
                if (fabsf(now.x - was.x) < 1e-3f && fabsf(now.y - was.y) < 1e-3f && fabsf(now.z - was.z) < 1e-3f) continue;
                std::memcpy(org, &now, sizeof(now));
                o.F[4] += 1.0f;
            }
        }
    }
}
using VisTreeInsertFn = void (*)(void*, VisBox*);

char* visual_of(VisMap const& map, std::uint32_t id) {
    if (map.HashSize == 0 || map.HashSize > 65536 || map.Buckets == nullptr) return nullptr;
    VisNode* n = nullptr;
    if (!safe_read(map.Buckets + id % map.HashSize, &n, sizeof(n))) return nullptr;
    for (int guard = 0; n != nullptr && guard < 4096; ++guard) {
        VisNode node{};
        if (!safe_read(n, &node, sizeof(node))) return nullptr;
        if (node.Key == id) return node.Value;
        n = node.Next;
    }
    return nullptr;
}

bool sight_sync(Out& o, PoolState& p, int mode) {
    if (mode == 1) sight_records_follow(o, p);
    auto* level = current_level(true);
    const std::uintptr_t bias = g_api.LoadBias();
    std::uintptr_t vmt = 0;
    char* mgr = nullptr;
    void* back = nullptr;
    if (level == nullptr || !safe_read(level, &vmt, sizeof(vmt)) || vmt != bias + kVisLevelVmt) {
        return fail(o, 48, "no client level of this build's kind");
    }
    if (!safe_read((char const*)level + kVisManager, &mgr, sizeof(mgr)) || mgr == nullptr
        || !safe_read(mgr + kVisBack, &back, sizeof(back)) || back != (void*)level) {
        return fail(o, 49, "the level's AI grid visuals are not where this build has them");
    }
    unsigned char c1[sizeof(kVisTreeInitCode)] = {}, c2[sizeof(kVisTreeInsertCode)] = {};
    if (!safe_read((void const*)(bias + kVisTreeInit), c1, sizeof(c1)) || std::memcmp(c1, kVisTreeInitCode, sizeof(c1)) != 0
        || !safe_read((void const*)(bias + kVisTreeInsert), c2, sizeof(c2)) || std::memcmp(c2, kVisTreeInsertCode, sizeof(c2)) != 0) {
        return fail(o, 50, "BG3's AI grid visual box tree is not where this build has it");
    }
    VisMap map{};
    VisList* list = nullptr;
    VisList l{};
    if (!safe_read(mgr + kVisMap, &map, sizeof(map)) || !safe_read(mgr + kVisList, &list, sizeof(list)) || list == nullptr
        || !safe_read(list, &l, sizeof(l)) || l.Count > 4096 || l.Boxes == nullptr) {
        return fail(o, 51, "the AI grid visuals are unreadable");
    }
    auto* grid = ai_grid(true);
    if (grid == nullptr) return fail(o, 52, "no client AI grid");
    int moved = 0, boxes = 0;
    bool first = true;
    for (auto const& s : p.Subs) {
        ++o.R[3];
        char* v = visual_of(map, (std::uint32_t)s.Id);
        if (v == nullptr || s.Sg == nullptr) continue;
        glm::vec3 now{}, was{};
        if (!safe_read(&s.Sg->Translate, &now, sizeof(now)) || !safe_read(v + kVisOrigin, &was, sizeof(was))) continue;
        const float dx = now.x - was.x, dz = now.z - was.z;
        if (fabsf(dx) < 1e-3f && fabsf(dz) < 1e-3f && fabsf(now.y - was.y) < 1e-3f) continue;
        if (first) {
            o.F[0] = was.x; o.F[1] = was.z; o.F[2] = now.x; o.F[3] = now.z;
            first = false;
        }
        if (mode != 1) continue;
        std::memcpy(v + kVisOrigin, &now, sizeof(now));
        ++moved;
        for (std::uint32_t k = 0; k < l.Count; ++k) {
            VisBox* b = nullptr;
            if (!safe_read(l.Boxes + k, &b, sizeof(b)) || b == nullptr || b->Id != (std::uint32_t)s.Id) continue;
            b->Min[0] += dx; b->Min[1] = now.y; b->Min[2] += dz;
            b->Max[0] += dx; b->Max[2] += dz;
            for (std::uint32_t c = 0; c < b->ChunkCount && c < 4096 && b->Chunks != nullptr; ++c) {
                float* f = reinterpret_cast<float*>(b->Chunks + (std::size_t)c * 0x30 + 0x10);
                f[0] += dx; f[1] = now.y; f[2] += dz; f[3] += dx; f[5] += dz;
            }
            ++boxes;
        }
    }
    o.R[0] = moved;
    o.R[1] = boxes;
    if (mode != 1 || moved == 0) return true;
    reinterpret_cast<VisTreeInitFn>(bias + kVisTreeInit)(list, grid);
    VisList fresh{};
    if (!safe_read(list, &fresh, sizeof(fresh)) || fresh.Root == nullptr || fresh.Root == l.Root) {
        return fail(o, 53, "the box tree was not rebuilt");
    }
    for (std::uint32_t k = 0; k < fresh.Count; ++k) {
        VisBox* b = nullptr;
        if (safe_read(fresh.Boxes + k, &b, sizeof(b)) && b != nullptr) reinterpret_cast<VisTreeInsertFn>(bias + kVisTreeInsert)(fresh.Root, b);
    }
    o.R[2] = 1;
    logf("client AI grid visuals: %d moved with the pool, %d boxes, tree rebuilt over %u boxes", moved, boxes, fresh.Count);
    return true;
}

void pool_sight_follow(PoolState& p) {
    Out o{};
    if (!sight_sync(o, p, 1)) {
        logf("client AI grid visuals: not moved with the pool (%d: %s)", o.Err, o.Why);
    } else if (o.F[4] > 0) {
        logf("client sight overlay: %d of %d records moved with the pool", (int)o.F[4], (int)o.F[5]);
    }
}

// ==== Pool subgrids' sight and surface textures from their live tiles (op 50) ====
//
// The chosen approach (BG3's own systems draw cones and surfaces on WoW's ground). BG3's client
// keeps textures per AI subgrid, made from the subgrid's tiles when the level loads, in an AI grid
// texture manager (client level + 0xb8; its client AI grid at +0x38, a worker holding each kind's CPU
// data at +0x48, each kind's GPU textures in a map by subgrid id at +0x50 + kind * 0x40). Kind 4 is
// the heights (R16G16: a walkable tile's MinHeight in 0.02 m above the subgrid's Translate, 0xffff;
// blocked tiles take their walkable neighbours'), kind 5 the blocked mask (R8, 1 blocked). BG3's
// sight overlay draws from them (EoCSight b9 and b10: nothing where a pixel lies more than 0.7 m off
// its tile's height, or on a blocked tile) and EoCSurfaceMeshGenerate builds ground surfaces only where
// the visible ground is within 1 m of that height. After Place they still held the pool subgrids'
// home tiles (research: on four placed subgrids 16364 to 16384
// of 16384 walkable tiles' heights differed from the live tiles and 143 to 9010 tiles were masked
// that are walkable now; the unplaced one matched its tiles exactly), so the overlay and surfaces
// could not show on WoW's ground. This does what BG3 does when a subgrid is added (bg3+0x38f5ce0) and
// when its textures change (its update queue, bg3+0x2dc8c80): the CPU data again from the live tiles
// (bg3+0x34c69e0), new GPU textures of kinds 4 and 5 from it (bg3+0x34c2de0), and every player's
// EoCSight record of the subgrid pointed at them (bg3+0x2dc8af0 heights, bg3+0x2dc9360 mask; records
// made later take them from the manager). Place and PoolRestore call it for what they moved.

constexpr std::size_t kLevelGridTex = 0xb8;
constexpr std::size_t kGtexBack = 0x30, kGtexGrid = 0x38, kGtexWorker = 0x48, kGtexMaps = 0x50, kGtexMapStride = 0x40;
constexpr std::size_t kGtexWorkerMaps = 0x10, kGtexWorkerStride = 0x10;  // worker: CPU data map of kind k at +0x10 + k * 0x10
constexpr int kGtexHeights = 4, kGtexMask = 5;
constexpr std::uintptr_t kGtexBuild = 0x34c69e0, kGtexMake = 0x34c2de0, kSightSetHeights = 0x2dc8af0, kSightSetMask = 0x2dc9360;
constexpr unsigned char kGtexBuildCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0x7f, 0x38, 0x00};
constexpr unsigned char kGtexMakeCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x89, 0xf5};
constexpr unsigned char kSightSetCode[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89};
using GtexBuildFn = void (*)(void* mgr, void* subgrid, std::uint32_t const* id);
using GtexMakeFn = void (*)(void* mgr, std::uint32_t kind, std::uint32_t const* id);
using SightSetFn = void (*)(void* pass, std::uint32_t id, void* texture);

// A value of one of BG3's id-keyed maps (the SightRecMap layout: buckets, hash size, next, keys,
// count, values); nullptr when absent.
void* id_map_value(char const* map, std::uint32_t id) {
    SightRecMap r{};
    if (!safe_read(map, &r, sizeof(r)) || r.HashSize <= 0 || r.HashSize > 65536 || r.Count < 0 || r.Count > 1 << 20
        || r.Buckets == nullptr || r.Records == nullptr) {
        return nullptr;  // (Count is not checked: these maps keep entries up to their capacity)
    }
    std::int32_t e = -1;
    if (!safe_read(r.Buckets + id % (std::uint32_t)r.HashSize, &e, sizeof(e))) return nullptr;
    for (int g = 0; e >= 0 && e < (1 << 20) && g < 65536; ++g) {
        std::uint32_t k = 0;
        if (!safe_read(r.Keys + e, &k, sizeof(k))) return nullptr;
        if (k == id) {
            void* v = nullptr;
            return safe_read(r.Records + (std::size_t)e * sizeof(void*), &v, sizeof(v)) ? v : nullptr;
        }
        if (!safe_read(r.Next + e, &e, sizeof(e))) return nullptr;
    }
    return nullptr;
}

// Each player's EoCSight pass (the client's per-player render data), at most 8.
int sight_passes(char** out) {
    const std::uintptr_t bias = g_api.LoadBias();
    char* reg = nullptr;
    std::uint32_t hs = 0;
    SightPlayerNode** buckets = nullptr;
    int n = 0;
    if (!safe_read((void const*)(bias + kSightPlayers), &reg, sizeof(reg)) || reg == nullptr || !safe_read(reg + 0x34, &hs, sizeof(hs))
        || !safe_read(reg + 0x38, &buckets, sizeof(buckets)) || hs == 0 || hs > 4096 || buckets == nullptr) {
        return 0;
    }
    for (std::uint32_t b = 0; b < hs && n < 8; ++b) {
        SightPlayerNode* nd = nullptr;
        if (!safe_read(buckets + b, &nd, sizeof(nd))) return n;
        for (int guard = 0; nd != nullptr && guard < 64 && n < 8; ++guard) {
            SightPlayerNode node{};
            if (!safe_read(nd, &node, sizeof(node))) break;
            nd = node.Next;
            char* pass = nullptr;
            std::uintptr_t vmt = 0;
            if (node.Data != nullptr && safe_read(node.Data + kSightPass, &pass, sizeof(pass)) && pass != nullptr
                && safe_read(pass, &vmt, sizeof(vmt)) && vmt == bias + kSightPassVmt) {
                out[n++] = pass;
            }
        }
    }
    return n;
}

// mode 1: rebuild pool subgrid `only` (-1 every one); 2: report how the heights and mask the manager
// holds (CPU data) compare with the live tiles. R0 subgrids done, R1 with both textures, R2 player
// passes updated; R3 pool subgrids seen; report: F0 walkable tiles whose height differs, F1 tiles
// whose mask differs from blocked, F2 subgrids matching their tiles.
bool grid_textures(Out& o, PoolState& p, int mode, int only) {
    auto* level = current_level(true);
    const std::uintptr_t bias = g_api.LoadBias();
    std::uintptr_t vmt = 0;
    char* mgr = nullptr;
    void* back = nullptr;
    void* grid = nullptr;
    char* worker = nullptr;
    if (level == nullptr || !safe_read(level, &vmt, sizeof(vmt)) || vmt != bias + kVisLevelVmt) {
        return fail(o, 500, "no client level of this build's kind");
    }
    if (!safe_read((char const*)level + kLevelGridTex, &mgr, sizeof(mgr)) || mgr == nullptr || !safe_read(mgr + kGtexBack, &back, sizeof(back))
        || back != (void*)level || !safe_read(mgr + kGtexGrid, &grid, sizeof(grid)) || grid != (void*)ai_grid(true)
        || !safe_read(mgr + kGtexWorker, &worker, sizeof(worker)) || worker == nullptr) {
        return fail(o, 501, "the AI grid textures are not where this build has them");
    }
    unsigned char c1[sizeof(kGtexBuildCode)] = {}, c2[sizeof(kGtexMakeCode)] = {}, c3[sizeof(kSightSetCode)] = {},
                  c4[sizeof(kSightSetCode)] = {};
    if (!safe_read((void const*)(bias + kGtexBuild), c1, sizeof(c1)) || std::memcmp(c1, kGtexBuildCode, sizeof(c1)) != 0
        || !safe_read((void const*)(bias + kGtexMake), c2, sizeof(c2)) || std::memcmp(c2, kGtexMakeCode, sizeof(c2)) != 0
        || !safe_read((void const*)(bias + kSightSetHeights), c3, sizeof(c3)) || std::memcmp(c3, kSightSetCode, sizeof(c3)) != 0
        || !safe_read((void const*)(bias + kSightSetMask), c4, sizeof(c4)) || std::memcmp(c4, kSightSetCode, sizeof(c4)) != 0) {
        return fail(o, 502, "BG3's AI grid texture builders are not where this build has them");
    }
    char* passes[8] = {};
    const int np = mode == 1 ? sight_passes(passes) : 0;
    for (int k = 0; k < (int)p.Subs.size(); ++k) {
        auto const& s = p.Subs[k];
        if (s.Sg == nullptr || (only >= 0 && k != only)) continue;
        ++o.R[3];
        const std::uint32_t id = (std::uint32_t)s.Id;
        if (mode == 2) {
            // CPU data objects: +0x10 texels, +0x18 bytes, +0x20 width, height
            char* hmap = nullptr;
            char* mmap = nullptr;
            if (!safe_read(worker + kGtexWorkerMaps + kGtexHeights * kGtexWorkerStride, &hmap, sizeof(hmap))
                || !safe_read(worker + kGtexWorkerMaps + kGtexMask * kGtexWorkerStride, &mmap, sizeof(mmap)) || hmap == nullptr || mmap == nullptr) {
                return fail(o, 507, "the worker's data maps are missing");
            }
            char* hobj = (char*)id_map_value(hmap, id);
            char* mobj = (char*)id_map_value(mmap, id);
            std::uint8_t* hd = nullptr;
            std::uint8_t* md = nullptr;
            std::int32_t hw[2] = {}, mw[2] = {};
            if (hobj == nullptr || mobj == nullptr || !safe_read(hobj + 0x10, &hd, sizeof(hd)) || !safe_read(hobj + 0x20, hw, sizeof(hw))
                || !safe_read(mobj + 0x10, &md, sizeof(md)) || !safe_read(mobj + 0x20, mw, sizeof(mw)) || hd == nullptr || md == nullptr
                || hw[0] != kCellTiles || hw[1] != kCellTiles || mw[0] != kCellTiles || mw[1] != kCellTiles) {
                continue;
            }
            int hdiff = 0, mdiff = 0;
            auto* tiles = s.Sg->TileGrid->Tiles;
            for (int i = 0; i < kCellTiles * kCellTiles; ++i) {
                std::uint16_t h = 0;
                std::uint8_t mk = 0;
                if (!safe_read(hd + (std::size_t)i * 4, &h, sizeof(h)) || !safe_read(md + i, &mk, sizeof(mk))) return fail(o, 503, "texels unreadable");
                const bool blocked = (tiles[i].Flags.Flags & 1) != 0;
                if (!blocked && h != tiles[i].MinHeight) ++hdiff;
                if ((mk != 0) != blocked) ++mdiff;
            }
            o.F[0] += (float)hdiff;
            o.F[1] += (float)mdiff;
            if (hdiff == 0 && mdiff == 0) o.F[2] += 1.0f;
            ++o.R[0];
            continue;
        }
        reinterpret_cast<GtexBuildFn>(bias + kGtexBuild)(mgr, s.Sg, &id);
        reinterpret_cast<GtexMakeFn>(bias + kGtexMake)(mgr, kGtexHeights, &id);
        reinterpret_cast<GtexMakeFn>(bias + kGtexMake)(mgr, kGtexMask, &id);
        void* th = id_map_value(mgr + kGtexMaps + kGtexHeights * kGtexMapStride, id);
        void* tm = id_map_value(mgr + kGtexMaps + kGtexMask * kGtexMapStride, id);
        ++o.R[0];
        if (th != nullptr && tm != nullptr) ++o.R[1];
        for (int i = 0; i < np; ++i) {
            reinterpret_cast<SightSetFn>(bias + kSightSetHeights)(passes[i], id, th);
            reinterpret_cast<SightSetFn>(bias + kSightSetMask)(passes[i], id, tm);
        }
    }
    o.R[2] = np;
    return true;
}

void pool_textures_follow(PoolState& p, int only) {
    Out o{};
    if (!grid_textures(o, p, 1, only)) {
        logf("client AI grid textures: not rebuilt (%d: %s)", o.Err, o.Why);
    } else {
        logf("client AI grid textures: %d pool subgrid(s) rebuilt from their tiles (%d with both textures), %d player(s)' sight records",
             o.R[0], o.R[1], o.R[2]);
    }
}

}  // namespace

// ==== WoW portraits and display data on stand-ins ====
//
// Portraits go through BG3's own custom-icon pipeline, the one Tav's portrait takes: an
// eoc::CustomIconComponent holds a WebP image in a ScratchBuffer (Tav's: a 300 x 300 lossy WebP of
// 7868 bytes); the component replicates to the client, whose systems decode it into
// ecl::CustomIconTextureComponent, and BG3's UI (turn order, portraits) binds that texture. The mod
// (Portraits.lua) creates the component on a stand-in and op 40 fills its buffer with the WoW
// creature's portrait, link/portraits/<key>.webp, which benilla bakes with WoW's portrait camera.
//
// The bytes are ours, one copy per key for the whole session (never freed: every stand-in of that
// creature points at it), so the buffer is marked External: the engine copies it (replication,
// component moves) but never frees or writes it.

namespace {

struct ScratchMeta {
    std::uint8_t Origin;  // 1 owned (the engine frees it), 2 external
    std::uint8_t Flags;   // 1 mutable
    std::uint16_t Tag;
    std::uint32_t Align;
};
// ls::ScratchBuffer as the engine lays it out (Tav's eoc::CustomIconComponent, read):
// the bytes, a write stream and a read stream over them, the size.
struct ScratchLayout {
    ScratchMeta Meta;
    void* Buffer;
    std::uint64_t Size;
    ScratchMeta WriteMeta;
    void* WriteBuffer;
    std::uint64_t WriteSize;
    std::uint64_t WriteOffset;
    void* ReadBuffer;
    std::uint64_t ReadSize;
    std::uint64_t ReadOffset;
    std::uint64_t Total;
};
static_assert(sizeof(ScratchLayout) == 0x58);

struct IconBytes {
    std::unique_ptr<unsigned char[]> Data;
    std::size_t Size = 0;
    int Width = 0, Height = 0;
    std::int64_t Stamp = 0;  // the file's mtime (ns) and size when read
};
std::mutex g_icons_mutex;
std::vector<std::pair<int, IconBytes>> g_icons;  // every buffer ever handed out (key, bytes); the newest per key last

// WebP container check: RIFF/WEBP with a VP8, VP8L or VP8X first chunk; the canvas size out.
bool webp_size(std::vector<unsigned char> const& b, int& w, int& h) {
    if (b.size() < 30 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WEBP", 4) != 0) return false;
    std::uint32_t riff = 0;
    std::memcpy(&riff, b.data() + 4, 4);
    if ((std::size_t)riff + 8 != b.size()) return false;
    unsigned char const* c = b.data() + 12;
    if (std::memcmp(c, "VP8L", 4) == 0) {
        if (c[8] != 0x2f) return false;
        const std::uint32_t bits = c[9] | (c[10] << 8) | (c[11] << 16) | ((std::uint32_t)c[12] << 24);
        w = (int)(bits & 0x3fff) + 1;
        h = (int)((bits >> 14) & 0x3fff) + 1;
    } else if (std::memcmp(c, "VP8 ", 4) == 0) {
        if (c[11] != 0x9d || c[12] != 0x01 || c[13] != 0x2a) return false;
        w = (c[14] | (c[15] << 8)) & 0x3fff;
        h = (c[16] | (c[17] << 8)) & 0x3fff;
    } else if (std::memcmp(c, "VP8X", 4) == 0) {
        w = (int)(c[12] | (c[13] << 8) | (c[14] << 16)) + 1;
        h = (int)(c[15] | (c[16] << 8) | (c[17] << 16)) + 1;
    } else {
        return false;
    }
    return w >= 16 && h >= 16 && w <= 1024 && h <= 1024;
}

// The bytes for a key: the cached copy while the file is unchanged, else a new copy (the old one
// stays alive for the components that still point at it).
IconBytes const* icon_bytes(int key, char const** why) {
    const std::string name = "link/portraits/" + std::to_string(key) + ".webp";
    struct stat st{};
    if (::stat((io_dir() + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "no portrait file for that key";
        return nullptr;
    }
    const std::int64_t stamp = (std::int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec + (std::int64_t)st.st_size;
    std::lock_guard<std::mutex> lock(g_icons_mutex);
    for (auto it = g_icons.rbegin(); it != g_icons.rend(); ++it) {
        if (it->first == key) {
            if (it->second.Stamp == stamp) return &it->second;
            break;
        }
    }
    std::vector<unsigned char> raw;
    if (!read_file(name, raw, 512u * 1024)) {
        *why = "portrait file unreadable or over 512 KB";
        return nullptr;
    }
    IconBytes ib;
    if (!webp_size(raw, ib.Width, ib.Height)) {
        *why = "portrait file is not a WebP image of 16 to 1024 pixels";
        return nullptr;
    }
    if (g_icons.size() >= 4096) {
        *why = "portrait cache full";
        return nullptr;
    }
    ib.Data.reset(new unsigned char[raw.size()]);
    std::memcpy(ib.Data.get(), raw.data(), raw.size());
    ib.Size = raw.size();
    ib.Stamp = stamp;
    g_icons.emplace_back(key, std::move(ib));
    return &g_icons.back().second;
}

bool icon_is_ours(void const* p) {
    std::lock_guard<std::mutex> lock(g_icons_mutex);
    for (auto const& e : g_icons) {
        if (e.second.Data.get() == p) return true;
    }
    return false;
}

// Op 40: fill the ScratchBuffer at `at` (an eoc::CustomIconComponent's Icon the mod just made, or
// one this op filled before) with the portrait for `key`. Never touches a buffer the engine filled.
bool custom_icon(Out& o, void* at, int key, int mode) {
    if (at == nullptr) return fail(o, 400, "bad buffer address");
    ScratchLayout now{};
    if (!safe_read(at, &now, sizeof(now))) return fail(o, 401, "buffer unreadable");
    const bool fresh = now.Buffer == nullptr && now.Size == 0 && now.WriteBuffer == nullptr && now.ReadBuffer == nullptr
        && now.Total == 0;
    const bool ours = !fresh && now.Buffer != nullptr && icon_is_ours(now.Buffer) && now.Meta.Origin == 2;
    o.R[3] = fresh ? 1 : ours ? 2 : 0;
    if (!fresh && !ours) return fail(o, 402, "the buffer holds data that is not ours (left alone)");
    char const* why = nullptr;
    IconBytes const* ib = icon_bytes(key, &why);
    if (ib == nullptr) return fail(o, 403, why);
    o.R[0] = (int)ib->Size;
    o.R[1] = ib->Width;
    o.R[2] = ib->Height;
    if (mode != 1) return true;
    ScratchLayout s{};
    s.Meta = ScratchMeta{2, 0, 0, 1};
    s.Buffer = ib->Data.get();
    s.Size = ib->Size;
    s.WriteMeta = ScratchMeta{2, 0, 0, 0};
    s.WriteBuffer = ib->Data.get();
    s.WriteSize = ib->Size;
    s.WriteOffset = 0;
    s.ReadBuffer = ib->Data.get();
    s.ReadSize = ib->Size;
    s.ReadOffset = 0;
    s.Total = ib->Size;
    std::memcpy(at, &s, sizeof(s));
    return true;
}

// ---- WoW levels on stand-ins, display only (op 41) ----
//
// BG3's UI (the hover popup's "Lv.", tooltips, the turn order) shows a character's level from the
// client's copy of eoc::LevelComponent, and reads it again when the server replicates that component
// (found: a server-side change + Replicate showed at once; a client-side write did not,
// with or without the component marked changed). The server's copy is what BG3's rules read
// (proficiency, attack bonus, saves). Op 41 keeps a table of server entities and their WoW level,
// and a hook on the server's replication serializer of eoc::LevelComponent sends that level instead
// of the component's for those entities only: the client, and so BG3's UI, sees the WoW level; the
// server's rules keep the template's level.
//
// Only eoc::LevelComponent carries it. eoc::exp::AvailableLevelComponent
// is the level a character's experience has reached, and BG3's client reacts to it: ecl::exp::
// ExperienceSystem (vtable bg3+0x79e3408; slot 3, bg3+0x4590b80, subscribes to the component's
// replicated changes) hands each change (entity, old level, new level) to the level-up handler
// bg3+0x3369800, which for any rise plays the gameplay VFX "VFX_LevelUp" (Shared VFX.lsx:
// VFX_UI_LevelUp_01) on the character, the "Level Up!" overhead text and the level-up sounds. A
// stand-in's client copy arrives with its template's level, so rewriting AvailableLevel too (as this
// hook first did, so the two would agree) played BG3's level-up on every stand-in whose WoW level is
// higher, as its WoW level came in. A LevelComponent change starts nothing there (harness: Level-only
// changes no effect; AvailableLevel rises 1 -> 7, 7 -> 11, 3 -> 15 each started VFX_UI_LevelUp_01,
// drops none) and BG3's UI reads the level from LevelComponent, so AvailableLevel stays the engine's:
// stand-ins never rise there, and Tav's real level-ups are BG3's own.
//
// The serializer is a per-type class: slot 2 writes each listed entity's whole 4-byte component
// (a new entity for the client), slot 3 the changes (count, then per entity its net id, its field
// mask and the masked field; what Replicate sends). Both slots of its vtable are replaced in place
// (the page made writable for the one store) after checking the slots and the functions' code
// (this build's, call displacements aside); the getter and the stream calls are taken from that code.
// With no table entry in a batch the engine's own function runs.

struct LevelHook {
    char const* Name;
    std::uintptr_t Vtable;   // the serializer class's vtable (its slot 0), bg3 address
    std::uintptr_t Full;     // slot 2: Serialize(self, stream, world, entities, indices), the whole component
    std::uintptr_t Changes;  // slot 3: SerializeChanges(self, stream, world, entities, field masks), updates
    char const* ChangesCode; // slot 3's code, call displacements as xx
    int ChangesCalls[5];     // its calls: count write, net id write, mask write, getter, field write
    // Taken from the code at install:
    void* Getter = nullptr;
    void* Write = nullptr;
    void* NetId = nullptr;
    void* Mask = nullptr;
    void* Field = nullptr;
    bool On = false;
};
// The serializer's slot 2 (bg3 0x33a79a0), calls at 0x37 (getter) and 0x47 (write).
constexpr char kFullCode[] =
    "4157415641554154534d8b204d8b68084d39ec74404889cb4989d64989f749630424488b0b498bbe10020000498bb6480300"
    "00488b14c1e8xxxxxxxx498b3fba040000004889c6e8xxxxxxxx4983c4044d39ec75c95b415c415d415e415fc3";
// A field write (bg3 0x4032ce0): field 0 in the mask -> 4 bytes through the stream's write.
constexpr char kFieldCode[] = "837a08007e18837a0c417c03488b12f60201740aba04000000e9xxxxxxxxc3";
LevelHook g_level_hooks[1] = {
    {"eoc::LevelComponent", 0x78ea788, 0x33a79a0, 0x4032c10,
     "554157415641554154534883ec28488b5908482b194989f4488d7424244889542410ba040000004c8944241848894c240848c1"
     "eb03891e498b3c24e8xxxxxxxx85db7e6f89db31c94531ff48ffcb488b442408488b542418498b3c24498b7424084989ce488b"
     "00488b2a4e8b2cf84801cd4c89eae8xxxxxxxx4c89e74889eee8xxxxxxxx488b4424104c89ea488bb810020000488bb0480300"
     "00e8xxxxxxxx498b3c244889c64889eae8xxxxxxxx4c39fb750f4883c4285b415c415d415e415f5dc34c89f149ffc74883c110"
     "eb80",
     {0x3b, 0x74, 0x7f, 0x9a, 0xa9}},
};
std::mutex g_levels_mutex;
std::vector<std::pair<std::uint64_t, int>> g_levels;  // server entity handle -> WoW level
std::atomic<std::uint64_t> g_level_sent{0};
constexpr std::size_t kMaxLevels = 4096;

int level_for_locked(std::uint64_t e) {
    for (auto const& p : g_levels) {
        if (p.first == e) return p.second;
    }
    return 0;
}
int level_for(std::uint64_t e) {
    std::lock_guard<std::mutex> lock(g_levels_mutex);
    return level_for_locked(e);
}

using LevelGetter = std::int32_t* (*)(void*, void*, std::uint64_t);
using StreamWrite = void (*)(void*, void const*, std::size_t);
using NetIdWrite = void (*)(void*, void*, std::uint64_t);
using MaskWrite = void (*)(void**, void*);
using FieldWrite = void (*)(void*, void const*, void*);
using FullFn = void (*)(void*, void**, char*, std::uint64_t**, std::int32_t**);
using ChangesFn = void (*)(void*, void**, char*, std::uint64_t**, char**);

std::int32_t* level_component(LevelHook const& h, char* world, std::uint64_t e) {
    return reinterpret_cast<LevelGetter>(h.Getter)(*reinterpret_cast<void**>(world + 0x210),
                                                   *reinterpret_cast<void**>(world + 0x348), e);
}

// Slot 2, as the engine's: each listed entity's 4 bytes, the WoW level for a table entry.
template <int K>
void level_full(void* self, void** stream, char* world, std::uint64_t** entities, std::int32_t** range) {
    LevelHook const& h = g_level_hooks[K];
    std::int32_t const* it = range[0];
    std::int32_t const* end = range[1];
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_levels_mutex);
        for (auto p = it; !g_levels.empty() && p != end && !any; ++p) any = level_for_locked((*entities)[*p]) > 0;
    }
    if (!any) {
        reinterpret_cast<FullFn>(g_api.LoadBias() + h.Full)(self, stream, world, entities, range);
        return;
    }
    for (; it != end; ++it) {
        const std::uint64_t e = (*entities)[*it];
        std::int32_t v = *level_component(h, world, e);
        if (const int wow = level_for(e); wow > 0) {
            v = wow;
            g_level_sent.fetch_add(1, std::memory_order_relaxed);
        }
        reinterpret_cast<StreamWrite>(h.Write)(*stream, &v, 4);
    }
}

// Slot 3, as the engine's: the count, then per entity its net id, its field mask and the masked
// field, the WoW level for a table entry.
template <int K>
void level_changes(void* self, void** stream, char* world, std::uint64_t** range, char** masks) {
    LevelHook const& h = g_level_hooks[K];
    std::uint64_t const* begin = range[0];
    std::uint64_t const* end = range[1];
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_levels_mutex);
        for (auto p = begin; !g_levels.empty() && p < end && !any; ++p) any = level_for_locked(*p) > 0;
    }
    if (!any) {
        reinterpret_cast<ChangesFn>(g_api.LoadBias() + h.Changes)(self, stream, world, range, masks);
        return;
    }
    const auto count = (std::uint32_t)(end - begin);
    reinterpret_cast<StreamWrite>(h.Write)(stream[0], &count, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t e = begin[i];
        void* mask = masks[0] + (std::size_t)i * 0x10;
        reinterpret_cast<NetIdWrite>(h.NetId)(stream[0], stream[1], e);
        reinterpret_cast<MaskWrite>(h.Mask)(stream, mask);
        std::int32_t v = *level_component(h, world, e);
        if (const int wow = level_for(e); wow > 0) {
            v = wow;
            g_level_sent.fetch_add(1, std::memory_order_relaxed);
        }
        reinterpret_cast<FieldWrite>(h.Field)(stream[0], &v, mask);
    }
}

// The code at `at` against `hex` (xx: any byte); its length out.
bool code_is(std::uintptr_t at, char const* hex, std::size_t* len) {
    const std::size_t n = std::strlen(hex) / 2;
    std::vector<unsigned char> code(n);
    if (!safe_read(reinterpret_cast<void const*>(at), code.data(), n)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (hex[2 * i] == 'x') continue;
        const unsigned v = (unsigned)std::strtoul(std::string(hex + 2 * i, 2).c_str(), nullptr, 16);
        if (code[i] != v) return false;
    }
    if (len != nullptr) *len = n;
    return true;
}
std::uintptr_t call_target(std::uintptr_t at, int off) {
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<void const*>(at + off + 1), 4);
    return at + off + 5 + (std::intptr_t)rel;
}

bool set_slot(std::uintptr_t* slot, std::uintptr_t value, char const** why) {
    const long pg = ::sysconf(_SC_PAGESIZE);
    auto* page = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(slot) & ~(std::uintptr_t)(pg - 1));
    if (::mprotect(page, (std::size_t)pg, PROT_READ | PROT_WRITE) != 0) return (*why = "vtable page not writable"), false;
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
    ::mprotect(page, (std::size_t)pg, PROT_READ);
    return true;
}

template <int K>
bool level_hook_one(std::uintptr_t bias, char const** why) {
    LevelHook& h = g_level_hooks[K];
    if (h.On) return true;
    auto* slots = reinterpret_cast<std::uintptr_t*>(bias + h.Vtable);
    std::uintptr_t cur[2] = {};
    if (!safe_read(slots + 2, cur, sizeof(cur))) return (*why = "serializer vtable unreadable"), false;
    const auto full = reinterpret_cast<std::uintptr_t>(&level_full<K>);
    const auto changes = reinterpret_cast<std::uintptr_t>(&level_changes<K>);
    if ((cur[0] != full && cur[0] != bias + h.Full) || (cur[1] != changes && cur[1] != bias + h.Changes)) {
        return (*why = "serializer slots are not the engine's"), false;
    }
    const std::uintptr_t f = bias + h.Full, c = bias + h.Changes;
    if (!code_is(f, kFullCode, nullptr)) return (*why = "serializer (whole) code differs from the known build"), false;
    if (!code_is(c, h.ChangesCode, nullptr)) return (*why = "serializer (changes) code differs from the known build"), false;
    const std::uintptr_t getter = call_target(f, 0x37), write = call_target(f, 0x47);
    const std::uintptr_t field = call_target(c, h.ChangesCalls[4]);
    if (call_target(c, h.ChangesCalls[0]) != write || call_target(c, h.ChangesCalls[3]) != getter) {
        return (*why = "the serializers' calls disagree"), false;
    }
    if (!code_is(field, kFieldCode, nullptr) || call_target(field, 0x19) != write) {
        return (*why = "field write code differs from the known build"), false;
    }
    h.Getter = reinterpret_cast<void*>(getter);
    h.Write = reinterpret_cast<void*>(write);
    h.NetId = reinterpret_cast<void*>(call_target(c, h.ChangesCalls[1]));
    h.Mask = reinterpret_cast<void*>(call_target(c, h.ChangesCalls[2]));
    h.Field = reinterpret_cast<void*>(field);
    if (cur[0] != full && !set_slot(slots + 2, full, why)) return false;
    if (cur[1] != changes && !set_slot(slots + 3, changes, why)) return false;
    h.On = true;
    logf("levels: %s replication serializers hooked (getter %p, write %p, net id %p, mask %p, field %p)", h.Name,
         h.Getter, h.Write, h.NetId, h.Mask, h.Field);
    return true;
}

bool level_hooks_install(char const** why) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (g_api.LoadBias == nullptr) return (*why = "no load bias"), false;
    const std::uintptr_t bias = g_api.LoadBias();
    return level_hook_one<0>(bias, why);
}

// Op 41 (server): a stand-in's WoW level for the client (level 1..99), 0 forgets one, -1 forgets all,
// -2 only installs and reports. The mod then replicates the components, so the client gets them now.
bool display_level(Out& o, bool client, std::uint64_t handle, int level) {
    if (client) return fail(o, 410, "levels are sent by the server");
    if (level < -2 || level > 99) return fail(o, 411, "level is 1 to 99, 0 forget, -1 forget all, -2 report");
    char const* why = nullptr;
    if (!level_hooks_install(&why)) return fail(o, 412, why != nullptr ? why : "level hook refused");
    {
        std::lock_guard<std::mutex> lock(g_levels_mutex);
        if (level == -1) {
            g_levels.clear();
        } else if (level == 0 || level > 0) {
            auto it = std::find_if(g_levels.begin(), g_levels.end(), [&](auto const& p) { return p.first == handle; });
            if (level == 0) {
                if (it != g_levels.end()) g_levels.erase(it);
            } else if (it != g_levels.end()) {
                it->second = level;
            } else if (g_levels.size() >= kMaxLevels) {
                return fail(o, 413, "level table full");
            } else {
                g_levels.emplace_back(handle, level);
            }
        }
        o.R[1] = (int)g_levels.size();
    }
    o.R[0] = g_level_hooks[0].On ? 1 : 0;
    o.R[2] = (int)std::min<std::uint64_t>(g_level_sent.load(), 0x7fffffff);
    return true;
}

}  // namespace

// ==== Our images in BG3's GUI: WoW's minimap art ====
//
// BG3's GUI (Noesis) asks for every image through the texture provider BG3 installs
// (Noesis::GUI::SetTextureProvider; the provider is VGL's global, read by bg3+0x53590b0):
// GetTextureInfo(uri) for its size on the UI thread, LoadTexture(uri, device) for the texture on the
// render thread. BG3's provider only resolves pack URIs into its GUI paks and its own "Assets/Uri/..."
// entries (virtual textures, custom icons): an image file of ours never loads (an absolute path or an
// Ext.IO path override; harness). Op 110 gives the provider object a private copy of its
// vtable (as the physics scenes get one) whose GetTextureInfo and LoadTexture answer URIs inside
// this run's link/minimap/ folder (benilla's minimap images: uncompressed BGRA8 DDS, one level) and
// hand every other URI to BG3's own code. The texture is made by the render device Noesis passes in
// (RenderDevice::CreateTexture, RGBA8, one level, immutable), so it is an ordinary Noesis texture to
// BG3's GUI. The minimap layer WowUi.lua adds to BG3's map template binds such a URI.
// The original vtable is kept; op 110 mode 0 puts it back while ours is still installed.

namespace {

struct NsTextureInfo {
    std::uint32_t Width, Height, X, Y;
    float DpiScale;
};
// Itanium ABI: TextureInfo (20 bytes) and Ptr<Texture> (non-trivial) come back through a hidden
// pointer in the first register, `this` second.
using NsGetInfoFn = void (*)(NsTextureInfo* ret, void* self, void const* uri);
using NsLoadFn = void (*)(void** ret, void* self, void const* uri, void* device);
using NsCreateTexFn = void (*)(void** ret, void* self, char const* label, std::uint32_t w, std::uint32_t h,
                               std::uint32_t levels, int format, void const** data);
using NsUriStrFn = char const* (*)(void const* uri);

constexpr std::uintptr_t kNsGetTexProvider = 0x53590b0;  // Noesis::VGL::GetTextureProvider(): mov rax, [rip+..]; ret
constexpr unsigned char kNsGetTexProviderCode[] = {0x48, 0x8b, 0x05, 0x39, 0xd8, 0x80, 0x02, 0xc3};
constexpr std::uintptr_t kNsUriStr = 0x35e5000;  // Noesis::Uri::Str() const
constexpr unsigned char kNsUriStrCode[] = {0x81, 0x7f, 0x04, 0x00, 0x00, 0x00, 0x01, 0x48, 0x89, 0xf8, 0x73, 0x05};
constexpr std::uintptr_t kNsToString = 0x5316dc0, kNsEquals = 0x2be29a0;  // BaseObject's, slots 3 and 4 of every BaseComponent
constexpr int kNsSlotGetInfo = 6, kNsSlotLoad = 7, kNsSlotCreateTexture = 9;
constexpr int kNsVtCopy = 64;  // entries copied (from offset-to-top), enough for BG3's provider class

void* g_uitex_vt[kNsVtCopy];               // our copy: [0] offset to top, [1] type info, [2..] the slots
void* g_uitex_provider = nullptr;          // the provider whose vptr we replaced
void* const* g_uitex_orig_vptr = nullptr;  // its own vtable (slot 0)
NsGetInfoFn g_uitex_orig_info = nullptr;
NsLoadFn g_uitex_orig_load = nullptr;
std::atomic<std::uint32_t> g_uitex_infos{0}, g_uitex_loads{0}, g_uitex_fails{0};
std::atomic<std::uint32_t> g_uitex_last_us{0}, g_uitex_max_us{0};  // LoadTexture's own time (render thread)

bool code_at(std::uintptr_t va, unsigned char const* code, std::size_t n) {
    unsigned char got[16] = {};
    return n <= sizeof(got) && safe_read((void const*)(g_api.LoadBias() + va), got, n) && std::memcmp(got, code, n) == 0;
}

// Our folder's file for this URI, or empty: a name ending in .dds right after ".../wow-bg3/link/minimap/"
// (BG3's GUI hands the provider the URI as the binding wrote it, perhaps with a pack prefix), then
// optionally "?<anything>" (WowUi.lua makes each image's URI unique that way while benilla reuses
// four files). The file is always io_dir()/link/minimap/<name>.
std::atomic<int> g_uitex_logged{0};
std::string uitex_path(void const* uri) {
    static const NsUriStrFn str = code_at(kNsUriStr, kNsUriStrCode, sizeof(kNsUriStrCode))
        ? reinterpret_cast<NsUriStrFn>(g_api.LoadBias() + kNsUriStr) : nullptr;
    if (str == nullptr || uri == nullptr) return {};
    char const* s = str(uri);
    if (s == nullptr) return {};
    static constexpr char kTail[] = "/wow-bg3/link/minimap/";
    char const* at = std::strstr(s, kTail);
    if (at == nullptr) return {};
    if (g_uitex_logged.fetch_add(1) < 4) logf("wowbg3 ui textures: uri %.300s", s);
    char const* name = at + sizeof(kTail) - 1;
    char const* q = std::strchr(name, '?');
    const std::size_t n = q != nullptr ? (std::size_t)(q - name) : std::strlen(name);
    if (n < 5 || n > 64) return {};
    const std::string file(name, n);
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos || file.find("..") != std::string::npos
        || file.compare(n - 4, 4, ".dds") != 0) {
        return {};
    }
    return io_dir() + "link/minimap/" + file;
}

// The DDS header of our images (legacy, A8R8G8B8, BGRA in memory): width and height, or false.
bool uitex_header(unsigned char const* h, std::size_t n, std::uint32_t& w, std::uint32_t& ht) {
    if (n < 128 || std::memcmp(h, "DDS ", 4) != 0) return false;
    std::uint32_t v[32];
    std::memcpy(v, h, 128);
    ht = v[3];
    w = v[4];
    // v[20] pixel format flags (RGB, alpha), v[22] bits, v[23..26] R, G, B, A masks
    return v[1] == 124 && (v[20] & 0x40) != 0 && v[22] == 32 && v[23] == 0x00ff0000u && v[24] == 0x0000ff00u
        && v[25] == 0x000000ffu && w >= 1 && ht >= 1 && w <= 4096 && ht <= 4096;
}

bool uitex_read(std::string const& path, std::vector<unsigned char>& out, std::size_t want) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    out.resize(want);
    std::size_t got = 0;
    while (got < want) {
        const ssize_t r = ::read(fd, out.data() + got, want - got);
        if (r <= 0) break;
        got += (std::size_t)r;
    }
    ::close(fd);
    out.resize(got);
    return got == want;
}

void uitex_get_info(NsTextureInfo* ret, void* self, void const* uri) {
    const std::string path = uitex_path(uri);
    if (path.empty()) {
        g_uitex_orig_info(ret, self, uri);
        return;
    }
    *ret = NsTextureInfo{0, 0, 0, 0, 1.0f};
    std::vector<unsigned char> h;
    std::uint32_t w = 0, ht = 0;
    if (uitex_read(path, h, 128) && uitex_header(h.data(), h.size(), w, ht)) {
        ret->Width = w;
        ret->Height = ht;
        g_uitex_infos.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_uitex_fails.fetch_add(1, std::memory_order_relaxed);
    }
}

void uitex_load(void** ret, void* self, void const* uri, void* device) {
    const std::string path = uitex_path(uri);
    if (path.empty()) {
        g_uitex_orig_load(ret, self, uri, device);
        return;
    }
    *ret = nullptr;
    const double t0 = mono_seconds();
    // The device must be a Noesis RenderDevice (BaseObject's ToString and Equals in slots 3 and 4).
    void* const* dvt = nullptr;
    void* slots[kNsSlotCreateTexture + 1] = {};
    const std::uintptr_t bias = g_api.LoadBias();
    if (device == nullptr || !safe_read(device, &dvt, sizeof(dvt)) || dvt == nullptr || !safe_read(dvt, slots, sizeof(slots))
        || (std::uintptr_t)slots[3] != bias + kNsToString || (std::uintptr_t)slots[4] != bias + kNsEquals) {
        g_uitex_fails.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<unsigned char> h;
    std::uint32_t w = 0, ht = 0;
    if (!uitex_read(path, h, 128) || !uitex_header(h.data(), h.size(), w, ht)) {
        g_uitex_fails.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<unsigned char> file;
    if (!uitex_read(path, file, 128 + (std::size_t)w * ht * 4)) {
        g_uitex_fails.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // BGRA -> RGBA in place (RenderDevice's RGBA8 is R first).
    unsigned char* px = file.data() + 128;
    for (std::size_t i = 0, n = (std::size_t)w * ht; i < n; ++i) std::swap(px[i * 4], px[i * 4 + 2]);
    void const* levels[1] = {px};
    void* tex = nullptr;
    reinterpret_cast<NsCreateTexFn>(slots[kNsSlotCreateTexture])(&tex, device, "wowbg3-minimap", w, ht, 1, 0 /* RGBA8 */, levels);
    if (tex == nullptr) {
        g_uitex_fails.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    *ret = tex;  // the Ptr's reference moves to the caller
    const std::uint32_t us = (std::uint32_t)((mono_seconds() - t0) * 1e6);
    g_uitex_last_us.store(us);
    if (us > g_uitex_max_us.load()) g_uitex_max_us.store(us);
    if (g_uitex_loads.fetch_add(1, std::memory_order_relaxed) < 20) {
        logf("wowbg3 ui textures: %ux%u minimap texture made in %.2f ms", w, ht, us / 1000.0);
    }
}

void* uitex_current_provider() {
    if (!code_at(kNsGetTexProvider, kNsGetTexProviderCode, sizeof(kNsGetTexProviderCode))) return nullptr;
    // mov rax, [rip + 0x280d839] at +0: the global sits at +7 + 0x280d839.
    void* p = nullptr;
    return safe_read((void const*)(g_api.LoadBias() + kNsGetTexProvider + 7 + 0x280d839), &p, sizeof(p)) ? p : nullptr;
}

// mode 1: install on the current provider (again if BG3 made a new one), 0: put BG3's vtable back,
// 2: report. R0 1 ours is installed, R1 textures made, R2 sizes answered, R3 failures; F0 1 when this
// call installed it, F1 the last texture's time in LoadTexture (ms, render thread), F2 the longest.
bool ui_textures(Out& o, int mode) {
    void* p = uitex_current_provider();
    if (p == nullptr) return fail(o, 1100, "no Noesis texture provider (or the code changed)");
    void* const* vptr = nullptr;
    if (!safe_read(p, &vptr, sizeof(vptr)) || vptr == nullptr) return fail(o, 1101, "provider unreadable");
    const bool ours = vptr == (void* const*)&g_uitex_vt[2];
    if (mode == 1 && !ours) {
        void* copy[kNsVtCopy] = {};
        if (!safe_read(vptr - 2, copy, sizeof(copy))) return fail(o, 1102, "provider vtable unreadable");
        const std::uintptr_t bias = g_api.LoadBias();
        if ((std::uintptr_t)copy[2 + 3] != bias + kNsToString || (std::uintptr_t)copy[2 + 4] != bias + kNsEquals
            || copy[2 + kNsSlotGetInfo] == nullptr || copy[2 + kNsSlotLoad] == nullptr) {
            return fail(o, 1103, "provider vtable is not a Noesis TextureProvider's");
        }
        if (g_uitex_provider != nullptr && g_uitex_provider != p) {
            logf("wowbg3 ui textures: a new texture provider %p (was %p)", p, g_uitex_provider);
        }
        g_uitex_orig_info = reinterpret_cast<NsGetInfoFn>(copy[2 + kNsSlotGetInfo]);
        g_uitex_orig_load = reinterpret_cast<NsLoadFn>(copy[2 + kNsSlotLoad]);
        copy[2 + kNsSlotGetInfo] = reinterpret_cast<void*>(&uitex_get_info);
        copy[2 + kNsSlotLoad] = reinterpret_cast<void*>(&uitex_load);
        std::memcpy(g_uitex_vt, copy, sizeof(copy));
        g_uitex_provider = p;
        g_uitex_orig_vptr = vptr;
        std::atomic_thread_fence(std::memory_order_release);
        reinterpret_cast<std::atomic<void* const*>*>(p)->store((void* const*)&g_uitex_vt[2]);
        logf("wowbg3 ui textures: provider %p (vtable bg3+0x%lx) answers link/minimap images", p,
             (unsigned long)((std::uintptr_t)vptr - bias));
        o.F[0] = 1.0f;
    } else if (mode == 0 && ours && p == g_uitex_provider && g_uitex_orig_vptr != nullptr) {
        reinterpret_cast<std::atomic<void* const*>*>(p)->store(g_uitex_orig_vptr);
        logf("wowbg3 ui textures: provider %p has its own vtable again", p);
    }
    void* const* now = nullptr;
    safe_read(p, &now, sizeof(now));
    o.R[0] = now == (void* const*)&g_uitex_vt[2] ? 1 : 0;
    o.R[1] = (int)g_uitex_loads.load();
    o.R[2] = (int)g_uitex_infos.load();
    o.R[3] = (int)g_uitex_fails.load();
    o.F[1] = g_uitex_last_us.load() / 1000.0f;
    o.F[2] = g_uitex_max_us.load() / 1000.0f;
    return true;
}

}  // namespace

// ==== The channel: one plugin command per reserved query ====

namespace {

// ---- stand-ins make no sound (op 150) ----
//
// Found in playtesting: the hidden stand-ins played their own BG3 sounds (wolves' idle
// howls, barks, footsteps, their bite's cast, pain and death vocals) over the WoW creatures, which play
// their own since benilla 0030. On BG3's client a character's sounds go to its Wwise game objects:
// ls::SkeletonSoundObjectsComponent holds up to nine, slot 0 the character's own ls::SoundComponent, the
// others sound objects on bones (head, hands, body), and BG3's systems (animation text keys, vocals,
// footsteps, barks, spell casts, whooshes) post their events there. Op 150 takes one game object out of
// Wwise's mix the engine's own way: AK::SoundEngine::SetListeners with no listener, so Wwise renders
// nothing that object emits, whichever system posts it. Mode 0 gives it the default listeners back
// (AK::SoundEngine::ResetListenersToDefault, what BG3's own Wwise manager calls for a listener of -1).
// Both functions are found by name in the game's symbol table (bg3le's resolver) and checked against
// this build's code; both only queue a message for Wwise's audio thread, as every Wwise call does.
// Nothing global changes, and an object BG3 unregisters takes the setting with it (a later registration
// of the same id starts with the default listeners again). Silence.lua decides which objects are a
// stand-in's.

constexpr char kAkSetListenersCode[] =
    "803dxxxxxxxx000f84xxxxxxxx415741565389d38d04dd1c0000000fb7d0488b0dxxxxxxxxb81f00";
constexpr char kAkResetListenersCode[] = "803dxxxxxxxx007436534889fbe8xxxxxxxx0fb7d0488b3dxxxxxxxxbe11000000e8";
using AkSetListenersFn = int (*)(std::uint64_t, std::uint64_t const*, std::uint32_t);
using AkResetListenersFn = int (*)(std::uint64_t);
struct SoundMute {
    std::mutex M;
    AkSetListenersFn Set = nullptr;
    AkResetListenersFn Reset = nullptr;
    char const* Why = nullptr;  // why the functions are off (looked up once)
    bool Tried = false;
    std::vector<std::uint64_t> Muted;  // objects op 150 took out and did not give back (report only)
    std::uint64_t Calls = 0;
};
SoundMute g_mute;

bool sound_mute_resolve(char const** why) {
    if (g_mute.Set != nullptr && g_mute.Reset != nullptr) return true;
    if (g_mute.Tried) return (*why = g_mute.Why), false;
    g_mute.Tried = true;
    auto off = [&](char const* w) { g_mute.Why = w; *why = w; return false; };
    using ResolveFn = std::uintptr_t (*)(char const*);
    auto resolve = reinterpret_cast<ResolveFn>(::dlsym(RTLD_DEFAULT, "_ZN5bg3le14resolve_symbolEPKc"));
    if (resolve == nullptr || g_api.LoadBias == nullptr) return off("bg3le's symbol resolver is not exported");
    const std::uintptr_t bias = g_api.LoadBias();
    const std::uintptr_t set = resolve("_ZN2AK11SoundEngine12SetListenersEmPKmj");
    const std::uintptr_t reset = resolve("_ZN2AK11SoundEngine23ResetListenersToDefaultEm");
    if (set == 0 || reset == 0) return off("Wwise's listener functions are not in the game's symbols");
    if (!code_is(bias + set, kAkSetListenersCode, nullptr)) return off("AK::SoundEngine::SetListeners differs from the known build");
    if (!code_is(bias + reset, kAkResetListenersCode, nullptr)) {
        return off("AK::SoundEngine::ResetListenersToDefault differs from the known build");
    }
    g_mute.Set = reinterpret_cast<AkSetListenersFn>(bias + set);
    g_mute.Reset = reinterpret_cast<AkResetListenersFn>(bias + reset);
    logf("sound mute: Wwise SetListeners bg3+%#zx, ResetListenersToDefault bg3+%#zx", (std::size_t)set, (std::size_t)reset);
    return true;
}

// Op 150 (client): mode 1 leaves the game object no listener, 0 gives it the default ones back, 2 reports.
// R0 Wwise's result (1 AK_Success; 1 for a report once the functions are found), R1 objects taken out and
// not given back, R2 calls.
bool sound_mute(Out& o, bool client, std::uint64_t object, int mode) {
    if (!client) return fail(o, 1500, "client only: the sound objects are the client's");
    if (mode < 0 || mode > 2) return fail(o, 1501, "mode is 0 (default listeners), 1 (no listener) or 2 (report)");
    std::lock_guard<std::mutex> lock(g_mute.M);
    char const* why = nullptr;
    if (!sound_mute_resolve(&why)) return fail(o, 1502, why != nullptr ? why : "Wwise's listener functions not found");
    o.R[0] = 1;
    if (mode != 2) {
        if (object == 0 || object == ~0ull) return fail(o, 1503, "not a game object");
        const int r = mode == 1 ? g_mute.Set(object, nullptr, 0) : g_mute.Reset(object);
        ++g_mute.Calls;
        o.R[0] = r;
        auto it = std::find(g_mute.Muted.begin(), g_mute.Muted.end(), object);
        if (r == 1 && mode == 1 && it == g_mute.Muted.end() && g_mute.Muted.size() < 4096) g_mute.Muted.push_back(object);
        if (r == 1 && mode == 0 && it != g_mute.Muted.end()) g_mute.Muted.erase(it);
        if (r != 1) {
            o.R[1] = (int)g_mute.Muted.size();
            return fail(o, 1504, "Wwise refused the listener change");
        }
    }
    o.R[1] = (int)g_mute.Muted.size();
    o.R[2] = (int)std::min<std::uint64_t>(g_mute.Calls, 0x7fffffff);
    return true;
}

enum Op : std::uint32_t {
    kPing = 1,
    kStamp = 2,        // a: minX, minZ, anchorY; b.x layer tolerance (m), b.y void, b.z void layer Y; tiles from arena-tiles.bin
    kRestore = 3,
    kRays = 4,         // a.x void, a.y group mask, a.z contexts left to BG3 (bits); triangles from arena-triangles.bin
    kRaysOff = 5,
    kRaysStats = 6,    // writes native-<side>-rays.json
    kCamInstall = 7,   // controller address in 16-bit pieces a.x, a.y, a.z
    kCamPose = 8,      // a.x 1: view from native-camera-view.txt (16 numbers); 0: BG3's own
    kCamOff = 9,
    kCamStats = 10,    // R: getter calls, served; F: BG3's current view
    kCountOn = 11,     // address pieces a.x..a.z, b.x slots
    kCountRead = 12,   // writes native-count.json
    kCountOff = 13,
    kBasis = 14,       // a: WoW origin, b: BG3 origin
    kFollow = 15,      // a.x yaw (rad), a.y follow on, a.z use WoW's FOV, b.x mirror
    kStatus = 16,
    kLimits = 17,      // a.x max tiles per side, a.y max triangle span (m)
    kCamHold = 18,     // a.x seconds (0..5) of BG3's own camera (0 ends a hold)
    kGridSurvey = 19,  // read-only: native-<side>-grid.json/.bin
    kColumns = 20,     // read-only: a (x0, z0, step), b (x1, z1, top) -> native-<side>-columns.json
    kSceneCounts = 21, // read-only: calls per scene vtable slot -> native-<side>-scene-counts.json
    kLead = 22,        // a.x 1: write BG3's own camera to link/bg3cam.bin every frame (benilla follows); 0: stop
    kPool = 23,        // a.x layer Y, a.y count: snapshot whole plain subgrids of that layer -> native-<side>-pool.json
    kPlace = 24,       // a.x pool index, a.y minX, a.z minZ, b.x base Y, b.y serial; tiles from section-<k>[-<serial>].bin
    kPoolRestore = 25, // every pool subgrid back home
    kProxyCheck = 26,  // R0: 0 proxy shape kept, 1 replaced, 2 none left (rays off)
    kBounds = 27,      // read-only: R grid bounds (min x, z, max x, z in tiles), F0..2 min corner (m), F3 1: Pool found
                       // them where expected, F4 1: they match the subgrids now
    kLeadSource = 29,  // client, leading: address pieces a.x..a.z of the camera controller that renders (BG3's
                       // action camera), 0 0 0 the game camera; R0 1 while another camera leads, R1 switches
    kPickRegion = 28,  // client: a.x 1 BG3's picking region map emptied (world mode), 0 back, 2 report; R0 on,
                       // R1 cells to put back, R2 width, R3 player 1's region; F0 scale, F1/F2 origin, F3 cells now
    kSpatialGrid = 30, // a.x..a.z address pieces of this side's spatial_grid::UpdateSystem, b.x 1: the engine rebuilds
                       // its spatial grid from the AI grid's bounds (world mode far from the level), 2: report;
                       // R0 flags (1 clear, 2 bounds, 4 reinsert), R1 1 set now; F0..F3 AI grid bounds (m)
    kSightSync = 31,   // client: BG3's copies of where the pool subgrids are (AI grid visuals with their boxes,
                       // the sight overlay's records) follow them (Pool and PoolRestore do this themselves);
                       // a.x 1 move them (and rebuild the engine's box tree), 2 report; R0 visuals moved, R1 boxes
                       // moved, R2 1 tree rebuilt, R3 pool subgrids seen; F0..F3 the first one's visual x, z
                       // before and its subgrid's x, z; F4 sight records moved, F5 sight records found
    kGridTextures = 50, // client: the pool subgrids' sight / surface textures (heights, blocked mask) again from their live
                        // tiles, as BG3 builds them (Place and PoolRestore do this themselves); a.x 1 rebuild, 2 report;
                        // a.y pool index (-1 every one); R0 subgrids, R1 with both textures, R2 player passes, R3 seen;
                        // report: F0 walkable tiles whose height differs from the tile's, F1 mask, F2 subgrids that match
                        // (see op 50)
    kPoolSurfaces = 60, // server: ground surfaces on pool subgrid a.x (-1 every one); a.y 1: remove them the engine's
                        // way (only from a placed one; Place and PoolRestore do it themselves), 2: report; R0 tiles
                        // with a ground surface, R1 with a cloud, R2 subgrids looked at, R3 1 removed
    kPoolFree = 61,     // a.x pool index: R0 1 when Place would take it now (nothing registered on its tiles,
                        // nothing attached), R1 tiles holding something, R2 attached objects, R3 entities seen;
                        // F0/F1 the first such tile (x, y); native-<side>-occupants.json lists them
    kCustomIcon = 40,  // a.x..a.z address pieces of an eoc::CustomIconComponent's Icon (ScratchBuffer), b.x the
                       // portrait key (link/portraits/<key>.webp), b.y 1: fill it, 2: report; R0 bytes, R1/R2 size,
                       // R3 1 the buffer was empty, 2 it was ours
    kUiTextures = 110,  // client: a.x 1 BG3's GUI texture provider also answers link/minimap/*.dds (WoW's minimap
                        // art, uncompressed BGRA8), 0 BG3's vtable back, 2 report; R0 installed, R1 textures
                        // made, R2 sizes answered, R3 failures
    kDisplayLevel = 41, // server: a.x, a.y, a.z, b.x the stand-in's entity handle in 16-bit pieces (low first), b.y
                        // its WoW level (1..99; 0 forget it, -1 forget all, -2 report): its replicated
                        // eoc::LevelComponent carries that level to the client (AvailableLevel stays BG3's:
                        // a rise there is BG3's level-up); R0 1 hook on, R1 table size, R2 levels sent so far
    kSoundMute = 150,   // client: a.x, a.y, a.z, b.x a Wwise game object id in 16-bit pieces (low first), b.y 1:
                        // no listener (Wwise renders nothing it emits; a stand-in's sound objects), 0: the default
                        // listeners back, 2: report; R0 Wwise's result (1 ok), R1 objects without listeners, R2 calls
};

void* address_of(glm::vec3 const& a) {
    const std::uint64_t p0 = (std::uint64_t)a.x, p1 = (std::uint64_t)a.y, p2 = (std::uint64_t)a.z;
    if (a.x < 0 || a.y < 0 || a.z < 0 || p0 > 0xffff || p1 > 0xffff || p2 > 0xffff) return nullptr;
    return reinterpret_cast<void*>(p0 | (p1 << 16) | (p2 << 32));
}

bool channel_call(int side, std::uint32_t op, glm::vec3 const& a, glm::vec3 const& b) {
    Out& o = g_out[side];
    out_reset(o);
    const bool client = side == 1;
    char const* why = nullptr;
    switch (op) {
    case kPing:
        o.R[0] = BG3LE_PLUGIN_ABI;
        o.R[1] = side;
        o.R[2] = g_scene[side].Scene != nullptr;
        return true;
    case kStamp: {
        std::vector<unsigned char> raw;
        if (!read_file("arena-tiles.bin", raw, 5u * 1024 * 1024)) return fail(o, 2, "no arena tiles file");
        const std::size_t len = raw.size();
        const int n = (int)std::lround(std::sqrt((double)(len / 5)));
        if (len % 5 != 0 || n < 1 || (std::size_t)n * n * 5 != len || n > g_arena_max_side) {
            return fail(o, 3, "arena tiles must be n*n records of 5 bytes within the side limit");
        }
        std::vector<unsigned char> walk((std::size_t)n * n);
        std::vector<float> floors((std::size_t)n * n);
        for (std::size_t i = 0; i < walk.size(); ++i) {
            walk[i] = raw[i * 5];
            std::memcpy(&floors[i], raw.data() + i * 5 + 1, 4);
            if (!std::isfinite(floors[i])) return fail(o, 4, "arena floor is not finite");
        }
        int counts[3] = {};
        if (!arena_stamp(client, a.x, a.y, a.z, n, walk.data(), floors.data(), b.x, b.y != 0.0f, b.z, counts, &why)) {
            return fail(o, 5, why != nullptr ? why : "arena stamp refused");
        }
        for (int i = 0; i < 3; ++i) o.R[i] = counts[i];
        o.R[3] = n;
        return true;
    }
    case kRestore: {
        int counts[3] = {};
        arena_restore(client, counts);
        for (int i = 0; i < 3; ++i) o.R[i] = counts[i];
        return true;
    }
    case kRays: {
        std::vector<unsigned char> raw;
        if (!read_file("arena-triangles.bin", raw, 36u * kMaxTris)) return fail(o, 6, "no arena triangles file");
        if (raw.empty() || raw.size() % 36 != 0) return fail(o, 7, "arena triangles must be records of 9 floats");
        std::vector<float> tris(raw.size() / 4);
        std::memcpy(tris.data(), raw.data(), raw.size());
        // Optional: what each triangle is (one byte each; another count means another set, ignored).
        std::vector<unsigned char> kinds;
        const bool haveKinds = read_file("arena-triangles.kinds", kinds, kMaxTris) && kinds.size() == raw.size() / 36;
        if (!haveKinds && !kinds.empty()) logf("arena-triangles.kinds does not match the triangles: ignored");
        if (!rays_on(side, tris.data(), raw.size() / 36, haveKinds ? kinds.data() : nullptr, a.x != 0.0f,
                     (std::uint32_t)a.y, (std::uint32_t)a.z, &why)) {
            return fail(o, 8, why != nullptr ? why : "arena rays refused");
        }
        o.R[0] = (int)(raw.size() / 36);
        return true;
    }
    case kRaysOff:
        rays_off(side);
        return true;
    case kRaysStats:
        write_file(client ? "native-client-rays.json" : "native-server-rays.json", rays_stats_json(side));
        for (int k = 0; k < 3; ++k) o.R[k] = (int)g_scene[side].Stats.Wow[k].load();
        return true;
    case kCamInstall:
        if (!client) return fail(o, 9, "the camera is client-side");
        if (!cam_install(address_of(a), &why)) return fail(o, 10, why != nullptr ? why : "camera hook refused");
        o.F[0] = g_cam.OwnFov;
        return true;
    case kCamPose: {
        if (!client) return fail(o, 9, "the camera is client-side");
        if (g_cam.Controller == nullptr) return fail(o, 11, "camera not hooked");
        if (a.x == 0.0f) {
            cam_pose(nullptr);
            return true;
        }
        std::vector<unsigned char> raw;
        if (!read_file("native-camera-view.txt", raw, 1024)) return fail(o, 12, "no view file");
        raw.push_back(0);
        float v[16];
        char const* p = reinterpret_cast<char const*>(raw.data());
        for (int i = 0; i < 16; ++i) {
            char* end = nullptr;
            v[i] = std::strtof(p, &end);
            if (end == p || !std::isfinite(v[i])) return fail(o, 13, "camera pose is not 16 finite numbers");
            p = end;
        }
        cam_pose(v);
        return true;
    }
    case kCamOff:
        if (client) {
            g_follow.On = false;
            src_remove("camera off");
            cam_remove();
        }
        return true;
    case kCamStats: {
        if (!client) return fail(o, 9, "the camera is client-side");
        o.R[0] = (int)std::min<std::uint64_t>(g_cam.Calls[0] + g_cam.Calls[1] + g_cam.Calls[2], 0x7fffffff);
        o.R[1] = (int)std::min<std::uint64_t>(g_cam.Served[0] + g_cam.Served[1] + g_cam.Served[2], 0x7fffffff);
        o.R[2] = (int)std::min<std::uint64_t>(g_cam.Served[3], 0x7fffffff);
        o.R[3] = (int)std::min<std::uint64_t>(g_follow.Frames, 0x7fffffff);
        if (!cam_view(o.F)) return fail(o, 11, "camera not hooked");
        return true;
    }
    case kCountOn:
        if (!count_install(address_of(a), (int)b.x, &why)) return fail(o, 14, why != nullptr ? why : "counting refused");
        return true;
    case kCountRead: {
        std::string j = "[";
        if (g_count.Page != nullptr) {
            auto* counters = reinterpret_cast<std::uint64_t const*>(g_count.Page + 4096 + 1024);
            for (int i = 0; i < g_count.Slots; ++i) {
                j += (i ? "," : "") + std::to_string(__atomic_load_n(&counters[i], __ATOMIC_RELAXED));
            }
        }
        j += "]";
        write_file("native-count.json", j);
        o.R[0] = g_count.Slots;
        return true;
    }
    case kCountOff:
        count_remove();
        return true;
    case kBasis:
        if (!client) return fail(o, 9, "the camera is client-side");
        g_basis.Wow[0] = a.x; g_basis.Wow[1] = a.y; g_basis.Wow[2] = a.z;
        g_basis.Bg3[0] = b.x; g_basis.Bg3[1] = b.y; g_basis.Bg3[2] = b.z;
        g_basis.Set = true;
        logf("follow: basis WoW %.2f %.2f %.2f -> BG3 %.2f %.2f %.2f", a.x, a.y, a.z, b.x, b.y, b.z);
        return true;
    case kLeadSource: {
        if (!client) return fail(o, 9, "the camera is client-side");
        if (g_cam.Controller == nullptr) return fail(o, 11, "camera not hooked");
        void* c = (a.x == 0.0f && a.y == 0.0f && a.z == 0.0f) ? nullptr : address_of(a);
        if (c == nullptr) {
            src_remove("asked");
        } else if (c != g_src.Controller.load() && !src_install(c, &why)) {
            return fail(o, 34, why != nullptr ? why : "lead source refused");
        }
        o.R[0] = g_src.Controller.load() != nullptr;
        o.R[1] = (int)std::min<std::uint64_t>(g_src.Switches, 0x7fffffff);
        return true;
    }
    case kLead:
        if (!client) return fail(o, 9, "the camera is client-side");
        if (g_cam.Controller == nullptr || !g_basis.Set) return fail(o, 11, "camera not hooked or no basis");
        g_lead.On = a.x != 0.0f;
        if (!g_lead.On) src_remove("lead off");
        if (g_lead.On) {
            g_follow.On = false;  // one owner of the camera at a time
            g_cam.Current.store(-1, std::memory_order_release);
            g_cam.Fov.store(0.0f, std::memory_order_release);
        }
        o.R[0] = (int)std::min<std::uint64_t>(g_lead.Frames, 0x7fffffff);
        logf("lead: %s", g_lead.On ? "on" : "off");
        return true;
    case kFollow:
        if (!client) return fail(o, 9, "the camera is client-side");
        g_basis.Yaw = a.x;
        g_basis.Mirror = b.x != 0.0f;
        g_follow.On = a.y != 0.0f;
        if (g_follow.On) g_lead.On = false;
        g_follow.UseFov = a.z != 0.0f;
        g_follow.LastSeq = 0;
        if (!g_follow.On) {
            cam_pose(nullptr);
            g_cam.Fov.store(0.0f);
        }
        logf("follow: %s, yaw %.4f, fov %d, mirror %d", g_follow.On ? "on" : "off", a.x, g_follow.UseFov ? 1 : 0,
             g_basis.Mirror ? 1 : 0);
        return true;
    case kStatus:
        o.R[0] = (int)g_arena[side].Tiles.size();
        o.R[1] = g_scene[side].Rays.load() != nullptr;
        o.R[2] = client && g_cam.Controller != nullptr;
        o.R[3] = (int)std::min<std::uint64_t>(g_follow.Frames, 0x7fffffff);
        o.F[0] = (float)(g_follow.LastAt > 0 ? mono_seconds() - g_follow.LastAt : -1.0);
        o.F[1] = (float)g_follow.Stale;
        // Leading's eye clearance: eyes computed, moved, last and largest move (m), on.
        o.F[2] = (float)g_clear.Computed.load();
        o.F[3] = (float)g_clear.Moved.load();
        o.F[4] = g_clear.LastDist.load();
        o.F[5] = g_clear.MaxDist.load();
        o.F[6] = g_clear.On ? 1.0f : 0.0f;
        if (const int i = g_clear.Current[0].load(); i >= 0) {  // the game camera's own eye, latest computed
            o.F[7] = g_clear.Ring[0][i].Key.x;
            o.F[8] = g_clear.Ring[0][i].Key.y;
            o.F[9] = g_clear.Ring[0][i].Key.z;
        }
        o.F[10] = g_src.Controller.load() != nullptr ? 1.0f : 0.0f;  // the action camera renders and leads
        o.F[11] = (float)g_src.Switches;
        return true;
    case kGridSurvey: {
        int counts[3] = {};
        if (!grid_survey(client, counts, &why)) return fail(o, 15, why != nullptr ? why : "survey failed");
        for (int i = 0; i < 3; ++i) o.R[i] = counts[i];
        return true;
    }
    case kColumns: {
        int counts[2] = {};
        if (!column_probe(side, a.x, a.y, b.x, b.y, a.z, b.z, counts, &why)) {
            return fail(o, 16, why != nullptr ? why : "column probe failed");
        }
        o.R[0] = counts[0];
        o.R[1] = counts[1];
        return true;
    }
    case kSceneCounts:
        write_file(client ? "native-client-scene-counts.json" : "native-server-scene-counts.json", scene_counts_json(side));
        return true;
    case kCamHold:
        if (!client) return fail(o, 9, "the camera is client-side");
        g_cam.HoldUntil = mono_seconds() + std::clamp(a.x, 0.0f, 5.0f);
        g_cam.Hold.store(a.x > 0.0f, std::memory_order_release);
        return true;
    case kPool: {
        int counts[3] = {};
        std::vector<bg3se::AiSubgridId> ids;
        if (a.z != 0.0f) {
            std::vector<unsigned char> raw;
            if (!read_file("pool-ids.txt", raw, 4096)) return fail(o, 17, "no pool-ids.txt");
            raw.push_back(0);
            char const* q = reinterpret_cast<char const*>(raw.data());
            while (*q != 0 && ids.size() < (std::size_t)kPoolMax) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(q, &end, 10);
                if (end == q) break;
                ids.push_back((bg3se::AiSubgridId)v);
                q = end;
            }
            if (ids.empty()) return fail(o, 17, "pool-ids.txt lists no ids");
        }
        // b.x 1 (0.2.9): the layers from pool-layers.txt (heights, whitespace separated), else a.x alone.
        std::vector<float> layers;
        if (b.x == 1.0f) {
            std::vector<unsigned char> raw;
            if (!read_file("pool-layers.txt", raw, 4096)) return fail(o, 17, "no pool-layers.txt");
            raw.push_back(0);
            char const* q = reinterpret_cast<char const*>(raw.data());
            while (*q != 0 && (int)layers.size() < kPoolLayersMax) {
                char* end = nullptr;
                const float v = std::strtof(q, &end);
                if (end == q) break;
                if (std::isfinite(v)) layers.push_back(v);
                q = end;
            }
            if (layers.empty()) return fail(o, 17, "pool-layers.txt lists no layers");
        } else {
            layers.push_back(a.x);
        }
        if (!pool_init(client, layers, (int)a.y, a.z != 0.0f ? &ids : nullptr, counts, &why)) {
            return fail(o, 17, why != nullptr ? why : "no pool");
        }
        o.R[0] = counts[0];
        return true;
    }
    case kPlace: {
        const int k = (int)a.x, serial = (int)b.y;
        std::vector<unsigned char> raw;
        const std::string name = "section-" + std::to_string(k) + (serial > 0 ? "-" + std::to_string(serial) : "") + ".bin";
        if (k < 0 || k >= kPoolMax || serial < 0 || serial >= 4096 || !read_file(name, raw, 1u << 20)) {
            return fail(o, 18, "no section tiles file for that pool index");
        }
        int counts[3] = {};
        if (!pool_place(client, k, a.y, a.z, b.x, raw, counts, &why)) return fail(o, 19, why != nullptr ? why : "place refused");
        write_file(client ? "native-client-pool.json" : "native-server-pool.json", pool_json(g_pool[side]));
        o.R[0] = counts[0];
        o.R[1] = counts[1];
        return true;
    }
    case kPoolFree: {
        auto& p = g_pool[side];
        const int k = (int)a.x;
        if (p.Subs.empty() || !pool_alive(client)) return fail(o, 64, "no pool (or the level changed)");
        if (k < 0 || k >= (int)p.Subs.size()) return fail(o, 65, "pool index out of range");
        auto const& s = p.Subs[k];
        const Occupants occ = pool_occupants(p.Grid, s.Sg);
        o.R[0] = occ.Tiles == 0 && occ.Attached == 0 ? 1 : 0;
        o.R[1] = occ.Tiles;
        o.R[2] = occ.Attached;
        o.R[3] = (int)occ.Entities.size();
        o.F[0] = (float)occ.FirstX;
        o.F[1] = (float)occ.FirstY;
        write_file(client ? "native-client-occupants.json" : "native-server-occupants.json", occupants_json(k, s, occ));
        return true;
    }
    case kPoolSurfaces: {
        if (client) return fail(o, 60, "server only: the server owns the surfaces");
        auto& p = g_pool[0];
        if (p.Subs.empty() || !pool_alive(false)) return fail(o, 61, "no pool (or the level changed)");
        const int k = (int)a.x, mode = (int)a.y;
        if (mode != 1 && mode != 2) return fail(o, 62, "mode is 1 (remove) or 2 (report)");
        for (int i = 0; i < (int)p.Subs.size(); ++i) {
            if (k >= 0 && i != k) continue;
            auto& s = p.Subs[i];
            if (mode == 1 && !s.Moved) continue;  // at home: the level's own chasm
            int found[2] = {};
            if (!pool_unsurface(p, s, mode == 1, found, &why)) return fail(o, 63, why != nullptr ? why : "no surfaces");
            o.R[0] += found[0];
            o.R[1] += found[1];
            ++o.R[2];
        }
        o.R[3] = mode == 1 ? 1 : 0;
        return true;
    }
    case kPoolRestore: {
        int counts[3] = {};
        pool_restore(client, counts);
        for (int i = 0; i < 3; ++i) o.R[i] = counts[i];
        return true;
    }
    case kProxyCheck:
        o.R[0] = proxy_check(side);
        return true;
    case kBounds: {
        auto* grid = ai_grid(client);
        GridBounds now{};
        if (grid == nullptr || !safe_read(grid_bounds(grid), &now, sizeof(now))) return fail(o, 20, "no AI grid");
        o.R[0] = now.Min.x;
        o.R[1] = now.Min.y;
        o.R[2] = now.Max.x;
        o.R[3] = now.Max.y;
        o.F[0] = now.WorldMin.x;
        o.F[1] = now.WorldMin.y;
        o.F[2] = now.WorldMin.z;
        o.F[3] = g_pool[side].BoundsOk ? 1.0f : 0.0f;
        o.F[4] = bounds_equal(now, computed_bounds(grid)) ? 1.0f : 0.0f;
        return true;
    }
    case kPickRegion:
        if (!client) return fail(o, 32, "the picking region map is client only");
        if (a.x != 0.0f && a.x != 1.0f && a.x != 2.0f) return fail(o, 33, "mode is 0, 1 or 2");
        return pick_region(o, (int)a.x);
    case kLimits:
        if (a.x >= 8 && a.x <= 512) g_arena_max_side = (int)a.x;
        if (a.y >= 10 && a.y <= kMaxSpanLimit) g_max_span = a.y;
        o.R[0] = g_arena_max_side;
        o.F[0] = g_max_span;
        return true;
    case kSpatialGrid:
        if (b.x != 1.0f && b.x != 2.0f) return fail(o, 45, "mode is 1 or 2");
        return spatial_grid(o, side, address_of(a), (int)b.x);
    case kGridTextures:
        if (!client) return fail(o, 504, "client only");
        if (a.x != 1.0f && a.x != 2.0f) return fail(o, 505, "mode is 1 or 2");
        if (a.y != std::floor(a.y) || a.y < -1.0f || a.y >= (float)g_pool[1].Subs.size()) return fail(o, 506, "pool index out of range");
        return grid_textures(o, g_pool[1], (int)a.x, (int)a.y);
    case kSightSync:
        if (!client) return fail(o, 46, "client only");
        if (a.x != 1.0f && a.x != 2.0f) return fail(o, 47, "mode is 1 or 2");
        return sight_sync(o, g_pool[1], (int)a.x);
    case kCustomIcon:
        if (b.y != 1.0f && b.y != 2.0f) return fail(o, 404, "mode is 1 or 2");
        if (!(b.x >= 0.0f && b.x < 16777216.0f) || b.x != std::floor(b.x)) return fail(o, 405, "bad portrait key");
        return custom_icon(o, address_of(a), (int)b.x, (int)b.y);
    case kDisplayLevel: {
        const float p[4] = {a.x, a.y, a.z, b.x};
        std::uint64_t handle = 0;
        for (int i = 0; i < 4; ++i) {
            if (!(p[i] >= 0.0f && p[i] <= 65535.0f) || p[i] != std::floor(p[i])) return fail(o, 414, "bad handle piece");
            handle |= (std::uint64_t)p[i] << (16 * i);
        }
        if (b.y != std::floor(b.y)) return fail(o, 415, "level is a whole number");
        return display_level(o, client, handle, (int)b.y);
    }
    case kUiTextures:
        if (!client) return fail(o, 1104, "client only: the GUI is the client's");
        if (a.x != 0.0f && a.x != 1.0f && a.x != 2.0f) return fail(o, 1105, "mode is 0, 1 or 2");
        return ui_textures(o, (int)a.x);
    case kSoundMute: {
        const float p[4] = {a.x, a.y, a.z, b.x};
        std::uint64_t object = 0;
        for (int i = 0; i < 4; ++i) {
            if (!(p[i] >= 0.0f && p[i] <= 65535.0f) || p[i] != std::floor(p[i])) return fail(o, 1505, "bad game object piece");
            object |= (std::uint64_t)p[i] << (16 * i);
        }
        if (b.y != std::floor(b.y)) return fail(o, 1501, "mode is 0, 1 or 2");
        return sound_mute(o, client, object, (int)b.y);
    }
    default:
        return fail(o, 1, "unknown plugin op");
    }
}

// ---- settings and the frame handler ----

int g_set_channel = 0;         // status: 1 server scene hooked, 2 client scene hooked
int g_set_out[4] = {};         // status: server Out lo/hi, client Out lo/hi
int g_set_frames = 0;          // status: frames seen
int g_set_follow_frames = 0;   // status: camera poses taken from benilla
std::uint64_t g_frames = 0;

void publish_out() {
    for (int side = 0; side < 2; ++side) {
        const auto p = reinterpret_cast<std::uintptr_t>(&g_out[side]);
        g_set_out[side * 2] = (int)(std::uint32_t)(p & 0xffffffffu);
        g_set_out[side * 2 + 1] = (int)(std::uint32_t)(p >> 32);
    }
}

void frame(void*, double) {
    ++g_frames;
    g_set_frames = (int)std::min<std::uint64_t>(g_frames, 0x7fffffff);
    if (g_frames % 30 == 0) {
        std::lock_guard<std::mutex> held(g_install_lock);
        int mask = 0;
        for (int side = 0; side < 2; ++side) {
            char const* why = nullptr;
            void* scene = physics_scene_raw(side == 1);
            if (scene == nullptr) {
                // Between levels: rays off; the scene object may come back (the client's does),
                // so its table stays tracked and a new object is handled by scene_hook.
                if (g_scene[side].Rays.exchange(nullptr) != nullptr) {
                    logf("%s level gone: arena rays off", side ? "client" : "server");
                }
                continue;
            }
            if (scene_hook(side, &why)) {
                mask |= 1 << side;
            } else {
                static int warned[2] = {};
                if (warned[side]++ < 3) logf("%s scene not hooked: %s", side ? "client" : "server", why);
            }
        }
        g_set_channel = mask;
        pick_level_check();
        // No level: BG3's own camera back and following stops (the controller stays hooked,
        // pass-through; Lua's restore removes it).
        if (g_cam.Controller != nullptr && physics_scene_raw(true) == nullptr && g_lead.On) {
            g_lead.On = false;
            logf("lead off: no client level");
        }
        if (g_cam.Controller != nullptr && physics_scene_raw(true) == nullptr && g_follow.On) {
            g_follow.On = false;
            g_cam.Current.store(-1);
            g_cam.Fov.store(0.0f);
            logf("follow off: no client level");
        }
    }
    if (g_cam.Hold.load(std::memory_order_relaxed) && mono_seconds() > g_cam.HoldUntil) {
        g_cam.Hold.store(false, std::memory_order_release);
    }
    follow_frame();
    lead_frame();
    g_set_follow_frames = (int)std::min<std::uint64_t>(g_follow.Frames, 0x7fffffff);
}

void* sym(char const* name) {
    void* p = ::dlsym(RTLD_DEFAULT, name);
    if (p == nullptr) logf("bg3le export %s missing", name);
    return p;
}

}  // namespace

extern "C" BG3LE_PLUGIN_EXPORT int bg3le_plugin_init(const bg3le_host* host, bg3le_plugin* self) {
    g_host = host;
    g_self = self;
    // bg3le patch 0003 grows the host table after add_frame_handler: a wowbg3.so built on
    // the patched header must still load on a bg3le without it, so only what this plugin uses is required.
    if (host->abi != BG3LE_PLUGIN_ABI || host->size < offsetof(bg3le_host, add_frame_handler) + sizeof(void*)) {
        host->warn(self, "wowbg3: bg3le's plugin API is too old (needs add_frame_handler)");
        return 1;
    }
    host->describe(self, "wowbg3", WOWBG3_VERSION);
    g_api.ServerLevelManager = reinterpret_cast<void* (*)()>(sym("bg3le_server_level_manager"));
    g_api.AllocatorReady = reinterpret_cast<bool (*)()>(sym("bg3le_game_allocator_ready"));
    g_api.ClientLevelManagerOffset = reinterpret_cast<std::uintptr_t (*)()>(sym("_ZN5bg3le6target18ClientLevelManagerEv"));
    g_api.LoadBias = reinterpret_cast<std::uintptr_t (*)()>(sym("_ZN5bg3le9load_biasEv"));
    g_api.Alloc = reinterpret_cast<void* (*)(std::size_t)>(sym("_ZN5bg3se12GameAllocRawEm"));
    g_api.Free = reinterpret_cast<void (*)(void*)>(sym("_ZN5bg3se8GameFreeEPv"));
    if (g_api.ServerLevelManager == nullptr || g_api.ClientLevelManagerOffset == nullptr || g_api.LoadBias == nullptr) {
        host->warn(self, "wowbg3: bg3le does not export the level lookups this plugin needs");
        return 2;
    }
    publish_out();
    if (char const* v = std::getenv("WOWBG3_CAM_CLEAR"); v != nullptr && std::strcmp(v, "0") == 0) g_clear.On = false;
    int rc = 0;
    rc |= host->add_setting(self, "channel", BG3LE_SETTING_INT, &g_set_channel, 0, 0);
    rc |= host->add_setting(self, "out_server_lo", BG3LE_SETTING_INT, &g_set_out[0], 0, 0);
    rc |= host->add_setting(self, "out_server_hi", BG3LE_SETTING_INT, &g_set_out[1], 0, 0);
    rc |= host->add_setting(self, "out_client_lo", BG3LE_SETTING_INT, &g_set_out[2], 0, 0);
    rc |= host->add_setting(self, "out_client_hi", BG3LE_SETTING_INT, &g_set_out[3], 0, 0);
    rc |= host->add_setting(self, "frames", BG3LE_SETTING_INT, &g_set_frames, 0, 0);
    rc |= host->add_setting(self, "follow_frames", BG3LE_SETTING_INT, &g_set_follow_frames, 0, 0);
    // Settings are restored from the saved file at registration: put the live values back.
    publish_out();
    g_set_channel = 0;
    g_set_frames = 0;
    g_set_follow_frames = 0;
    rc |= host->add_frame_handler(self, &frame, nullptr);
    if (rc != 0) return 3;
    logf("wowbg3 %s ready: channel context 0x%x, IO %s", WOWBG3_VERSION, kChannel, io_dir().c_str());
    return 0;
}
