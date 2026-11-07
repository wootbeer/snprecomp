// Interpolation IDs, so RT64 pairs each matrix with the same one last frame
// instead of guessing (alike models swapped parts on in-between frames).
//
// Matrices are recorded as they're built (hooks in snp.jp.toml): a skeleton's
// limbs by joint and mesh, any other matrix as an object of the kind its
// caller makes. Each skeleton and object is an instance; after a display list
// has been walked (src/game/display_list.cpp), the instances it draws are
// numbered by matching them to those of their kind drawn last time, and the
// number goes into their IDs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace {
    uint32_t current_joint = 0;
    uint32_t generation = 0;

    // A skeleton or object as drawn: its kind, where its first matrix puts it
    // and (objects only) its normalized axes.
    struct Instance {
        uint32_t key = 0;
        float pos[3] = {};
        float axes[3][3] = {};
        // Objects: where the display list puts it, its matrix combined with those it's drawn
        // under (src/game/display_list.cpp), for telling apart alike ones at the same pos.
        float place[3] = {};
        // Objects: how much its matrix scales (its axes' mean length; 0 for a skeleton or an
        // unscaled-looking one).
        float scale = 0.0f;
        uint32_t number = 0;
        uint32_t built = 0; // generation
    };
    std::unordered_map<uint32_t, Instance> instances; // serial -> instance
    uint32_t current_instance = 0;
    uint32_t first_pose = 0; // the current instance's first pose entry
    uint32_t current_kind = 0;
    constexpr uint32_t object_serials = 0x80000000; // objects' serials, apart from skeletons'
    uint32_t object_serial = 0;
    // The matrix snp_interp_matrix just recorded (guMtxF2L builds it next).
    uint32_t recorded_matrix = 0;

    void set_pos(Instance& inst, uint32_t x, uint32_t y, uint32_t z) {
        memcpy(&inst.pos[0], &x, 4);
        memcpy(&inst.pos[1], &y, 4);
        memcpy(&inst.pos[2], &z, 4);
        for (float& f : inst.pos) {
            if (!std::isfinite(f)) f = 0.0f;
        }
    }

    struct Entry {
        uint32_t id;
        uint32_t generation;
        uint32_t instance;
    };
    // Matrix address -> ID and instance.
    std::unordered_map<uint32_t, Entry> matrices;

    // Per kind, the instances drawn in the latest display list that had any.
    struct Placed {
        float pos[3];
        float axes[3][3];
        float vel[3]; // since the time before
        float place[3];
        float scale;
        float growth; // scale change since the time before, a factor
        uint32_t number;
    };
    struct Kind {
        std::vector<Placed> placed;
        uint32_t generation = 0; // when last drawn
    };
    std::unordered_map<uint32_t, Kind> kinds;

    // Every object drawn in the latest display list: its kind and pos. An object can be drawn
    // with another model from one frame to the next (the city stage's cones: two models, each
    // spot drawn with either), so one that's now exactly where another kind's was is that one
    // drawn anew, not one of its own kind come from elsewhere.
    struct ObjectPlace {
        uint32_t kind;
        float pos[3];
    };
    std::vector<ObjectPlace> last_object_places;
    constexpr float same_spot = 1.0f;

    // The kind of object last time exactly at pos, if none of `kind` was (0: none, or one of
    // its own kind was).
    uint32_t another_kind_at(uint32_t kind, const float* pos) {
        uint32_t other = 0;
        for (const ObjectPlace& p : last_object_places) {
            float dx = p.pos[0] - pos[0], dy = p.pos[1] - pos[1], dz = p.pos[2] - pos[2];
            if (dx * dx + dy * dy + dz * dz <= same_spot * same_spot) {
                if (p.kind == kind) {
                    return 0;
                }
                other = p.kind;
            }
        }
        return other;
    }

    // Kinds of object that are one kind drawn with different models (the cones' two frames):
    // numbered and given IDs together, so one keeps its number whichever it's drawn with, also
    // while it moves (a cone kicked into the air). Two kinds are joined once objects of each
    // have taken over the other's spot a few times; a single such meeting (something spawned
    // where another thing just was) joins nothing.
    std::unordered_map<uint32_t, uint32_t> family_parent;
    std::unordered_map<uint64_t, uint32_t> takeovers; // by pair of kinds
    constexpr uint32_t takeovers_to_join = 3;

    uint32_t family(uint32_t kind) {
        auto it = family_parent.find(kind);
        while (it != family_parent.end() && it->second != kind) {
            kind = it->second;
            it = family_parent.find(kind);
        }
        return kind;
    }

    void note_takeover(uint32_t a, uint32_t b) {
        a = family(a);
        b = family(b);
        if (a == b) {
            return;
        }
        const uint64_t pair = a < b ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a);
        if (++takeovers[pair] >= takeovers_to_join) {
            family_parent[std::max(a, b)] = std::min(a, b);
            family_parent[std::min(a, b)] = std::min(a, b);
        }
    }

    // SNP_INTERP_LOG=1 (on Android: adb shell setprop debug.snp.interplog 1, or a file
    // snp-interp-log in the app's storage, then start the game) writes snp_interp.log in the
    // working directory (on Android the app's storage): for each display list, how old the
    // instances it draws are, and how each kind of object with more than one instance drawn
    // was numbered. For finding why alike objects trade places.
    FILE* interp_log() {
        static FILE* file = [] {
            const char* v = std::getenv("SNP_INTERP_LOG");
            bool on = v != nullptr && v[0] == '1';
#if defined(__ANDROID__)
            char prop[PROP_VALUE_MAX] = "";
            on = on || (__system_property_get("debug.snp.interplog", prop) > 0 && prop[0] == '1');
            // Or a file of that name in the app's storage (adb shell run-as <package> touch
            // files/snp-interp-log), in case the property can't be read.
            if (FILE* flag = std::fopen("snp-interp-log", "r")) {
                std::fclose(flag);
                on = true;
            }
#endif
            FILE* f = on ? std::fopen("snp_interp.log", "w") : nullptr;
            if (on) {
                std::fprintf(stderr, "Interpolation log: %s\n", f != nullptr ? "on, snp_interp.log" : "couldn't create snp_interp.log");
            }
            return f;
        }();
        // 50 MB at most.
        if (file != nullptr && std::ftell(file) > 50L * 1024 * 1024) {
            std::fclose(file);
            file = nullptr;
        }
        return file;
    }

    // For the log: this display list's matrix lookups (interp_matrix_id) by outcome.
    struct {
        uint32_t found[3];    // recorded 0, 1, 2 lists ago
        uint32_t stale[4];    // 3, 4, 5, 6+ lists ago: left automatic
        uint32_t unrecorded;  // no record of the matrix: automatic
    } lookups = {};

    float distance2(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // How differently two objects are turned, 0 (alike) to 6, as RT64 measures
    // it; 0 if either has no axes (a skeleton).
    float orientation_difference(const float (&a)[3][3], const float (&b)[3][3]) {
        auto none = [](const float (&m)[3][3]) { return m[0][0] == 0.0f && m[0][1] == 0.0f && m[0][2] == 0.0f; };
        if (none(a) || none(b)) {
            return 0.0f;
        }
        float d = 0.0f;
        for (int i = 0; i < 3; i++) {
            d += 1.0f - (a[i][0] * b[i][0] + a[i][1] * b[i][1] + a[i][2] * b[i][2]);
        }
        return d;
    }

    // Number one kind's instances in a display list. Pairs of one now and one
    // last time are taken closest first, each taking that one's number; the
    // rest get new numbers. Closeness is to where its last movement would take
    // the one from last time (fast rows of alike objects), plus a little for
    // how differently they're turned, up to 24 units: enough to tell alike
    // layers in one place apart, not to outweigh a real distance (spinning
    // parts, turned anew every frame, swapped numbers 80 units apart and
    // blended into each other in the intro).
    void number_kind(std::vector<Placed>& placed, const std::vector<Instance*>& now, bool objects, FILE* log) {
        struct Pair {
            float d;
            float place_d; // breaks ties of d
            uint32_t now;
            uint32_t before;
        };
        // RT64 doesn't pair matrices further apart than this either.
        const float max_distance = 256.0f;
        std::vector<Pair> pairs;
        for (uint32_t i = 0; i < now.size(); i++) {
            // Taken over from an object of another kind (not yet known to be one with it): a
            // new number, drawn where it is.
            if (objects && another_kind_at(now[i]->key, now[i]->pos) != 0) {
                if (log != nullptr) {
                    std::fprintf(log, "  (at (%.1f %.1f %.1f): another kind's spot last time)\n", now[i]->pos[0], now[i]->pos[1], now[i]->pos[2]);
                }
                continue;
            }
            for (uint32_t j = 0; j < placed.size(); j++) {
                if (distance2(now[i]->pos, placed[j].pos) > max_distance * max_distance) {
                    continue;
                }
                const Placed& p = placed[j];
                const float expected[3] = { p.pos[0] + p.vel[0], p.pos[1] + p.vel[1], p.pos[2] + p.vel[2] };
                const float turn = 4.0f * orientation_difference(now[i]->axes, p.axes);
                // How differently they're scaled, from where the last growth would take the one
                // from last time, 64 units a doubling: an explosion is alike sphere layers at one
                // place, each its own size, and by place alone they swapped and pulsed on
                // in-between frames (issue 19). One more than 3 times larger or smaller than the
                // one from last time is something else (a new explosion where one just ended:
                // it showed up huge and shrank).
                float size = 0.0f;
                if (now[i]->scale > 0.0f && p.scale > 0.0f) {
                    if (std::fabs(std::log(now[i]->scale / p.scale)) > std::log(3.0f)) {
                        continue;
                    }
                    size = 64.0f / std::log(2.0f) * std::fabs(std::log(now[i]->scale / (p.scale * p.growth)));
                }
                pairs.push_back({ distance2(now[i]->pos, expected) + turn * turn + size * size, distance2(now[i]->place, p.place), i, j });
            }
        }
        // Stable, so equal distances keep the order they were listed in (now, then before):
        // std::sort leaves ties to the library, and Android's (libc++) put them differently
        // from the desktop ones'.
        // Alike objects under different parents can be at exactly the same pos in their own
        // matrices (the city stage's cones): then where the display list puts them decides, or
        // their numbers went by drawing order, and swapped when that changed.
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) {
            return x.d < y.d || (x.d == y.d && x.place_d < y.place_d);
        });
        std::vector<bool> numbered(now.size(), false);
        std::vector<bool> before_used(placed.size(), false);
        std::vector<Placed> next(now.size());
        for (uint32_t i = 0; i < now.size(); i++) {
            memcpy(next[i].pos, now[i]->pos, sizeof(next[i].pos));
            memcpy(next[i].axes, now[i]->axes, sizeof(next[i].axes));
            memcpy(next[i].place, now[i]->place, sizeof(next[i].place));
            memset(next[i].vel, 0, sizeof(next[i].vel));
            next[i].scale = now[i]->scale;
            next[i].growth = 1.0f;
        }
        for (const Pair& pair : pairs) {
            if (numbered[pair.now] || before_used[pair.before]) {
                continue;
            }
            numbered[pair.now] = true;
            before_used[pair.before] = true;
            now[pair.now]->number = placed[pair.before].number;
            if (log != nullptr) {
                std::fprintf(log, "  #%u at (%.1f %.1f %.1f) <- (%.1f %.1f %.1f) d %.1f, scale %.3f <- %.3f, placed (%.1f %.1f %.1f) <- (%.1f %.1f %.1f)\n",
                    now[pair.now]->number,
                    now[pair.now]->pos[0], now[pair.now]->pos[1], now[pair.now]->pos[2],
                    placed[pair.before].pos[0], placed[pair.before].pos[1], placed[pair.before].pos[2], std::sqrt(pair.d),
                    now[pair.now]->scale, placed[pair.before].scale,
                    now[pair.now]->place[0], now[pair.now]->place[1], now[pair.now]->place[2],
                    placed[pair.before].place[0], placed[pair.before].place[1], placed[pair.before].place[2]);
            }
            for (int k = 0; k < 3; k++) {
                next[pair.now].vel[k] = now[pair.now]->pos[k] - placed[pair.before].pos[k];
            }
            if (now[pair.now]->scale > 0.0f && placed[pair.before].scale > 0.0f) {
                next[pair.now].growth = std::clamp(now[pair.now]->scale / placed[pair.before].scale, 0.5f, 2.0f);
            }
        }
        auto used = [&](uint32_t n) {
            for (const Placed& p : placed) {
                if (p.number == n) return true;
            }
            for (uint32_t i = 0; i < now.size(); i++) {
                if (numbered[i] && now[i]->number == n) return true;
            }
            return false;
        };
        for (uint32_t i = 0; i < now.size(); i++) {
            if (!numbered[i]) {
                uint32_t n = 0;
                while (used(n)) n++;
                now[i]->number = n;
                numbered[i] = true;
                if (log != nullptr) {
                    std::fprintf(log, "  #%u at (%.1f %.1f %.1f) new, scale %.3f, placed (%.1f %.1f %.1f)\n", n,
                        now[i]->pos[0], now[i]->pos[1], now[i]->pos[2], now[i]->scale, now[i]->place[0], now[i]->place[1], now[i]->place[2]);
                }
            }
            next[i].number = now[i]->number;
        }
        placed.swap(next);
    }
}

