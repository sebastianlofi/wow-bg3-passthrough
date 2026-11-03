// ---------------------------------------------------------------- effects capture for replay (VFX)
// Goal: BG3's effects drawn inside WoW, exactly as BG3 draws them ("like
// SkyCraft"). Every draw of BG3's effects pass (see fx_pass) is recorded with what it needs to be
// replayed by benilla with BG3's own SPIR-V: the pipeline recipe, vertex and index data, the
// uniform and storage ranges it reads, its textures and samplers. Format: docs in
// link/benilla/fx-replay-format.md ("FXF1" frames, "FXP1" recipes, "FXS1" samplers, "FXT1"
// textures, SPIR-V modules). Included by wowbg3_capture.c (shares its tables and helpers).
//
// On while composing with the compose word "fxcopy". Recipes, SPIR-V, samplers and textures go
// to FXC_DIR once (keyed by content); the frame goes to <tav dir>/fxframe.bin every present.


static uint64_t fnv64(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV0 0xcbf29ce484222325ull

static void fxc_dirs(void) {
    static int done;
    if (done) return;
    done = 1;
    mkdir(fxc_dir(), 0755);
    mkdir(outp("/fx/spv"), 0755);
    mkdir(outp("/fx/pipe"), 0755);
    mkdir(outp("/fx/sampler"), 0755);
    mkdir(outp("/fx/tex"), 0755);
}

// Writes FXC_DIR/<sub>/<key><ext> unless it exists.
static void fxc_write_once(const char* sub, uint64_t key, const char* ext, const void* a, size_t na) {
    char path[256], tmp[280];
    snprintf(path, sizeof path, "%s/%s/%016llx%s", fxc_dir(), sub, (unsigned long long)key, ext);
    struct stat st;
    if (stat(path, &st) == 0) return;
    fxc_dirs();
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(a, 1, na, f) == na;
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp, path); else unlink(tmp);
}

// ---- growable byte buffer
typedef struct { uint8_t* p; size_t n, cap; } Bytes;
static void by_put(Bytes* b, const void* a, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t* q = realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, a, n);
    b->n += n;
}
static void by_u32(Bytes* b, uint32_t v) { by_put(b, &v, 4); }
static void by_u64(Bytes* b, uint64_t v) { by_put(b, &v, 8); }
static void by_f32(Bytes* b, float v) { by_put(b, &v, 4); }
static void by_pad(Bytes* b, size_t align) { static const uint8_t z[256]; size_t r = b->n % align; if (r) by_put(b, z, align - r); }

// ---- shader modules: handle -> SPIR-V key
// used: bindings per set the module declares; skin, nskin: object-constant offsets of the skinned
// vertex stream's bases (spv_meta); clus: reads BG3's clustered lights (set 2 bindings 10 and 11)
typedef struct { uint64_t h; uint64_t key; uint32_t used[8]; uint16_t skin[4]; uint8_t nskin, clus, push; } ShaderInfo;
#define SHADER_CAP (1u << 17)
static ShaderInfo* g_shader;
// Compute pipelines: handle -> SPIR-V key (post-processing, research traces)
typedef struct { uint64_t h; uint64_t spv; } CPipeInfo;
#define CPIPE_CAP (1u << 14)
static CPipeInfo* g_cpipe;

static uint64_t spv_store(const uint32_t* code, size_t size) {
    uint64_t key = fnv64(code, size, FNV0);
    fxc_write_once("spv", key, ".spv", code, size);
    return key;
}

// The descriptor bindings a SPIR-V module declares (OpDecorate DescriptorSet / Binding), as a
// bitmask per set. DXC drops resources a shader never reads, so this is what it uses. Returns 1 when
// the module declares push constants (an OpVariable of storage class PushConstant).
static int spv_bindings(const uint32_t* w, size_t size, uint32_t used[8]) {
    size_t n = size / 4;
    int push = 0;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    // id -> set, id -> binding (small open table by id)
    enum { CAP = 4096 };
    static __thread uint32_t ids[CAP], sets[CAP], binds[CAP];
    memset(ids, 0, sizeof ids);
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) break;
        if (op == 71 && wc >= 4 && (w[i + 2] == 34 || w[i + 2] == 33)) {  // OpDecorate DescriptorSet(34) / Binding(33)
            uint32_t id = w[i + 1], slot = id % CAP;
            while (ids[slot] && ids[slot] != id) slot = (slot + 1) % CAP;
            if (!ids[slot]) { ids[slot] = id; sets[slot] = 0xffffffffu; binds[slot] = 0xffffffffu; }
            if (w[i + 2] == 34) sets[slot] = w[i + 3]; else binds[slot] = w[i + 3];
        }
        if (op == 59 && wc >= 4 && w[i + 3] == 9) push = 1;  // OpVariable ... PushConstant
        i += wc;
    }
    for (uint32_t s = 0; s < CAP; s++)
        if (ids[s] && sets[s] < 8 && binds[s] < 32) used[sets[s]] |= 1u << binds[s];
    return push;
}

// What the effects capture needs to copy only the parts of BG3's big shared buffers a draw reads
// (a creature's burning status effect, 492 indices, carried 17.6 MB a frame).
// Skinned vertex stream: BG3's vertex shaders that declare set 0 binding 13 (653 seen, all alike)
// read a skinned vertex's position and tangent frame from that buffer of 20-byte records at
// ((VertexIndex + base) - 1) * 20 when base, a uint of the object constants (set 1 binding 0),
// is not 0; the member varies by shader (one has two, current and previous frame). Found here by
// that expression (OpIAdd of a VertexIndex load and a member load, minus 1, times 20): `skin`
// gets each member's byte offset. Clustered lights: fragment shaders that declare set 2 bindings 10
// (a uvec4 per light cluster: x the first entry, y two 12-bit counts, z a 10-bit count) and 11 (the
// clusters' light index lists, a capacity of 64 per cluster, filled from the start).
static int spv_meta(const uint32_t* w, size_t size, uint16_t skin[4], uint8_t* clus) {
    size_t n = size / 4;
    *clus = 0;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return 0;
    // per id: 0 nothing, else a tag and a value
    enum { T_CONST = 1, T_PTR, T_VAR, T_CHAIN, T_VI, T_MLOAD, T_ADD, T_SUB };
    uint8_t* tag = calloc(bound, 1);
    uint32_t* val = calloc(bound, sizeof *val);
    if (!tag || !val) { free(tag); free(val); return 0; }
    uint32_t vi_var = 0, ubo_var = 0;
    int has13 = 0, has10 = 0, has11 = 0;
    struct { uint32_t st, mem, off; } mo[256];
    uint32_t nmo = 0;
    uint64_t found = 0;  // members confirmed (bit per member < 64)
    // decorations first (they precede everything they decorate)
    enum { DCAP = 1024 };
    static __thread uint32_t did[DCAP], dset[DCAP], dbind[DCAP];
    uint32_t nd = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (op == 71 && wc >= 4) {  // OpDecorate
            uint32_t id = w[i + 1], dec = w[i + 2], v = w[i + 3];
            if (dec == 11 && v == 42) vi_var = id;  // BuiltIn VertexIndex
            if (dec == 33 || dec == 34) {
                uint32_t k = 0;
                while (k < nd && did[k] != id) k++;
                if (k == nd && nd < DCAP) { did[nd] = id; dset[nd] = dbind[nd] = 0xffffffffu; nd++; }
                if (k < nd) { if (dec == 34) dset[k] = v; else dbind[k] = v; }
            }
        } else if (op == 72 && wc >= 5 && w[i + 3] == 35 && nmo < 256) {  // OpMemberDecorate Offset
            mo[nmo].st = w[i + 1]; mo[nmo].mem = w[i + 2]; mo[nmo].off = w[i + 4]; nmo++;
        }
        i += wc;
    }
    for (uint32_t k = 0; k < nd; k++) {
        if (dset[k] == 1 && dbind[k] == 0) ubo_var = did[k];
        if (dset[k] == 0 && dbind[k] == 13) has13 = 1;
        if (dset[k] == 2 && dbind[k] == 10) has10 = 1;
        if (dset[k] == 2 && dbind[k] == 11) has11 = 1;
    }
    *clus = (uint8_t)(has10 && has11);
    for (size_t i = 5; has13 && vi_var && ubo_var && i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        const uint32_t* a = w + i + 1;
        uint32_t id = wc >= 3 ? a[1] : 0;
        if (op == 43 && wc == 4 && a[1] < bound) { tag[a[1]] = T_CONST; val[a[1]] = a[2]; }  // OpConstant (32-bit)
        else if (op == 32 && wc == 4 && a[0] < bound) { tag[a[0]] = T_PTR; val[a[0]] = a[2]; }  // OpTypePointer -> pointee
        else if (op == 59 && wc >= 4 && a[1] < bound) { tag[a[1]] = T_VAR; val[a[1]] = a[0]; }  // OpVariable -> its type
        else if ((op == 65 || op == 66) && wc == 5 && id < bound && a[2] == ubo_var && a[3] < bound && tag[a[3]] == T_CONST) {
            tag[id] = T_CHAIN; val[id] = val[a[3]];  // OpAccessChain ubo member
        } else if (op == 61 && wc >= 4 && id < bound && a[2] < bound) {  // OpLoad
            if (a[2] == vi_var) tag[id] = T_VI;
            else if (tag[a[2]] == T_CHAIN) { tag[id] = T_MLOAD; val[id] = val[a[2]]; }
        } else if ((op == 124 || op == 83) && wc == 4 && id < bound && a[2] < bound) {  // OpBitcast, OpCopyObject
            uint8_t t = tag[a[2]];
            if (t == T_VI || t == T_MLOAD || t == T_ADD || t == T_SUB) { tag[id] = t; val[id] = val[a[2]]; }
        } else if (op == 128 && wc == 5 && id < bound && a[2] < bound && a[3] < bound) {  // OpIAdd
            uint8_t t0 = tag[a[2]], t1 = tag[a[3]];
            if (t0 == T_VI && t1 == T_MLOAD) { tag[id] = T_ADD; val[id] = val[a[3]]; }
            else if (t1 == T_VI && t0 == T_MLOAD) { tag[id] = T_ADD; val[id] = val[a[2]]; }
            else if (t0 == T_ADD && t1 == T_CONST && val[a[3]] == 0xffffffffu) { tag[id] = T_SUB; val[id] = val[a[2]]; }
            else if (t1 == T_ADD && t0 == T_CONST && val[a[2]] == 0xffffffffu) { tag[id] = T_SUB; val[id] = val[a[3]]; }
        } else if (op == 130 && wc == 5 && id < bound && a[2] < bound && a[3] < bound) {  // OpISub
            if (tag[a[2]] == T_ADD && tag[a[3]] == T_CONST && val[a[3]] == 1) { tag[id] = T_SUB; val[id] = val[a[2]]; }
        } else if (op == 132 && wc == 5 && a[2] < bound && a[3] < bound) {  // OpIMul by 20
            uint32_t m = 0xffffffffu;
            if (tag[a[2]] == T_SUB && tag[a[3]] == T_CONST && val[a[3]] == 20) m = val[a[2]];
            if (tag[a[3]] == T_SUB && tag[a[2]] == T_CONST && val[a[2]] == 20) m = val[a[3]];
            if (m < 64) found |= 1ull << m;
        }
        i += wc;
    }
    int ns = 0;
    if (found) {
        uint32_t ty = ubo_var < bound && tag[ubo_var] == T_VAR ? val[ubo_var] : 0;  // pointer type
        uint32_t st = ty < bound && tag[ty] == T_PTR ? val[ty] : 0;                // the block struct
        for (uint32_t m = 0; m < 64 && ns < 4; m++) {
            if (!(found >> m & 1)) continue;
            for (uint32_t k = 0; k < nmo; k++)
                if (mo[k].st == st && mo[k].mem == m && mo[k].off < 0xfff0) { skin[ns++] = (uint16_t)mo[k].off; break; }
        }
    }
    free(tag); free(val);
    return ns;
}
// Shader modules made before the render plugin: BG3 makes its
// shader modules about 1.2 s in, the plugin hooks at about 2 s, and BG3 builds most of its pipelines from
// those modules later (8.7 s: its ground surfaces and its sight overlay), so their stages had no SPIR-V key
// and the surfaces and the vision cones were never recognised through the plugin. The plugin reads the
// module's SPIR-V back from the driver (g_mod_recover, wowbg3_render.c); the answer is filed here as if
// the module had been made after it. g_fxl_lock held.
static uint32_t* (*g_mod_recover)(uint64_t module, size_t* size);  // malloc'd copy, or NULL
static uint64_t g_mod_recovered, g_mod_missed;
static ShaderInfo* shader_info(uint64_t module) {
    if (!module) return NULL;
    ShaderInfo* sh = g_shader ? TABLE_FIND(g_shader, SHADER_CAP, module, 0) : NULL;
    if (sh || !g_mod_recover) return sh;
    size_t size = 0;
    uint32_t* code = g_mod_recover(module, &size);
    if (!code) { g_mod_missed++; return NULL; }
    if (!g_shader) g_shader = calloc(SHADER_CAP, sizeof *g_shader);
    sh = g_shader ? TABLE_FIND(g_shader, SHADER_CAP, module, 1) : NULL;
    if (sh) {
        sh->key = spv_store(code, size);
        memset(sh->used, 0, sizeof sh->used);
        sh->push = (uint8_t)spv_bindings(code, size, sh->used);
        uint8_t clus = 0;
        sh->nskin = (uint8_t)spv_meta(code, size, sh->skin, &clus);
        sh->clus = clus;
        g_mod_recovered++;
    }
    free(code);
    return sh;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* ci,
                                                               const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateShaderModule(device, ci, alloc, out);
    if (r == VK_SUCCESS && ci->pCode && ci->codeSize) {
        uint64_t key = spv_store(ci->pCode, ci->codeSize);
        uint32_t used[8] = {0};
        int push = spv_bindings(ci->pCode, ci->codeSize, used);
        uint16_t skin[4] = {0, 0, 0, 0};
        uint8_t clus = 0;
        int nskin = spv_meta(ci->pCode, ci->codeSize, skin, &clus);
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_shader) g_shader = calloc(SHADER_CAP, sizeof *g_shader);
        ShaderInfo* s = g_shader ? TABLE_FIND(g_shader, SHADER_CAP, (uint64_t)*out, 1) : NULL;
        if (s) { s->key = key; memcpy(s->used, used, sizeof used); memcpy(s->skin, skin, sizeof skin); s->nskin = (uint8_t)nskin; s->clus = clus; s->push = (uint8_t)push; }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}

// ---- samplers
typedef struct { uint64_t h; uint64_t key; } SampInfo;
#define SAMP_CAP (1u << 14)
static SampInfo* g_samp;

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSampler(VkDevice device, const VkSamplerCreateInfo* ci,
                                                          const VkAllocationCallbacks* alloc, VkSampler* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateSampler(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        Bytes b = {0};
        by_put(&b, "FXS1", 4);
        by_u32(&b, ci->magFilter); by_u32(&b, ci->minFilter); by_u32(&b, ci->mipmapMode);
        by_u32(&b, ci->addressModeU); by_u32(&b, ci->addressModeV); by_u32(&b, ci->addressModeW);
        by_f32(&b, ci->mipLodBias); by_u32(&b, ci->anisotropyEnable); by_f32(&b, ci->maxAnisotropy);
        by_u32(&b, ci->compareEnable); by_u32(&b, ci->compareOp); by_f32(&b, ci->minLod); by_f32(&b, ci->maxLod);
        by_u32(&b, ci->borderColor); by_u32(&b, ci->unnormalizedCoordinates);
        uint64_t key = fnv64(b.p, b.n, FNV0);
        fxc_write_once("sampler", key, ".bin", b.p, b.n);
        free(b.p);
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_samp) g_samp = calloc(SAMP_CAP, sizeof *g_samp);
        SampInfo* s = g_samp ? TABLE_FIND(g_samp, SAMP_CAP, (uint64_t)*out, 1) : NULL;
        if (s) s->key = key;
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
static uint64_t sampler_key(uint64_t h) {
    SampInfo* s = (g_samp && h) ? TABLE_FIND(g_samp, SAMP_CAP, h, 0) : NULL;
    return s ? s->key : 0;
}

// ---- descriptor set layouts and pipeline layouts
#define DSL_SLOTS 32
typedef struct { uint64_t h; uint32_t n; struct { uint32_t binding, type, count, stages; uint64_t imm; } b[DSL_SLOTS]; uint8_t rec; } DslInfo;
#define DSL_CAP (1u << 14)
static DslInfo* g_dsl;
typedef struct { uint64_t h; uint32_t nsets, npush; uint64_t sets[8]; uint32_t push[4][3]; uint8_t rec; } PlInfo;
// rec: read back from the driver (a layout made before the render plugin; g_layout_recover, wowbg3_render.c):
// no stage flags (the recipe takes them from its stages' SPIR-V), no push constant ranges, no immutable samplers.
static int (*g_layout_recover)(uint64_t pipeline_layout);  // fills g_pl and g_dsl, g_fxl_lock held; 1 when done
#define PL_CAP (1u << 14)
static PlInfo* g_pl;

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci,
                                                                     const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateDescriptorSetLayout(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_dsl) g_dsl = calloc(DSL_CAP, sizeof *g_dsl);
        DslInfo* l = g_dsl ? TABLE_FIND(g_dsl, DSL_CAP, (uint64_t)*out, 1) : NULL;
        if (l) {
            l->n = 0;
            l->rec = 0;
            for (uint32_t i = 0; i < ci->bindingCount && l->n < DSL_SLOTS; i++) {
                const VkDescriptorSetLayoutBinding* b = &ci->pBindings[i];
                l->b[l->n].binding = b->binding; l->b[l->n].type = b->descriptorType; l->b[l->n].count = b->descriptorCount;
                l->b[l->n].stages = b->stageFlags;
                l->b[l->n].imm = b->pImmutableSamplers && b->descriptorCount ? sampler_key((uint64_t)b->pImmutableSamplers[0]) : 0;
                l->n++;
            }
        }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* ci,
                                                                const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreatePipelineLayout(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_pl) g_pl = calloc(PL_CAP, sizeof *g_pl);
        PlInfo* p = g_pl ? TABLE_FIND(g_pl, PL_CAP, (uint64_t)*out, 1) : NULL;
        if (p) {
            p->rec = 0;
            p->nsets = ci->setLayoutCount > 8 ? 8 : ci->setLayoutCount;
            for (uint32_t i = 0; i < p->nsets; i++) p->sets[i] = (uint64_t)ci->pSetLayouts[i];
            p->npush = ci->pushConstantRangeCount > 4 ? 4 : ci->pushConstantRangeCount;
            for (uint32_t i = 0; i < p->npush; i++) {
                p->push[i][0] = ci->pPushConstantRanges[i].stageFlags;
                p->push[i][1] = ci->pPushConstantRanges[i].offset;
                p->push[i][2] = ci->pPushConstantRanges[i].size;
            }
        }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}

// ---- pipeline recipes (allocated per pipeline; libraries merge into the linked pipeline)
typedef struct {
    uint32_t stage; uint64_t key; char entry[64];
    uint32_t nspec; uint32_t spec[16][3]; uint32_t spec_size; uint8_t spec_data[128];
    uint32_t used[8]; uint8_t push;  // this stage's bindings and push constants (recipe_whole, layouts read back)
} StageRec;
typedef struct Recipe_ {
    uint32_t have;  // 1 vertex input, 2 pre-raster (vertex shader, raster), 4 fragment shader, 8 output (blend, formats)
    uint32_t topo, restart, polygon, cull, front, bias_en; float bias_c, bias_clamp, bias_s;
    uint32_t ztest, zwrite, zcmp;
    uint32_t ncolor; uint32_t blend[8][9];
    uint32_t depth_format;
    uint32_t nstages; StageRec st[3];
    uint32_t nbind; uint32_t bind[8][3];
    uint32_t nattr; uint32_t attr[16][4];
    uint64_t layout;
    uint32_t used[8];  // descriptor bindings the stages read, per set
    uint16_t skin[4]; uint8_t nskin, clus;  // spv_meta of the stages
    uint32_t ndyn; uint32_t dyn[24];
    uint64_t key;  // recipe key once written
} Recipe;
typedef struct { uint64_t h; Recipe* r; } RecInfo;
#define REC_CAP (1u << 18)
static RecInfo* g_rec;

static void recipe_meta(Recipe* r, const uint16_t skin[4], int nskin, int clus) {
    if (clus) r->clus = 1;
    for (int k = 0; k < nskin; k++) {
        int dup = 0;
        for (int j = 0; j < r->nskin; j++) dup |= r->skin[j] == skin[k];
        if (!dup && r->nskin < 4) r->skin[r->nskin++] = skin[k];
    }
}
static void recipe_from(Recipe* r, const VkGraphicsPipelineCreateInfo* ci) {
    VkGraphicsPipelineLibraryFlagsEXT parts = 0;
    for (const VkBaseInStructure* e = ci->pNext; e; e = e->pNext)
        if (e->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
            parts = ((const VkGraphicsPipelineLibraryCreateInfoEXT*)e)->flags;
    int all = parts == 0;
    if ((all || (parts & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT)) && ci->pVertexInputState) {
        const VkPipelineVertexInputStateCreateInfo* vi = ci->pVertexInputState;
        r->nbind = 0;
        for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount && r->nbind < 8; b++, r->nbind++) {
            r->bind[r->nbind][0] = vi->pVertexBindingDescriptions[b].binding;
            r->bind[r->nbind][1] = vi->pVertexBindingDescriptions[b].stride;
            r->bind[r->nbind][2] = vi->pVertexBindingDescriptions[b].inputRate;
        }
        r->nattr = 0;
        for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount && r->nattr < 16; a++, r->nattr++) {
            r->attr[r->nattr][0] = vi->pVertexAttributeDescriptions[a].location;
            r->attr[r->nattr][1] = vi->pVertexAttributeDescriptions[a].binding;
            r->attr[r->nattr][2] = vi->pVertexAttributeDescriptions[a].format;
            r->attr[r->nattr][3] = vi->pVertexAttributeDescriptions[a].offset;
        }
        if (ci->pInputAssemblyState) { r->topo = ci->pInputAssemblyState->topology; r->restart = ci->pInputAssemblyState->primitiveRestartEnable; }
        r->have |= 1;
    }
    if (ci->layout) r->layout = (uint64_t)ci->layout;
    for (uint32_t s = 0; s < ci->stageCount && r->nstages < 3; s++) {
        const VkPipelineShaderStageCreateInfo* si = &ci->pStages[s];
        StageRec* st = &r->st[r->nstages];
        memset(st, 0, sizeof *st);
        st->stage = si->stage;
        if (si->module) {
            ShaderInfo* sh = shader_info((uint64_t)si->module);
            st->key = sh ? sh->key : 0;
            for (int k = 0; sh && k < 8; k++) { r->used[k] |= sh->used[k]; st->used[k] = sh->used[k]; }
            st->push = sh ? sh->push : 0;
            if (sh) recipe_meta(r, sh->skin, sh->nskin, sh->clus);
        } else {
            for (const VkBaseInStructure* e = si->pNext; e; e = e->pNext)
                if (e->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo* mi = (const VkShaderModuleCreateInfo*)e;
                    st->key = spv_store(mi->pCode, mi->codeSize);
                    st->push = (uint8_t)spv_bindings(mi->pCode, mi->codeSize, st->used);
                    for (int k = 0; k < 8; k++) r->used[k] |= st->used[k];
                    uint16_t skin[4] = {0, 0, 0, 0};
                    uint8_t clus = 0;
                    int nskin = spv_meta(mi->pCode, mi->codeSize, skin, &clus);
                    recipe_meta(r, skin, nskin, clus);
                }
        }
        snprintf(st->entry, sizeof st->entry, "%s", si->pName ? si->pName : "main");
        const VkSpecializationInfo* sp = si->pSpecializationInfo;
        if (sp) {
            st->nspec = sp->mapEntryCount > 16 ? 16 : sp->mapEntryCount;
            for (uint32_t k = 0; k < st->nspec; k++) {
                st->spec[k][0] = sp->pMapEntries[k].constantID; st->spec[k][1] = sp->pMapEntries[k].offset;
                st->spec[k][2] = (uint32_t)sp->pMapEntries[k].size;
            }
            st->spec_size = sp->dataSize > sizeof st->spec_data ? sizeof st->spec_data : (uint32_t)sp->dataSize;
            memcpy(st->spec_data, sp->pData, st->spec_size);
        }
        r->nstages++;
        r->have |= si->stage == VK_SHADER_STAGE_FRAGMENT_BIT ? 4 : 2;
    }
    if ((all || (parts & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)) && ci->pRasterizationState) {
        const VkPipelineRasterizationStateCreateInfo* rs = ci->pRasterizationState;
        r->polygon = rs->polygonMode; r->cull = rs->cullMode; r->front = rs->frontFace;
        r->bias_en = rs->depthBiasEnable; r->bias_c = rs->depthBiasConstantFactor; r->bias_clamp = rs->depthBiasClamp;
        r->bias_s = rs->depthBiasSlopeFactor;
        r->have |= 2;
    }
    if ((all || (parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT)) && ci->pDepthStencilState) {
        r->ztest = ci->pDepthStencilState->depthTestEnable; r->zwrite = ci->pDepthStencilState->depthWriteEnable;
        r->zcmp = ci->pDepthStencilState->depthCompareOp;
    }
    if (all || (parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) {
        const VkPipelineColorBlendStateCreateInfo* cb = ci->pColorBlendState;
        VkFormat fmts[8] = {0};
        uint32_t nf = 0;
        VkFormat depth = VK_FORMAT_UNDEFINED;
        for (const VkBaseInStructure* e = ci->pNext; e; e = e->pNext)
            if (e->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                const VkPipelineRenderingCreateInfo* ri = (const VkPipelineRenderingCreateInfo*)e;
                nf = ri->colorAttachmentCount > 8 ? 8 : ri->colorAttachmentCount;
                for (uint32_t k = 0; k < nf; k++) fmts[k] = ri->pColorAttachmentFormats[k];
                depth = ri->depthAttachmentFormat;
            }
        if (!nf && ci->renderPass && g_rp) {
            RpInfo* rp = TABLE_FIND(g_rp, RP_CAP, (uint64_t)ci->renderPass, 0);
            for (uint32_t k = 0; rp && k < rp->n && k < 8; k++) {
                if (is_depth_format(rp->fmts[k])) depth = rp->fmts[k];
                else fmts[nf++] = rp->fmts[k];
            }
        }
        if (cb) {
            r->ncolor = cb->attachmentCount > 8 ? 8 : cb->attachmentCount;
            for (uint32_t k = 0; k < r->ncolor; k++) {
                const VkPipelineColorBlendAttachmentState* a = &cb->pAttachments[k];
                uint32_t v[9] = {a->blendEnable, a->srcColorBlendFactor, a->dstColorBlendFactor, a->colorBlendOp,
                                 a->srcAlphaBlendFactor, a->dstAlphaBlendFactor, a->alphaBlendOp, a->colorWriteMask,
                                 k < nf ? (uint32_t)fmts[k] : 0};
                memcpy(r->blend[k], v, sizeof v);
            }
            r->have |= 8;
        }
        if (depth) r->depth_format = depth;
    }
    if (ci->pDynamicState) {
        r->ndyn = 0;
        for (uint32_t k = 0; k < ci->pDynamicState->dynamicStateCount && r->ndyn < 24; k++)
            r->dyn[r->ndyn++] = ci->pDynamicState->pDynamicStates[k];
    }
    // Linked from libraries: take each part from the library that has it.
    for (const VkBaseInStructure* e = ci->pNext; e; e = e->pNext) {
        if (e->sType != VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) continue;
        const VkPipelineLibraryCreateInfoKHR* li = (const VkPipelineLibraryCreateInfoKHR*)e;
        for (uint32_t l = 0; l < li->libraryCount; l++) {
            RecInfo* ri = TABLE_FIND(g_rec, REC_CAP, (uint64_t)li->pLibraries[l], 0);
            Recipe* q = ri ? ri->r : NULL;
            if (!q) continue;
            if ((q->have & 1) && !(r->have & 1)) {
                r->nbind = q->nbind; memcpy(r->bind, q->bind, sizeof r->bind);
                r->nattr = q->nattr; memcpy(r->attr, q->attr, sizeof r->attr);
                r->topo = q->topo; r->restart = q->restart; r->have |= 1;
            }
            if ((q->have & 2) && !(r->have & 2)) {
                r->polygon = q->polygon; r->cull = q->cull; r->front = q->front; r->bias_en = q->bias_en;
                r->bias_c = q->bias_c; r->bias_clamp = q->bias_clamp; r->bias_s = q->bias_s; r->have |= 2;
            }
            if ((q->have & 4) && !(r->have & 4)) {
                r->ztest = q->ztest; r->zwrite = q->zwrite; r->zcmp = q->zcmp; r->have |= 4;
            }
            if ((q->have & 8) && !(r->have & 8)) {
                r->ncolor = q->ncolor; memcpy(r->blend, q->blend, sizeof r->blend); r->depth_format = q->depth_format; r->have |= 8;
            }
            for (uint32_t s = 0; s < q->nstages && r->nstages < 3; s++) {
                int dup = 0;
                for (uint32_t t = 0; t < r->nstages; t++) if (r->st[t].stage == q->st[s].stage) dup = 1;
                if (!dup) r->st[r->nstages++] = q->st[s];
            }
            if (!r->layout) r->layout = q->layout;
            for (int k = 0; k < 8; k++) r->used[k] |= q->used[k];
            recipe_meta(r, q->skin, q->nskin, q->clus);
            for (uint32_t k = 0; k < q->ndyn && r->ndyn < 24; k++) r->dyn[r->ndyn++] = q->dyn[k];
        }
    }
}

