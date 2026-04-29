/* stdlib_noise.c — Noise generation module for Luna.
 *
 * Three independent noise algorithms, each as its own class:
 *   Perlin  — layered smooth noise
 *   Simplex — gradient noise (fewer artifacts than Perlin)
 *   Voronoi — cellular / Worley noise
 *
 * All algorithms are deterministic given a seed.
 * Instances are callable: p(x, y) or p(x, y, z).
 * Voronoi additionally exposes :edge(x, y) / :edge(x, y, z).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vm.h"
#include "value.h"
#include "stdlib_noise.h"

/* ============================================================ */
/* Hash helpers (deterministic, seed-based)                      */
/* ============================================================ */

static uint32_t hash_u32(uint32_t a) {
    a = (a ^ 61) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2d;
    a = a ^ (a >> 15);
    return a;
}

static uint32_t hash_coords(uint32_t seed, int x, int y) {
    uint32_t h = seed;
    h ^= hash_u32((uint32_t)x + 0x9e3779b9u);
    h ^= hash_u32((uint32_t)y + 0x9e3779b9u + h);
    return hash_u32(h);
}

static uint32_t hash_coords_3d(uint32_t seed, int x, int y, int z) {
    uint32_t h = seed;
    h ^= hash_u32((uint32_t)x + 0x9e3779b9u);
    h ^= hash_u32((uint32_t)y + 0x9e3779b9u + h);
    h ^= hash_u32((uint32_t)z + 0x9e3779b9u + h);
    return hash_u32(h);
}

static float hash_float(uint32_t h) {
    return (float)(h & 0x7fffff) / (float)0x7fffff;
}

/* ============================================================ */
/* Shared math helpers                                           */
/* ============================================================ */

static float perlin_fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float perlin_lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/* ============================================================ */
/* Perlin 2D                                                     */
/* ============================================================ */

static float perlin_grad2d(int hash, float x, float y) {
    int h = hash & 3;
    float u = h < 2 ? x : y;
    float v = h < 2 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

static float perlin2d(uint32_t seed, float x, float y) {
    int X = (int)floorf(x) & 255;
    int Y = (int)floorf(y) & 255;

    float xf = x - floorf(x);
    float yf = y - floorf(y);

    float u = perlin_fade(xf);
    float v = perlin_fade(yf);

    uint32_t p00 = hash_coords(seed, X, Y);
    uint32_t p10 = hash_coords(seed, X + 1, Y);
    uint32_t p01 = hash_coords(seed, X, Y + 1);
    uint32_t p11 = hash_coords(seed, X + 1, Y + 1);

    float n00 = perlin_grad2d((int)p00, xf, yf);
    float n10 = perlin_grad2d((int)p10, xf - 1.0f, yf);
    float n01 = perlin_grad2d((int)p01, xf, yf - 1.0f);
    float n11 = perlin_grad2d((int)p11, xf - 1.0f, yf - 1.0f);

    float x1 = perlin_lerp(n00, n10, u);
    float x2 = perlin_lerp(n01, n11, u);

    return perlin_lerp(x1, x2, v);
}

static float perlin2d_01(uint32_t seed, float x, float y) {
    float v = perlin2d(seed, x, y);
    return (v + 1.0f) * 0.5f;
}

/* ============================================================ */
/* Perlin 3D                                                     */
/* ============================================================ */

static float grad3d(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static float perlin3d(uint32_t seed, float x, float y, float z) {
    int X = (int)floorf(x) & 255;
    int Y = (int)floorf(y) & 255;
    int Z = (int)floorf(z) & 255;

    float xf = x - floorf(x);
    float yf = y - floorf(y);
    float zf = z - floorf(z);

    float u = perlin_fade(xf);
    float v = perlin_fade(yf);
    float w = perlin_fade(zf);

    float n000 = grad3d((int)hash_coords_3d(seed, X,     Y,     Z),     xf,      yf,      zf);
    float n100 = grad3d((int)hash_coords_3d(seed, X + 1, Y,     Z),     xf - 1,  yf,      zf);
    float n010 = grad3d((int)hash_coords_3d(seed, X,     Y + 1, Z),     xf,      yf - 1,  zf);
    float n110 = grad3d((int)hash_coords_3d(seed, X + 1, Y + 1, Z),     xf - 1,  yf - 1,  zf);
    float n001 = grad3d((int)hash_coords_3d(seed, X,     Y,     Z + 1), xf,      yf,      zf - 1);
    float n101 = grad3d((int)hash_coords_3d(seed, X + 1, Y,     Z + 1), xf - 1,  yf,      zf - 1);
    float n011 = grad3d((int)hash_coords_3d(seed, X,     Y + 1, Z + 1), xf,      yf - 1,  zf - 1);
    float n111 = grad3d((int)hash_coords_3d(seed, X + 1, Y + 1, Z + 1), xf - 1,  yf - 1,  zf - 1);

    float nx00 = perlin_lerp(n000, n100, u);
    float nx10 = perlin_lerp(n010, n110, u);
    float nx01 = perlin_lerp(n001, n101, u);
    float nx11 = perlin_lerp(n011, n111, u);

    float nxy0 = perlin_lerp(nx00, nx10, v);
    float nxy1 = perlin_lerp(nx01, nx11, v);

    return perlin_lerp(nxy0, nxy1, w);
}

static float perlin3d_01(uint32_t seed, float x, float y, float z) {
    float v = perlin3d(seed, x, y, z);
    return (v + 1.0f) * 0.5f;
}

/* ============================================================ */
/* Simplex 2D                                                    */
/* ============================================================ */

static float simplex2d(uint32_t seed, float x, float y) {
    const float F2 = 0.5f * (sqrtf(3.0f) - 1.0f);
    const float G2 = (3.0f - sqrtf(3.0f)) / 6.0f;

    float s = (x + y) * F2;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);

    float t = (i + j) * G2;
    float X0 = i - t;
    float Y0 = j - t;
    float x0 = x - X0;
    float y0 = y - Y0;

    int i1, j1;
    if (x0 > y0) {
        i1 = 1; j1 = 0;
    } else {
        i1 = 0; j1 = 1;
    }

    float x1 = x0 - i1 + G2;
    float y1 = y0 - j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    uint32_t gi0 = hash_coords(seed, i, j);
    uint32_t gi1 = hash_coords(seed, i + i1, j + j1);
    uint32_t gi2 = hash_coords(seed, i + 1, j + 1);

    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 >= 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * perlin_grad2d((int)gi0, x0, y0);
    }

    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 >= 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * perlin_grad2d((int)gi1, x1, y1);
    }

    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 >= 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * perlin_grad2d((int)gi2, x2, y2);
    }

    return 70.0f * (n0 + n1 + n2);
}