// The skeleton walkers' entry points: a new instance, its kind from the
// skeleton argument. Not the animation: a character changing animations
// (Saki starting and stopping a strafe) stays the same kind, and keeps its
// number and IDs, so RT64 keeps interpolating it.
extern "C" void snp_interp_instance(uint32_t skeleton, uint32_t) {
    current_instance++;
    first_pose = 0;
    uint64_t h = (uint64_t)skeleton * 0x9E3779B97F4A7C15ull;
    current_kind = (uint32_t)(h >> 32) | 1;
}

// A joint's index in its skeleton, from its pose entry (0x20 bytes each),
// counted from the instance's first one (its root, walked first), plus one
// (0 is no joint). Not the pose entry's address: the entries move when the
// animation changes (Saki's whenever she starts or stops strafing), and new
// IDs left RT64 nothing to pair, so she snapped into place each time. Not
// the order meshes are drawn in either: a skeleton that leaves out a part
// (the intro's room, one skeleton) shifted the rest, and its crates blended
// into each other.
static uint32_t joint_index(uint32_t pose) {
    if (first_pose == 0) {
        first_pose = pose;
    }
    return (uint32_t)((int32_t)(pose - first_pose) / 0x20) + 1;
}

// func_80032F2C, around drawing a joint's meshes: its pose entry, or 0 after.
extern "C" void snp_interp_joint(uint32_t joint) {
    current_joint = joint != 0 ? joint_index(joint) : 0;
}