// ---- BG3's route line (BG3's white path line, shown in turn-based mode and
// while sneaking, on WoW's ground). BG3 draws it into a screen-size offscreen target of its own
// (no depth) as a tube per path segment, one instanced box draw with vertex shader ROUTE_VS, then
// lays that target over the scene (research, "tracepath"). Replayed, the tube would
// run at BG3's floor heights, not on WoW's ground, so the layer sends the segments themselves:
// <tav dir>/bg3route.bin ("RTE1", link/benilla/fx-replay-format.md), and benilla (patch 0012,
// wowbg3_route.rs) projects the line onto WoW's ground with BG3's colours, dashes and fades. A BG3
// update that changes the shader loses the line until ROUTE_VS follows (the log names the
// pipelines found).
#define ROUTE_VS 0xacc03298eae8ab7eull
#define ROUTE_PIPES 16u
#define ROUTE_MAX 256u
static uint64_t g_route_pipe[ROUTE_PIPES];  // pipelines whose vertex shader is ROUTE_VS
static uint32_t g_route_npipe;
static struct {
    uint32_t n, seq, shown, miss, unmapped, said;
    uint64_t hash;
    float consts[8];             // set 1 binding 0: fade in, fade out, dash weight, length, dash offset, gap
    float seg[ROUTE_MAX][28];    // per instance: locations 1..7, xyzw each
} g_route;

// Notes (or forgets, for a reused handle) a pipeline that draws the route. g_fxl_lock held.
static void route_note(const Recipe* r, uint64_t pipe) {
    int route = 0;
    for (uint32_t s = 0; s < r->nstages; s++)
        if (r->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT && r->st[s].key == ROUTE_VS) route = 1;
    uint32_t k = 0;
    while (k < g_route_npipe && g_route_pipe[k] != pipe) k++;
    if (route && k == g_route_npipe) {
        if (g_route_npipe < ROUTE_PIPES) __atomic_store_n(&g_route_pipe[g_route_npipe], pipe, __ATOMIC_RELAXED);
        else __atomic_store_n(&g_route_pipe[pipe % ROUTE_PIPES], pipe, __ATOMIC_RELAXED);
        if (g_route_npipe < ROUTE_PIPES) __atomic_store_n(&g_route_npipe, g_route_npipe + 1, __ATOMIC_RELEASE);
        logf_("route: pipeline %llx draws BG3's route line", (unsigned long long)pipe);
    } else if (!route && k < g_route_npipe) {
        __atomic_store_n(&g_route_pipe[k], 0, __ATOMIC_RELAXED);
    }
}
static int route_pipe(uint64_t pipe) {
    uint32_t n = __atomic_load_n(&g_route_npipe, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < n; k++) if (__atomic_load_n(&g_route_pipe[k], __ATOMIC_RELAXED) == pipe) return 1;
    return 0;
}

static void recipe_record(const VkGraphicsPipelineCreateInfo* ci, VkPipeline out) {
    Recipe* r = calloc(1, sizeof *r);
    if (!r) return;
    r->topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    r->zcmp = VK_COMPARE_OP_ALWAYS;
    recipe_from(r, ci);
    route_note(r, (uint64_t)out);
    if (!g_rec) g_rec = calloc(REC_CAP, sizeof *g_rec);
    RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, (uint64_t)out, 1) : NULL;
    if (!ri) { free(r); return; }
    free(ri->r);
    ri->r = r;
}

// Whether a recipe is whole: every stage's SPIR-V known, and every
// descriptor binding its stages read present in a known set layout. Through the render plugin a pipeline
// can use a pipeline or set layout made before the plugin loaded: its recipe then lacks those bindings,
// and benilla's replay built a pipeline whose layout misses a binding the SPIR-V reads, which crashed
// RADV (radv_nir_lower_immediate_samplers, SIGSEGV in benilla; harness). Such a pipeline
// stays unknown (its draws are not captured).
static uint64_t g_rec_partial;
static int recipe_layouts_known(const Recipe* r) {
    PlInfo* pl = (g_pl && r->layout) ? TABLE_FIND(g_pl, PL_CAP, r->layout, 0) : NULL;
    for (uint32_t s = 0; s < 8; s++) {
        if (!r->used[s]) continue;
        if (!pl || s >= pl->nsets || !g_dsl || !TABLE_FIND(g_dsl, DSL_CAP, pl->sets[s], 0)) return 0;
    }
    return 1;
}
static const char* g_whole_why;  // why recipe_whole said no (the log line)
static int recipe_whole(const Recipe* r) {
    g_whole_why = "a shader";
    for (uint32_t s = 0; s < r->nstages; s++) if (!r->st[s].key) return 0;
    if (!recipe_layouts_known(r) && g_layout_recover && r->layout) g_layout_recover(r->layout);
    PlInfo* pl = (g_pl && r->layout) ? TABLE_FIND(g_pl, PL_CAP, r->layout, 0) : NULL;
    g_whole_why = "push constants with a layout read back";
    if (pl && pl->rec)
        for (uint32_t s = 0; s < r->nstages; s++) if (r->st[s].push) return 0;  // its push ranges are unknown
    for (uint32_t s = 0; s < 8; s++) {
        if (!r->used[s]) continue;
        g_whole_why = "a pipeline layout";
        if (!pl || s >= pl->nsets) return 0;
        DslInfo* l = g_dsl ? TABLE_FIND(g_dsl, DSL_CAP, pl->sets[s], 0) : NULL;
        g_whole_why = "a set layout";
        if (!l) return 0;
        g_whole_why = "a binding";
        uint32_t have = 0;
        for (uint32_t k = 0; k < l->n; k++) if (l->b[k].binding < 32) have |= 1u << l->b[k].binding;
        if (r->used[s] & ~have) return 0;
    }
    return 1;
}
// The recipe file of a pipeline (written on first use); 0 when unknown.
static uint64_t recipe_key(uint64_t pipe) {
    RecInfo* ri = (g_rec && pipe) ? TABLE_FIND(g_rec, REC_CAP, pipe, 0) : NULL;
    Recipe* r = ri ? ri->r : NULL;
    if (!r) return 0;
    if (r->key) return r->key == 1 ? 0 : r->key;  // key 1: not whole (recipe_whole), checked once
    if (!recipe_whole(r)) {
        r->key = 1;
        g_rec_partial++;
        if (!(g_rec_partial & (g_rec_partial - 1)))  // 1, 2, 4, 8 ...
            logf_("fx: pipeline %llx left out: %s made before the render plugin (%llu so far)",
                  (unsigned long long)pipe, g_whole_why, (unsigned long long)g_rec_partial);
        return 0;
    }
    Bytes b = {0};
    by_put(&b, "FXP1", 4); by_u32(&b, 1);
    by_u32(&b, r->topo); by_u32(&b, r->restart);
    by_u32(&b, r->polygon); by_u32(&b, r->cull); by_u32(&b, r->front);
    by_u32(&b, r->bias_en); by_f32(&b, r->bias_c); by_f32(&b, r->bias_clamp); by_f32(&b, r->bias_s);
    by_u32(&b, r->ztest); by_u32(&b, r->zwrite); by_u32(&b, r->zcmp);
    by_u32(&b, r->ncolor);
    for (uint32_t k = 0; k < r->ncolor; k++) by_put(&b, r->blend[k], sizeof r->blend[k]);
    by_u32(&b, r->depth_format);
    by_u32(&b, r->nstages);
    for (uint32_t s = 0; s < r->nstages; s++) {
        StageRec* st = &r->st[s];
        by_u32(&b, st->stage); by_u64(&b, st->key); by_put(&b, st->entry, 64);
        by_u32(&b, st->nspec);
        for (uint32_t k = 0; k < st->nspec; k++) by_put(&b, st->spec[k], 12);
        by_u32(&b, st->spec_size); by_put(&b, st->spec_data, st->spec_size); by_pad(&b, 4);
    }
    by_u32(&b, r->nbind);
    for (uint32_t k = 0; k < r->nbind; k++) by_put(&b, r->bind[k], 12);
    by_u32(&b, r->nattr);
    for (uint32_t k = 0; k < r->nattr; k++) by_put(&b, r->attr[k], 16);
    PlInfo* pl = (g_pl && r->layout) ? TABLE_FIND(g_pl, PL_CAP, r->layout, 0) : NULL;
    by_u32(&b, pl ? pl->nsets : 0);
    for (uint32_t s = 0; pl && s < pl->nsets; s++) {
        DslInfo* l = g_dsl ? TABLE_FIND(g_dsl, DSL_CAP, pl->sets[s], 0) : NULL;
        uint32_t nu = 0;
        for (uint32_t k = 0; l && k < l->n; k++) if (l->b[k].binding < 32 && (r->used[s] >> l->b[k].binding & 1)) nu++;
        by_u32(&b, nu);
        for (uint32_t k = 0; l && k < l->n; k++) {
            if (!(l->b[k].binding < 32 && (r->used[s] >> l->b[k].binding & 1))) continue;
            uint32_t stages = l->b[k].stages;
            if (l->rec) {  // read back: the stages whose SPIR-V reads it
                stages = 0;
                for (uint32_t t = 0; t < r->nstages; t++) if (r->st[t].used[s] >> l->b[k].binding & 1) stages |= r->st[t].stage;
            }
            by_u32(&b, l->b[k].binding); by_u32(&b, l->b[k].type); by_u32(&b, l->b[k].count); by_u32(&b, stages);
            by_u32(&b, l->b[k].imm ? 1 : 0);
            if (l->b[k].imm) by_u64(&b, l->b[k].imm);
        }
    }
    by_u32(&b, pl ? pl->npush : 0);
    for (uint32_t k = 0; pl && k < pl->npush; k++) by_put(&b, pl->push[k], 12);
    by_u32(&b, r->ndyn);
    for (uint32_t k = 0; k < r->ndyn; k++) by_u32(&b, r->dyn[k]);
    r->key = fnv64(b.p, b.n, FNV0) | 1;  // never 0
    if (r->key == 1) r->key = 3;  // 1 means not whole
    fxc_write_once("pipe", r->key, ".bin", b.p, b.n);
    free(b.p);
    return r->key;
}

// ---- per-frame capture

typedef struct { uint64_t buffer, off, size; uint32_t blob; } GpuReq;  // fill the frame blob from a GPU-only buffer
typedef struct { uint64_t image; uint64_t key; } TexReq;
// A frame's clustered-lights pair (spv_meta): the cluster headers and the copied start of the light
// index lists, blob offsets and sizes; fxc_clus_fix checks the headers against it once filled.
typedef struct { uint32_t hdr, hsize, list, lsize; } FxClus;
#define FXC_NCLUS 16
static struct {
    int on;
    Bytes draws, blob;
    GpuReq* gpu; uint32_t ngpu, capgpu;
    TexReq tex[256]; uint32_t ntex;
    uint32_t ndraws, frame, dropped, far;
    float eye[3];
    FxClus clus[FXC_NCLUS]; uint32_t nclus;
    // since the last log line: skinned-stream draws trimmed (and bytes copied, bytes their whole
    // ranges had), left whole (no index range yet), light index lists copied (bytes, whole)
    uint64_t skin_draws, skin_bytes, skin_whole, skin_full, clus_bytes, clus_whole;
    uint64_t unsup;  // draws dropped since the last log line: a texture the layer cannot export
} g_fxc;
// Light index list bytes copied per frame (fxc_clus_fix adapts it to what the headers use).
static uint64_t g_clus_want = 512u << 10;
static struct { uint64_t need, cut, frames, small; } g_clus;  // worker side (g_fxj_data)
// The effects pass's colour target, per recording thread (BG3 records passes on several threads),
// and the images copied from it right before (BG3 samples a copy of the scene for refraction).
static __thread uint64_t t_pass_target;
static struct { uint64_t src, dst; } g_copyof[64];
static uint32_t g_ncopyof;
static void fxc_copy(uint64_t src, uint64_t dst) {
    pthread_mutex_lock(&g_fxl_lock);
    uint32_t k = 0;
    while (k < g_ncopyof && g_copyof[k].dst != dst) k++;
    if (k == g_ncopyof) { if (g_ncopyof < 64) g_ncopyof++; else k = (uint32_t)(dst % 64); }
    g_copyof[k].src = src; g_copyof[k].dst = dst;
    pthread_mutex_unlock(&g_fxl_lock);
}
static uint64_t fxc_copy_source(uint64_t dst) {  // g_fxl_lock held
    for (uint32_t k = 0; k < g_ncopyof; k++) if (g_copyof[k].dst == dst) return g_copyof[k].src;
    return 0;
}

// Static GPU buffer ranges already read (vertex meshes, quad index buffers): key -> bytes.
typedef struct { uint64_t h; uint64_t buffer, off, size; uint8_t* data; } GpuCache;
#define GPUC_CAP 4096
static GpuCache g_gpuc[GPUC_CAP];
static pthread_mutex_t g_fxj_data = PTHREAD_MUTEX_INITIALIZER;  // g_gpuc and g_post_lut (present thread and worker)

static uint32_t fxc_blob(const void* src, uint64_t size) {
    by_pad(&g_fxc.blob, 256);
    uint32_t at = (uint32_t)g_fxc.blob.n;
    if (src) by_put(&g_fxc.blob, src, size);
    else { static const uint8_t z[4096]; for (uint64_t k = 0; k < size; k += sizeof z) by_put(&g_fxc.blob, z, size - k > sizeof z ? sizeof z : size - k); }
    return at;
}

// A buffer range into the blob area: copied now when mapped, else queued for the GPU at present.
typedef struct { uint64_t h; uint32_t blob, size; } RangeSeen;
#define SEEN_CAP 2048
static RangeSeen g_seen[SEEN_CAP];  // this frame's ranges (cleared each present)
static uint32_t fxc_range_raw(uint64_t buffer, uint64_t off, uint64_t size);
// A buffer range into the blob area, once per frame (BG3 binds the same view constants to every draw).
static uint32_t fxc_range(uint64_t buffer, uint64_t off, uint64_t size, uint32_t* out_size) {
    uint64_t k3[3] = {buffer, off, size};
    uint64_t h = fnv64(k3, sizeof k3, FNV0) | 1;
    RangeSeen* e = &g_seen[h % SEEN_CAP];
    if (e->h != h) {
        uint32_t at = fxc_range_raw(buffer, off, size);
        *e = (RangeSeen){h, at, (uint32_t)(g_fxc.blob.n - at)};
    }
    if (out_size) *out_size = e->size;
    return e->blob;
}
// Blob bytes at `at` from a buffer range: copied now when mapped, else queued for the GPU at present.
static void fxc_fill_from(uint64_t buffer, uint64_t off, uint64_t size, uint32_t at) {
    uint64_t left = 0;
    const uint8_t* src = buf_cpu(buffer, off, &left);
    if (src) {
        memcpy(g_fxc.blob.p + at, src, size > left ? left : size);
        return;
    }
    if (g_fxc.ngpu == g_fxc.capgpu) {
        uint32_t c = g_fxc.capgpu ? g_fxc.capgpu * 2 : 64;
        GpuReq* q = realloc(g_fxc.gpu, c * sizeof *q);
        if (!q) return;
        g_fxc.gpu = q; g_fxc.capgpu = c;
    }
    g_fxc.gpu[g_fxc.ngpu++] = (GpuReq){buffer, off, size, at};
}
static uint32_t fxc_range_raw(uint64_t buffer, uint64_t off, uint64_t size) {
    uint64_t left = 0;
    const uint8_t* src = buf_cpu(buffer, off, &left);
    if (src) {
        if (size > left) size = left;
        return fxc_blob(src, size);
    }
    BufInfo* b = g_buf ? TABLE_FIND(g_buf, BUF_CAP, buffer, 0) : NULL;
    if (!b || !b->size || off >= b->size) return fxc_blob(NULL, 0);
    if (size > b->size - off) size = b->size - off;
    uint32_t at = fxc_blob(NULL, size);
    fxc_fill_from(buffer, off, size, at);
    return at;
}

// A filled frame's clustered lights (blob area `data`, `n` bytes): a cluster whose list runs past
// the copied start of the light index lists gets no lights in this frame (its counts cleared, so the
// replay never reads past what was copied), and the copied size follows what the headers use
// (half again plus 64 KB; it shrinks after 600 frames needing less than half of it).
static void fxc_clus_fix(uint8_t* data, size_t n, const FxClus* c, uint32_t nc) {
    uint64_t need = 0, cut = 0;
    for (uint32_t k = 0; k < nc; k++) {
        if ((size_t)c[k].hdr + c[k].hsize > n || (size_t)c[k].list + c[k].lsize > n || (c[k].hdr & 3)) continue;
        uint32_t* h = (uint32_t*)(void*)(data + c[k].hdr);
        uint64_t nl = c[k].lsize / 4;
        for (uint32_t i = 0; i + 4 <= c[k].hsize / 4; i += 4) {
            uint64_t tot = (h[i + 1] & 4095u) + ((h[i + 1] >> 12) & 4095u) + (h[i + 2] & 1023u);
            if (!tot) continue;
            uint64_t end = (uint64_t)h[i] + tot;
            if (end > need) need = end;
            if (end > nl) { h[i + 1] &= 0xff000000u; h[i + 2] &= ~1023u; cut++; }
        }
    }
    if (!nc) return;
    uint64_t want = __atomic_load_n(&g_clus_want, __ATOMIC_RELAXED);
    uint64_t target = ((need * 4 * 3 / 2 + (64u << 10)) + 0xffff) & ~(uint64_t)0xffff;
    pthread_mutex_lock(&g_fxj_data);
    if (target > want) { want = target; g_clus.small = 0; }
    else if (target * 2 < want) { if (++g_clus.small > 600) { want = target; g_clus.small = 0; } }
    else g_clus.small = 0;
    if (need > g_clus.need) g_clus.need = need;
    g_clus.cut += cut;
    g_clus.frames++;
    pthread_mutex_unlock(&g_fxj_data);
    __atomic_store_n(&g_clus_want, want, __ATOMIC_RELAXED);
}

// Texture keys the layer cannot export (their FXC_DIR/tex/<key>.unsupported markers, g_fxl_lock):
// benilla skips every draw that samples one (wowbg3_fx.rs), so fxc_draw drops such draws before
// copying anything (decision 'U'; a burning creature's status shell: R16G16_UNORM).
#define UNSUP_CAP 256
static uint64_t g_unsup[UNSUP_CAP];
static uint32_t g_nunsup;
static void unsup_add(uint64_t key) {
    uint32_t k = (uint32_t)(key % UNSUP_CAP);
    for (uint32_t n = 0; n < UNSUP_CAP; n++, k = (k + 1) % UNSUP_CAP) {
        if (g_unsup[k] == key) return;
        if (!g_unsup[k]) { g_unsup[k] = key; g_nunsup++; return; }
    }
}
static int unsup_has(uint64_t key) {
    uint32_t k = (uint32_t)(key % UNSUP_CAP);
    for (uint32_t n = 0; n < UNSUP_CAP && g_unsup[k]; n++, k = (k + 1) % UNSUP_CAP)
        if (g_unsup[k] == key) return 1;
    return 0;
}
// The texture key of a sampled image view (fxc_texkey's), 0 for render targets and unknown views.
static uint64_t fxc_texkey_plain(uint64_t view) {
    ViewInfo* v = (g_view && view) ? TABLE_FIND(g_view, VIEW_CAP, view, 0) : NULL;
    ImgInfo* i = v && v->image ? TABLE_FIND(g_img, IMG_CAP, v->image, 0) : NULL;
    if (!i || i->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) return 0;
    uint64_t image = v->image;
    return fnv64(&image, 8, fnv64(&i->id, 4, FNV0)) | 4;
}

// The texture key of an image view: special keys for render targets, else the image (queued for
// export at present when new). A figure's glow (t_fxc_glow, this draw is one: glow_draw) also binds BG3's
// virtual texture cache, a layered image export cannot take, and benilla would wait for it forever and skip
// the draw: the neutral stand-in's alpha of 1 is all the glow's alpha test needs.
static __thread int t_fxc_glow;
static uint64_t fxc_texkey(uint64_t view) {
    ViewInfo* v = (g_view && view) ? TABLE_FIND(g_view, VIEW_CAP, view, 0) : NULL;
    uint64_t image = v ? v->image : 0;
    if (!image) return 3;
    if (image == g_role.depth) return 1;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, image, 0);
    if (!i) return 3;
    if (i->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        if (image == t_pass_target || (t_pass_target && fxc_copy_source(image) == t_pass_target)) return 2;
        if (i->fmt == VK_FORMAT_R32_SFLOAT && i->w == g_role.ew && i->hgt == g_role.eh) return 4;  // BG3's linear view depth
        // BG3's G-buffer normal (R16G16, the view-space normal encoded; projected decals read it).
        if (i->fmt == VK_FORMAT_R16G16_SFLOAT && i->w == g_role.ew && i->hgt == g_role.eh) return 6;
        // BG3's froxel light grid (B10G11R11 at froxel size; its shaders read value * 32 as light).
        if (i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (i->w != g_role.ew || i->hgt != g_role.eh)) return 5;
        // Render targets the effects read (soft particles, fog volumes): logged once each, for mapping.
        static uint64_t said[64];
        static uint32_t nsaid;
        int known = 0;
        for (uint32_t k = 0; k < nsaid; k++) if (said[k] == image) known = 1;
        if (!known && nsaid < 64) {
            said[nsaid++] = image;
            logf_("fxcopy: effects read render target %llx id %u: format %d %ux%u layers %u mips %u usage %x", (unsigned long long)image,
                  i->id, (int)i->fmt, i->w, i->hgt, i->layers, i->mips, (unsigned)i->usage);
        }
        return 3;
    }
    uint64_t key = fnv64(&image, 8, fnv64(&i->id, 4, FNV0)) | 4;
    char path[256];
    snprintf(path, sizeof path, "%s/tex/%016llx.bin", fxc_dir(), (unsigned long long)key);
    if (t_fxc_glow && i->layers > 1 && i->type != VK_IMAGE_TYPE_3D) return 3;
    struct stat st;
    char bad[256];
    snprintf(bad, sizeof bad, "%s/tex/%016llx.unsupported", fxc_dir(), (unsigned long long)key);
    if (stat(path, &st) != 0) {
        if (stat(bad, &st) == 0) { unsup_add(key); return key; }
        int seen = 0;
        for (uint32_t k = 0; k < g_fxc.ntex; k++) if (g_fxc.tex[k].key == key) seen = 1;
        if (!seen && g_fxc.ntex < 256) g_fxc.tex[g_fxc.ntex++] = (TexReq){image, key};
    }
    return key;
}