static float simplex2d_01(uint32_t seed, float x, float y) {
    float v = simplex2d(seed, x, y);
    return (v + 1.0f) * 0.5f;
}

/* ============================================================ */
/* Simplex 3D                                                    */
/* ============================================================ */

static float simplex3d(uint32_t seed, float x, float y, float z) {
    const float F3 = 1.0f / 3.0f;
    const float G3 = 1.0f / 6.0f;

    float s = (x + y + z) * F3;
    int i = (int)floorf(x + s);
    int j = (int)floorf(y + s);
    int k = (int)floorf(z + s);

    float t = (i + j + k) * G3;
    float X0 = i - t;
    float Y0 = j - t;
    float Z0 = k - t;
    float x0 = x - X0;
    float y0 = y - Y0;
    float z0 = z - Z0;

    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0)       { i1=1; j1=0; k1=0; i2=1; j2=1; k2=0; }
        else if (x0 >= z0)  { i1=1; j1=0; k1=0; i2=1; j2=0; k2=1; }
        else                { i1=0; j1=0; k1=1; i2=1; j2=0; k2=1; }
    } else {
        if (y0 < z0)        { i1=0; j1=0; k1=1; i2=0; j2=1; k2=1; }
        else if (x0 < z0)   { i1=0; j1=1; k1=0; i2=0; j2=1; k2=1; }
        else                { i1=0; j1=1; k1=0; i2=1; j2=1; k2=0; }
    }

    float x1 = x0 - i1 + G3;
    float y1 = y0 - j1 + G3;
    float z1 = z0 - k1 + G3;
    float x2 = x0 - i2 + 2.0f * G3;
    float y2 = y0 - j2 + 2.0f * G3;
    float z2 = z0 - k2 + 2.0f * G3;
    float x3 = x0 - 1.0f + 3.0f * G3;
    float y3 = y0 - 1.0f + 3.0f * G3;
    float z3 = z0 - 1.0f + 3.0f * G3;

    uint32_t gi0 = hash_coords_3d(seed, i, j, k);
    uint32_t gi1 = hash_coords_3d(seed, i + i1, j + j1, k + k1);
    uint32_t gi2 = hash_coords_3d(seed, i + i2, j + j2, k + k2);
    uint32_t gi3 = hash_coords_3d(seed, i + 1, j + 1, k + 1);

    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;

    float t0 = 0.6f - x0*x0 - y0*y0 - z0*z0;
    if (t0 >= 0.0f) { t0 *= t0; n0 = t0 * t0 * grad3d((int)gi0, x0, y0, z0); }

    float t1 = 0.6f - x1*x1 - y1*y1 - z1*z1;
    if (t1 >= 0.0f) { t1 *= t1; n1 = t1 * t1 * grad3d((int)gi1, x1, y1, z1); }

    float t2 = 0.6f - x2*x2 - y2*y2 - z2*z2;
    if (t2 >= 0.0f) { t2 *= t2; n2 = t2 * t2 * grad3d((int)gi2, x2, y2, z2); }

    float t3 = 0.6f - x3*x3 - y3*y3 - z3*z3;
    if (t3 >= 0.0f) { t3 *= t3; n3 = t3 * t3 * grad3d((int)gi3, x3, y3, z3); }

    return 32.0f * (n0 + n1 + n2 + n3);
}