// func_80033758 (the other skeleton walker), the same (its slot in the
// animation channels moves along with the pose entry).
extern "C" void snp_interp_joint2(uint32_t pose, uint32_t) {
    current_joint = joint_index(pose) ^ 0x80000000u;
}

// func_80032AA0, as it builds a mesh's matrix: the matrix, the mesh and the
// float matrix's translation.
extern "C" void snp_interp_matrix(uint32_t mtx, uint32_t mesh, uint32_t x, uint32_t y, uint32_t z) {
    if (current_joint == 0) {
        return;
    }
    // A joint's key: its index and its mesh.
    uint64_t h = ((uint64_t)current_joint << 32 | mesh) * 0x9E3779B97F4A7C15ull;
    uint32_t key = (uint32_t)(h >> 33);
    Instance& inst = instances[current_instance];
    if (inst.key == 0) {
        inst.key = current_kind != 0 ? current_kind : (key | 1);
        inst.built = generation;
        set_pos(inst, x, y, z);
    }
    matrices[mtx & 0x1FFFFFFF] = { key, generation, current_instance };
    recorded_matrix = mtx & 0x1FFFFFFF;
}

// guMtxF2L, as it builds any matrix: the float matrix, the matrix and its
// caller. A matrix not just recorded as a skeleton's is an object of its own,
// its kind the caller.
extern "C" void snp_interp_object_matrix(uint8_t* rdram, uint32_t fmtx, uint32_t mtx, uint32_t ra) {
    mtx &= 0x1FFFFFFF;
    if (mtx == recorded_matrix) {
        recorded_matrix = 0;
        return;
    }
    auto word = [&](uint32_t offset) { return *(uint32_t*)(rdram + ((fmtx + offset) & 0x1FFFFFFF)); };
    uint32_t serial = object_serials | (++object_serial & 0x7FFFFFFF);
    uint32_t kind = (uint32_t)((ra * 0x9E3779B97F4A7C15ull) >> 32) | 0x80000001u;
    Instance& inst = instances[serial];
    inst = Instance();
    inst.key = kind;
    inst.built = generation;
    set_pos(inst, word(0x30), word(0x34), word(0x38));
    float scale_sum = 0.0f;
    for (int i = 0; i < 3; i++) {
        float len2 = 0.0f;
        for (int j = 0; j < 3; j++) {
            uint32_t w = word(i * 0x10 + j * 4);
            memcpy(&inst.axes[i][j], &w, 4);
            if (!std::isfinite(inst.axes[i][j])) inst.axes[i][j] = 0.0f;
            len2 += inst.axes[i][j] * inst.axes[i][j];
        }
        float scale = len2 > 0.0f ? 1.0f / std::sqrt(len2) : 0.0f;
        for (int j = 0; j < 3; j++) {
            inst.axes[i][j] *= scale;
        }
        scale_sum += std::sqrt(len2);
    }
    inst.scale = std::isfinite(scale_sum) ? scale_sum / 3.0f : 0.0f;
    matrices[mtx] = { kind >> 1, generation, serial };
}