// A mapped uniform range of a bound set slot (dynamic offsets applied), or NULL.
static const uint8_t* fxc_slot_cpu(uint32_t set, uint32_t binding, uint32_t need) {
    if (set >= 8 || binding >= DESC_SLOTS || !t_set[set]) return NULL;
    SetInfo* si = set_find(t_set[set]);
    if (!si) return NULL;
    DescSlot* sl = &si->slot[binding];
    if (!sl->res) return NULL;
    uint64_t off = sl->off, left = 0;
    if (sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
        uint32_t at = 0;
        for (uint32_t b = 0; b < binding; b++)
            if (si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) at++;
        if (at < t_ndyn[set] && at < 8) off += t_dyn[set][at];
    }
    const uint8_t* p = buf_cpu(sl->res, off, &left);
    return p && left >= need ? p : NULL;
}

// Where a draw is placed (BG3 world metres, `at`) and how: 1 by the object constants (set 1,
// binding 0: a 16-byte offset from the eye at w = 1, or a row-major 4x4 object matrix whose last
// row is its offset from the eye; decal boxes keep their world matrix first in a 208-byte block),
// 2 by instance rows (three f32x4 rows of an instance-rate binding, a 3x4 placement with the
// translation in .w), 3 vertices in world coordinates (the object matrix only takes the eye off:
// identity rotation, offset -eye; BG3's ribbons, the jump arc and the spell line keep theirs in
// GPU-only buffers, so `at` is unknown and fxc_late places them from the first vertex once it is
// read back), 4 instance rows in a GPU-only buffer (BG3's cloud surfaces, Poison Cloud and the like,
// Instanced quads; placed by fxc_late from the first instance's rows once read back),
// 0 unplaced, -1 level-scale dressing (a placement scaled past FXC_MAX_SCALE: Avernus
// hangs a 20000x storm-cloud dome on BG3's camera, 700 m below the eye, research).
// BG3 renders camera-relative; the view constants (set 0, binding 1) hold the eye at float 24.
#define FXC_MAX_SCALE 50.0f
static const DescSlot* fxc_slot(uint32_t set, uint32_t binding) {
    if (set >= 8 || binding >= DESC_SLOTS || !t_set[set]) return NULL;
    SetInfo* si = set_find(t_set[set]);
    return si && si->slot[binding].res ? &si->slot[binding] : NULL;
}
static int fxc_place(const Recipe* r, uint32_t fi, float at[3], float obj_out[16]) {
    const float* eye = (const float*)fxc_slot_cpu(0, 1, 112);
    if (!eye) return 0;
    float rel[3] = {0, 0, 0};
    int have = 0, gpu_rows = 0;
    for (uint32_t k = 0; k < r->nbind && !have; k++) {
        if (r->bind[k][2] != VK_VERTEX_INPUT_RATE_INSTANCE) continue;
        uint32_t b = r->bind[k][0], stride = r->bind[k][1], offs[3], n = 0;
        for (uint32_t a = 0; a < r->nattr && n < 3; a++)
            if (r->attr[a][1] == b && r->attr[a][2] == VK_FORMAT_R32G32B32A32_SFLOAT) offs[n++] = r->attr[a][3];
        if (n < 3 || b >= 8 || !t_vb[b]) continue;
        // rows sorted by offset; the colour (if any) sits before or after them
        for (int x = 0; x < 2; x++) for (int y = 0; y < 2 - x; y++) if (offs[y] > offs[y + 1]) { uint32_t q = offs[y]; offs[y] = offs[y + 1]; offs[y + 1] = q; }
        uint64_t left = 0;
        const uint8_t* v = buf_cpu(t_vb[b], t_vbo[b] + (uint64_t)fi * stride, &left);
        if (!v || left < offs[2] + 16) { gpu_rows = 1; break; }  // not mapped: placed by the object constants alone, else late
        const float* row[3] = {(const float*)(v + offs[0]), (const float*)(v + offs[1]), (const float*)(v + offs[2])};
        for (int c = 0; c < 3; c++) {
            float sc = row[0][c] * row[0][c] + row[1][c] * row[1][c] + row[2][c] * row[2][c];
            if (sc > FXC_MAX_SCALE * FXC_MAX_SCALE) return -1;
        }
        rel[0] = row[0][3]; rel[1] = row[1][3]; rel[2] = row[2][3];
        have = 2;
    }
    // The object constants place it unless they are zero (instanced meshes then carry their
    // placement in the rows).
    const DescSlot* sl = fxc_slot(1, 0);
    uint64_t range = sl ? sl->range : 0;
    const float* obj = (const float*)fxc_slot_cpu(1, 0, 16);
    if (obj && range >= 64 && range != VK_WHOLE_SIZE && fxc_slot_cpu(1, 0, 64) && obj[3] == 0.0f && obj[7] == 0.0f &&
        obj[11] == 0.0f && obj[15] == 1.0f) {
        // a row-major object matrix: rows 0..2 the axes, row 3 the offset from the eye
        float tx = obj[12], ty = obj[13], tz = obj[14];
        int ident = fabsf(obj[0] - 1) < 1e-4f && fabsf(obj[5] - 1) < 1e-4f && fabsf(obj[10] - 1) < 1e-4f && fabsf(obj[1]) < 1e-4f &&
                    fabsf(obj[2]) < 1e-4f && fabsf(obj[4]) < 1e-4f && fabsf(obj[6]) < 1e-4f && fabsf(obj[8]) < 1e-4f && fabsf(obj[9]) < 1e-4f;
        if (ident && fabsf(tx + eye[24]) < 0.5f && fabsf(ty + eye[25]) < 0.5f && fabsf(tz + eye[26]) < 0.5f) {
            if (obj_out) memcpy(obj_out, obj, 64);
            return 3;
        }
        for (int c = 0; c < 3; c++) {
            float sc = obj[c] * obj[c] + obj[4 + c] * obj[4 + c] + obj[8 + c] * obj[8 + c];
            if (sc > FXC_MAX_SCALE * FXC_MAX_SCALE) return -1;
        }
        if (tx != 0 || ty != 0 || tz != 0) { rel[0] = tx; rel[1] = ty; rel[2] = tz; have = 1; }
    } else if (obj && obj[3] == 1.0f && (obj[0] != 0 || obj[1] != 0 || obj[2] != 0)) {
        rel[0] = obj[0]; rel[1] = obj[1]; rel[2] = obj[2];
        have = 1;
    }
    if (!have) return gpu_rows ? 4 : 0;
    at[0] = eye[24] + rel[0]; at[1] = eye[25] + rel[1]; at[2] = eye[26] + rel[2];
    return have;
}
// The instance-rate binding with three f32x4 placement rows (fxc_place 2 and 4): its binding, stride
// and the rows' offsets in ascending order. 1: found.
static int fxc_rows(const Recipe* r, uint32_t* bind, uint32_t* stride, uint32_t offs[3]) {
    for (uint32_t k = 0; k < r->nbind; k++) {
        if (r->bind[k][2] != VK_VERTEX_INPUT_RATE_INSTANCE) continue;
        uint32_t b = r->bind[k][0], n = 0;
        for (uint32_t a = 0; a < r->nattr && n < 3; a++)
            if (r->attr[a][1] == b && r->attr[a][2] == VK_FORMAT_R32G32B32A32_SFLOAT) offs[n++] = r->attr[a][3];
        if (n < 3 || b >= 8) continue;
        for (int x = 0; x < 2; x++) for (int y = 0; y < 2 - x; y++) if (offs[y] > offs[y + 1]) { uint32_t q = offs[y]; offs[y] = offs[y + 1]; offs[y + 1] = q; }
        *bind = b; *stride = r->bind[k][1];
        return 1;
    }
    return 0;
}

// Whether a draw belongs near Tav (her effects and the fight's), not to BG3's level far around:
// 1 near, 0 not, 2 to be decided from its vertices (world-space, fxc_late). With BG3's map off
// (compose word "void", world mode) the level is a kilometre away and the radius covers far
// destination clicks and spell ranges; without it, BG3's level stands around Tav.
#define FXC_RADIUS 25.0f
#define FXC_RADIUS_VOID 120.0f
static int g_keep_active;  // world mode's map-off state (defined with the void below)
static int mesh_draw(uint32_t count, uint32_t first, int32_t vo);  // a character's or the level's mesh (with the void below)
static int hide_has(uint64_t ib);  // a stand-in's or NPC's own index buffer (link/hide.txt, with the void below)
static float fxc_radius(void) { return g_cc.void_ && g_keep_active ? FXC_RADIUS_VOID : FXC_RADIUS; }
// Near BG3's camera counts too while the void is on: a
// party member far from Tav (its own treadmill, hundreds of metres off) casts while BG3 shows that
// member, and its spell effects and ability indicators were dropped as "far from Tav". The level stays
// a kilometre away from every member, so the radius round the eye keeps it out as before.
static int fxc_by_eye(void) { return g_cc.void_ && g_keep_active && !g_cc.fxtav; }  // word "fxtav": Tav only, as before
static int fxc_near_tav(const Recipe* r, uint32_t fi) {
    if (!g_tav_root_ok) return 1;
    float at[3], rad = fxc_radius();
    int how = fxc_place(r, fi, at, NULL);
    if (how == 3 || how == 4) return 2;
    if (how <= 0) return 0;
    float dx = at[0] - g_tav_root[0], dy = at[1] - g_tav_root[1], dz = at[2] - g_tav_root[2];
    if (dx * dx + dy * dy + dz * dz < rad * rad) return 1;
    const float* eye = fxc_by_eye() ? (const float*)fxc_slot_cpu(0, 1, 112) : NULL;
    if (!eye) return 0;
    dx = at[0] - eye[24]; dy = at[1] - eye[25]; dz = at[2] - eye[26];
    return dx * dx + dy * dy + dz * dz < rad * rad;
}

// World-space draws placed after the fact (fxc_place 3): the first vertex's position, once the
// blob holds it, decides; a draw far from Tav gets count and instances 0 in the frame file.
typedef struct { uint32_t rec;  /* the draw's record offset in the draws area */
                 uint32_t blob; /* the position's offset in the blob area */
                 uint32_t fmt; float m[16]; float eye[3];
                 uint32_t rows[3]; /* instance rows (fxc_place 4): the translation's three floats, else 0 */ } FxLate;
static FxLate* g_fxlate;
static uint32_t g_nfxlate, g_capfxlate;
static void fxc_late_add(uint32_t rec, uint32_t blob, uint32_t fmt, const float m[16], const float eye[3]) {
    if (g_nfxlate == g_capfxlate) {
        uint32_t c = g_capfxlate ? g_capfxlate * 2 : 32;
        FxLate* q = realloc(g_fxlate, c * sizeof *q);
        if (!q) return;
        g_fxlate = q; g_capfxlate = c;
    }
    FxLate* l = &g_fxlate[g_nfxlate++];
    l->rec = rec; l->blob = blob; l->fmt = fmt;
    memcpy(l->m, m, sizeof l->m); memcpy(l->eye, eye, sizeof l->eye);
    l->rows[0] = l->rows[1] = l->rows[2] = 0;
}
// An instanced draw placed by GPU-only instance rows: its first instance's translation (row c's .w at
// blob offset t[c]) plus the eye decides.
static void fxc_late_rows(uint32_t rec, const uint32_t t[3], const float eye[3]) {
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    fxc_late_add(rec, t[0], VK_FORMAT_R32G32B32A32_SFLOAT, ident, eye);
    if (g_nfxlate) memcpy(g_fxlate[g_nfxlate - 1].rows, t, 12);
}
// Applies the checks to a whole frame file: `draws` and `data` its draws and blob areas.
static uint32_t fxc_late(uint8_t* draws, uint8_t* data, size_t data_size, const FxLate* l, uint32_t n, const float tav[3], float rad, int by_eye) {
    uint32_t cut = 0;
    for (uint32_t k = 0; k < n; k++, l++) {
        if ((size_t)l->blob + 12 > data_size) continue;
        float v[3];
        if (l->rows[0] || l->rows[1] || l->rows[2]) {
            if ((size_t)l->rows[0] + 4 > data_size || (size_t)l->rows[1] + 4 > data_size || (size_t)l->rows[2] + 4 > data_size) continue;
            for (int c = 0; c < 3; c++) memcpy(&v[c], data + l->rows[c], 4);
        } else {
            memcpy(v, data + l->blob, 12);
        }
        float w[3];
        for (int c = 0; c < 3; c++) w[c] = l->eye[c] + v[0] * l->m[c] + v[1] * l->m[4 + c] + v[2] * l->m[8 + c] + l->m[12 + c];
        float dx = w[0] - tav[0], dy = w[1] - tav[1], dz = w[2] - tav[2];
        float ex = w[0] - l->eye[0], ey = w[1] - l->eye[1], ez = w[2] - l->eye[2];
        if (!(dx * dx + dy * dy + dz * dz < rad * rad) && !(by_eye && ex * ex + ey * ey + ez * ez < rad * rad)) {  // NaN counts as far
            memset(draws + l->rec + 12, 0, 8);  // count, instances
            cut++;
        }
    }
    return cut;
}

// One draw of BG3's route line: its instances' attributes (locations 1..7: three placement rows,
// then each end's clipping plane with its colour in .w, then each end's dash, path distance and
// threshold) and the line's constants (set 1 binding 0), for this present's bg3route.bin.
static int route_draw(uint32_t inst, uint32_t fi) {
    RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* r = ri ? ri->r : NULL;
    if (!r) return 'R';
    uint32_t bind = 99, off[7] = {0}, got = 0, stride = 0;
    for (uint32_t a = 0; a < r->nattr; a++) {
        uint32_t loc = r->attr[a][0];
        if (loc < 1 || loc > 7 || r->attr[a][2] != VK_FORMAT_R32G32B32A32_SFLOAT) continue;
        if (bind == 99) bind = r->attr[a][1];
        if (r->attr[a][1] != bind) return 'N';
        off[loc - 1] = r->attr[a][3];
        got |= 1u << (loc - 1);
    }
    for (uint32_t k = 0; k < r->nbind; k++)
        if (r->bind[k][0] == bind && r->bind[k][2] == VK_VERTEX_INPUT_RATE_INSTANCE) stride = r->bind[k][1];
    if (got != 0x7f || !stride || bind >= 8 || !t_vb[bind]) return 'N';
    const float* consts = (const float*)fxc_slot_cpu(1, 0, 24);
    pthread_mutex_lock(&g_fxl_lock);
    for (uint32_t i = 0; i < inst && g_route.n < ROUTE_MAX; i++) {
        uint64_t left = 0;
        const uint8_t* v = buf_cpu(t_vb[bind], t_vbo[bind] + (uint64_t)(fi + i) * stride, &left);
        if (!v) {  // GPU-only instance data: not seen so far
            if (!g_route.unmapped++) logf_("route: instance data not mapped, no route line");
            break;
        }
        int ok = 1;
        for (int l = 0; l < 7; l++) {
            if (left < (uint64_t)off[l] + 16) ok = 0;
            else memcpy(g_route.seg[g_route.n] + 4 * l, v + off[l], 16);
        }
        if (ok) g_route.n++;
    }
    if (consts) memcpy(g_route.consts, consts, 24);
    pthread_mutex_unlock(&g_fxl_lock);
    return 'P';
}

// At each present: this frame's route segments to bg3route.bin when they changed; an empty record
// once BG3 stopped drawing the line for 3 presents (a frame's draws can land a present late).
static void route_present(int on) {
    Bytes b = {0};
    int say = 0;
    uint32_t n, seq = 0;
    pthread_mutex_lock(&g_fxl_lock);
    n = on ? g_route.n : 0;
    g_route.miss = n ? 0 : g_route.miss + 1;
    if (n || (g_route.shown && g_route.miss >= 3)) {
        uint64_t h = fnv64(&n, 4, FNV0);
        h = fnv64(g_route.consts, sizeof g_route.consts, h);
        h = fnv64(g_route.seg, (size_t)n * sizeof g_route.seg[0], h);
        if (h != g_route.hash) {
            g_route.hash = h;
            seq = ++g_route.seq;
            by_put(&b, "RTE1", 4); by_u32(&b, seq); by_u32(&b, n);
            by_put(&b, g_route.consts, sizeof g_route.consts);
            by_put(&b, g_route.seg, (size_t)n * sizeof g_route.seg[0]);
            by_u32(&b, seq);
            say = n && !g_route.shown && g_route.said++ < 5;
            g_route.shown = n > 0;
        }
    }
    g_route.n = 0;
    pthread_mutex_unlock(&g_fxl_lock);
    if (!b.p) return;
    if (tav_write("bg3route.bin", b.p, b.n) && say) logf_("route: BG3's route line, %u segments (record %u)", n, seq);
    free(b.p);
}

// Records one draw of the effects pass for the replay. Returns what happened (the research log's
// decision, see fxw_draw): 0 not the effects pass (or capture off), 'R' no recipe, 'N' not an
// effect, 'F' far from Tav or unplaced, 'B' over the blob budget, 'U' samples a texture the layer
// cannot export (benilla skips it), 'C' captured, 'P' a route line
// draw (sent as segments, route_draw).
// The effects pass, and BG3's deferred decal pass with the compose word "fxdecals" (projected
// decals into the G-buffer, see fxl_pass; benilla tells them by their recipe's four colour
// attachments). Off by default: those seen only darken BG3's albedo under the bright
// previews the effects pass draws, and benilla 0012 does not replay them.
// ---- BG3's sight overlay (EoCSight.bshd) in the effects frame (enemy
// vision cones with BG3's own logic and visuals). BG3 draws it over its lit scene as one full-screen
// draw per AI subgrid visual with something to show (set 1 binding 0: float 11 the cones' strength,
// float 15 the darkness overlay's), rebuilding each pixel's world position from the linear depth and
// tinting it from the subgrid's textures. Over the void that depth is BG3's empty scene, so benilla
// replays the draw with WoW's (compose word "fxsight"): it is recorded like an effect, with its scene
// inputs as special keys (set 1 binding 4 the scene colour, 5 the G-buffer normal, 6 the linear
// depth) and the subgrid's four textures (7 cones, 8 darkness, 9 tile heights, 10 a mask), which BG3
// rewrites as enemies turn and walk, inline in the frame: key SIGHT_INLINE | the blob offset of an
// FXT1 copy, its texel bytes in bits 32..61 (header now, texels filled by the frame's GPU readback,
// fxc_gpu_fill).
#define SIGHT_FS 0xf9ac3c52a928d4f4ull
#define SIGHT_INLINE 0xc000000000000000ull
static uint64_t fxc_image_regions(const ImgInfo* i, VkBufferImageCopy* regs, uint32_t* nreg, uint32_t* bs_out, uint32_t* bw_out);
typedef struct { uint64_t image; uint32_t blob, size; VkBufferImageCopy reg; } SightImg;
static SightImg g_simg[32];
static uint32_t g_nsimg;  // this frame's (g_fxl_lock)
static __thread uint64_t t_sight_pipe;
static __thread int t_sight_is;
static int sight_pipe(uint64_t pipe) {
    if (pipe != t_sight_pipe) {
        t_sight_pipe = pipe;
        t_sight_is = 0;
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, pipe, 0) : NULL;
        for (uint32_t s = 0; ri && ri->r && s < ri->r->nstages; s++)
            if (ri->r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && ri->r->st[s].key == SIGHT_FS) t_sight_is = 1;
    }
    return t_sight_is;
}
// Whether this sight draw shows anything (cones or darkness on).
static int sight_wanted(void) {
    const float* c = (const float*)fxc_slot_cpu(1, 0, 96);
    return c && (c[11] > 0.0f || c[15] > 0.0f);
}
// The inline key of a subgrid texture (g_fxl_lock held): its FXT1 header now, its texels at present.
static uint64_t sight_inline(uint64_t view) {
    uint64_t image = view_image(view);
    ImgInfo* i = image && g_img ? TABLE_FIND(g_img, IMG_CAP, image, 0) : NULL;
    if (!i) return 3;
    for (uint32_t k = 0; k < g_nsimg; k++)
        if (g_simg[k].image == image) return SIGHT_INLINE | (uint64_t)g_simg[k].size << 32 | g_simg[k].blob;
    VkBufferImageCopy regs[16 * 6];
    uint32_t nreg = 0, bs = 0, bw = 1;
    uint64_t total = fxc_image_regions(i, regs, &nreg, &bs, &bw);
    static int said;
    if (!total || nreg != 1 || g_nsimg >= 32) {
        if (!said++) logf_("fxsight: subgrid texture %u (format %d, %u regions) not sent", i->id, (int)i->fmt, nreg);
        return 3;
    }
    uint64_t px = (uint64_t)((i->w + bw - 1) / bw) * ((i->hgt + bw - 1) / bw) * bs;
    if (px > (4u << 20)) return 3;
    uint32_t h[8] = {0, (uint32_t)i->fmt, i->w, i->hgt, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D};
    memcpy(h, "FXT1", 4);
    uint32_t at = fxc_blob(h, sizeof h);
    static const uint8_t z[4096];
    for (uint64_t k = 0; k < px; k += sizeof z) by_put(&g_fxc.blob, z, px - k > sizeof z ? sizeof z : px - k);
    g_simg[g_nsimg++] = (SightImg){image, at, (uint32_t)px, regs[0]};
    return SIGHT_INLINE | px << 32 | at;
}
static uint64_t sight_key(uint32_t binding, uint64_t view) {
    switch (binding) {
    case 4: return 2;  // the scene colour it tints
    case 5: return 6;  // the G-buffer normal (the darkness pattern's projection)
    case 6: return 4;  // the linear depth it rebuilds positions from
    case 7: case 8: case 9: case 10: return sight_inline(view);
    default: return fxc_texkey(view);
    }
}