static float simplex3d_01(uint32_t seed, float x, float y, float z) {
    float v = simplex3d(seed, x, y, z);
    return (v + 1.0f) * 0.5f;
}

/* ============================================================ */
/* Voronoi 2D (cellular / Worley)                                */
/* ============================================================ */

static float voronoi2d(uint32_t seed, float x, float y, float cell_size) {
    if (cell_size <= 0.0f) cell_size = 1.0f;

    float cx = x / cell_size;
    float cy = y / cell_size;

    int icx = (int)floorf(cx);
    int icy = (int)floorf(cy);

    float min_dist = 1e10f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cell_x = icx + dx;
            int cell_y = icy + dy;

            uint32_t h = hash_coords(seed, cell_x, cell_y);
            float off_x = hash_float(h);
            float off_y = hash_float(hash_u32(h + 1));

            float fx = (cell_x + off_x) * cell_size;
            float fy = (cell_y + off_y) * cell_size;

            float dx_ = x - fx;
            float dy_ = y - fy;
            float dist = dx_*dx_ + dy_*dy_;

            if (dist < min_dist) {
                min_dist = dist;
            }
        }
    }

    float max_possible = cell_size * cell_size * 2.0f;
    return sqrtf(min_dist) / sqrtf(max_possible);
}

static float voronoi_edge2d(uint32_t seed, float x, float y, float cell_size) {
    if (cell_size <= 0.0f) cell_size = 1.0f;

    float cx = x / cell_size;
    float cy = y / cell_size;

    int icx = (int)floorf(cx);
    int icy = (int)floorf(cy);

    float min_dist = 1e10f;
    float min_dist2 = 1e10f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cell_x = icx + dx;
            int cell_y = icy + dy;

            uint32_t h = hash_coords(seed, cell_x, cell_y);
            float off_x = hash_float(h);
            float off_y = hash_float(hash_u32(h + 1));

            float fx = (cell_x + off_x) * cell_size;
            float fy = (cell_y + off_y) * cell_size;

            float dist = sqrtf((x - fx) * (x - fx) + (y - fy) * (y - fy));

            if (dist < min_dist) {
                min_dist2 = min_dist;
                min_dist = dist;
            } else if (dist < min_dist2) {
                min_dist2 = dist;
            }
        }
    }

    float diff = min_dist2 - min_dist;
    float max_possible = cell_size * 1.41421356f;
    return diff / max_possible;
}

/* ============================================================ */
/* Voronoi 3D                                                    */
/* ============================================================ */

static float voronoi3d(uint32_t seed, float x, float y, float z, float cell_size) {
    if (cell_size <= 0.0f) cell_size = 1.0f;

    float cx = x / cell_size;
    float cy = y / cell_size;
    float cz = z / cell_size;

    int icx = (int)floorf(cx);
    int icy = (int)floorf(cy);
    int icz = (int)floorf(cz);

    float min_dist = 1e10f;

    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cell_x = icx + dx;
                int cell_y = icy + dy;
                int cell_z = icz + dz;

                uint32_t h = hash_coords_3d(seed, cell_x, cell_y, cell_z);
                float off_x = hash_float(h);
                float off_y = hash_float(hash_u32(h + 1));
                float off_z = hash_float(hash_u32(h + 2));

                float fx = (cell_x + off_x) * cell_size;
                float fy = (cell_y + off_y) * cell_size;
                float fz = (cell_z + off_z) * cell_size;

                float dx_ = x - fx;
                float dy_ = y - fy;
                float dz_ = z - fz;
                float dist = dx_*dx_ + dy_*dy_ + dz_*dz_;

                if (dist < min_dist) {
                    min_dist = dist;
                }
            }
        }
    }

    float max_possible = cell_size * cell_size * 3.0f;
    return sqrtf(min_dist) / sqrtf(max_possible);
}