namespace snp {
    // Once per graphics task, before its display list is walked. A frame's
    // matrices are built up to two tasks earlier and the matrix pools are
    // reused, so older entries belong to some other matrix now.
    void interp_new_frame() {
        generation++;
        if (matrices.size() > 4096) {
            for (auto it = matrices.begin(); it != matrices.end();) {
                it = (generation - it->second.generation > 3) ? matrices.erase(it) : std::next(it);
            }
        }
        if (instances.size() > 4096) {
            for (auto it = instances.begin(); it != instances.end();) {
                it = (generation - it->second.built > 4) ? instances.erase(it) : std::next(it);
            }
        }
    }

    // The ID recorded for the matrix at addr, or 0xFFFFFFFF (automatic), its
    // instance (0 if none) and whether that's an object's rather than a
    // skeleton's.
    uint32_t interp_matrix_id(uint32_t addr, uint32_t& instance, bool& object) {
        instance = 0;
        object = false;
        auto it = matrices.find(addr & 0x1FFFFFFF);
        if (it == matrices.end()) {
            lookups.unrecorded++;
            return 0xFFFFFFFF;
        }
        const uint32_t age = generation - it->second.generation;
        if (age > 2) {
            lookups.stale[std::min<uint32_t>(age - 3, 3)]++;
            return 0xFFFFFFFF;
        }
        lookups.found[age]++;
        // SNP_INTERP_INSTANCES=0 turns instance numbering off, for comparing.
        static const bool instances_off = [] { const char* v = std::getenv("SNP_INTERP_INSTANCES"); return v && v[0] == '0'; }();
        auto inst = instances.find(it->second.instance);
        object = (it->second.instance & object_serials) != 0;
        if (!instances_off && inst != instances.end() && inst->second.key != 0) {
            instance = it->second.instance;
        }
        else if (object) {
            return 0xFFFFFFFF; // an object's ID is only of use with its number
        }
        return it->second.id;
    }