// 1 when the draw samples a texture the layer cannot export (g_fxl_lock held): benilla would skip it.
static int fxc_unsup_draw(const Recipe* r) {
    PlInfo* pl = (g_pl && r->layout) ? TABLE_FIND(g_pl, PL_CAP, r->layout, 0) : NULL;
    uint32_t nsets = pl ? pl->nsets : 0;
    for (uint32_t s = 0; s < nsets && s < 8; s++) {
        DslInfo* l = g_dsl ? TABLE_FIND(g_dsl, DSL_CAP, pl->sets[s], 0) : NULL;
        SetInfo* si = set_find(t_set[s]);
        for (uint32_t k = 0; l && si && k < l->n; k++) {
            uint32_t binding = l->b[k].binding, type = l->b[k].type;
            if (binding >= 32 || binding >= DESC_SLOTS || !(r->used[s] >> binding & 1)) continue;
            if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
            uint64_t key = fxc_texkey_plain(si->slot[binding].res);
            if (key && unsup_has(key)) return 1;
        }
    }
    return 0;
}
// A spell's solid piece: an unblended, depth-tested, unskinned draw of the effects pass. BG3 draws
// Grease's liquid burst (the blob leaving the hand and the splash) and other spells' solid parts as
// opaque meshes there, next to their blended smoke, and draws the same geometry again in its depth
// prepass and G-buffer, so the mesh test below calls their index buffer a character's. A figure's own
// body is skinned and never drawn in this pass, so `nskin` tells them apart (a playtest report:
// Grease showed its smoke alone).
// The research filters name a recipe by one of its shader keys (the fxwhy line's `vs` and `fs`),
// which are known from the first draw; its own key is only made when the recipe is first written.
static int fxc_stage_is(const Recipe* r, uint64_t key) {
    for (uint32_t s = 0; s < r->nstages; s++) if (r->st[s].key == key) return 1;
    return 0;
}
static int fxc_solid(const Recipe* r) {
    if (t_fx_surf) return 0;  // BG3's own ground surface: wowbg3_surf.inc.c draws it on WoW's ground
    if (!(g_cc.fxsolid && r && (r->have & 1) && r->ncolor && !r->blend[0][0] && r->ztest && !r->nskin)) return 0;
    // A piece whose depth test is equality draws only where its own depth was written first. BG3
    // masks these inside this very pass: one recipe lays the mesh down with an alpha test and a
    // depth write (zw1, greater-or-equal; fragment shader with an OpKill), the next draws the
    // colour with equality and no write and has no discard of its own. The replay used to force
    // every draw's depth write off, so the equality draws failed everywhere, which is why Grease's
    // blob and splash never arrived; forcing the test to greater-or-equal instead drew the whole
    // unmasked billboard (a black quad and black shards). benilla 0080 keeps the write
    // and resets its own depth copy between the over-black and over-white runs, so the mask is
    // back and these are taken like any other solid piece.
    for (uint32_t k = 0; k < g_cc.n_solid_skip; k++) if (fxc_stage_is(r, g_cc.solid_skip[k])) return 0;
    if (g_cc.n_solid_only) {
        for (uint32_t k = 0; k < g_cc.n_solid_only; k++) if (fxc_stage_is(r, g_cc.solid_only[k])) return 1;
        return 0;
    }
    if (!g_cc.fxsolidlog) return 1;  // research logging off the shipped path
    // Research (word "fxsolidlog"): every recipe taken as solid, once, by the shader keys the filters name.
    static uint64_t said[64];
    static uint32_t nsaid;
    uint64_t vs = 0, fs = 0;
    for (uint32_t s = 0; s < r->nstages; s++) {
        if (r->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = r->st[s].key;
        if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = r->st[s].key;
    }
    int known = 0;
    for (uint32_t k = 0; k < nsaid; k++) if (said[k] == vs) known = 1;
    if (!known && nsaid < 64) {
        said[nsaid++] = vs;
        logf_("fxsolid: taking vs %016llx fs %016llx (zt%u zw%u zc%u nc%u)", (unsigned long long)vs, (unsigned long long)fs,
              r->ztest, r->zwrite, r->zcmp, r->ncolor);
    }
    return 1;
}
// Not the figure itself: a blended, unskinned draw on a figure's index buffer is something BG3 hangs
// on the character. BG3's hover highlight (the bright ring with arrows under the character it draws
// under the pointer) comes in as an attachment visual, so TavPlan.lua lists its index buffer with
// hers and the figure drop took it with her, for everyone (a playtest report). Such a draw goes on
// through the effects capture instead; her skinned meshes (body, hair, eyelashes) still stop here.
//
// Word "fxglow": a spell's glow on a figure (BG3's overlay materials: Stillness of Mind, Wholeness of
// Body, Stunning Strike). BG3 redraws the figure's own meshes, blended, in the effects pass, and the
// figure drop took those with her. WoW draws figures in BG3's pose, so the replay fits them. A figure's
// own blended pieces (hair edges) are to stay dropped, and they differ by when they begin: they draw
// from the moment the figure does, while a glow begins on a buffer that has long been drawing. So a
// blended recipe is a glow when first seen on a figure's buffer GLOW_SETTLE presents or more after that
// buffer first drew; the verdict stays for that pair. "A spell is on" (g_fx_seen_at) cannot decide
// this: Stillness of Mind draws no effect but the glow. A stand-in is not a figure: its body is
// culled, WoW's mob has another shape, so its glow is not shown.
#define GLOW_SETTLE 60u
#define GLOW_BUFS 4096u
#define GLOW_PAIRS 4096u
typedef struct { uint64_t h, v; } GlowSlot;
static GlowSlot g_glow_buf[GLOW_BUFS];    // figure buffer -> present + 1 it first drew
static GlowSlot g_glow_pair[GLOW_PAIRS];  // buffer and recipe -> 1 own, 2 glow
static uint64_t g_glow_kept, g_glow_full;
// The slot's value, set to `v` if the key is new. 0: past the probe limit.
static uint64_t glow_slot(GlowSlot* t, uint32_t cap, uint64_t k, uint64_t v) {
    uint32_t i = hslot(k, cap);
    for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (cap - 1)) {
        uint64_t h = __atomic_load_n(&t[i].h, __ATOMIC_ACQUIRE);
        if (!h) {
            uint64_t zero = 0;
            if (__atomic_compare_exchange_n(&t[i].h, &zero, k, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&t[i].v, v, __ATOMIC_RELEASE);
                return v;
            }
            h = zero;
        }
        if (h == k) {
            uint64_t got = __atomic_load_n(&t[i].v, __ATOMIC_ACQUIRE);
            return got ? got : v;  // being stored by the thread that claimed it
        }
    }
    __atomic_add_fetch(&g_glow_full, 1, __ATOMIC_RELAXED);
    return 0;
}
static void glow_forget(void) {  // a load: buffers are recycled
    memset(g_glow_buf, 0, sizeof g_glow_buf);
    memset(g_glow_pair, 0, sizeof g_glow_pair);
}
static int glow_draw(VkCommandBuffer cb, const Recipe* r) {
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    uint64_t first = glow_slot(g_glow_buf, GLOW_BUFS, t_ib | 1, now + 1);
    if (t_fxl_cb != cb || !first || !r->ncolor || !r->blend[0][0] || !r->ztest || r->zwrite) return 0;
    uint64_t k[2] = {t_ib, (uint64_t)t_pipe};
    uint64_t verdict = glow_slot(g_glow_pair, GLOW_PAIRS, fnv64(k, sizeof k, FNV0) | 1, now + 1 >= first + GLOW_SETTLE ? 2 : 1);
    if (verdict != 2) return 0;
    t_fxc_glow = 1;
    __atomic_add_fetch(&g_glow_kept, 1, __ATOMIC_RELAXED);
    return 1;
}
static int fxc_figure_extra(VkCommandBuffer cb) {
    if ((!g_cc.fxhl && !g_cc.fxglow) || t_bind_cb != cb || !t_pipe) return 0;
    RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* r = ri ? ri->r : NULL;
    if (!r || !(r->have & 1)) return 0;
    if (g_cc.fxglow && glow_draw(cb, r)) return 1;
    return g_cc.fxhl && r->ncolor && r->blend[0][0] && r->ztest && !r->zwrite && !r->nskin;
}
// Research: every texture a spell's solid piece samples, once per recipe, with what the layer makes
// of it (key 1 the scene depth, 2 the scene colour, 3 a render target it has no name for and benilla
// binds black, 4 the linear depth, 5 the light grid, 6 the G-buffer normal; anything else an exported
// texture). These shaders are BG3's forward-lit material ones, so a black stand-in for what they read
// is why the pieces came out black.
static void fxc_solid_tex(const Recipe* r, uint32_t set, uint32_t binding, uint64_t view, uint64_t key) {
    if (!g_cc.fxsolidlog || !g_cc.fxsolid || !fxc_solid(r)) return;  // research only (word "fxsolidlog")
    static uint64_t said[256];
    static uint32_t nsaid;
    uint64_t h = (uint64_t)r->key * 1315423911u + set * 64 + binding;
    for (uint32_t k = 0; k < nsaid; k++) if (said[k] == h) return;
    if (nsaid < 256) said[nsaid++] = h;
    ViewInfo* v = (g_view && view) ? TABLE_FIND(g_view, VIEW_CAP, view, 0) : NULL;
    ImgInfo* i = v && v->image ? TABLE_FIND(g_img, IMG_CAP, v->image, 0) : NULL;
    logf_("fxsolid: pipe %016llx set %u binding %u -> key %llx%s (format %d %ux%u layers %u usage %x)",
          (unsigned long long)r->key, set, binding, (unsigned long long)key, key == 3 ? " BLACK" : "",
          i ? (int)i->fmt : -1, i ? i->w : 0, i ? i->hgt : 0, i ? i->layers : 0, i ? (unsigned)i->usage : 0);
}
static uint64_t g_fx_seen_at;  // present + 1 of the last effects-pass draw near Tav (fxprops' "a spell is on")
static int fxc_draw(VkCommandBuffer cb, int indexed, uint32_t count, uint32_t inst, uint32_t first, int32_t vo, uint32_t fi) {
    if (!g_fxc_on || t_bind_cb != cb || !t_pipe) return 0;
    if (route_pipe(t_pipe)) return route_draw(inst, fi);  // BG3's route line, from its own pass
    const int sight = !indexed && g_cc.fxsight && sight_pipe(t_pipe);
    if (sight) {
        if (!sight_wanted()) return 'S';
    } else if (t_fxl_cb != cb && !(t_dec_cb == cb && g_cc.fxdecals)) {
        return 0;
    }
    RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* r = ri ? ri->r : NULL;
    if (!r || !(r->have & 1)) return 'R';
    int near = 1;
    if (!sight) {
        // Effects only: blended, depth-tested, not writing depth. The same pass also forward-renders
        // parts of BG3's level (opaque, or full-screen haze without a depth test); those stay BG3's.
        const int solid = fxc_solid(r);
        if (!solid && (!r->ncolor || !r->blend[0][0] || !r->ztest || r->zwrite)) return 'N';
        // A character's or the level's mesh. A spell's solid piece shares its slice with the depth and
        // G-buffer passes BG3 draws it in as well, so this test is for the blended draws.
        if (!solid && indexed && t_ib_cb == cb && mesh_draw(count, first, vo)) return 'M';
        // Skinning tells a spell's solid piece from a body, not from what a stand-in holds: a sword or a
        // shield is rigid (unskinned) too. The hide list names those by their index buffers.
        if (solid && indexed && t_ib_cb == cb && hide_has(t_ib)) return 'M';
        near = fxc_near_tav(r, fi);
        if (!near) { g_fxc.far++; return 'F'; }
        __atomic_store_n(&g_fx_seen_at, __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    }
    // World-space vertices: the first vertex's position (lowest location float3 or float4 of a
    // per-vertex binding) decides once it is in the blob (fxc_late).
    float lm[16], lat[3];
    uint32_t lbind = 99, loff = 0, lfmt = 0, lloc = 99;
    uint32_t rbind = 99, rstride = 0, roffs[3] = {0, 0, 0};  // GPU-only instance rows (fxc_place 4)
    int lhow = near == 2 ? fxc_place(r, fi, lat, lm) : 0;
    if (lhow == 4 && !fxc_rows(r, &rbind, &rstride, roffs)) rbind = 99;
    if (lhow == 3) {
        for (uint32_t a = 0; a < r->nattr; a++) {
            uint32_t loc = r->attr[a][0], b = r->attr[a][1], fmt = r->attr[a][2];
            int rate = 0;
            for (uint32_t k = 0; k < r->nbind; k++) if (r->bind[k][0] == b) rate = (int)r->bind[k][2];
            if (rate || loc >= lloc || (fmt != VK_FORMAT_R32G32B32_SFLOAT && fmt != VK_FORMAT_R32G32B32A32_SFLOAT)) continue;
            lloc = loc; lbind = b; loff = r->attr[a][3]; lfmt = fmt;
        }
    }
    const float* leye = near == 2 ? (const float*)fxc_slot_cpu(0, 1, 112) : NULL;
    pthread_mutex_lock(&g_fxl_lock);
    if (g_fxc.blob.n > (48u << 20)) { g_fxc.dropped++; pthread_mutex_unlock(&g_fxl_lock); return 'B'; }
    if (g_nunsup && !sight && !g_cc.fxwhole && fxc_unsup_draw(r)) { g_fxc.unsup++; pthread_mutex_unlock(&g_fxl_lock); return 'U'; }
    uint64_t pk = recipe_key(t_pipe);
    if (!pk) { pthread_mutex_unlock(&g_fxl_lock); return 'R'; }  // not whole (recipe_whole)
    Bytes* d = &g_fxc.draws;
    uint32_t rec = (uint32_t)d->n;
    by_u64(d, pk);
    by_u32(d, (uint32_t)indexed); by_u32(d, count); by_u32(d, inst); by_u32(d, first); by_put(d, &vo, 4); by_u32(d, fi);
    // Index data: from the first index used, so the replay draws from index 0.
    uint32_t isz = t_ibt == VK_INDEX_TYPE_UINT32 ? 4 : 2;
    uint32_t maxv = count;
    uint32_t imin = 0xffffffffu, imax = 0;  // the draw's index range, when its indices are known
    if (indexed && t_ib && t_ib_cb == cb) {
        uint64_t off = t_ibo + (uint64_t)first * isz, left = 0;
        const uint8_t* ip = buf_cpu(t_ib, off, &left);
        uint8_t* cached = NULL;
        if (!(ip && left >= (uint64_t)count * isz) && r->nskin) {
            // A GPU-only index buffer: its copy from an earlier frame (static data, fxc_gpu_fill).
            BufInfo* bi = g_buf ? TABLE_FIND(g_buf, BUF_CAP, t_ib, 0) : NULL;
            uint64_t sz = (uint64_t)count * isz;
            if (bi && bi->size > off && sz <= bi->size - off) {
                GpuReq key = {t_ib, off, sz, 0};
                uint64_t ck = fnv64(&key, 24, FNV0) | 1;
                pthread_mutex_lock(&g_fxj_data);
                GpuCache* c = &g_gpuc[ck % GPUC_CAP];
                if (c->h == ck && c->data && (cached = malloc(sz))) memcpy(cached, c->data, sz);
                pthread_mutex_unlock(&g_fxj_data);
            }
            if (cached) { ip = cached; left = sz; }
        }
        if (ip && left >= (uint64_t)count * isz) {
            maxv = 0;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t v = isz == 4 ? ((const uint32_t*)ip)[k] : ((const uint16_t*)ip)[k];
                if (v == (isz == 4 ? 0xffffffffu : 0xffffu)) continue;
                if (v + 1 > maxv) maxv = v + 1;
                if (v < imin) imin = v;
                if (v > imax) imax = v;
            }
        }
        free(cached);
        by_u32(d, isz == 4 ? 2 : 1);
        uint32_t isize = 0;
        uint32_t at = fxc_range(t_ib, off, (uint64_t)count * isz, &isize);
        by_u32(d, at); by_u32(d, isize);
    } else {
        by_u32(d, 0); by_u32(d, 0); by_u32(d, 0);
    }
    // Vertex buffers of the recipe's bindings, from their bound offsets.
    by_u32(d, r->nbind);
    for (uint32_t k = 0; k < r->nbind; k++) {
        uint32_t b = r->bind[k][0], stride = r->bind[k][1], rate = r->bind[k][2];
        uint64_t want = rate ? (uint64_t)stride * (fi + inst)
                             : (uint64_t)stride * ((indexed ? (uint64_t)(vo > 0 ? vo : 0) + maxv : (uint64_t)first + count) + 1);
        if (want > (8u << 20)) want = 8u << 20;
        uint32_t at = 0, size = 0;
        if (b < 8 && t_vb[b]) at = fxc_range(t_vb[b], t_vbo[b], want, &size);
        by_u32(d, b); by_u32(d, at); by_u32(d, size);
        if (b == lbind && leye && size) {
            uint64_t v0 = indexed ? (uint64_t)(vo > 0 ? vo : 0) : first;
            uint64_t pos = (uint64_t)at + v0 * stride + loff;
            if (v0 * stride + loff + 12 <= size) fxc_late_add(rec, (uint32_t)pos, lfmt, lm, leye + 24);
        }
        if (b == rbind && leye && size && (uint64_t)fi * rstride + roffs[2] + 16 <= size) {
            uint32_t t[3];
            for (int c = 0; c < 3; c++) t[c] = at + fi * rstride + roffs[c] + 12;
            fxc_late_rows(rec, t, leye + 24);
        }
    }
    // Skinned vertex stream (spv_meta): only the records this draw's vertices read, from each base
    // (object constants, set 1 binding 0) its vertex shader adds, one after another in the blob,
    // and the bases in the blob's copy of the object constants rewritten to find them there.
    // VertexIndex runs over [vmin, vmax]: the index range plus the vertex offset, or first ..
    // first + count - 1. Without a known index range (a GPU-only index buffer's first frame) the
    // whole range is copied as before.
    uint32_t sbase[4] = {0, 0, 0, 0}, snew[4] = {0, 0, 0, 0}, vmin = 0, vmax = 0;
    int skin_ok = 0, skin_rw = 0;
    if (r->nskin && (r->used[0] >> 13 & 1) && !g_cc.fxwhole) {
        int vok = 0;
        if (indexed && imin <= imax) {
            int64_t a = (int64_t)vo + imin, b = (int64_t)vo + imax;
            if (a >= 0 && b < 0xffffffffll) { vmin = (uint32_t)a; vmax = (uint32_t)b; vok = 1; }
        } else if (!indexed && count && (uint64_t)first + count - 1 < 0xffffffffull) {
            vmin = first; vmax = first + count - 1; vok = 1;
        }
        uint32_t need = 0;
        for (int k = 0; k < r->nskin; k++) if (r->skin[k] + 4u > need) need = r->skin[k] + 4u;
        const uint8_t* obj = vok && vmax - vmin < (1u << 20) ? fxc_slot_cpu(1, 0, need) : NULL;
        if (obj) {
            for (int k = 0; k < r->nskin; k++) memcpy(&sbase[k], obj + r->skin[k], 4);
            skin_ok = 1;
        } else {
            g_fxc.skin_full++;
        }
    }
    // Descriptor sets of the pipeline layout.
    PlInfo* pl = (g_pl && r->layout) ? TABLE_FIND(g_pl, PL_CAP, r->layout, 0) : NULL;
    uint32_t nsets = pl ? pl->nsets : 0;
    if (nsets > 8) nsets = 8;
    by_u32(d, nsets);
    for (uint32_t s = 0; s < nsets; s++) {
        DslInfo* l = g_dsl ? TABLE_FIND(g_dsl, DSL_CAP, pl->sets[s], 0) : NULL;
        SetInfo* si = set_find(t_set[s]);
        uint32_t n = l ? l->n : 0, nu = 0;
        for (uint32_t k = 0; k < n; k++) if (l->b[k].binding < 32 && (r->used[s] >> l->b[k].binding & 1)) nu++;
        by_u32(d, s); by_u32(d, nu);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t binding = l->b[k].binding, type = l->b[k].type;
            if (!(binding < 32 && (r->used[s] >> binding & 1))) continue;
            DescSlot* sl = (si && binding < DESC_SLOTS) ? &si->slot[binding] : NULL;
            by_u32(d, binding); by_u32(d, type);
            switch (type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                uint64_t off = sl ? sl->off : 0, range = sl ? sl->range : 0;
                if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                    // Dynamic offsets come in binding order (the layout may list bindings unordered).
                    uint32_t at = 0;
                    for (uint32_t j = 0; j < n; j++)
                        if ((l->b[j].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || l->b[j].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
                            l->b[j].binding < binding)
                            at++;
                    if (at < t_ndyn[s] && at < 8) off += t_dyn[s][at];
                }
                if (range == VK_WHOLE_SIZE || range == 0) {
                    BufInfo* bi = (sl && g_buf) ? TABLE_FIND(g_buf, BUF_CAP, sl->res, 0) : NULL;
                    range = bi && bi->size > off ? bi->size - off : 0;
                    if (range > (1u << 20)) range = 1u << 20;
                }
                uint32_t bsize = (uint32_t)range, at = 0;
                int done = 0;
                if (skin_ok && s == 0 && binding == 13 && sl && sl->res && type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    // the skinned stream: each base's records [vmin, vmax], one after another
                    uint64_t span = (uint64_t)(vmax - vmin) + 1, j = 0, jk[4] = {0, 0, 0, 0};
                    int fits = 1;
                    for (int k = 0; k < r->nskin; k++) {
                        if (!sbase[k]) continue;
                        uint64_t from = ((uint64_t)sbase[k] - 1 + vmin) * 20;
                        if (from + span * 20 > range) fits = 0;
                    }
                    int zero = 1;
                    for (int pad = 0; fits && zero && pad < 2; pad++) {  // a rewritten base must not be 0 (0: not skinned)
                        j = (uint64_t)pad;
                        zero = 0;
                        for (int k = 0; k < r->nskin; k++) {
                            if (!sbase[k]) continue;
                            jk[k] = j; j += span;
                            snew[k] = (uint32_t)(1u - vmin + (uint32_t)jk[k]);
                            zero |= snew[k] == 0;
                        }
                    }
                    if (zero) fits = 0;
                    if (fits) {
                        at = fxc_blob(NULL, j * 20);
                        bsize = (uint32_t)(j * 20);
                        for (int k = 0; k < r->nskin; k++)
                            if (sbase[k]) fxc_fill_from(sl->res, off + ((uint64_t)sbase[k] - 1 + vmin) * 20, span * 20, at + (uint32_t)(jk[k] * 20));
                        skin_rw = 1;
                        done = 1;
                        g_fxc.skin_draws++;
                        g_fxc.skin_bytes += bsize;
                        g_fxc.skin_whole += range;
                    } else {
                        g_fxc.skin_full++;
                    }
                } else if (skin_rw && s == 1 && binding == 0 && sl && sl->res) {
                    // the object constants with the rewritten bases (a copy of this draw's own)
                    uint64_t left = 0;
                    const uint8_t* src = buf_cpu(sl->res, off, &left);
                    if (src && left >= range && range <= (1u << 16)) {
                        at = fxc_blob(src, range);
                        for (int k = 0; k < r->nskin; k++)
                            if (sbase[k] && r->skin[k] + 4u <= range) memcpy(g_fxc.blob.p + at + r->skin[k], &snew[k], 4);
                        done = 1;
                    }
                } else if (r->clus && !g_cc.fxwhole && s == 2 && binding == 11 && si && sl && sl->res && type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    // the light index lists: their start, as much as the cluster headers use (fxc_clus_fix)
                    const DescSlot* h = &si->slot[10];
                    uint64_t hoff = h->off, hr = h->range;
                    if (hr == VK_WHOLE_SIZE || hr == 0) {
                        BufInfo* bi = (h->res && g_buf) ? TABLE_FIND(g_buf, BUF_CAP, h->res, 0) : NULL;
                        hr = bi && bi->size > hoff ? bi->size - hoff : 0;
                        if (hr > (1u << 20)) hr = 1u << 20;
                    }
                    if (h->res && h->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && hr && range == hr * 16) {
                        uint32_t hsize = 0, hat = fxc_range(h->res, hoff, hr, &hsize);
                        uint64_t want = __atomic_load_n(&g_clus_want, __ATOMIC_RELAXED);
                        if (want > range) want = range;
                        at = fxc_range(sl->res, off, want & ~3ull, &bsize);
                        done = 1;
                        int dup = 0;
                        for (uint32_t k = 0; k < g_fxc.nclus; k++) dup |= g_fxc.clus[k].hdr == hat && g_fxc.clus[k].list == at;
                        if (!dup && g_fxc.nclus < FXC_NCLUS) {
                            g_fxc.clus[g_fxc.nclus++] = (FxClus){hat, hsize, at, bsize};
                            g_fxc.clus_bytes += bsize;
                            g_fxc.clus_whole += range;
                        }
                    }
                }
                if (!done) at = sl && sl->res ? fxc_range(sl->res, off, range, &bsize) : fxc_blob(NULL, range);
                by_u32(d, at); by_u32(d, bsize);
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                by_u64(d, l->b[k].imm ? l->b[k].imm : sampler_key(sl ? sl->samp : 0));
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                uint64_t tk = sl ? fxc_texkey(sl->res) : 3;
                fxc_solid_tex(r, s, binding, sl ? sl->res : 0, tk);
                by_u64(d, tk);
                by_u64(d, l->b[k].imm ? l->b[k].imm : sampler_key(sl ? sl->samp : 0));
                break;
            }
            default: {  // sampled and storage images, input attachments
                uint64_t tk = sl ? (sight && s == 1 ? sight_key(binding, sl->res) : fxc_texkey(sl->res)) : 3;
                fxc_solid_tex(r, s, binding, sl ? sl->res : 0, tk);
                by_u64(d, tk);
                break;
            }
            }
        }
    }
    t_fxc_glow = 0;
    // Push constants only when the pipeline's layout declares them (t_pc can be left over from an
    // earlier pipeline in the command buffer).
    uint32_t pc = (pl && pl->npush) ? t_pc : 0;
    by_u32(d, pc);
    by_put(d, t_pcb, pc > 256 ? 256 : pc);
    by_pad(d, 4);
    g_fxc.ndraws++;
    pthread_mutex_unlock(&g_fxl_lock);
    return sight ? 'G' : 'C';
}