static float voronoi_edge3d(uint32_t seed, float x, float y, float z, float cell_size) {
    if (cell_size <= 0.0f) cell_size = 1.0f;

    float cx = x / cell_size;
    float cy = y / cell_size;
    float cz = z / cell_size;

    int icx = (int)floorf(cx);
    int icy = (int)floorf(cy);
    int icz = (int)floorf(cz);

    float min_dist = 1e10f;
    float min_dist2 = 1e10f;

    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cell_x = icx + dx;
                int cell_y = icy + dy;
                int cell_z = icz + dz;

                uint32_t h = hash_coords_3d(seed, cell_x, cell_y, cell_z);
                float off_x = hash_float(h);
                float off_y = hash_float(hash_u32(h + 1));
                float off_z = hash_float(hash_u32(h + 2));

                float fx = (cell_x + off_x) * cell_size;
                float fy = (cell_y + off_y) * cell_size;
                float fz = (cell_z + off_z) * cell_size;

                float dist = sqrtf((x-fx)*(x-fx) + (y-fy)*(y-fy) + (z-fz)*(z-fz));

                if (dist < min_dist) {
                    min_dist2 = min_dist;
                    min_dist = dist;
                } else if (dist < min_dist2) {
                    min_dist2 = dist;
                }
            }
        }
    }

    float diff = min_dist2 - min_dist;
    float max_possible = cell_size * 1.73205081f;
    return diff / max_possible;
}

/* ============================================================ */
/* Class helpers                                                 */
/* ============================================================ */

static void class_add_native_method(ObjClass *cls, const char *name, NativeFn fn) {
    ObjFunction *f = new_native_function(name, fn);
    if (cls->method_count >= cls->method_capacity) {
        int new_cap = cls->method_capacity < 4 ? 4 : cls->method_capacity * 2;
        cls->methods = realloc(cls->methods, sizeof(ObjFunction*) * new_cap);
        cls->method_names = realloc(cls->method_names, sizeof(char*) * new_cap);
        cls->method_capacity = new_cap;
    }
    cls->method_names[cls->method_count] = strdup(name);
    cls->methods[cls->method_count] = f;
    retain_obj((Object*)f);
    cls->method_count++;
}

/* ============================================================ */
/* Perlin class                                                  */
/* ============================================================ */

static Value perlin_init(VM *vm, Value *args, int n) {
    (void)vm;
    uint32_t seed = 0;
    if (n >= 2 && (IS_INT(args[1]) || IS_DOUBLE(args[1]))) {
        seed = (uint32_t)value_to_double(args[1]);
    }
    instance_set_field((ObjInstance*)AS_OBJ(args[0]), "_seed", make_int((int32_t)seed));
    return args[0];
}