    // Once a display list has been walked, before its instances are numbered: logs its matrix
    // lookups, how many matrix loads didn't fit the tag area (left untagged), and how many kept
    // ones called a tag entry since reused for another load (restored from the record).
    void interp_walk_done(uint32_t untagged, uint32_t reused_entries) {
        if (FILE* log = interp_log()) {
            // Quitting ends the process without flushing files, so the last display list's
            // lines go out now.
            std::fflush(log);
            std::fprintf(log, "generation %u: lookups found 0/1/2 lists ago %u %u %u, stale 3/4/5/6+ %u %u %u %u, "
                "unrecorded %u, untagged %u, reused entries %u\n", generation,
                lookups.found[0], lookups.found[1], lookups.found[2],
                lookups.stale[0], lookups.stale[1], lookups.stale[2], lookups.stale[3],
                lookups.unrecorded, untagged, reused_entries);
        }
        lookups = {};
    }

    // Once a display list has been walked: number the instances it draws.
    void interp_number_instances(const std::vector<uint32_t>& drawn) {
        std::unordered_map<uint32_t, std::vector<Instance*>> by_kind;
        std::unordered_map<uint32_t, bool> object_kind; // kinds of objects, not skeletons
        FILE* log = interp_log();
        uint32_t ages[4] = {};
        for (uint32_t serial : drawn) {
            auto it = instances.find(serial);
            if (it != instances.end()) {
                if (serial & object_serials) {
                    // Its model's kind (interp_set_object_model) to its family's.
                    Instance& inst = it->second;
                    if (uint32_t other = another_kind_at(family(inst.key), inst.pos)) {
                        note_takeover(inst.key, other);
                    }
                    inst.key = family(inst.key);
                }
                by_kind[it->second.key].push_back(&it->second);
                object_kind[it->second.key] = (serial & object_serials) != 0;
                ages[std::min<uint32_t>(generation - it->second.built, 3)]++;
            }
        }
        if (log != nullptr) {
            std::fprintf(log, " instances %zu, built 0/1/2/3+ lists ago: %u %u %u %u\n",
                drawn.size(), ages[0], ages[1], ages[2], ages[3]);
        }
        for (auto& kind : by_kind) {
            // Ones not drawn for a while are gone: they'd keep their numbers
            // from the ones drawn now.
            Kind& k = kinds[kind.first];
            if (generation - k.generation > 2) {
                k.placed.clear();
            }
            k.generation = generation;
            const bool objects = object_kind[kind.first];
            FILE* kind_log = (log != nullptr && objects && kind.second.size() > 1) ? log : nullptr;
            if (kind_log != nullptr) {
                std::fprintf(kind_log, " kind %08X: %zu now, %zu before\n", kind.first, kind.second.size(), k.placed.size());
            }
            number_kind(k.placed, kind.second, objects, kind_log);
        }
        last_object_places.clear();
        for (auto& kind : by_kind) {
            if (!object_kind[kind.first]) {
                continue;
            }
            for (const Instance* inst : kind.second) {
                last_object_places.push_back({ kind.first, { inst->pos[0], inst->pos[1], inst->pos[2] } });
            }
        }
    }