// ---- effects readbacks off the present path
// fxc_gpu_fill waited at every present for its copy of GPU-only ranges (BG3's storage buffers),
// and post_lut_key every POST_LUT_EVERY presents for BG3's rendered LUTs: each wait lasts until
// BG3's whole GPU frame is done (longer while another program loads the GPU), on BG3's render
// thread. Now those copies are submitted with a fence and a worker thread (fxj_main) finishes
// them: it waits for the fence, fills the frame and writes fxframe.bin, or content-keys and
// writes a LUT. Every frame file goes through the worker while it runs, so frames stay in
// order. Compose word "fxsync" restores the waits at the present.
#define FXJ_N 4
typedef struct { uint32_t blob; int cache; uint64_t at, size, ck, buffer; } FxFill;
typedef struct {
    int kind;                 // 1 frame file, 2 texture export (LUT re-read or new texture)
    int fenced;               // a submitted GPU copy to wait for (changed under g_fxj.m)
    int lost;                 // its fence never signalled: make a new one
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    GBuf st;                  // host-visible copy target
    Bytes out;                // frame: the whole fxframe.bin, GPU-only ranges still zero
    size_t data_off, post_at;
    uint32_t exp_blob[2];
    int exp;
    FxFill* fill;
    uint32_t nfill, capfill;
    FxLate* late;             // world-space draws to place once filled (fxc_late)
    uint32_t nlate, caplate;
    FxClus clus[FXC_NCLUS];   // clustered lights to check once filled (fxc_clus_fix)
    uint32_t nclus;
    size_t draws_at;
    float tav[3], rad;
    int by_eye;  // near BG3's eye counts too (fxc_by_eye at the frame)
    int which;                // texture: g_post_lut slot (-1 none), the key (0: by content), the image,
    uint64_t key, image;      // its description and copy regions
    ImgInfo info;
    uint32_t bs, bw, nreg;
    VkBufferImageCopy regs[16 * 6];
} FxJob;
static struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    FxJob j[FXJ_N];
    int busy[FXJ_N];          // taken by the present thread or queued
    uint32_t q[FXJ_N], qhead, qn;
    pthread_t th;
    int started;              // 1 running, -1 failed (readbacks wait at the present)
    int stop;
    DeviceData* d;
    uint32_t family;
    uint64_t waits, wait_ns;  // present waits for a free job (the GPU four frames behind)
    uint64_t pend[64];        // texture keys submitted lately (not submitted twice while in flight)
    uint32_t npend;
} g_fxj = {.m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void fxj_lut_done(int which, uint64_t image, uint64_t key);  // after g_post_lut
static void fxc_image_bytes(Bytes* out, const ImgInfo* i, const VkBufferImageCopy* regs, uint32_t nreg, uint32_t bs, uint32_t bw,
                            const uint8_t* map);

static void fxj_finish_frame(FxJob* j, int ok) {
    if (ok) {
        for (uint32_t f = 0; f < j->nfill; f++) {
            const FxFill* x = &j->fill[f];
            const uint8_t* src = (const uint8_t*)j->st.map + x->at;
            memcpy(j->out.p + j->data_off + x->blob, src, x->size);
            if (!x->cache) continue;  // static data (not written by shaders) is kept for the next frames
            uint8_t* copy = malloc(x->size);
            if (!copy) continue;
            memcpy(copy, src, x->size);
            pthread_mutex_lock(&g_fxj_data);
            GpuCache* c = &g_gpuc[x->ck % GPUC_CAP];
            free(c->data);
            c->h = x->ck;
            c->data = copy;
            pthread_mutex_unlock(&g_fxj_data);
        }
        if (j->nclus) fxc_clus_fix(j->out.p + j->data_off, j->out.n - j->data_off, j->clus, j->nclus);
        if (j->exp && j->post_at) {  // the post block's exposure pair, now that its ranges are filled
            memcpy(j->out.p + j->post_at + 8, j->out.p + j->data_off + j->exp_blob[0] + 4, 4);
            memcpy(j->out.p + j->post_at + 12, j->out.p + j->data_off + j->exp_blob[1], 4);
            static int said;  // post_block's own log line shows 0 x 0: it ran before the fill
            if (!said) {
                said = 1;
                float ea, eb;
                memcpy(&ea, j->out.p + j->post_at + 8, 4);
                memcpy(&eb, j->out.p + j->post_at + 12, 4);
                logf_("fxcopy: post exposure (filled by the readback worker): %g x %g", ea, eb);
            }
        }
        if (j->nlate) {
            uint32_t cut = fxc_late(j->out.p + j->draws_at, j->out.p + j->data_off, j->out.n - j->data_off, j->late, j->nlate, j->tav, j->rad, j->by_eye);
            static uint32_t said;
            if (cut && said++ < 5) logf_("fxcopy: %u of %u world-space draws far from Tav and BG3's eye (dropped)", cut, j->nlate);
        }
        tav_write("fxframe.bin", j->out.p, j->out.n);
    }
    free(j->out.p);
    j->out = (Bytes){0};
}
static void fxj_finish_tex(FxJob* j) {
    Bytes out = {0};
    fxc_image_bytes(&out, &j->info, j->regs, j->nreg, j->bs, j->bw, j->st.map);
    if (!out.p) return;
    uint64_t key = j->key ? j->key : fnv64(out.p, out.n, FNV0) | 4;
    char path[256];
    struct stat st;
    snprintf(path, sizeof path, "%s/tex/%016llx.bin", fxc_dir(), (unsigned long long)key);
    if (stat(path, &st) != 0) {
        fxc_write_once("tex", key, ".bin", out.p, out.n);
        logf_("fxcopy: texture %016llx (%ux%ux%u format %d, %u mips) exported", (unsigned long long)key, j->info.w, j->info.hgt,
              j->info.type == VK_IMAGE_TYPE_3D && j->info.depth ? j->info.depth : 1, (int)j->info.fmt, j->info.mips ? j->info.mips : 1);
    }
    free(out.p);
    if (j->which >= 0) fxj_lut_done(j->which, j->image, key);
}
static void* fxj_main(void* arg) {
    (void)arg;
    pthread_mutex_lock(&g_fxj.m);
    for (;;) {
        while (!g_fxj.qn && !g_fxj.stop) pthread_cond_wait(&g_fxj.cv, &g_fxj.m);
        if (!g_fxj.qn) break;  // stopping, nothing left
        uint32_t i = g_fxj.q[g_fxj.qhead];
        FxJob* j = &g_fxj.j[i];
        int fenced = j->fenced;
        pthread_mutex_unlock(&g_fxj.m);
        int ok = !fenced || g_fxj.d->WaitForFences(g_fxj.d->device, 1, &j->fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
        if (!ok) logf_("fxcopy: an effects readback did not finish in 5 s; dropped");
        if (j->kind == 1) fxj_finish_frame(j, ok);
        else if (j->kind == 2 && ok) fxj_finish_tex(j);
        pthread_mutex_lock(&g_fxj.m);
        j->fenced = 0;
        if (!ok) j->lost = 1;
        g_fxj.qhead = (g_fxj.qhead + 1) % FXJ_N;
        g_fxj.qn--;
        g_fxj.busy[i] = 0;
        pthread_cond_broadcast(&g_fxj.cv);
    }
    pthread_mutex_unlock(&g_fxj.m);
    return NULL;
}
// A free job for the present thread (it waits while all are in flight), or NULL: readbacks then
// wait at the present as before.
static FxJob* fxj_take(QueueData* q) {
    DeviceData* d = q->dev;
    pthread_mutex_lock(&g_fxj.m);
    if (!g_fxj.started) {
        g_fxj.d = d;
        g_fxj.family = q->family;
        g_fxj.started = pthread_create(&g_fxj.th, NULL, fxj_main, NULL) == 0 ? 1 : -1;
        if (g_fxj.started < 0) logf_("fxcopy: no readback thread; readbacks wait at the present");
    }
    if (g_fxj.started != 1 || g_fxj.d != d || g_fxj.family != q->family) {
        pthread_mutex_unlock(&g_fxj.m);
        return NULL;
    }
    int i = -1;
    uint64_t t0 = 0;
    for (;;) {
        for (int k = 0; k < FXJ_N && i < 0; k++) if (!g_fxj.busy[k]) i = k;
        if (i >= 0) break;
        if (!t0) { t0 = now_ns(); g_fxj.waits++; }
        pthread_cond_wait(&g_fxj.cv, &g_fxj.m);
    }
    if (t0) g_fxj.wait_ns += now_ns() - t0;
    g_fxj.busy[i] = 1;
    FxJob* j = &g_fxj.j[i];
    if (!j->pool || j->lost) {
        int ok = 1;
        if (!j->pool) {
            VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, q->family};
            ok = d->CreateCommandPool(d->device, &pci, NULL, &j->pool) == VK_SUCCESS;
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, j->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            ok = ok && d->AllocateCommandBuffers(d->device, &ai, &j->cmd) == VK_SUCCESS &&
                 (!d->setLoaderData || d->setLoaderData(d->device, j->cmd) == VK_SUCCESS);
        }
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        ok = ok && d->CreateFence(d->device, &fci, NULL, &j->fence) == VK_SUCCESS;
        if (!ok) {
            g_fxj.busy[i] = 0;
            g_fxj.started = -1;  // the worker idles; readbacks wait at the present from now on
            logf_("fxcopy: cannot make readback objects; readbacks wait at the present");
            pthread_mutex_unlock(&g_fxj.m);
            return NULL;
        }
        j->lost = 0;
    }
    pthread_mutex_unlock(&g_fxj.m);
    j->kind = 0;
    j->fenced = 0;
    j->nfill = 0;
    j->nlate = 0;
    j->nclus = 0;
    j->exp = 0;
    j->post_at = j->data_off = 0;
    free(j->out.p);
    j->out = (Bytes){0};
    return j;
}
static void fxj_submitted(FxJob* j, int ok) {  // after its QueueSubmit (g_trace_lock held while buffers are copied)
    pthread_mutex_lock(&g_fxj.m);
    j->fenced = ok;
    if (!ok) j->nfill = 0;
    pthread_mutex_unlock(&g_fxj.m);
}
static void fxj_queue(FxJob* j) {
    pthread_mutex_lock(&g_fxj.m);
    g_fxj.q[(g_fxj.qhead + g_fxj.qn) % FXJ_N] = (uint32_t)(j - g_fxj.j);
    g_fxj.qn++;
    pthread_cond_broadcast(&g_fxj.cv);
    pthread_mutex_unlock(&g_fxj.m);
}
static void fxj_drop(FxJob* j) {
    pthread_mutex_lock(&g_fxj.m);
    g_fxj.busy[j - g_fxj.j] = 0;
    pthread_cond_broadcast(&g_fxj.cv);
    pthread_mutex_unlock(&g_fxj.m);
}
static void fxj_drain(void) {  // before the present writes a frame itself ("fxsync")
    pthread_mutex_lock(&g_fxj.m);
    while (g_fxj.started == 1 && g_fxj.qn) pthread_cond_wait(&g_fxj.cv, &g_fxj.m);
    pthread_mutex_unlock(&g_fxj.m);
}
// BG3 destroys a buffer (after the trace table marked it gone): copies still reading it finish first.
static void fxj_retire_buffer(uint64_t buffer) {
    if (g_fxj.started != 1 || !buffer) return;
    pthread_mutex_lock(&g_fxj.m);
    for (int k = 0; k < FXJ_N; k++) {
        FxJob* j = &g_fxj.j[k];
        if (!g_fxj.busy[k] || !j->fenced) continue;
        for (uint32_t f = 0; f < j->nfill; f++)
            if (j->fill[f].buffer == buffer) {
                g_fxj.d->WaitForFences(g_fxj.d->device, 1, &j->fence, VK_TRUE, 2000000000ull);
                break;
            }
    }
    pthread_mutex_unlock(&g_fxj.m);
}
static void fxj_device_gone(DeviceData* d) {
    if (g_fxj.started != 1 || g_fxj.d != d) return;
    pthread_mutex_lock(&g_fxj.m);
    g_fxj.stop = 1;
    pthread_cond_broadcast(&g_fxj.cv);
    pthread_mutex_unlock(&g_fxj.m);
    pthread_join(g_fxj.th, NULL);
    g_fxj.started = 0;
    g_fxj.stop = 0;
    g_fxj.d = NULL;
}

// Bytes per block, block width and copy regions of an image (up to 6 layers and 16 mips); the
// bytes they take, 0 when the format is not exported (*bs 0) or too large.
static uint64_t fxc_image_regions(const ImgInfo* i, VkBufferImageCopy* regs, uint32_t* nreg, uint32_t* bs_out, uint32_t* bw_out) {
    uint32_t bs = 0, bw = 1, f = (uint32_t)i->fmt;
    if (f >= 9 && f <= 15) bs = 1;                     // R8
    else if (f >= 16 && f <= 22) bs = 2;               // R8G8
    else if (f >= 37 && f <= 69) bs = 4;               // RGBA8, BGRA8, packed 32-bit
    else if (f >= 70 && f <= 76) bs = 2;               // R16
    else if (f >= 77 && f <= 83) bs = 4;               // R16G16
    else if (f >= 91 && f <= 97) bs = 8;               // R16G16B16A16
    else if (f >= 98 && f <= 100) bs = 4;              // R32
    else if (f >= 101 && f <= 103) bs = 8;             // R32G32
    else if (f >= 107 && f <= 109) bs = 16;            // R32G32B32A32
    else if (f == 122 || f == 123) bs = 4;             // B10G11R11, E5B9G9R9
    else if (f >= 131 && f <= 134) { bs = 8; bw = 4; }   // BC1
    else if (f >= 135 && f <= 138) { bs = 16; bw = 4; }  // BC2, BC3
    else if (f == 139 || f == 140) { bs = 8; bw = 4; }   // BC4
    else if (f >= 141 && f <= 146) { bs = 16; bw = 4; }  // BC5, BC6H, BC7
    *bs_out = bs;
    *bw_out = bw;
    *nreg = 0;
    if (!bs) return 0;
    int vol = i->type == VK_IMAGE_TYPE_3D;
    uint32_t mips = i->mips ? i->mips : 1, layers = i->layers ? i->layers : 1, depth = vol && i->depth ? i->depth : 1;
    uint64_t total = 0;
    uint32_t n = 0;
    for (uint32_t l = 0; l < layers && l < 6; l++)
        for (uint32_t m = 0; m < mips && m < 16; m++) {
            uint32_t w = i->w >> m ? i->w >> m : 1, h = i->hgt >> m ? i->hgt >> m : 1, z = depth >> m ? depth >> m : 1;
            uint64_t sz = (uint64_t)((w + bw - 1) / bw) * ((h + bw - 1) / bw) * bs * z;
            regs[n++] = (VkBufferImageCopy){total, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1}, {0, 0, 0}, {w, h, z}};
            total += sz;
            total = (total + 15) & ~15ull;
        }
    *nreg = n;
    return total > (256u << 20) ? 0 : total;
}
// FXT1: the image's description and its levels, from its copy at `map`.
static void fxc_image_bytes(Bytes* out, const ImgInfo* i, const VkBufferImageCopy* regs, uint32_t nreg, uint32_t bs, uint32_t bw,
                            const uint8_t* map) {
    int vol = i->type == VK_IMAGE_TYPE_3D;
    uint32_t mips = i->mips ? i->mips : 1, layers = i->layers ? i->layers : 1, depth = vol && i->depth ? i->depth : 1;
    by_put(out, "FXT1", 4);
    by_u32(out, i->fmt); by_u32(out, i->w); by_u32(out, i->hgt); by_u32(out, depth);
    by_u32(out, mips > 16 ? 16 : mips); by_u32(out, layers > 6 ? 6 : layers);
    by_u32(out, vol ? VK_IMAGE_VIEW_TYPE_3D : layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D);
    for (uint32_t k = 0; k < nreg; k++) {
        uint32_t w = regs[k].imageExtent.width, h = regs[k].imageExtent.height, z = regs[k].imageExtent.depth;
        uint64_t sz = (uint64_t)((w + bw - 1) / bw) * ((h + bw - 1) / bw) * bs * z;
        by_put(out, map + regs[k].bufferOffset, sz);
    }
}
// Records the copy of a sampled image (SHADER_READ_ONLY around it) into `cmd`, begun and ended.
static void fxc_image_record(DeviceData* d, VkCommandBuffer cmd, uint64_t image, VkBuffer dst, const VkBufferImageCopy* regs, uint32_t nreg) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, (VkImage)image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    d->CmdCopyImageToBuffer(cmd, (VkImage)image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, nreg, regs);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    d->EndCommandBuffer(cmd);
}

// At present (g_lock held): GPU-only ranges and new textures are read back, the frame written.
// A texture is written once under `key`; key 0 names it by its content instead (images BG3
// renders into, re-read now and then: the grading LUT). Returns the key written, 0 on failure.
static uint64_t fxc_image_export(QueueData* q, uint64_t image, uint64_t key) {
    DeviceData* d = q->dev;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, image, 0);
    if (!i || !d->CmdCopyImageToBuffer) return 0;
    VkBufferImageCopy regs[16 * 6];
    uint32_t nreg = 0, bs = 0, bw = 1;
    uint64_t total = fxc_image_regions(i, regs, &nreg, &bs, &bw);
    if (!bs) {
        logf_("fxcopy: texture format %d not exported", (int)i->fmt);
        if (key) { fxc_write_once("tex", key, ".unsupported", "", 0); unsup_add(key); }
        return 0;
    }
    if (!total) return 0;
    Staging* st = staging_for(d, q->family, total);
    if (!st) return 0;
    d->ResetCommandBuffer(st->cmd, 0);
    fxc_image_record(d, st->cmd, image, st->buffer, regs, nreg);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &st->cmd, 0, NULL};
    d->ResetFences(d->device, 1, &st->fence);
    if (d->QueueSubmit(q->queue, 1, &si, st->fence) != VK_SUCCESS) return 0;
    if (d->WaitForFences(d->device, 1, &st->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return 0;
    Bytes out = {0};
    fxc_image_bytes(&out, i, regs, nreg, bs, bw, st->mapped);
    int fresh = !key;
    if (!key) key = fnv64(out.p, out.n, FNV0) | 4;
    char path[256];
    struct stat stt;
    snprintf(path, sizeof path, "%s/tex/%016llx.bin", fxc_dir(), (unsigned long long)key);
    if (!fresh || stat(path, &stt) != 0) {
        fxc_write_once("tex", key, ".bin", out.p, out.n);
        logf_("fxcopy: texture %016llx (%ux%ux%u format %d, %u mips) exported", (unsigned long long)key, i->w, i->hgt,
              i->type == VK_IMAGE_TYPE_3D && i->depth ? i->depth : 1, (int)i->fmt, i->mips ? i->mips : 1);
    }
    free(out.p);
    return key;
}
// The same export through the worker: the copy is submitted now and the worker writes the file
// under `key`, or by content for a LUT re-read (`which` its g_post_lut slot, fxj_lut_done). The
// frame that names a new texture is queued after it, so benilla never reads one before its
// texture. Returns whether it was submitted (else the caller exports it at the present).
static int fxc_image_export_async(QueueData* q, uint64_t image, int which, uint64_t key) {
    DeviceData* d = q->dev;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, image, 0);
    if (!i || !d->CmdCopyImageToBuffer) return 0;
    if (key) {
        for (uint32_t k = 0; k < g_fxj.npend && k < 64; k++)
            if (g_fxj.pend[k] == key) return 1;  // in flight already
    }
    FxJob* j = fxj_take(q);
    if (!j) return 0;
    uint64_t total = fxc_image_regions(i, j->regs, &j->nreg, &j->bs, &j->bw);
    if (!total || !gbuf_make(d, &j->st, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) { fxj_drop(j); return 0; }
    d->ResetCommandBuffer(j->cmd, 0);
    fxc_image_record(d, j->cmd, image, j->st.buf, j->regs, j->nreg);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &j->cmd, 0, NULL};
    d->ResetFences(d->device, 1, &j->fence);
    if (d->QueueSubmit(q->queue, 1, &si, j->fence) != VK_SUCCESS) { fxj_drop(j); return 0; }
    j->kind = 2;
    j->which = which;
    j->key = key;
    j->image = image;
    j->info = *i;
    if (key) g_fxj.pend[g_fxj.npend++ % 64] = key;
    fxj_submitted(j, 1);
    fxj_queue(j);
    return 1;
}