static Value perlin_sample(VM *vm, Value *args, int n) {
    if (n < 3) {
        luna_throw(vm, vm->argument_error_class,
            "perlin.sample requires at least (x, y)");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value seed_val = instance_get_field(inst, "_seed");
    uint32_t seed = (uint32_t)(IS_INT(seed_val) ? AS_INT(seed_val) : 0);
    float x = (float)value_to_double(args[1]);
    float y = (float)value_to_double(args[2]);
    if (n >= 4) {
        float z = (float)value_to_double(args[3]);
        return make_double((double)perlin3d_01(seed, x, y, z));
    }
    return make_double((double)perlin2d_01(seed, x, y));
}

/* ============================================================ */
/* Simplex class                                                 */
/* ============================================================ */

static Value simplex_init(VM *vm, Value *args, int n) {
    (void)vm;
    uint32_t seed = 0;
    if (n >= 2 && (IS_INT(args[1]) || IS_DOUBLE(args[1]))) {
        seed = (uint32_t)value_to_double(args[1]);
    }
    instance_set_field((ObjInstance*)AS_OBJ(args[0]), "_seed", make_int((int32_t)seed));
    return args[0];
}

static Value simplex_sample(VM *vm, Value *args, int n) {
    if (n < 3) {
        luna_throw(vm, vm->argument_error_class,
            "simplex.sample requires at least (x, y)");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value seed_val = instance_get_field(inst, "_seed");
    uint32_t seed = (uint32_t)(IS_INT(seed_val) ? AS_INT(seed_val) : 0);
    float x = (float)value_to_double(args[1]);
    float y = (float)value_to_double(args[2]);
    if (n >= 4) {
        float z = (float)value_to_double(args[3]);
        return make_double((double)simplex3d_01(seed, x, y, z));
    }
    return make_double((double)simplex2d_01(seed, x, y));
}

/* ============================================================ */
/* Voronoi class                                                 */
/* ============================================================ */

static Value voronoi_init(VM *vm, Value *args, int n) {
    (void)vm;
    uint32_t seed = 0;
    float cell_size = 1.0f;
    if (n >= 2 && (IS_INT(args[1]) || IS_DOUBLE(args[1]))) {
        seed = (uint32_t)value_to_double(args[1]);
    }
    if (n >= 3 && (IS_INT(args[2]) || IS_DOUBLE(args[2]))) {
        cell_size = (float)value_to_double(args[2]);
        if (cell_size <= 0.0f) cell_size = 1.0f;
    }
    instance_set_field((ObjInstance*)AS_OBJ(args[0]), "_seed", make_int((int32_t)seed));
    instance_set_field((ObjInstance*)AS_OBJ(args[0]), "_cell_size", make_double((double)cell_size));
    return args[0];
}

static Value voronoi_sample(VM *vm, Value *args, int n) {
    if (n < 3) {
        luna_throw(vm, vm->argument_error_class,
            "voronoi.sample requires at least (x, y)");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value seed_val = instance_get_field(inst, "_seed");
    uint32_t seed = (uint32_t)(IS_INT(seed_val) ? AS_INT(seed_val) : 0);
    Value cs_val = instance_get_field(inst, "_cell_size");
    float cell_size = IS_INT(cs_val) ? (float)AS_INT(cs_val) :
                      IS_DOUBLE(cs_val) ? (float)AS_DOUBLE(cs_val) : 1.0f;
    if (cell_size <= 0.0f) cell_size = 1.0f;
    float x = (float)value_to_double(args[1]);
    float y = (float)value_to_double(args[2]);
    if (n >= 4) {
        float z = (float)value_to_double(args[3]);
        return make_double((double)voronoi3d(seed, x, y, z, cell_size));
    }
    return make_double((double)voronoi2d(seed, x, y, cell_size));
}

static Value voronoi_edge(VM *vm, Value *args, int n) {
    if (n < 3) {
        luna_throw(vm, vm->argument_error_class,
            "voronoi.edge requires at least (x, y)");
    }
    ObjInstance *inst = (ObjInstance*)AS_OBJ(args[0]);
    Value seed_val = instance_get_field(inst, "_seed");
    uint32_t seed = (uint32_t)(IS_INT(seed_val) ? AS_INT(seed_val) : 0);
    Value cs_val = instance_get_field(inst, "_cell_size");
    float cell_size = IS_INT(cs_val) ? (float)AS_INT(cs_val) :
                      IS_DOUBLE(cs_val) ? (float)AS_DOUBLE(cs_val) : 1.0f;
    if (cell_size <= 0.0f) cell_size = 1.0f;
    float x = (float)value_to_double(args[1]);
    float y = (float)value_to_double(args[2]);
    if (n >= 4) {
        float z = (float)value_to_double(args[3]);
        return make_double((double)voronoi_edge3d(seed, x, y, z, cell_size));
    }
    return make_double((double)voronoi_edge2d(seed, x, y, cell_size));
}

/* ============================================================ */
/* Module registration                                           */
/* ============================================================ */

void vm_register_noise_module(VM *vm) {
    ObjClass *perlin_class = new_class("Perlin", NULL);
    class_add_native_method(perlin_class, "_init",  perlin_init);
    class_add_native_method(perlin_class, "sample", perlin_sample);

    ObjClass *simplex_class = new_class("Simplex", NULL);
    class_add_native_method(simplex_class, "_init",  simplex_init);
    class_add_native_method(simplex_class, "sample", simplex_sample);

    ObjClass *voronoi_class = new_class("Voronoi", NULL);
    class_add_native_method(voronoi_class, "_init",  voronoi_init);
    class_add_native_method(voronoi_class, "sample", voronoi_sample);
    class_add_native_method(voronoi_class, "edge",   voronoi_edge);

    ObjModule *mod = new_module("noise");

    dict_set(mod->exports,
             make_obj((Object*)new_string("Perlin", 6)),
             make_obj((Object*)perlin_class));

    dict_set(mod->exports,
             make_obj((Object*)new_string("Simplex", 7)),
             make_obj((Object*)simplex_class));

    dict_set(mod->exports,
             make_obj((Object*)new_string("Voronoi", 7)),
             make_obj((Object*)voronoi_class));

    dict_set(vm->module_cache,
             make_obj((Object*)new_string("noise", 5)),
             make_obj((Object*)mod));
}