    // An object's kind from the model its matrix draws (the first vertices or
    // display list after the load): its caller can't tell them apart (the
    // recompiled code doesn't keep $ra), and the jets, debris and Saki's
    // surroundings all paired by closeness, wrongly at times.
    void interp_set_object_model(uint32_t instance, uint32_t model) {
        auto it = instances.find(instance);
        if (it == instances.end() || !(instance & object_serials) || model == 0) {
            return;
        }
        it->second.key = (uint32_t)(((uint64_t)model * 0x9E3779B97F4A7C15ull) >> 32) | 0x80000001u;
    }

    // Where the display list puts an object (its matrix combined with those it's drawn under).
    void interp_set_object_place(uint32_t instance, const float (&place)[3]) {
        auto it = instances.find(instance);
        if (it == instances.end() || !(instance & object_serials)) {
            return;
        }
        for (int i = 0; i < 3; i++) {
            it->second.place[i] = std::isfinite(place[i]) ? place[i] : 0.0f;
        }
    }

    // The ID with its instance's number in it (an object's with its kind,
    // so alike numbers of different models don't pair).
    uint32_t interp_numbered_id(uint32_t id, uint32_t instance) {
        auto it = instances.find(instance);
        uint32_t n = it != instances.end() ? it->second.number : 0;
        if (it != instances.end() && (instance & object_serials)) {
            id = it->second.key >> 1;
        }
        return (uint32_t)(((uint64_t)id << 32 | n) * 0x9E3779B97F4A7C15ull >> 33);
    }
}