// GPU-only ranges of this frame's draws into the blob (cached static data first). With `job`
// the copy is submitted and the worker fills the frame file; without, it is waited for here.
static void fxc_gpu_fill(QueueData* q, FxJob* job) {
    DeviceData* d = q->dev;
    uint64_t total = 0;
    pthread_mutex_lock(&g_fxj_data);
    for (uint32_t k = 0; k < g_fxc.ngpu; k++) {
        GpuReq* r = &g_fxc.gpu[k];
        uint64_t ck = fnv64(r, 24, FNV0) | 1;
        GpuCache* c = &g_gpuc[ck % GPUC_CAP];
        if (c->h == ck && c->data) { memcpy(g_fxc.blob.p + r->blob, c->data, r->size); r->size = 0; continue; }
        total += (r->size + 15) & ~15ull;
    }
    pthread_mutex_unlock(&g_fxj_data);
    for (uint32_t k = 0; k < g_nsimg; k++) total += (g_simg[k].size + 15) & ~15ull;  // BG3's sight textures
    if (!total || !d->CmdCopyBuffer) return;
    if (total > (64u << 20)) { logf_("fxcopy: %llu bytes of GPU data, skipped", (unsigned long long)total); return; }
    VkCommandBuffer cmd;
    VkBuffer dst;
    VkFence fence;
    Staging* st = NULL;
    if (job) {
        if (!gbuf_make(d, &job->st, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return;
        cmd = job->cmd; dst = job->st.buf; fence = job->fence;
    } else {
        st = staging_for(d, q->family, total);
        if (!st) return;
        cmd = st->cmd; dst = st->buffer; fence = st->fence;
    }
    pthread_mutex_lock(&g_trace_lock);
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    uint64_t at = 0;
    for (uint32_t k = 0; k < g_fxc.ngpu; k++) {
        GpuReq* r = &g_fxc.gpu[k];
        if (!r->size) continue;
        BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, r->buffer, 0);
        if (!b || !b->size || r->off + r->size > b->size) { r->size = 0; continue; }
        VkBufferCopy rg = {r->off, at, r->size};
        d->CmdCopyBuffer(cmd, (VkBuffer)r->buffer, dst, 1, &rg);
        if (job) {
            if (job->nfill == job->capfill) {
                uint32_t c = job->capfill ? job->capfill * 2 : 64;
                FxFill* f = realloc(job->fill, c * sizeof *f);
                if (!f) { r->size = 0; continue; }
                job->fill = f; job->capfill = c;
            }
            job->fill[job->nfill++] = (FxFill){r->blob, !(b->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && r->size <= (4u << 20), at, r->size,
                                               fnv64(r, 24, FNV0) | 1, r->buffer};
        }
        at += (r->size + 15) & ~15ull;
    }
    for (uint32_t k = 0; k < g_nsimg; k++) {  // the sight textures, after the ranges
        SightImg* s = &g_simg[k];
        if (!TABLE_FIND(g_img, IMG_CAP, s->image, 0)) { s->size = 0; continue; }
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                  VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)s->image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rg = s->reg;
        rg.bufferOffset = at;
        d->CmdCopyImageToBuffer(cmd, (VkImage)s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &rg);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        if (job) {
            if (job->nfill == job->capfill) {
                uint32_t c = job->capfill ? job->capfill * 2 : 64;
                FxFill* f = realloc(job->fill, c * sizeof *f);
                if (!f) { s->size = 0; continue; }
                job->fill = f; job->capfill = c;
            }
            job->fill[job->nfill++] = (FxFill){s->blob + 32, 0, at, s->size, 0, 0};
        }
        at += (s->size + 15) & ~15ull;
    }
    d->EndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    d->ResetFences(d->device, 1, &fence);
    if (job) {
        fxj_submitted(job, d->QueueSubmit(q->queue, 1, &si, fence) == VK_SUCCESS);
        pthread_mutex_unlock(&g_trace_lock);
        return;
    }
    int ok = d->QueueSubmit(q->queue, 1, &si, fence) == VK_SUCCESS &&
             d->WaitForFences(d->device, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
    pthread_mutex_unlock(&g_trace_lock);
    if (!ok) return;
    at = 0;
    for (uint32_t k = 0; k < g_fxc.ngpu; k++) {
        GpuReq* r = &g_fxc.gpu[k];
        if (!r->size) continue;
        const uint8_t* src = (const uint8_t*)st->mapped + at;
        memcpy(g_fxc.blob.p + r->blob, src, r->size);
        // Static data (not written by shaders) is kept for the next frames.
        BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, r->buffer, 0);
        if (b && !(b->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && r->size <= (4u << 20)) {
            uint64_t ck = fnv64(r, 24, FNV0) | 1;
            uint8_t* copy = malloc(r->size);
            if (copy) {
                memcpy(copy, src, r->size);
                pthread_mutex_lock(&g_fxj_data);
                GpuCache* c = &g_gpuc[ck % GPUC_CAP];
                free(c->data); c->h = ck; c->data = copy;
                pthread_mutex_unlock(&g_fxj_data);
            }
        }
        at += (r->size + 15) & ~15ull;
    }
    for (uint32_t k = 0; k < g_nsimg; k++) {
        if (!g_simg[k].size) continue;
        memcpy(g_fxc.blob.p + g_simg[k].blob + 32, (const uint8_t*)st->mapped + at, g_simg[k].size);
        at += (g_simg[k].size + 15) & ~15ull;
    }
}

// ---- BG3's post-processing after the effects, for the replay's colour parity. Traced on
// (request "trace"): after the effects pass BG3
//   1. multiplies the scene by its exposure (two GPU-written storage values, POST_EXPOSURE),
//   2. TAA, then bloom: thresholded downsample (POST_BLOOM_IN), blur chain, weighted upsamples
//      (POST_BLOOM_UP), scene + 0.2 * bloom,
//   3. grades (POST_GRADE): HSV adjustments, x / 222, PQ (ST 2084) encoding, a 32^3 LUT,
//   4. maps to display values (POST_DISPLAY, compute): ACEScc encoding, a 3D LUT, fade to black,
//      into a 10-bit UNORM image (display-encoded), then FSR1 upscaling and the UI.
// The shaders are told apart by their SPIR-V keys (content hashes, stable for a game build). Each
// frame their parameters go into the frame file's post block ("FXQ1", fx-replay-format.md).
#define POST_EXPOSURE 0xe7a0be45256eff51ull
#define POST_BLOOM_IN 0xa36299ac206b341bull
#define POST_BLOOM_UP 0x607c487692650814ull
#define POST_GRADE 0xd58487d054d6d0b9ull
#define POST_DISPLAY 0x35d9685ac5afceceull
#define POST_LUT_EVERY 60  // presents between re-reads of a LUT BG3 renders (grading blends by area)

static struct {
    uint32_t have;  // 1 exposure, 2 bloom in, 4 bloom up, 8 grade, 16 display
    uint32_t exp_blob[2];
    uint8_t bloom_in[16], bloom_up[16], grade[16], display[32];
    uint64_t grade_lut, grade_samp, display_lut, display_samp;  // images (resolved to keys at present)
} g_post;
static struct { uint64_t image, key; uint32_t frame; } g_post_lut[2];
static __thread uint64_t t_post_pipe, t_post_key, t_post_cpipe, t_post_ckey;

// A bound slot of `sets` (dynamic offsets applied), or NULL.
static const DescSlot* post_slot(const uint64_t* sets, uint32_t dyn[8][8], const uint32_t* ndyn, uint32_t set, uint32_t binding, uint64_t* off) {
    if (set >= 8 || binding >= DESC_SLOTS || !sets[set]) return NULL;
    SetInfo* si = set_find(sets[set]);
    if (!si || (!si->slot[binding].res && !si->slot[binding].samp)) return NULL;
    DescSlot* sl = &si->slot[binding];
    *off = sl->off;
    if (sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
        uint32_t at = 0;
        for (uint32_t b = 0; b < binding; b++)
            if (si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) at++;
        if (at < ndyn[set] && at < 8) *off += dyn[set][at];
    }
    return sl;
}
// Copies `n` mapped bytes of a uniform slot into `out`; 0 when not mapped.
static int post_bytes(const uint64_t* sets, uint32_t dyn[8][8], const uint32_t* ndyn, uint32_t set, uint32_t binding, void* out, uint32_t n) {
    uint64_t off = 0, left = 0;
    const DescSlot* sl = post_slot(sets, dyn, ndyn, set, binding, &off);
    const uint8_t* p = sl ? buf_cpu(sl->res, off, &left) : NULL;
    if (!p || left < n) return 0;
    memcpy(out, p, n);
    return 1;
}
static uint64_t post_image(const uint64_t* sets, uint32_t set, uint32_t binding) {
    uint64_t off = 0;
    const DescSlot* sl = post_slot(sets, NULL, (const uint32_t[8]){0}, set, binding, &off);
    return sl && sl->res ? view_image(sl->res) : 0;
}
static uint64_t post_sampler(const uint64_t* sets, uint32_t set, uint32_t binding) {
    uint64_t off = 0;
    const DescSlot* sl = post_slot(sets, NULL, (const uint32_t[8]){0}, set, binding, &off);
    return sl && sl->samp ? sampler_key(sl->samp) : 0;  // 0: immutable or unknown (linear, clamp)
}

// ---- Research (request "sight<N>"): BG3's sight overlay (EoCSight.bshd: a full-screen
// draw over the lit scene that tints where enemies see, their vision cones, and where it is dark).
// Logs its constants (set 1 binding 0), the eye, the view rays and the images it reads to sight.txt.
#define SIGHT_FS 0xf9ac3c52a928d4f4ull
static void sight_note(void) {  // g_fxl_lock held
    if (g_sight_lines <= 0) return;
    float c[24] = {0}, v[56] = {0}, b1[40] = {0};
    int okc = post_bytes(t_set, t_dyn, t_ndyn, 1, 0, c, 96);
    int okv = post_bytes(t_set, t_dyn, t_ndyn, 0, 2, v, 224);
    int okr = post_bytes(t_set, t_dyn, t_ndyn, 0, 1, b1, 160);
    FILE* f = fopen(outp("/sight.txt"), "a");
    if (!f) { g_sight_lines = 0; return; }
    fprintf(f, "sight %d %d%d%d c", g_sight_lines, okc, okv, okr);
    if (!okc) {  // why set 1 binding 0 was not read (release batch: through the plugin)
        SetInfo* si = set_find(t_set[1]);
        const DescSlot* sl = si ? &si->slot[0] : NULL;
        BufInfo* bi = (sl && sl->res && g_buf) ? TABLE_FIND(g_buf, BUF_CAP, sl->res, 0) : NULL;
        MemInfo* mi = (bi && bi->mem && g_mem) ? TABLE_FIND(g_mem, MEM_CAP, bi->mem, 0) : NULL;
        uint64_t left = 0;
        const uint8_t* rp = (sl && sl->res) ? buf_cpu_recovered(sl->res, sl->off, &left) : NULL;
        fprintf(f, " [why: set %llx %s, slot type %d res %llx off %llu, buffer %s size %llu mem %llx %s, driver read %s %llu]",
                (unsigned long long)t_set[1], si ? "known" : "unknown", sl ? sl->type : -1, sl ? (unsigned long long)sl->res : 0ull,
                sl ? (unsigned long long)sl->off : 0ull, bi ? "known" : "unknown", bi ? (unsigned long long)bi->size : 0ull,
                bi ? (unsigned long long)bi->mem : 0ull, mi ? (mi->ptr ? "mapped" : "not mapped") : "memory unknown",
                rp ? "ok" : (g_buf_recover ? "failed" : "not ready"), (unsigned long long)left);
    }
    for (int i = 0; i < 24; i++) fprintf(f, " %.6g", c[i]);
    fprintf(f, " | eye %.3f %.3f %.3f near %g far %g P00 %g P11 %g | axes", v[52], v[53], v[54], v[48], v[49], v[16], v[21]);
    for (int i = 12; i < 24; i++) fprintf(f, " %.4g", b1[i]);
    fprintf(f, " | rays");
    for (int i = 28; i < 40; i++) fprintf(f, " %.4g", b1[i]);
    fprintf(f, " |");
    for (uint32_t b = 1; b <= 12; b++) {
        uint64_t img = post_image(t_set, 1, b);
        ImgInfo* i = img && g_img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
        if (i) fprintf(f, " b%u=i%u:f%d:%ux%u", b, i->id, (int)i->fmt, i->w, i->hgt);
        else if (img) fprintf(f, " b%u=?", b);
    }
    fprintf(f, "\n");
    fclose(f);
    g_sight_lines--;
}

static void fxp_draw(VkCommandBuffer cb) {
    if (!g_fxc_on || t_bind_cb != cb || !t_pipe) return;
    if (t_post_pipe != t_pipe) {
        t_post_pipe = t_pipe; t_post_key = 0;
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        for (uint32_t s = 0; ri && ri->r && s < ri->r->nstages; s++)
            if (ri->r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) t_post_key = ri->r->st[s].key;
    }
    uint64_t k = t_post_key;
    if (k == SIGHT_FS && g_sight_lines > 0) { pthread_mutex_lock(&g_fxl_lock); sight_note(); pthread_mutex_unlock(&g_fxl_lock); }
    if (k != POST_EXPOSURE && k != POST_BLOOM_IN && k != POST_BLOOM_UP && k != POST_GRADE) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (k == POST_EXPOSURE) {
        uint64_t o0 = 0, o1 = 0;
        const DescSlot* a = post_slot(t_set, t_dyn, t_ndyn, 1, 0, &o0);
        const DescSlot* b = post_slot(t_set, t_dyn, t_ndyn, 1, 1, &o1);
        if (a && b) {
            g_post.exp_blob[0] = fxc_range_raw(a->res, o0, 16);
            g_post.exp_blob[1] = fxc_range_raw(b->res, o1, 16);
            g_post.have |= 1;
        }
    } else if (k == POST_BLOOM_IN && !(g_post.have & 2)) {
        if (post_bytes(t_set, t_dyn, t_ndyn, 1, 0, g_post.bloom_in, 16)) g_post.have |= 2;
    } else if (k == POST_BLOOM_UP && !(g_post.have & 4)) {
        if (post_bytes(t_set, t_dyn, t_ndyn, 1, 0, g_post.bloom_up, 16)) g_post.have |= 4;
    } else if (k == POST_GRADE) {
        if (post_bytes(t_set, t_dyn, t_ndyn, 1, 0, g_post.grade, 16)) {
            g_post.grade_lut = post_image(t_set, 1, 2);
            g_post.grade_samp = post_sampler(t_set, 1, 3);
            g_post.have |= 8;
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
}
static void fxp_dispatch(VkCommandBuffer cb) {
    if (!g_fxc_on || t_cbind_cb != cb || !t_cpipe) return;
    if (t_post_cpipe != t_cpipe) {
        t_post_cpipe = t_cpipe;
        CPipeInfo* p = g_cpipe ? TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0) : NULL;
        t_post_ckey = p ? p->spv : 0;
    }
    if (t_post_ckey != POST_DISPLAY) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (post_bytes(t_cset, t_cdyn, t_cndyn, 1, 4, g_post.display, 32)) {
        g_post.display_lut = post_image(t_cset, 1, 1);
        g_post.display_samp = post_sampler(t_cset, 1, 2);
        g_post.have |= 16;
    }
    pthread_mutex_unlock(&g_fxl_lock);
}

// The texture key of a post LUT: static textures once; images BG3 renders re-read every
// POST_LUT_EVERY presents and keyed by content. g_fxl_lock held.
static uint64_t post_lut_key(QueueData* q, int which, uint64_t image) {
    if (!image) return 0;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, image, 0);
    if (!i) return 0;
    int rt = (i->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) != 0;
    pthread_mutex_lock(&g_fxj_data);
    uint64_t had = g_post_lut[which].image, had_key = g_post_lut[which].key;
    uint32_t had_frame = g_post_lut[which].frame;
    pthread_mutex_unlock(&g_fxj_data);
    if (had == image && had_key && (!rt || g_fxc.frame - had_frame < POST_LUT_EVERY)) return had_key;
    // A re-read of a LUT BG3 renders goes through the worker; the last key serves meanwhile.
    if (rt && had == image && had_key && !g_cc.fxsync) {
        pthread_mutex_lock(&g_fxj_data);
        g_post_lut[which].frame = g_fxc.frame;
        pthread_mutex_unlock(&g_fxj_data);
        fxc_image_export_async(q, image, which, 0);
        return had_key;
    }
    uint64_t key = rt ? 0 : fnv64(&image, 8, fnv64(&i->id, 4, FNV0)) | 4;
    char path[256];
    struct stat st;
    snprintf(path, sizeof path, "%s/tex/%016llx.bin", fxc_dir(), (unsigned long long)key);
    if (rt || stat(path, &st) != 0) key = fxc_image_export(q, image, key);
    if (!key) return had_key;
    pthread_mutex_lock(&g_fxj_data);
    g_post_lut[which].image = image; g_post_lut[which].key = key; g_post_lut[which].frame = g_fxc.frame;
    pthread_mutex_unlock(&g_fxj_data);
    return key;
}
// The worker finished a LUT re-read (fxc_image_export_async).
static void fxj_lut_done(int which, uint64_t image, uint64_t key) {
    pthread_mutex_lock(&g_fxj_data);
    if (g_post_lut[which].image == image) g_post_lut[which].key = key;
    pthread_mutex_unlock(&g_fxj_data);
}

static int pace_present_cam(uint8_t out[96]);  // wowbg3_pace.inc.c: the camera record, read once per present
// The post block (after the blob) of this frame's file; returns its size. g_fxl_lock held, after
// fxc_gpu_fill.
static void post_block(QueueData* q, Bytes* out) {
    float ea = 0, eb = 0;
    if (g_post.have & 1) {
        memcpy(&ea, g_fxc.blob.p + g_post.exp_blob[0] + 4, 4);
        memcpy(&eb, g_fxc.blob.p + g_post.exp_blob[1], 4);
    }
    by_put(out, "FXQ1", 4); by_u32(out, g_post.have);
    by_f32(out, ea); by_f32(out, eb);
    by_put(out, g_post.bloom_in, 16); by_put(out, g_post.bloom_up, 16); by_put(out, g_post.grade, 16);
    by_u64(out, post_lut_key(q, 0, g_post.grade_lut)); by_u64(out, g_post.grade_samp);
    by_put(out, g_post.display, 32);
    by_u64(out, post_lut_key(q, 1, g_post.display_lut)); by_u64(out, g_post.display_samp);
    // The plugin's camera record (bg3cam.bin) sequence read at this present: pairs the frame with
    // the BG3 camera pose benilla follows.
    uint8_t cam[96];
    uint32_t cseq = 0;
    if (pace_present_cam(cam)) memcpy(&cseq, cam + 4, 4);  // the record this present's packet carries
    by_u32(out, cseq);
    static int said;
    if (!said && (g_post.have & 31) == 31) {
        said = 1;
        float g[4], th[2];
        memcpy(g, g_post.grade, 16); memcpy(th, g_post.bloom_in, 8);
        logf_("fxcopy: post: exposure %g x %g, bloom threshold %g scale %g, grade %g %g %g %g", ea, eb, th[0], th[1], g[0], g[1], g[2], g[3]);
    }
}

// Research (request "tavlog"): BG3's own draws of Tav (the ones world mode hides) with their
// shaders and every bound resource, to see what her materials read (virtual textures?).
static FILE* g_tavlog_f;
static void tav_log_draw(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_fxl_lock);
    if (!g_tavlog_f) g_tavlog_f = fopen(outp("/tavlog.txt"), "w");
    FILE* f = g_tavlog_f;
    if (f && t_bind_cb == cb) {
        RecInfo* ri = (g_rec && t_pipe) ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* r = ri ? ri->r : NULL;
        fprintf(f, "DRAW ib %llx pipe %llx recipe %016llx", (unsigned long long)t_ib, (unsigned long long)t_pipe,
                (unsigned long long)recipe_key(t_pipe));
        for (uint32_t k = 0; r && k < r->nstages; k++) fprintf(f, " st%x:%016llx", r->st[k].stage, (unsigned long long)r->st[k].key);
        for (uint32_t k = 0; r && k < r->ncolor; k++) fprintf(f, " c%u:f%u", k, r->blend[k][8]);
        fprintf(f, "\n");
        for (int s = 0; s < 8; s++) {
            SetInfo* si = set_find(t_set[s]);
            if (!si) continue;
            fprintf(f, "  set%d", s);
            for (uint32_t b = 0; b < DESC_SLOTS; b++) {
                DescSlot* sl = &si->slot[b];
                if (!sl->res && !sl->samp) continue;
                if (sl->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    sl->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    char dsc[96];
                    img_desc(dsc, sizeof dsc, view_image(sl->res));
                    fprintf(f, " b%u:t%u:%s", b, sl->type, dsc);
                } else if (sl->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    fprintf(f, " b%u:samp", b);
                } else {
                    // Items 11 and 12 (BG3's cloth): a buffer's handle, offset and range, and for the
                    // engine constant buffer (set 1 binding 0 by every character shader's desc) its
                    // first words, where `TransformedVerticesOffsetCurr` says the mesh's offset into
                    // the transformed-vertices pool sits (set 0 binding 13, the one buffer every
                    // character shader reads its posed vertices from).
                    BufInfo* bi = g_buf ? TABLE_FIND(g_buf, BUF_CAP, sl->res, 0) : NULL;
                    fprintf(f, " b%u:t%u:buf %llx+%llx:%llx(size %llx)", b, sl->type, (unsigned long long)sl->res,
                            (unsigned long long)sl->off, (unsigned long long)sl->range,
                            (unsigned long long)(bi ? bi->size : 0));
                    if (s == 1 && b == 0) {
                        uint64_t left = 0;
                        const uint8_t* cpu = buf_cpu(sl->res, sl->off, &left);
                        if (cpu && left >= 112) {
                            fprintf(f, " cb[96..112]=");
                            for (int o = 96; o < 112; o += 4) {
                                uint32_t v;
                                memcpy(&v, cpu + o, 4);
                                fprintf(f, "%08x ", v);
                            }
                        } else {
                            fprintf(f, " cb(no cpu mapping, left %llu)", (unsigned long long)left);
                        }
                    }
                }
            }
            fprintf(f, "\n");
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
}
static void tav_log_present(void) {
    if (g_tavlog <= 0) return;
    if (--g_tavlog > 0) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (g_tavlog_f) { fclose(g_tavlog_f); g_tavlog_f = NULL; logf_("tavlog: written"); }
    pthread_mutex_unlock(&g_fxl_lock);
}

// ---- BG3's map off (world mode, compose word "void"). Goal: Tav lives in an
// empty void; BG3's level must not show. In passes BG3's map draws into (the scene's depth: G-buffer,
// lighting, the effects pass; depth-only passes: shadow maps), an indexed draw is skipped unless its
// index buffer belongs to a character near Tav (WorldMode.lua lists them in link/keep.txt, every LOD)
// or, in the effects pass, it is one of BG3's effects near Tav (the capture's own test). Full-screen
// passes, post-processing and the UI are untouched, so BG3 shows its characters and effects over its
// sky, and the effects no longer pick up the level's light and haze. The level stays loaded.
#define KEEP_CAP 8192
static uint64_t g_keep[KEEP_CAP];  // index buffers (VkBuffer handles), open addressing, 0 empty
static uint32_t g_keep_n;
static int g_keep_active;  // world mode is running ("KEEP 1"); "KEEP 0" or no file: the map stays
static struct timespec g_keep_seen;

static int keep_has(uint64_t ib) {
    if (!g_keep_n || !ib) return 0;
    for (uint32_t i = hslot(ib, KEEP_CAP), n = 0; n < KEEP_CAP; n++, i = (i + 1) & (KEEP_CAP - 1)) {
        if (g_keep[i] == ib) return 1;
        if (!g_keep[i]) return 0;
    }
    return 0;
}
static void keep_read(void) {
    if (!g_tav.init || !g_tav.plan[0]) return;
    char path[sizeof g_tav.plan];
    snprintf(path, sizeof path, "%s", g_tav.plan);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "keep.txt");
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtim.tv_sec == g_keep_seen.tv_sec && st.st_mtim.tv_nsec == g_keep_seen.tv_nsec) return;
    if (st.st_mtim.tv_sec < g_tav.born.tv_sec) return;  // left over from an earlier BG3 run
    FILE* f = fopen(path, "r");
    if (!f) return;
    g_keep_seen = st.st_mtim;
    static uint64_t next[KEEP_CAP];
    memset(next, 0, sizeof next);
    uint32_t n = 0;
    char line[64];
    if (!fgets(line, sizeof line, f) || strncmp(line, "KEEP ", 5)) { fclose(f); return; }
    int active = line[5] == '1';
    while (fgets(line, sizeof line, f) && n < KEEP_CAP / 2) {
        uint64_t ib = strtoull(line, NULL, 16);
        if (!ib) continue;
        uint32_t i = hslot(ib, KEEP_CAP);
        while (next[i] && next[i] != ib) i = (i + 1) & (KEEP_CAP - 1);
        if (!next[i]) { next[i] = ib; n++; }
    }
    fclose(f);
    pthread_mutex_lock(&g_fxl_lock);
    memcpy(g_keep, next, sizeof g_keep);
    g_keep_n = n;
    if (active != g_keep_active) logf_("void: BG3's map %s (%u character index buffers kept)", active ? "off" : "back", n);
    g_keep_active = active;
    pthread_mutex_unlock(&g_fxl_lock);
}

// The other characters near Tav (link/hide.txt, TavPlan.PublishKeep): the stand-ins and NPCs whose
// BG3 bodies the void hides because WoW draws them. Read so "fxprops" never keeps them: a stand-in
// that has just spawned has young mesh slices exactly like a spell's prop (the blob is skinned too),
// and without this its BG3 body would be drawn over WoW's mob for the prop age (30 s). Characters have
// Health and spell props do not, so the mod can tell them apart exactly; the layer cannot.
static uint64_t g_hide[KEEP_CAP];
static uint32_t g_hide_n;
static uint64_t g_hide_at;  // the present the last list was read at
static struct timespec g_hide_seen;
static int hide_has(uint64_t ib) {
    if (!g_hide_n || !ib) return 0;
    for (uint32_t i = hslot(ib, KEEP_CAP), n = 0; n < KEEP_CAP; n++, i = (i + 1) & (KEEP_CAP - 1)) {
        if (g_hide[i] == ib) return 1;
        if (!g_hide[i]) return 0;
    }
    return 0;
}
static void hide_read(void) {
    if (!g_tav.init || !g_tav.plan[0]) return;
    char path[sizeof g_tav.plan];
    snprintf(path, sizeof path, "%s", g_tav.plan);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "hide.txt");
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtim.tv_sec == g_hide_seen.tv_sec && st.st_mtim.tv_nsec == g_hide_seen.tv_nsec) return;
    if (st.st_mtim.tv_sec < g_tav.born.tv_sec) return;  // left over from an earlier BG3 run
    FILE* f = fopen(path, "r");
    if (!f) return;
    g_hide_seen = st.st_mtim;
    static uint64_t next[KEEP_CAP];
    memset(next, 0, sizeof next);
    uint32_t n = 0;
    char line[64];
    if (!fgets(line, sizeof line, f) || strncmp(line, "HIDE ", 5)) { fclose(f); return; }
    while (fgets(line, sizeof line, f) && n < KEEP_CAP / 2) {
        uint64_t ib = strtoull(line, NULL, 16);
        if (!ib) continue;
        uint32_t i = hslot(ib, KEEP_CAP);
        while (next[i] && next[i] != ib) i = (i + 1) & (KEEP_CAP - 1);
        if (!next[i]) { next[i] = ib; n++; }
    }
    fclose(f);
    pthread_mutex_lock(&g_fxl_lock);
    memcpy(g_hide, next, sizeof g_hide);
    g_hide_n = n;
    pthread_mutex_unlock(&g_fxl_lock);
    __atomic_store_n(&g_hide_at, __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
}

// A pass begins (g_trace_lock held): is it one BG3's map draws into?
// Mesh slices: index data drawn in BG3's G-buffer pass (the scene depth and five targets) belongs to
// characters and the level.
// Placed by their object matrices, the blended parts of such meshes (fur, hair
// edges of the WoW mobs' BG3 stand-ins, which WoW draws itself) would otherwise pass as effects
// near Tav. Two insert-only tables, each cleared every MESH_EVERY presents in turn, so a buffer
// stays known while it is drawn and a recycled handle is forgotten within 2 * MESH_EVERY.
// Keyed by the slice a draw reads (ib_slice, as the prop ages below), not the buffer: BG3
// packs many meshes into one index buffer and recycles them, and an effect drawn from a buffer that
// also held a mesh was taken for that mesh and voided -- the 399-spell sweep lost Ray of Sickness's
// beam (346 draws 7 m from Tav), Lightning Bolt's, Gust of Wind's and Eyebite's pieces that way. A
// fur or hair layer redraws its body's own slice, so it is still caught; the stand-ins' own buffers
// (the hide list) count as meshes whole, as before, so a layer they draw only blended stays WoW's.
// The key also holds the vertex data bound at binding 0 (buffer and offset): every particle quad of
// every spell reads the same six indices, so by the index slice alone one effect's quad drawn in a
// mesh pass made all of them meshes (the second sweep: Ray of Sickness, Ray of Frost, Scorching Ray,
// Witch Bolt and Ice Knife still lost 44 to 195 draws each). And only the G-buffer pass registers
// meshes now: every character and level mesh is drawn there, while the depth-only passes (the
// prepass, the shadow maps) carry a spell's own particles and props as well.
#define MESH_CAP 16384u
#define MESH_EVERY 300u
static uint64_t g_mesh[2][MESH_CAP];
static uint64_t g_mesh_full;  // slices past the probe limit (then taken for an effect, not a mesh)
static __thread VkCommandBuffer t_mesh_cb;
static void mesh_add(uint64_t ib) {
    for (int t = 0; t < 2; t++) {
        uint32_t i = hslot(ib, MESH_CAP);
        for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (MESH_CAP - 1)) {
            uint64_t cur = __atomic_load_n(&g_mesh[t][i], __ATOMIC_RELAXED);
            if (cur == ib) break;
            if (!cur) {
                uint64_t zero = 0;
                if (__atomic_compare_exchange_n(&g_mesh[t][i], &zero, ib, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || zero == ib) break;
            }
            if (n == 63) __atomic_add_fetch(&g_mesh_full, 1, __ATOMIC_RELAXED);
        }
    }
}
static int mesh_has(uint64_t ib) {
    if (!ib) return 0;
    for (int t = 0; t < 2; t++) {
        uint32_t i = hslot(ib, MESH_CAP);
        for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (MESH_CAP - 1)) {
            uint64_t cur = __atomic_load_n(&g_mesh[t][i], __ATOMIC_RELAXED);
            if (cur == ib) return 1;
            if (!cur) break;
        }
    }
    return 0;
}
// Word "fxprops": a spell's 3D props kept in the void. Grease's blobs -- the lumps
// that pop during the cast animation and cast a shadow -- are ordinary opaque geometry: one mesh
// drawn in BG3's depth prepass, its G-buffer and its normal pass, every draw of it thrown away by
// `void_skip` because it is not on the keep list and not an effects-pass draw. Measured by grouping
// fxwhy draws by index buffer (word "fxwib") within 20 m of Tav: a cast adds exactly one new mesh.
// Proximity cannot tell a prop from the level (opaque and near Tav too), and skinning cannot tell it
// from a character (the blob is skinned, skin1/skin2, exactly like the stand-ins the void hides).
// Age can: a prop is made when the spell is cast. So a draw is kept while its mesh slice is younger
// than `fxpropage` presents (900, about 30 s), with two guards against things that are also young:
//  - after a load (BG3 leaves and re-enters Running) every slice is new: slices first seen within
//    IBAGE_WARM presents of it count as scenery;
//  - a character is young too when it spawns, changes form or comes back into view, and its draws look
//    like a skinned prop's. BG3 knows which is which: a character has Health, and TavPlan.PublishKeep
//    names every one around Tav in hide.txt. The only gap is the moments before the next list, so a
//    young skinned slice waits for a list read IBAGE_HIDE_AFTER presents after it was first seen, and
//    asks for one (hide-ask.txt); a slice the list does not name is the spell's. Level geometry that
//    streams in is kept out by the distance.
// The identity is the slice a draw reads -- buffer, byte offset, first index, index count and vertex
// offset -- not the buffer: BG3 recycles index buffers, and an age keyed on the handle called the
// blob's mesh old; the vertex offset too, as meshes packed into one buffer can share an index range.
//
// Two generations, as `g_mesh`: every lookup writes the slice's first present into both, and one is
// cleared every IBAGE_EVERY presents in turn, so a slice that keeps being drawn keeps its age for
// ever while a table that only grew (the first version: insert-only, 4096 slices, after which no new
// prop was ever kept) cannot fill. Lock-free (void_skip runs on BG3's recording threads in
// parallel): `h` by compare-and-swap, `first` stored by the thread that claimed the slot.
//
// Age since the slice last came back, not since it was first ever seen (the 399-spell sweep, second
// night): BG3 reuses one debris or shard mesh across spells -- the same chunk, the same slice, drawn
// instanced -- so after one spell had used it the next spell's identical chunk was "older than 30 s"
// and culled; which spells lost theirs depended on what was cast before. `last` is the present the
// slice was last drawn; one that comes back after IBAGE_GAP presents without a draw is made again, so
// its age starts over. Scenery coming back into view is the same event as before, and the same guards
// keep it out: placed near Tav, and a character's slices are on the hide list.
#define IBAGE_GAP 60u
// And only while a spell is on (a quiet-baseline check): with
// ages that start over, arriving somewhere made the things around Tav young -- at Razor Hill's inn an
// unskinned 21600-index mesh 11 m off was kept every frame with no cast, which the old binary culled.
// A spell that makes a prop also draws effects, so a slice is made only within IBAGE_FX presents of an
// effects-pass draw near Tav (fxc_draw); otherwise it counts as scenery until it next comes back. A
// slice seen first with no spell on waits IBAGE_PENDING presents (marked IBAGE_PEND, not kept): BG3
// draws the G-buffer before the effects, so a prop can come a frame or two before its spell's first
// effect; if one comes in that time the slice was made when first seen.
#define IBAGE_FX 300u
#define IBAGE_PENDING 30u
#define IBAGE_PEND (1ull << 63)
#define IBAGE_CAP 16384u
#define IBAGE_EVERY 3600u
#define IBAGE_WARM 300u
#define IBAGE_HIDE_AFTER 2u   // presents after a slice's first draw: a list read then was made after it
#define IBAGE_HIDE_ASK_GAP 8u  // presents between two asks for a list
typedef struct { uint64_t h; uint64_t first; uint64_t last; } IbAge;  // first, last: present + 1
static IbAge g_ibage[2][IBAGE_CAP];
static uint64_t g_ibage_warm;                 // slices first seen before this present are scenery
static uint64_t g_ibage_new_at, g_ibage_new_n;  // new slices counted for one present
static uint64_t g_fxprops_kept, g_fxprops_wait, g_ibage_full, g_ibage_newmax;
static uint32_t g_hide_ask;  // a new skinned slice wants a list made after it
static uint64_t ib_slice(uint64_t ib, uint64_t off, uint32_t first, uint32_t count, int32_t vo) {
    uint64_t k[4] = {ib, off, ((uint64_t)first << 32) | count, (uint64_t)(uint32_t)vo};
    return fnv64(k, sizeof k, FNV0) | 1;
}
// The slice's entry in table t, NULL when absent.
static IbAge* ibage_get(IbAge* t, uint64_t k) {
    uint32_t i = hslot(k, IBAGE_CAP);
    for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (IBAGE_CAP - 1)) {
        uint64_t h = __atomic_load_n(&t[i].h, __ATOMIC_ACQUIRE);
        if (h == k) return &t[i];
        if (!h) return NULL;
    }
    return NULL;
}
static void ibage_put(IbAge* t, uint64_t k, uint64_t first, uint64_t last) {
    uint32_t i = hslot(k, IBAGE_CAP);
    for (uint32_t n = 0; n < 64; n++, i = (i + 1) & (IBAGE_CAP - 1)) {
        uint64_t h = __atomic_load_n(&t[i].h, __ATOMIC_ACQUIRE);
        if (!h) {
            uint64_t zero = 0;
            if (__atomic_compare_exchange_n(&t[i].h, &zero, k, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&t[i].last, last, __ATOMIC_RELAXED);
                __atomic_store_n(&t[i].first, first, __ATOMIC_RELEASE);  // overwrites a cleared slot's stale value
                return;
            }
            h = zero;
        }
        if (h == k) {
            uint64_t z = 0;
            __atomic_compare_exchange_n(&t[i].first, &z, first, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            __atomic_store_n(&t[i].last, last, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_add_fetch(&g_ibage_full, 1, __ATOMIC_RELAXED);  // probe limit: the slice stays unaged (not kept)
}
// A spell's prop: a slice first seen (or back after a gap) within the last `fxpropage` presents, outside
// a load's warm-up. A skinned one (`skinned`) only once a list made after its first draw has been read, so the
// caller's hide test can answer for it: a body is on that list, a spell's mesh is not. This replaces a limit of
// 8 new skinned slices a present, which threw out every slice of such a present, the spell's too, counted
// bodies already listed or a kilometre off towards it, and said so for every later draw of them (the 51528 of
// the ship sweep is one body's draws, 172 a present, not as many lost effects).
static __thread int t_ib_aged;  // this slice passed fxpropage just now (a log line in void_skip)
static int ib_young(uint64_t k, int skinned) {
    t_ib_aged = 0;
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    IbAge *ea = ibage_get(g_ibage[0], k), *eb = ibage_get(g_ibage[1], k);
    uint64_t a = ea ? __atomic_load_n(&ea->first, __ATOMIC_ACQUIRE) : 0, b = eb ? __atomic_load_n(&eb->first, __ATOMIC_ACQUIRE) : 0;
    uint64_t la = ea ? __atomic_load_n(&ea->last, __ATOMIC_RELAXED) : 0, lb = eb ? __atomic_load_n(&eb->last, __ATOMIC_RELAXED) : 0;
    uint64_t f = a && b ? (a < b ? a : b) : (a ? a : b), l = la > lb ? la : lb;
    if (f && l && now + 1 > l + IBAGE_GAP) f = a = b = 0;  // not drawn for a while: made again
    uint64_t fx = __atomic_load_n(&g_fx_seen_at, __ATOMIC_RELAXED);
    if (f & IBAGE_PEND) {
        uint64_t p = f & ~IBAGE_PEND;  // the present + 1 it was first seen at
        if (fx >= p && fx <= p + IBAGE_PENDING) {                      // a spell came on right after: made then
            f = p, a = b = 0;
            if (skinned) __atomic_store_n(&g_hide_ask, 1, __ATOMIC_RELAXED);
        }
        else if (now + 1 > p + IBAGE_PENDING) f = 1, a = b = 0;        // none came: scenery
    }
    if (!f) {
        f = now < __atomic_load_n(&g_ibage_warm, __ATOMIC_RELAXED) ? 1 : now + 1;  // stored as present + 1
        if (f > 1 && now + 1 > fx + IBAGE_FX) f |= IBAGE_PEND;  // no spell on: wait and see
        if (f > 1 && !(f & IBAGE_PEND) && skinned) {  // a new slice this present
            __atomic_store_n(&g_hide_ask, 1, __ATOMIC_RELAXED);
            if (__atomic_exchange_n(&g_ibage_new_at, now, __ATOMIC_ACQ_REL) != now) __atomic_store_n(&g_ibage_new_n, 0, __ATOMIC_RELAXED);
            uint64_t c = __atomic_add_fetch(&g_ibage_new_n, 1, __ATOMIC_RELAXED);
            if (c > __atomic_load_n(&g_ibage_newmax, __ATOMIC_RELAXED)) __atomic_store_n(&g_ibage_newmax, c, __ATOMIC_RELAXED);
        }
    }
    if (ea && !a) __atomic_store_n(&ea->first, f, __ATOMIC_RELEASE);
    if (eb && !b) __atomic_store_n(&eb->first, f, __ATOMIC_RELEASE);
    if (ea) __atomic_store_n(&ea->last, now + 1, __ATOMIC_RELAXED); else ibage_put(g_ibage[0], k, f, now + 1);
    if (eb) __atomic_store_n(&eb->last, now + 1, __ATOMIC_RELAXED); else ibage_put(g_ibage[1], k, f, now + 1);
    if (f <= 1 || now + 1 < f) return 0;
    uint64_t p = f - 1;  // the present it was first seen at
    t_ib_aged = now - p > (uint64_t)g_cc.fxpropage && now - p <= (uint64_t)g_cc.fxpropage + 8;
    if (now - p > (uint64_t)g_cc.fxpropage) return 0;
    if (skinned && __atomic_load_n(&g_hide_at, __ATOMIC_ACQUIRE) < p + IBAGE_HIDE_AFTER) {
        __atomic_add_fetch(&g_fxprops_wait, 1, __ATOMIC_RELAXED);
        return 0;
    }
    return 1;
}
// Once per present: a new skinned slice asks TavPlan.PublishKeep for a list now (WorldMode.lua polls the file)
// rather than at its next turn, so a spell's skinned mesh waits a few presents, not a third of a second.
static void hide_ask(uint64_t now) {
    static uint64_t last;
    if (!__atomic_load_n(&g_hide_ask, __ATOMIC_RELAXED) || now < last + IBAGE_HIDE_ASK_GAP) return;
    __atomic_store_n(&g_hide_ask, 0, __ATOMIC_RELAXED);
    if (!g_tav.init || !g_tav.plan[0]) return;
    last = now;
    char path[sizeof g_tav.plan];
    snprintf(path, sizeof path, "%s", g_tav.plan);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "hide-ask.txt");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)now);
    fclose(f);
}
// Word "fxprops", a status's mesh. The age above suits what a spell leaves for a moment. A
// status plays its effect for as long as it lasts: NET (Hooked Net, Projectile_ThrowNet) loops
// VFX_Status_Ensnared_Net_Loop_Root_01, whose net is an opaque mesh (VFX_Model_DefaultLit_VertexOffset_FishNet)
// drawn in the G-buffer like any prop, so the age took it after 900 presents (6.4 s at 140 a second, seen in
// layer.log: 1000 draws per 300 presents, then none) and the hero stayed "Immobile: Ensnared" with no net. It
// is not on the keep list: BG3 hangs it on no character's Attachments.
//
// The mod knows what the layer can't: which status is on whom, and that it has an effect (StatusFx.lua, link/
// fxhold.txt). So a young slice placed at a character within HOLD_NEW presents of a status with an effect
// beginning on it belongs to that status, and stays kept until no one has that status any more. Keyed on
// the status's name, not the character: two nets are one slice.
#define HOLD_MAX 64u
#define HELD_MAX 64u
#define HOLD_NEW 300u      // presents after the status's start in which a slice can be claimed
#define HOLD_REACH 3.0f    // metres from the character (the net's effect: bounding sphere 2.2, 1 m below the root)
typedef struct { uint64_t status, holder, since; float at[3]; } HoldOn;
typedef struct { uint64_t slice, status; } Held;
static HoldOn g_hold[HOLD_MAX];
static uint32_t g_hold_n;
static Held g_held[HELD_MAX];
static uint32_t g_held_n;
static uint64_t g_hold_open;  // present + 1 until which some status is new enough to claim a slice
static struct timespec g_hold_seen;
static __thread float t_prop_at[3];  // prop_near: where the draw is
static int prop_held(uint64_t k) {
    if (!__atomic_load_n(&g_held_n, __ATOMIC_RELAXED)) return 0;
    for (uint32_t i = 0; i < HELD_MAX; i++) if (__atomic_load_n(&g_held[i].slice, __ATOMIC_RELAXED) == k) return 1;
    return 0;
}
static void hold_claim(uint64_t k, uint32_t count) {
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    if (now + 1 > __atomic_load_n(&g_hold_open, __ATOMIC_RELAXED) || prop_held(k)) return;
    uint64_t status = 0;
    pthread_mutex_lock(&g_fxl_lock);
    for (uint32_t i = 0; i < g_hold_n && !status; i++) {
        float dx = t_prop_at[0] - g_hold[i].at[0], dy = t_prop_at[1] - g_hold[i].at[1], dz = t_prop_at[2] - g_hold[i].at[2];
        if (now <= g_hold[i].since + HOLD_NEW && dx * dx + dy * dy + dz * dz < HOLD_REACH * HOLD_REACH) status = g_hold[i].status;
    }
    if (status) {
        uint32_t i = 0;
        while (i < HELD_MAX && g_held[i].slice) i++;
        if (i < HELD_MAX) {
            g_held[i].status = status;
            __atomic_store_n(&g_held[i].slice, k, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_held_n, 1, __ATOMIC_RELAXED);
        }
        logf_("fxhold: slice of ib %llx count %u at %.1f %.1f %.1f kept for status %016llx%s", (unsigned long long)t_ib, count, t_prop_at[0], t_prop_at[1],
              t_prop_at[2], (unsigned long long)status, i < HELD_MAX ? "" : " (table full, not held)");
    }
    pthread_mutex_unlock(&g_fxl_lock);
}
static uint64_t hold_hash(const char* c) { return fnv64(c, strlen(c), FNV0) | 1; }
static void hold_read(void) {
    if (!g_tav.init || !g_tav.plan[0]) return;
    char path[sizeof g_tav.plan];
    snprintf(path, sizeof path, "%s", g_tav.plan);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "fxhold.txt");
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtim.tv_sec == g_hold_seen.tv_sec && st.st_mtim.tv_nsec == g_hold_seen.tv_nsec) return;
    if (st.st_mtim.tv_sec < g_tav.born.tv_sec) return;  // left over from an earlier BG3 run
    FILE* f = fopen(path, "r");
    if (!f) return;
    g_hold_seen = st.st_mtim;
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    HoldOn next[HOLD_MAX];
    uint32_t n = 0;
    char line[256], name[96], who[128];
    float at[3];
    if (!fgets(line, sizeof line, f) || strncmp(line, "HOLD ", 5)) { fclose(f); return; }
    pthread_mutex_lock(&g_fxl_lock);
    while (fgets(line, sizeof line, f) && n < HOLD_MAX) {
        if (sscanf(line, "%95s %127s %f %f %f", name, who, &at[0], &at[1], &at[2]) != 5) continue;
        HoldOn e = {hold_hash(name), hold_hash(who), now, {at[0], at[1], at[2]}};
        int old = 0;
        for (uint32_t i = 0; i < g_hold_n; i++) if (g_hold[i].status == e.status && g_hold[i].holder == e.holder) { e.since = g_hold[i].since; old = 1; }
        if (!old) {
            logf_("fxhold: %s on %s at %.1f %.1f %.1f (status %016llx), present %llu", name, who, at[0], at[1], at[2], (unsigned long long)e.status, (unsigned long long)now);
            // A status beginning is a spell being on: its mesh may come with no other effect (a web from a surface).
            __atomic_store_n(&g_fx_seen_at, now + 1, __ATOMIC_RELAXED);
            __atomic_store_n(&g_hold_open, now + 1 + HOLD_NEW, __ATOMIC_RELAXED);
        }
        next[n++] = e;
    }
    fclose(f);
    memcpy(g_hold, next, n * sizeof next[0]);
    g_hold_n = n;
    for (uint32_t i = 0; i < HELD_MAX; i++) {
        if (!g_held[i].slice) continue;
        int on = 0;
        for (uint32_t j = 0; j < n; j++) if (g_hold[j].status == g_held[i].status) on = 1;
        if (on) continue;
        logf_("fxhold: status %016llx is over, its slice let go", (unsigned long long)g_held[i].status);
        __atomic_store_n(&g_held[i].slice, 0, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&g_held_n, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_fxl_lock);
}
static void hold_forget(void) {  // a load: buffers are recycled
    pthread_mutex_lock(&g_fxl_lock);
    memset(g_held, 0, sizeof g_held);
    __atomic_store_n(&g_held_n, 0, __ATOMIC_RELAXED);
    g_hold_n = 0;
    pthread_mutex_unlock(&g_fxl_lock);
}
// Once per present: turn the generations over, and start a warm-up when BG3 comes back from a load.
static void ibage_present(void) {
    static int was_running = 1, was_void, started;
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    // The table learns slices only inside void_skip, so on the present the void engages (world mode
    // starts: KEEP 1) every visible level slice is "first seen" at once; the same after a load.
    if (g_bg3_running && !was_running) { glow_forget(); hold_forget(); }
    if (!started || (g_bg3_running && !was_running) || (g_keep_active && !was_void)) {
        __atomic_store_n(&g_ibage_warm, now + IBAGE_WARM, __ATOMIC_RELAXED);
        started = 1;
    }
    was_running = g_bg3_running;
    was_void = g_keep_active;
    if (now && now % IBAGE_EVERY == 0) memset(g_ibage[(now / IBAGE_EVERY) & 1], 0, sizeof g_ibage[0]);
    if (g_cc.fxprops && g_keep_active) hide_ask(now);
    if (now && now % 300 == 0 && g_cc.fxglow)
        logf_("fxglow: %llu draws of a figure's glow kept, %llu past the probe limit", (unsigned long long)__atomic_exchange_n(&g_glow_kept, 0, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_exchange_n(&g_glow_full, 0, __ATOMIC_RELAXED));
    if (now && now % 300 == 0 && g_cc.fxprops)
        logf_("fxprops: %llu draws kept as a spell's props, %llu skinned draws waiting for a hide list, busiest present %llu new "
              "skinned slices, %llu slices past the probe limit",
              (unsigned long long)__atomic_exchange_n(&g_fxprops_kept, 0, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_exchange_n(&g_fxprops_wait, 0, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_exchange_n(&g_ibage_newmax, 0, __ATOMIC_RELAXED),
              (unsigned long long)__atomic_exchange_n(&g_ibage_full, 0, __ATOMIC_RELAXED));
}
static void mesh_present(void) {
    static uint32_t presents;
    if (++presents % MESH_EVERY == 0) {
        memset(g_mesh[(presents / MESH_EVERY) & 1], 0, sizeof g_mesh[0]);
        uint64_t full = __atomic_exchange_n(&g_mesh_full, 0, __ATOMIC_RELAXED);
        if (full) logf_("void: %llu mesh slices past the probe limit", (unsigned long long)full);
    }
}
static uint64_t mesh_key(uint32_t count, uint32_t first, int32_t vo) {
    uint64_t k[3] = {ib_slice(t_ib, t_ibo, first, count, vo), t_vb[0], t_vbo[0]};
    return fnv64(k, sizeof k, FNV0) | 1;
}
static int mesh_draw(uint32_t count, uint32_t first, int32_t vo) {
    return t_ib && (mesh_has(mesh_key(count, first, vo)) || hide_has(t_ib));
}

// Depth-only passes the void applies to: the world's shadow maps, square and 2048 or more (8192 and
// 2048 in traces). Smaller depth-only passes belong to BG3's own little renders, such as the portrait
// re-render (ecl::CharacterIconRenderSystem): voiding them made a custom hero's portrait black behind
// it. Each size is logged once.
static int void_depth_only(const ImgInfo* i) {
    int world = i->w == i->hgt && i->w >= 2048;
    static uint32_t seen[16];
    static int nseen;
    uint32_t key = i->w << 16 | i->hgt;
    int k = 0;
    while (k < nseen && seen[k] != key) k++;
    if (k == nseen && nseen < 16) {
        seen[nseen++] = key;
        logf_("void: depth-only passes %ux%u %s", i->w, i->hgt, world ? "voided (the world's shadow maps)" : "kept (not the world's)");
    }
    return world;
}
static void void_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs) {
    int colors = 0, scene = 0, depth = 0;
    ImgInfo* dimg = NULL;
    for (uint32_t k = 0; k < n; k++) {
        if (!imgs[k]) continue;
        if (imgs[k] == g_role.depth) { scene = 1; continue; }
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[k], 0);
        if (!i) continue;
        if (is_depth_format(i->fmt)) { depth = 1; dimg = i; } else colors++;
    }
    if (scene || (depth && !colors && dimg && void_depth_only(dimg))) t_void_cb = cb;
    t_mesh_cb = scene && colors >= 5 ? cb : VK_NULL_HANDLE;  // BG3's G-buffer pass
}
// fxc_near_tav for a prop, decided now: a draw placed by its world-space vertices (fxc_place 3, the
// capture decides those later from the blob) is placed by its first vertex when its index and vertex
// data are mapped. A spell's mesh particles are such draws: debris and shards a particle system emits
// as small opaque meshes (Ray of Frost, Witch Bolt, Horde Breaker; the index count changes every
// frame as particles come and go), drawn in the prepass and the G-buffer, so only the void decides
// them. 2 when it cannot tell (never kept). Near Tav only, not near BG3's camera as fxc_near_tav also
// allows: a body near the camera but not near Tav is not on the hide list (it names characters within
// 130 m of her), and the sweep caught one so, kept 510 m from Tav just after she moved spots.
static int prop_near(const Recipe* r, uint32_t fi, uint32_t first, int32_t vo) {
    float m[16], at[3];
    int how = fxc_place(r, fi, at, m);
    if (how == 1 || how == 2) {
        memcpy(t_prop_at, at, sizeof t_prop_at);
        float dx = at[0] - g_tav_root[0], dy = at[1] - g_tav_root[1], dz = at[2] - g_tav_root[2], rad = fxc_radius();
        return dx * dx + dy * dy + dz * dz < rad * rad;
    }
    if (how != 3) return how == 4 ? 2 : 0;
    uint32_t lloc = 99, lbind = 99, loff = 0, stride = 0;
    for (uint32_t a = 0; a < r->nattr; a++) {
        uint32_t loc = r->attr[a][0], b = r->attr[a][1], fmt = r->attr[a][2];
        int rate = 0;
        for (uint32_t k = 0; k < r->nbind; k++) if (r->bind[k][0] == b) rate = (int)r->bind[k][2];
        if (rate || loc >= lloc || (fmt != VK_FORMAT_R32G32B32_SFLOAT && fmt != VK_FORMAT_R32G32B32A32_SFLOAT)) continue;
        lloc = loc; lbind = b; loff = r->attr[a][3];
    }
    for (uint32_t k = 0; k < r->nbind; k++) if (r->bind[k][0] == lbind) stride = r->bind[k][1];
    if (lbind >= 8 || !t_vb[lbind] || !stride) return 2;
    uint32_t isz = t_ibt == VK_INDEX_TYPE_UINT32 ? 4 : 2;
    uint64_t left = 0;
    const uint8_t* ip = buf_cpu(t_ib, t_ibo + (uint64_t)first * isz, &left);
    if (!ip || left < isz) return 2;
    int64_t v = (int64_t)(isz == 4 ? *(const uint32_t*)ip : *(const uint16_t*)ip) + vo;
    if (v < 0) return 2;
    const uint8_t* vp = buf_cpu(t_vb[lbind], t_vbo[lbind] + (uint64_t)v * stride + loff, &left);
    if (!vp || left < 12) return 2;
    float p[3], w[3];
    memcpy(p, vp, 12);
    const float* eye = (const float*)fxc_slot_cpu(0, 1, 112);
    if (!eye) return 2;
    for (int c = 0; c < 3; c++) w[c] = eye[24 + c] + p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
    memcpy(t_prop_at, w, sizeof t_prop_at);
    float dx = w[0] - g_tav_root[0], dy = w[1] - g_tav_root[1], dz = w[2] - g_tav_root[2], rad = fxc_radius();
    return dx * dx + dy * dy + dz * dz < rad * rad;  // NaN counts as far
}
// What the void lets through on a young or solid guess, one log line per slice, so a BG3 mesh seen in the
// picture can be named afterwards: the buffer against the lines "WOW-BG3 hide:" in bg3le.log (TavPlan.lua:
// whose it is), the time (CLOCK_MONOTONIC seconds, as bg3le.log's) against "WOW-BG3 dialog:". No recording
// exists of the shine on a shield in a dialog (playtest feedback); this is what catches it next time.
#define VOID_SAY_CAP 512u
#define VOID_SAY_LINES 8u  // at most so many lines per 300 presents
static uint64_t g_void_said[VOID_SAY_CAP], g_void_said_win, g_void_line_win;
static uint32_t g_void_lines;
static void void_say(const char* what, const Recipe* r, uint32_t count, uint32_t first, int32_t vo, uint32_t fi) {
    uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    // Said again after a while: a slice kept now and one kept a minute ago are different events.
    if (__atomic_exchange_n(&g_void_said_win, now / 3600, __ATOMIC_RELAXED) != now / 3600) memset(g_void_said, 0, sizeof g_void_said);
    uint64_t k = ib_slice(t_ib, t_ibo, first, count, vo);
    uint32_t i = hslot(k, VOID_SAY_CAP), n = 0;
    for (; n < 16; n++, i = (i + 1) & (VOID_SAY_CAP - 1)) {
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(&g_void_said[i], &zero, k, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
        if (zero == k) return;
    }
    if (n == 16) return;
    if (__atomic_exchange_n(&g_void_line_win, now / 300, __ATOMIC_RELAXED) != now / 300) __atomic_store_n(&g_void_lines, 0, __ATOMIC_RELAXED);
    if (__atomic_fetch_add(&g_void_lines, 1, __ATOMIC_RELAXED) >= VOID_SAY_LINES) return;
    float at[3] = {0, 0, 0}, m[16];
    int how = fxc_place(r, fi, at, m);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    logf_("void: %s: ib %llx count %u first %u vo %d nskin %u blend %u place %d at %.1f %.1f %.1f, Tav %.1f %.1f %.1f, party dialog %d, time %lld.%03ld",
          what, (unsigned long long)t_ib, count, first, vo, (unsigned)r->nskin, (unsigned)(r->ncolor && r->blend[0][0]), how, at[0], at[1], at[2],
          g_tav_root[0], g_tav_root[1], g_tav_root[2], (int)g_screen_dialog, (long long)ts.tv_sec, ts.tv_nsec / 1000000);
}
// An indexed draw in such a pass: skip it unless it is a character's or an effect near Tav.
static int void_skip(VkCommandBuffer cb, uint32_t fi, uint32_t count, uint32_t first, int32_t vo) {
    t_prop_cb = VK_NULL_HANDLE;
    if (t_mesh_cb == cb && t_ib_cb == cb && t_ib) mesh_add(mesh_key(count, first, vo));
    // BG3's own 3D screens (level up, respec, the mirror): stock BG3 shows the character against the
    // level's own scenery, and the only reason ours stood in an empty void is that the world mode's
    // voiding was still culling the level's draws while the screen was up. It is not wanted there: the
    // screen is a close-up and WoW's ground is not drawn behind it anyway. Word "noscreenlevel" keeps
    // the voiding on, which is what 0.1.0 did.
    if (g_screen && g_cc.screenlevel && !g_screen_dialog) return 0;
    if (t_void_cb != cb || !g_cc.void_ || !g_keep_active || t_ib_cb != cb || !t_ib) return 0;
    if (keep_has(t_ib)) return 0;
    // A spell's 3D props (word "fxprops"): kept while their mesh is young, so BG3 draws them and the
    // compositor shows them where BG3's geometry is nearer than WoW's ground.
    // Aged before the other tests, so a character's slices grow old with it and one that leaves the
    // hide list (out of its radius) is not taken for a new prop. First drawn is not first made: a
    // level mesh entering BG3's frustum as the camera turns, or one switching level of detail, is new
    // to this table too. A spell's prop is at the caster or its target, scenery that comes into view
    // is almost always far off, so a young slice is kept only when it is placed near Tav.
    // Not in the decal pass: every BG3 decal is the same 36-index box, so one slice stands for all of
    // them, and with ages that start over when a slice comes back the footprints near Tav were kept as
    // props. Decals near Tav are the word "fxdecals"'s to keep (below).
    RecInfo* pri = g_cc.fxprops && t_bind_cb == cb && t_pipe && g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* pr = pri ? pri->r : NULL;
    if (g_cc.fxprops && t_dec_cb != cb) {
        uint64_t slice = ib_slice(t_ib, t_ibo, first, count, vo);
        const int young = ib_young(slice, !pr || pr->nskin), held = prop_held(slice);
        if ((young || held || t_ib_aged) && !hide_has(t_ib) && pr && (pr->have & 1) && g_tav_root_ok && prop_near(pr, fi, first, vo) == 1) {
            if (young || held) {
                __atomic_add_fetch(&g_fxprops_kept, 1, __ATOMIC_RELAXED);
                t_prop_cb = cb;  // fxwhy shows it as decision Y
                if (young) hold_claim(slice, count);
                void_say("kept as a prop", pr, count, first, vo, fi);
                return 0;
            }
            // A slice near Tav that BG3 still draws and the age has just taken: a status's or a long effect's
            // mesh no status claimed, or scenery kept by mistake. Names it, a few lines per 300 presents.
            static uint64_t said_win;
            static uint32_t said;
            uint64_t win = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED) / 300;
            if (__atomic_exchange_n(&said_win, win, __ATOMIC_RELAXED) != win) __atomic_store_n(&said, 0, __ATOMIC_RELAXED);
            if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 8)
                logf_("fxprops: aged out, still drawn: ib %llx count %u first %u vo %d nskin %u at %.1f %.1f %.1f", (unsigned long long)t_ib, count, first, vo,
                      (unsigned)pr->nskin, t_prop_at[0], t_prop_at[1], t_prop_at[2]);
        }
    }
    if ((t_fxl_cb == cb || (t_dec_cb == cb && g_cc.fxdecals)) && t_bind_cb == cb && t_pipe) {
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* r = ri ? ri->r : NULL;
        if (r && (r->have & 1) && fxc_near_tav(r, fi)) {
            if (!mesh_draw(count, first, vo) && r->ncolor && r->blend[0][0] && r->ztest && !r->zwrite) return 0;
            if (fxc_solid(r)) {
                // Not a stand-in's own buffer (fxc_draw the same): its sword and shield are unskinned, so
                // fxc_solid alone took them for a spell's piece whenever BG3 drew them in this pass.
                if (hide_has(t_ib)) { void_say("a stand-in's solid draw, dropped", r, count, first, vo, fi); return 1; }
                void_say("kept as a solid piece", r, count, first, vo, fi);
                return 0;  // a spell's solid piece: fxc_draw records it for the replay
            }
        }
    }
    return 1;
}

// ---- Research (request "fxwhy<N>"): which of BG3's draws make each overlay (path
// line, destination circle, cast effects, spell previews) and what the layer does with them. For
// N presents every draw of every pass is counted by (pass, shaders, kind, pipeline state,
// decision) into OUT_DIR/fxwhy.txt, with the first draw's details. Decisions: C captured for the
// replay, F far from Tav or unplaced, N not an effect by the capture's test, M a mesh's slice
// (characters, the level), B over the blob
// budget, R no recipe, U a texture the layer cannot export (benilla would skip it), V skipped by
// the void, Y kept through the void as a spell's young prop (word "fxprops"; P is the route line),
// H Tav's own (WoW
// draws her), I indirect (never captured), - drawn by BG3 outside the effects pass. Passes: attachments as WxH:format, D the
// scene depth, L loaded, C cleared; "sec" a secondary command buffer inside a pass.

static __thread VkCommandBuffer t_fxw_cb;
static __thread uint64_t t_fxw_pass;
static __thread char t_fxw_desc[192];
typedef struct {
    uint64_t h;
    uint32_t order, n, inst_max, count0, flags, blendf, nskin;
    char why, kind;
    int how;
    float dmin, dmax, at0[3], v0[4];
    uint64_t vs, fs, pipe, ib;
    int keep;
    char pass[192];
} FxwAgg;
#define FXW_CAP 8192
static FxwAgg* g_fxw;
static uint32_t g_fxw_n;
static FILE* g_fxw_f;

static void fxw_img(char* out, size_t n, uint64_t img, int load) {
    ImgInfo* i = img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
    if (!i) { snprintf(out, n, " ?"); return; }
    snprintf(out, n, " %ux%u:%d%s%s", i->w, i->hgt, (int)i->fmt, img == g_role.depth ? "D" : "",
             load == VK_ATTACHMENT_LOAD_OP_LOAD ? "L" : load == VK_ATTACHMENT_LOAD_OP_CLEAR ? "C" : "");
}
// A pass begins on this thread (g_trace_lock held by the caller): note its description.
static void fxw_pass(VkCommandBuffer cb, const char* what, uint32_t n, const uint64_t* imgs, const uint8_t* load) {
    if (g_fxw_frames <= 0) return;
    int o = snprintf(t_fxw_desc, sizeof t_fxw_desc, "%s", what);
    for (uint32_t k = 0; k < n && o < (int)sizeof t_fxw_desc - 24; k++) {
        char d[48];
        fxw_img(d, sizeof d, imgs[k], load ? load[k] : -1);
        o += snprintf(t_fxw_desc + o, sizeof t_fxw_desc - o, "%s", d);
    }
    t_fxw_pass = fnv64(t_fxw_desc, strlen(t_fxw_desc), FNV0);
    t_fxw_cb = cb;
}
// A secondary command buffer that continues a pass: its pass from the inheritance info.
static void fxw_secondary(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    if (g_fxw_frames <= 0 || !info || !info->pInheritanceInfo || !(info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) return;
    const VkCommandBufferInheritanceInfo* ii = info->pInheritanceInfo;
    uint64_t imgs[9];
    uint32_t n = 0;
    pthread_mutex_lock(&g_trace_lock);
    FbInfo* f = ii->framebuffer ? TABLE_FIND(g_fb, FB_CAP, (uint64_t)ii->framebuffer, 0) : NULL;
    for (uint32_t k = 0; f && k < f->n && n < 8; k++) imgs[n++] = view_image(f->views[k]);
    fxw_pass(cb, f ? "sec RP" : "sec ?", n, imgs, NULL);
    pthread_mutex_unlock(&g_trace_lock);
}
// One draw (any pass) with what the layer did with it.
static void fxw_draw(VkCommandBuffer cb, char kind, uint32_t count, uint32_t inst, uint32_t first, int32_t vo, uint32_t fi, int why) {
    if (g_fxw_frames <= 0) return;
    RecInfo* ri = (g_rec && t_bind_cb == cb && t_pipe) ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* r = ri ? ri->r : NULL;
    uint64_t vs = 0, fs = 0;
    for (uint32_t s = 0; r && s < r->nstages; s++) {
        if (r->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = r->st[s].key;
        if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = r->st[s].key;
    }
    uint32_t flags = r ? ((r->ncolor && r->blend[0][0]) | (r->ztest << 1) | (r->zwrite << 2) | (r->zcmp << 4) | (r->cull << 8) |
                          (r->topo << 12) | (r->ncolor << 20)) : 0xffffffffu;
    uint32_t blendf = r && r->ncolor ? (r->blend[0][1] | r->blend[0][2] << 5 | r->blend[0][4] << 10 | r->blend[0][5] << 15 | r->blend[0][7] << 20) : 0;
    if (!why) why = (t_fxl_cb == cb) ? '?' : '-';
    float at[3] = {0, 0, 0};
    int how = r && t_bind_cb == cb ? fxc_place(r, fi, at, NULL) : 0;
    float dist = -1;
    if (how > 0 && g_tav_root_ok) {
        float dx = at[0] - g_tav_root[0], dy = at[1] - g_tav_root[1], dz = at[2] - g_tav_root[2];
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    // The first vertex's lowest-location float3/float4 attribute, when mapped (unplaced draws).
    float v0[4] = {0, 0, 0, 0};
    for (uint32_t a = 0, best = 99; r && a < r->nattr; a++) {
        uint32_t loc = r->attr[a][0], b = r->attr[a][1], fmt = r->attr[a][2], off = r->attr[a][3];
        if (loc >= best || (fmt != VK_FORMAT_R32G32B32_SFLOAT && fmt != VK_FORMAT_R32G32B32A32_SFLOAT) || b >= 8 || !t_vb[b]) continue;
        uint32_t stride = 0, rate = 0;
        for (uint32_t k = 0; k < r->nbind; k++) if (r->bind[k][0] == b) { stride = r->bind[k][1]; rate = r->bind[k][2]; }
        uint64_t idx = rate ? fi : (kind == 'X' || kind == 'x') ? (uint64_t)(vo > 0 ? vo : 0) : first, left = 0;
        const uint8_t* v = buf_cpu(t_vb[b], t_vbo[b] + idx * stride + off, &left);
        if (!v || left < 16) continue;
        memcpy(v0, v, fmt == VK_FORMAT_R32G32B32_SFLOAT ? 12 : 16);
        best = loc;
    }
    pthread_mutex_lock(&g_fxl_lock);
    if (!g_fxw) g_fxw = calloc(FXW_CAP, sizeof *g_fxw);
    // Word "fxwib": the index buffer joins the group key, so every distinct mesh is its
    // own group instead of being collapsed with everything that shares its pipeline. Without it a
    // thing that appears only during a cast (Grease's 3D blobs) cannot be told from the draws it
    // shares shaders with: a cast grew a dozen groups by one or two draws each and named nothing.
    uint64_t key[9] = {t_fxw_cb == cb ? t_fxw_pass : 0, vs, fs, (uint64_t)kind, flags, (uint64_t)why, blendf, (uint64_t)(how > 0),
                       g_cc.fxwib && t_ib_cb == cb ? t_ib : 0};
    uint64_t h = fnv64(key, sizeof key, FNV0) | 1;
    FxwAgg* e = g_fxw && g_fxw_n < FXW_CAP / 2 ? TABLE_FIND(g_fxw, FXW_CAP, h, 1) : NULL;
    if (e) {
        if (!e->n) {
            e->order = g_fxw_n++;
            snprintf(e->pass, sizeof e->pass, "%s", t_fxw_cb == cb ? t_fxw_desc : "(no pass seen)");
            e->vs = vs; e->fs = fs; e->pipe = t_pipe; e->kind = kind; e->flags = flags; e->blendf = blendf; e->why = (char)why;
            e->how = how; e->count0 = count; e->ib = t_ib_cb == cb ? t_ib : 0; e->keep = keep_has(e->ib);
            e->nskin = r ? r->nskin : 0;
            memcpy(e->at0, at, sizeof at); memcpy(e->v0, v0, sizeof v0);
            e->dmin = 1e30f; e->dmax = -1;
        }
        e->n++;
        if (inst > e->inst_max) e->inst_max = inst;
        if (dist >= 0 && dist < e->dmin) e->dmin = dist;
        if (dist > e->dmax) e->dmax = dist;
    }
    pthread_mutex_unlock(&g_fxl_lock);
}
static int fxw_cmp(const void* a, const void* b) {
    const FxwAgg* x = *(const FxwAgg* const*)a;
    const FxwAgg* y = *(const FxwAgg* const*)b;
    return x->order < y->order ? -1 : x->order > y->order;
}
static void fxw_present(void) {
    if (g_fxw_frames <= 0) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (!g_fxw_f) g_fxw_f = fopen(outp("/fxwhy.txt"), "w");
    if (g_fxw_f && g_fxw) {
        FxwAgg** list = malloc(sizeof *list * (g_fxw_n + 1));
        uint32_t n = 0;
        for (uint32_t i = 0; list && i < FXW_CAP; i++) if (g_fxw[i].h && g_fxw[i].n) list[n++] = &g_fxw[i];
        if (list) qsort(list, n, sizeof *list, fxw_cmp);
        fprintf(g_fxw_f, "PRESENT %d tav %.2f %.2f %.2f (%s) groups %u\n", g_fxw_frames, g_tav_root[0], g_tav_root[1], g_tav_root[2],
                g_tav_root_ok ? "known" : "unknown", n);
        for (uint32_t i = 0; list && i < n; i++) {
            FxwAgg* e = list[i];
            fprintf(g_fxw_f, "%c %c n%u cnt%u inst%u | %s | vs %016llx fs %016llx pipe %llx | bl%u zt%u zw%u zc%u cull%u topo%u nc%u bf %x |"
                             " place %d skin%u d %.1f..%.1f at %.2f %.2f %.2f v0 %.3f %.3f %.3f %.3f ib %llx%s\n",
                    e->why, e->kind, e->n, e->count0, e->inst_max, e->pass, (unsigned long long)e->vs, (unsigned long long)e->fs,
                    (unsigned long long)e->pipe, e->flags & 1, e->flags >> 1 & 1, e->flags >> 2 & 1, e->flags >> 4 & 15, e->flags >> 8 & 15,
                    e->flags >> 12 & 255, e->flags >> 20 & 15, e->blendf, e->how, e->nskin, e->dmin > 1e29f ? -1.0f : e->dmin, e->dmax, e->at0[0],
                    e->at0[1], e->at0[2], e->v0[0], e->v0[1], e->v0[2], e->v0[3], (unsigned long long)e->ib, e->keep ? " keep" : "");
        }
        free(list);
        memset(g_fxw, 0, FXW_CAP * sizeof *g_fxw);
        g_fxw_n = 0;
        fflush(g_fxw_f);
    }
    if (--g_fxw_frames <= 0 && g_fxw_f) {
        fclose(g_fxw_f);
        g_fxw_f = NULL;
        logf_("fxwhy: written");
    }
    pthread_mutex_unlock(&g_fxl_lock);
}

static void fxc_present(QueueData* q) {
    mesh_present();
    ibage_present();
    tav_log_present();
    fxw_present();
    if (g_cc.on && g_cc.void_) keep_read();
    if (g_cc.on && g_cc.void_ && g_cc.fxprops) { hide_read(); hold_read(); }
    int want = g_cc.on && g_cc.fxcopy && !g_tav.off;
    route_present(want);
    if (!want) {
        if (g_fxc.on) logf_("fxcopy: off");
        g_fxc.on = g_fxc_on = 0;
        return;
    }
    if (!g_fxc.on) logf_("fxcopy: on");
    pthread_mutex_lock(&g_fxl_lock);
    int async = !g_cc.fxsync;
    if (!async) fxj_drain();
    if (g_fxc.on && g_fxc.ndraws) {
        FxJob* job = async ? fxj_take(q) : NULL;
        fxc_gpu_fill(q, job);
        if (job) {
            job->nclus = g_fxc.nclus;
            memcpy(job->clus, g_fxc.clus, g_fxc.nclus * sizeof *g_fxc.clus);
        } else if (g_fxc.nclus) {
            fxc_clus_fix(g_fxc.blob.p, g_fxc.blob.n, g_fxc.clus, g_fxc.nclus);
        }
        for (uint32_t k = 0; k < g_fxc.ntex; k++)  // new textures: through the worker, ahead of this frame
            if (!job || !fxc_image_export_async(q, g_fxc.tex[k].image, -1, g_fxc.tex[k].key))
                fxc_image_export(q, g_fxc.tex[k].image, g_fxc.tex[k].key);
        Bytes out = {0};
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        double t = (double)now.tv_sec + now.tv_nsec * 1e-9;
        by_put(&out, "FXF1", 4); by_u32(&out, 1); by_u32(&out, ++g_fxc.frame); by_u32(&out, g_fxc.ndraws);
        by_put(&out, &t, 8);
        by_put(&out, g_fxc.eye, 12);
        size_t post_at = out.n;
        by_u32(&out, 0);  // post block offset, filled below
        by_u32(&out, g_role.ew); by_u32(&out, g_role.eh);
        size_t head = out.n + 8;
        uint32_t data_off = (uint32_t)((head + g_fxc.draws.n + 255) & ~255ull);
        by_u32(&out, data_off); by_u32(&out, (uint32_t)g_fxc.blob.n);
        size_t draws_at = out.n;
        by_put(&out, g_fxc.draws.p, g_fxc.draws.n);
        by_pad(&out, 256);
        by_put(&out, g_fxc.blob.p, g_fxc.blob.n);
        uint32_t po = 0;
        if (g_post.have) {
            by_pad(&out, 16);
            po = (uint32_t)out.n;
            post_block(q, &out);
            memcpy(out.p + post_at, &po, 4);
        }
        float rad = fxc_radius(), tav[3] = {g_tav_root[0], g_tav_root[1], g_tav_root[2]};
        if (job && g_nfxlate) {
            if (job->caplate < g_nfxlate) {
                FxLate* q = realloc(job->late, g_nfxlate * sizeof *q);
                if (q) { job->late = q; job->caplate = g_nfxlate; }
            }
            if (job->caplate >= g_nfxlate) {
                memcpy(job->late, g_fxlate, g_nfxlate * sizeof *g_fxlate);
                job->nlate = g_nfxlate;
                job->draws_at = draws_at;
                memcpy(job->tav, tav, sizeof tav);
                job->rad = g_tav_root_ok ? rad : 1e30f;
                job->by_eye = fxc_by_eye();
            }
        } else if (!job && g_nfxlate && g_tav_root_ok) {
            fxc_late(out.p + draws_at, out.p + data_off, out.n - data_off, g_fxlate, g_nfxlate, tav, rad, fxc_by_eye());
        }
        if (job) {  // the worker fills the GPU-only ranges (and the exposure pair) and writes the file
            job->kind = 1;
            job->data_off = data_off;
            job->post_at = po;
            job->exp = (g_post.have & 1) != 0;
            memcpy(job->exp_blob, g_post.exp_blob, sizeof job->exp_blob);
            job->out = out;
            out = (Bytes){0};
            fxj_queue(job);
        } else {
            tav_write("fxframe.bin", out.p, out.n);
        }
        if (g_fxc.frame % 300 == 1) {
            logf_("fxcopy: frame %u, %u draws (%u far from Tav, %u over budget), %zu blob bytes, %u GPU ranges, %u textures exported;"
                  " readbacks %s, %llu waits for a free job (%.1f ms)",
                  g_fxc.frame, g_fxc.ndraws, g_fxc.far, g_fxc.dropped, g_fxc.blob.n, g_fxc.ngpu, g_fxc.ntex,
                  job ? "on the worker" : "at the present", (unsigned long long)g_fxj.waits, g_fxj.wait_ns / 1e6);
            pthread_mutex_lock(&g_fxj_data);
            uint64_t cneed = g_clus.need, ccut = g_clus.cut, cframes = g_clus.frames;
            g_clus.need = g_clus.cut = g_clus.frames = 0;
            pthread_mutex_unlock(&g_fxj_data);
            if (g_fxc.skin_draws || g_fxc.skin_full || cframes || g_fxc.unsup)
                logf_("fxcopy: since the last line: %llu draws dropped (a texture the layer cannot export, benilla skips them);"
                      " skinned stream trimmed in %llu draws (%.1f KB instead of %.1f MB), %llu draws whole"
                      " (no index range yet); light index lists %.1f KB instead of %.1f MB (copying %llu KB a frame, headers used up to %llu KB,"
                      " %llu clusters cut in %llu frames)",
                      (unsigned long long)g_fxc.unsup, (unsigned long long)g_fxc.skin_draws, g_fxc.skin_bytes / 1024.0, g_fxc.skin_whole / 1048576.0,
                      (unsigned long long)g_fxc.skin_full, g_fxc.clus_bytes / 1024.0, g_fxc.clus_whole / 1048576.0,
                      (unsigned long long)(__atomic_load_n(&g_clus_want, __ATOMIC_RELAXED) >> 10), (unsigned long long)(cneed * 4 >> 10),
                      (unsigned long long)ccut, (unsigned long long)cframes);
            g_fxc.skin_draws = g_fxc.skin_bytes = g_fxc.skin_whole = g_fxc.skin_full = g_fxc.clus_bytes = g_fxc.clus_whole = g_fxc.unsup = 0;
        }
        free(out.p);
    } else if (g_fxc.on && g_fxc.frame && g_fxc.ndraws == 0) {
        // No effects this frame: an empty frame so benilla stops drawing the last ones.
        Bytes out = {0};
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        double t = (double)now.tv_sec + now.tv_nsec * 1e-9;
        by_put(&out, "FXF1", 4); by_u32(&out, 1); by_u32(&out, ++g_fxc.frame); by_u32(&out, 0);
        by_put(&out, &t, 8); by_put(&out, g_fxc.eye, 12); by_u32(&out, 0);
        by_u32(&out, g_role.ew); by_u32(&out, g_role.eh); by_u32(&out, 0); by_u32(&out, 0);
        FxJob* job = async ? fxj_take(q) : NULL;
        if (job) {  // through the worker, after the frames before it
            job->kind = 1;
            job->out = out;
            fxj_queue(job);
        } else {
            tav_write("fxframe.bin", out.p, out.n);
            free(out.p);
        }
    }
    g_fxc.draws.n = g_fxc.blob.n = 0;
    memset(g_seen, 0, sizeof g_seen);
    g_fxc.ngpu = g_fxc.ntex = g_fxc.ndraws = g_fxc.dropped = g_fxc.far = g_fxc.nclus = 0;
    g_nsimg = 0;
    g_nfxlate = 0;
    g_post.have = 0;
    g_fxc.on = g_fxc_on = 1;
    pthread_mutex_unlock(&g_fxl_lock);
}


// ---- Research (request "trace..."): the frame graph from the effects pass to the screen. Each
// pass lists the render targets its draws sample and its first fragment shaders; each dispatch
// its compute shader, the render targets it samples and the storage images it writes. That finds
// BG3's post-processing and tonemap shaders, which the replay's colour parity follows.

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                   const VkComputePipelineCreateInfo* infos,
                                                                   const VkAllocationCallbacks* alloc, VkPipeline* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateComputePipelines(device, cache, n, infos, alloc, out);
    if (r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_cpipe) g_cpipe = calloc(CPIPE_CAP, sizeof *g_cpipe);
        for (uint32_t k = 0; k < n && g_cpipe; k++) {
            if (!out[k]) continue;
            CPipeInfo* p = TABLE_FIND(g_cpipe, CPIPE_CAP, (uint64_t)out[k], 1);
            if (!p) continue;
            const VkPipelineShaderStageCreateInfo* si = &infos[k].stage;
            p->spv = 0;
            if (si->module) {
                ShaderInfo* sh = shader_info((uint64_t)si->module);
                p->spv = sh ? sh->key : 0;
            } else {
                for (const VkBaseInStructure* e = si->pNext; e; e = e->pNext)
                    if (e->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                        const VkShaderModuleCreateInfo* mi = (const VkShaderModuleCreateInfo*)e;
                        p->spv = spv_store(mi->pCode, mi->codeSize);
                    }
            }
        }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}

// Render-target images (colour, depth or storage usage) bound in `sets`: sampled ones into
// rd, storage ones into wr. Returns through the counts.
static void bound_targets(const uint64_t* sets, uint64_t* rd, uint32_t* nrd, uint32_t rcap, uint64_t* wr, uint32_t* nwr, uint32_t wcap) {
    if (!g_settab) return;
    for (int s = 0; s < 8; s++) {
        SetInfo* si = set_find(sets[s]);
        for (uint32_t b = 0; si && b < DESC_SLOTS; b++) {
            DescSlot* sl = &si->slot[b];
            int st = sl->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (!sl->res || !(st || sl->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                              sl->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
                continue;
            uint64_t img = view_image(sl->res);
            ImgInfo* i = img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
            if (!i || !(i->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)))
                continue;
            uint64_t* to = st ? wr : rd;
            uint32_t* n = st ? nwr : nrd;
            uint32_t cap = st ? wcap : rcap, k = 0;
            while (k < *n && to[k] != img) k++;
            if (k == *n && *n < cap) to[(*n)++] = img;
        }
    }
}

// g_trace_lock held.
static void trace_draw_reads(CbLog* c, VkCommandBuffer cb) {
    if (!c->in_pass || t_bind_cb != cb) return;
    uint64_t wr[1]; uint32_t nwr = 0;
    bound_targets(t_set, c->reads, &c->nreads, 12, wr, &nwr, 0);
    RecInfo* ri = (g_rec && t_pipe) ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    for (uint32_t s = 0; ri && ri->r && s < ri->r->nstages; s++) {
        if (ri->r->st[s].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
        uint64_t key = ri->r->st[s].key;
        uint32_t k = 0;
        while (k < c->nfrag && c->frag[k] != key) k++;
        if (k == c->nfrag && c->nfrag < 4) c->frag[c->nfrag++] = key;
    }
}
static void trace_dispatch_reads(CbLog* c, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (t_cbind_cb != cb) return;
    uint64_t rd[8], wr[4]; uint32_t nrd = 0, nwr = 0;
    bound_targets(t_cset, rd, &nrd, 8, wr, &nwr, 4);
    CPipeInfo* p = (g_cpipe && t_cpipe) ? TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0) : NULL;
    char line[900]; int o = snprintf(line, sizeof line, "    dispatch %ux%ux%u cs %016llx", x, y, z, (unsigned long long)(p ? p->spv : 0));
    for (uint32_t k = 0; k < nrd && o < (int)sizeof line - 100; k++) {
        char d[96]; img_desc(d, sizeof d, rd[k]); o += snprintf(line + o, sizeof line - o, " r %s", d);
    }
    for (uint32_t k = 0; k < nwr && o < (int)sizeof line - 100; k++) {
        char d[96]; img_desc(d, sizeof d, wr[k]); o += snprintf(line + o, sizeof line - o, " w %s", d);
    }
    cb_printf(c, "%s\n", line);
}

static void fxc_pass_target(uint64_t image) { t_pass_target = image; }

// BG3's viewport in the effects pass (logged once: replay needs to know whether BG3 flips Y with a
// negative height).
static VKAPI_ATTR void VKAPI_CALL layer_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    static int said;
    if (!said && t_fxl_cb == cb && n) {
        said = 1;
        logf_("fxcopy: effects pass viewport x %.1f y %.1f w %.1f h %.1f depth %.2f..%.2f", v[0].x, v[0].y, v[0].width, v[0].height,
              v[0].minDepth, v[0].maxDepth);
    }
    ground_note_viewport(cb, first, n, v);
    d->CmdSetViewport(cb, first, n, v);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdSetScissor) return;
    ground_note_scissor(cb, first, n, r);
    d->CmdSetScissor(cb, first, n, r);
}
