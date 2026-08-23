// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/TreeWalker.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[E.1.2.E flat-SoA tree walker, leaf 1: XGBoost-JSON -> one aligned blob of per-class
//   contiguous 16B nodes (leaf self-loop transform; within-class file order preserved). Parse +
//   REFUSE gates + SoA build ONLY — the predict walk is leaf 2, backend integration leaf 3. Every
//   gate returns a DISTINCT code (Class-49: corrupt != unsupported != over-cap). Parse-time
//   allocation is load-path-sanctioned (aligned_alloc, HotSwap precedent); steady state owns zero.]
// [CONTAINS]
//   - [STRUCT]_[FlatTreeModel]      (blob header; nodes/roots arrays follow in the same allocation)
//   - [FUNCTION]_[TreeWalker_ParseFromJson] / [TreeWalker_Free]
//   - [REGISTRY]_[refuse-code constants WALKER_ERR_*]
//======================================================================================================
// PARITY CONTRACT (W-d, D-432): this file mirrors xgboost 3.3.0 semantics EXACTLY —
// route `fvalue < split_condition -> left`; missing = (value == the serve missing
// sentinel -1.0f) OR NaN -> default_left child; leaves carry their value in
// split_conditions; per-class accumulation in tree_info FILE ORDER (R5: the SoA
// groups classes contiguously but NEVER reorders within a class). The transform
// recipes (softprob double-wsum, sigmoid clamp+eps) land with the walk in leaf 2.
//======================================================================================================

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "simdjson.h"
#include "../Limits.hpp"

using namespace std;

//======================================================================
// [REGISTRY]_[refuse-code constants WALKER_ERR_*]
//----------------------------------------------------------------------
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one DISTINCT negative code per refuse gate (Class-49 discipline: the
//   caller can disposition corrupt-file vs unsupported-shape vs over-cap without
//   string-matching stderr). 0 = OK. Codes are INTERNAL (not persisted/wire) —
//   renumbering is free until one is logged/persisted, then H21 applies.]
//======================================================================
#define WALKER_OK                    0
#define WALKER_ERR_IO               -1   // file unreadable / simdjson load error
#define WALKER_ERR_JSON             -2   // malformed JSON / missing required key
#define WALKER_ERR_VERSION          -3   // version[0] != 3 (parity pinned to xgboost 3.x JSON)
#define WALKER_ERR_BOOSTER          -4   // gradient_booster.name != "gbtree" (DART needs tree weights)
#define WALKER_ERR_OBJECTIVE        -5   // objective not in {multi:softprob, binary:logistic, reg:squarederror}
#define WALKER_ERR_MULTI_TARGET     -6   // num_target != 1 (multi-target trees unsupported)
#define WALKER_ERR_VECTOR_LEAF      -7   // size_leaf_vector != 1 (vector leaves unsupported)
#define WALKER_ERR_PARALLEL_TREE    -8   // num_parallel_tree != 1 (forests unsupported)
#define WALKER_ERR_CATEGORICAL      -9   // split_type != 0 anywhere / non-empty categories arrays
#define WALKER_ERR_FEATURE_IDX     -10   // split_indices >= num_feature (corrupt / mismatched artifact)
#define WALKER_ERR_OVER_CAP        -11   // trees/nodes/classes exceed Limits.hpp WALKER_* caps
#define WALKER_ERR_BASE_SCORE      -12   // base_score missing / unparseable (scalar or vector-string form)
#define WALKER_ERR_SHAPE           -13   // internal inconsistency (tree_info len != num_trees, child OOB, ...)
#define WALKER_ERR_OOM             -14   // aligned_alloc failed

// objective transform selector (leaf 2 consumes; parsed + validated here).
#define WALKER_OBJ_SOFTPROB  1   // multi:softprob  — per-class margins -> softmax (double wsum recipe)
#define WALKER_OBJ_LOGISTIC  2   // binary:logistic — margin -> sigmoid (88.7f clamp + 1e-16 eps recipe)
#define WALKER_OBJ_IDENTITY  3   // reg:squarederror — margin IS the prediction
//======================================================================
// [END_REGISTRY]_[refuse-code constants WALKER_ERR_*]
//======================================================================

//======================================================================
// [STRUCT]_[FlatTreeNode]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [SLOW_PATH]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 16B walk node + blob layout doctrine — the node SoA and tree-root index live in the SAME
//   aligned_alloc(64) allocation, addressed via the offsets below. 16B/node:
//   {float cond; uint32 meta; int32 left; int32 right}. meta packs feature idx
//   (bits 0..23) + default_left (bit 30) + is_leaf (bit 31) via manual masks
//   (H14 — never C++ bitfields). Leaves: cond = leaf VALUE, children self-loop
//   (constant-iter walk support, leaf 2). NOT a wire/persist format — process-
//   lifetime only, rebuilt from JSON at every load (H21 not implicated).]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[16B]
//======================================================================
// [CODE]
//======================================================================

#define WALKER_META_FEAT_MASK   0x00FFFFFFu
#define WALKER_META_DEFAULT_LEFT (1u << 30)
#define WALKER_META_IS_LEAF      (1u << 31)

struct FlatTreeNode {
    float    cond;    // internal: split threshold; leaf: leaf value (xgboost split_conditions semantics)
    uint32_t meta;    // WALKER_META_* packed (H14 manual masks)
    int32_t  left;    // absolute node index into the blob's node array; leaf: self
    int32_t  right;   // absolute node index; leaf: self
};
static_assert(sizeof(FlatTreeNode) == 16, "walker node must stay 16B (L2 budget math in Limits.hpp)");
// [END_CODE]
// [END_STRUCT]_[FlatTreeNode]
//======================================================================

//======================================================================
// [STRUCT]_[FlatTreeModel]
//----------------------------------------------------------------------
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[blob header — geometry + per-class layout; nodes/tree_roots point into the ONE
//   aligned_alloc(64) allocation. Caller-owned; TreeWalker_Free reclaims the blob only.]
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//----------------------------------------------------------------------
// [SIZE]_[0B]
//======================================================================
// [CODE]
//======================================================================
struct FlatTreeModel {
    // ---- identity + shape (cold reads; filled once at parse) ----
    int      objective;                 // WALKER_OBJ_*
    int      num_class;                 // 1 for binary/regression; >=2 for softprob
    int      num_feature;               // model's own count — the authoritative NF (validated vs caller at integration, leaf 3)
    int      num_trees;                 // total across classes
    int      max_depth;                 // derived from the parsed trees (constant-iter bound for leaf 2)
    int      total_nodes;               // sum over trees
    int      trees_per_class[WALKER_MAX_CLASSES]; // ACTUAL counts (OQ6: leaf-3 consumer WARNs vs stamp's cfg-recorded n_estimators)
    float    base_score[WALKER_MAX_CLASSES];      // per-class MARGIN-space intercepts (vector form) or [0]-only (scalar form)
    int      _padding0 = 0;             // H12 hygiene (struct is not memcmp'd today; keep explicit anyway)

    // ---- blob geometry (offsets into the SAME allocation) ----
    FlatTreeNode* nodes;                // [total_nodes] — per-class contiguous, within-class file order (R5)
    int32_t*      tree_roots;           // [num_trees]   — absolute root index per tree, grouped per class
    int32_t       class_tree_start[WALKER_MAX_CLASSES]; // index into tree_roots where class c's trees begin
    int32_t       class_tree_count[WALKER_MAX_CLASSES]; // == trees_per_class (kept adjacent to start for the walk's read pattern)
};
// [END_CODE]
// [END_STRUCT]_[FlatTreeModel]
//======================================================================

//======================================================================
// [FUNCTION]_[TreeWalker_Free]
//----------------------------------------------------------------------
// [OVERVIEW]_[frees the single blob allocation. The header struct itself is
//   caller-owned (typically embedded or stack); only nodes points at the blob.]
//======================================================================
// [CODE]
//======================================================================
inline void TreeWalker_Free(FlatTreeModel* m) {
    if (!m) return;
    // nodes is the blob base (tree_roots points INTO the same allocation).
    free(m->nodes);
    m->nodes = nullptr;
    m->tree_roots = nullptr;
    m->total_nodes = 0;
    m->num_trees = 0;
}
// [END_CODE]
// [END_FUNCTION]_[TreeWalker_Free]
//======================================================================

//======================================================================
// [FUNCTION]_[TreeWalker_ParseFromJson]
//----------------------------------------------------------------------
// [OVERVIEW]_[two-pass parse of an xgboost-3.x JSON artifact into the SoA blob.
//   Pass 1 = validate every REFUSE gate + size the blob (no allocation until the
//   artifact is proven in-shape); pass 2 = fill nodes/roots. Returns WALKER_OK
//   or the DISTINCT WALKER_ERR_* of the FIRST failing gate (loud stderr either
//   way; caller dispositions per Class-49 — never a silent fallback).]
//======================================================================
// [CODE]
//======================================================================
inline int TreeWalker_ParseFromJson(FlatTreeModel* out, const char* path) {
    if (!out || !path || !path[0]) return WALKER_ERR_IO;
    memset(out, 0, sizeof(*out));

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.load(path).get(doc) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: unreadable or not JSON (ERR_IO)\n", path);
        return WALKER_ERR_IO;
    }

    // ---- version gate: [3, x, y] ----
    simdjson::dom::array ver;
    if (doc["version"].get(ver) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: no version array (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }
    {
        int64_t major = -1;
        auto it = ver.begin();
        if (it != ver.end()) { if ((*it).get(major) != simdjson::SUCCESS) major = -1; }
        if (major != 3) {
            fprintf(stderr, "[walker] %s: xgboost JSON major %lld != 3 — parity is pinned to 3.x; retrain or serve via the library (ERR_VERSION)\n",
                    path, (long long)major);
            return WALKER_ERR_VERSION;
        }
    }

    simdjson::dom::element learner;
    if (doc["learner"].get(learner) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: no learner (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }

    // ---- objective gate: exactly the trainer's three ----
    {
        std::string_view obj_name;
        if (learner["objective"]["name"].get(obj_name) != simdjson::SUCCESS) {
            fprintf(stderr, "[walker] %s: no objective.name (ERR_JSON)\n", path);
            return WALKER_ERR_JSON;
        }
        if      (obj_name == "multi:softprob")   out->objective = WALKER_OBJ_SOFTPROB;
        else if (obj_name == "binary:logistic")  out->objective = WALKER_OBJ_LOGISTIC;
        else if (obj_name == "reg:squarederror") out->objective = WALKER_OBJ_IDENTITY;
        else {
            fprintf(stderr, "[walker] %s: objective '%.*s' unsupported — walker serves exactly the trainer's three (ERR_OBJECTIVE)\n",
                    path, (int)obj_name.size(), obj_name.data());
            return WALKER_ERR_OBJECTIVE;
        }
    }

    // ---- learner_model_param: numbers-as-strings (xgboost JSON quirk) ----
    // num_class / num_feature / num_target / base_score. base_score is a
    // scalar-string OR a bracketed vector-string ("[-3.57E0,1.79E0,...]",
    // boost_from_average) — both parsed; margins land in base_score[].
    simdjson::dom::element lmp;
    if (learner["learner_model_param"].get(lmp) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: no learner_model_param (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }
    {
        std::string_view sv;
        // xgboost stores param numbers as STRINGS; simdjson string_views are
        // not guaranteed NUL-terminated — bounded stack copy before strtol
        // (cold path; 64B cap is generous for any int-as-string).
        char numbuf[64];
        auto sv_copy = [&](std::string_view v) -> const char* {
            size_t n = v.size() < sizeof(numbuf) - 1 ? v.size() : sizeof(numbuf) - 1;
            memcpy(numbuf, v.data(), n);
            numbuf[n] = '\0';
            return numbuf;
        };
        auto get_int_str = [&](const char* key, int* dst) -> bool {
            if (lmp[key].get(sv) != simdjson::SUCCESS) return false;
            *dst = (int)strtol(sv_copy(sv), nullptr, 10);
            return true;
        };
        int num_class = 0, num_feature = 0, num_target = 1;
        if (!get_int_str("num_class", &num_class) || !get_int_str("num_feature", &num_feature)) {
            fprintf(stderr, "[walker] %s: num_class/num_feature missing (ERR_JSON)\n", path);
            return WALKER_ERR_JSON;
        }
        get_int_str("num_target", &num_target);           // absent on older minors -> default 1
        if (num_target != 1) {
            fprintf(stderr, "[walker] %s: num_target=%d unsupported (ERR_MULTI_TARGET)\n", path, num_target);
            return WALKER_ERR_MULTI_TARGET;
        }
        // xgboost encodes binary/regression as num_class=0; normalize to 1 accumulator.
        out->num_class   = num_class > 1 ? num_class : 1;
        out->num_feature = num_feature;
        if (out->num_class > WALKER_MAX_CLASSES) {
            fprintf(stderr, "[walker] %s: num_class %d > cap %d (ERR_OVER_CAP)\n", path, out->num_class, WALKER_MAX_CLASSES);
            return WALKER_ERR_OVER_CAP;
        }
        if (out->objective == WALKER_OBJ_SOFTPROB && out->num_class < 2) {
            fprintf(stderr, "[walker] %s: softprob with num_class<2 (ERR_SHAPE)\n", path);
            return WALKER_ERR_SHAPE;
        }

        if (lmp["base_score"].get(sv) != simdjson::SUCCESS || sv.size() == 0) {
            fprintf(stderr, "[walker] %s: base_score missing (ERR_BASE_SCORE)\n", path);
            return WALKER_ERR_BASE_SCORE;
        }
        // both forms: "5E-1" (scalar) / "[-3.57E0,1.79E0,1.78E0]" (vector).
        {
            char bsbuf[256];
            size_t bn = sv.size() < sizeof(bsbuf) - 1 ? sv.size() : sizeof(bsbuf) - 1;
            memcpy(bsbuf, sv.data(), bn);
            bsbuf[bn] = '\0';
            const char* p = bsbuf;
            const char* end = bsbuf + bn;
            int is_vec = (*p == '[');
            if (is_vec) ++p;
            int i = 0;
            while (p < end && i < WALKER_MAX_CLASSES) {
                char* nxt = nullptr;
                float v = strtof(p, &nxt);
                if (nxt == p) break;                      // no progress -> unparseable tail
                out->base_score[i++] = v;
                p = nxt;
                while (p < end && (*p == ',' || *p == ' ')) ++p;
                if (p < end && *p == ']') { break; }
                if (!is_vec) break;                       // scalar form: one value only
            }
            if (i == 0) {
                fprintf(stderr, "[walker] %s: base_score unparseable '%.*s' (ERR_BASE_SCORE)\n",
                        path, (int)sv.size() > 48 ? 48 : (int)sv.size(), sv.data());
                return WALKER_ERR_BASE_SCORE;
            }
            // vector form must cover every class; scalar form broadcasts.
            if (i == 1) {
                for (int c = 1; c < out->num_class; ++c) out->base_score[c] = out->base_score[0];
            } else if (i != out->num_class) {
                fprintf(stderr, "[walker] %s: base_score vector len %d != num_class %d (ERR_BASE_SCORE)\n",
                        path, i, out->num_class);
                return WALKER_ERR_BASE_SCORE;
            }
        }
    }

    // ---- gradient_booster gates ----
    simdjson::dom::element gb;
    if (learner["gradient_booster"].get(gb) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: no gradient_booster (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }
    {
        std::string_view gb_name;
        if (gb["name"].get(gb_name) != simdjson::SUCCESS || gb_name != "gbtree") {
            fprintf(stderr, "[walker] %s: booster '%.*s' != gbtree — DART/linear unsupported (ERR_BOOSTER)\n",
                    path, gb_name.size() ? (int)gb_name.size() : 6, gb_name.size() ? gb_name.data() : "(none)");
            return WALKER_ERR_BOOSTER;
        }
    }
    simdjson::dom::element gbm;
    if (gb["model"].get(gbm) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: no gradient_booster.model (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }
    {
        std::string_view sv;
        char nbuf[32];
        int npt = 1;
        if (gbm["gbtree_model_param"]["num_parallel_tree"].get(sv) == simdjson::SUCCESS) {
            size_t n = sv.size() < sizeof(nbuf) - 1 ? sv.size() : sizeof(nbuf) - 1;
            memcpy(nbuf, sv.data(), n); nbuf[n] = '\0';
            npt = (int)strtol(nbuf, nullptr, 10);
        }
        if (npt != 1) {
            fprintf(stderr, "[walker] %s: num_parallel_tree=%d unsupported (ERR_PARALLEL_TREE)\n", path, npt);
            return WALKER_ERR_PARALLEL_TREE;
        }
    }

    simdjson::dom::array trees, tree_info;
    if (gbm["trees"].get(trees) != simdjson::SUCCESS || gbm["tree_info"].get(tree_info) != simdjson::SUCCESS) {
        fprintf(stderr, "[walker] %s: trees/tree_info missing (ERR_JSON)\n", path);
        return WALKER_ERR_JSON;
    }

    // ================= PASS 1 — validate + size (no allocation yet) =================
    int num_trees = 0, total_nodes = 0, max_depth_nodes = 0;
    for (simdjson::dom::element t : trees) {
        if (num_trees >= WALKER_MAX_TREES) {
            fprintf(stderr, "[walker] %s: > %d trees (ERR_OVER_CAP)\n", path, WALKER_MAX_TREES);
            return WALKER_ERR_OVER_CAP;
        }
        std::string_view sv;
        // vector-leaf gate (tree_param.size_leaf_vector, string-int)
        if (t["tree_param"]["size_leaf_vector"].get(sv) == simdjson::SUCCESS) {
            char slvbuf[32];
            size_t n = sv.size() < sizeof(slvbuf) - 1 ? sv.size() : sizeof(slvbuf) - 1;
            memcpy(slvbuf, sv.data(), n); slvbuf[n] = '\0';
            if (strtol(slvbuf, nullptr, 10) > 1) {
                fprintf(stderr, "[walker] %s: tree %d size_leaf_vector>1 (ERR_VECTOR_LEAF)\n", path, num_trees);
                return WALKER_ERR_VECTOR_LEAF;
            }
        }
        int64_t nn = 0;
        {
            simdjson::dom::array lc;
            if (t["left_children"].get(lc) != simdjson::SUCCESS) {
                fprintf(stderr, "[walker] %s: tree %d has no left_children (ERR_JSON)\n", path, num_trees);
                return WALKER_ERR_JSON;
            }
            nn = (int64_t)lc.size();
        }
        if (nn <= 0 || nn > WALKER_MAX_NODES_PER_TREE) {
            fprintf(stderr, "[walker] %s: tree %d has %lld nodes (cap %d) (ERR_OVER_CAP)\n",
                    path, num_trees, (long long)nn, WALKER_MAX_NODES_PER_TREE);
            return WALKER_ERR_OVER_CAP;
        }
        // categorical gates: any split_type != 0, or non-empty categories arrays.
        {
            simdjson::dom::array st;
            if (t["split_type"].get(st) == simdjson::SUCCESS) {
                for (simdjson::dom::element e : st) {
                    int64_t v = 0;
                    if (e.get(v) != simdjson::SUCCESS) {
                        fprintf(stderr, "[walker] %s: tree %d malformed split_type element (ERR_JSON)\n", path, num_trees);
                        return WALKER_ERR_JSON;
                    }
                    if (v != 0) {
                        fprintf(stderr, "[walker] %s: tree %d categorical split (ERR_CATEGORICAL)\n", path, num_trees);
                        return WALKER_ERR_CATEGORICAL;
                    }
                }
            }
            simdjson::dom::array cats;
            if (t["categories"].get(cats) == simdjson::SUCCESS && cats.size() > 0) {
                fprintf(stderr, "[walker] %s: tree %d non-empty categories (ERR_CATEGORICAL)\n", path, num_trees);
                return WALKER_ERR_CATEGORICAL;
            }
        }
        total_nodes += (int)nn;
        if ((int)nn > max_depth_nodes) max_depth_nodes = (int)nn;
        ++num_trees;
    }
    if (num_trees == 0) {
        fprintf(stderr, "[walker] %s: zero trees (ERR_SHAPE)\n", path);
        return WALKER_ERR_SHAPE;
    }
    if (total_nodes > WALKER_MAX_TOTAL_NODES) {
        fprintf(stderr, "[walker] %s: %d total nodes > cap %d (ERR_OVER_CAP)\n", path, total_nodes, WALKER_MAX_TOTAL_NODES);
        return WALKER_ERR_OVER_CAP;
    }
    if ((int)tree_info.size() != num_trees) {
        fprintf(stderr, "[walker] %s: tree_info len %d != num_trees %d (ERR_SHAPE)\n",
                path, (int)tree_info.size(), num_trees);
        return WALKER_ERR_SHAPE;
    }
    // class-id sanity + per-class counts (R5 grouping is by tree_info).
    {
        int idx = 0;
        for (simdjson::dom::element e : tree_info) {
            int64_t gid = 0;
            if (e.get(gid) != simdjson::SUCCESS) {
                fprintf(stderr, "[walker] %s: tree_info[%d] malformed (ERR_SHAPE)\n", path, idx);
                return WALKER_ERR_SHAPE;
            }
            if (gid < 0 || gid >= out->num_class) {
                fprintf(stderr, "[walker] %s: tree_info[%d]=%lld outside num_class %d (ERR_SHAPE)\n",
                        path, idx, (long long)gid, out->num_class);
                return WALKER_ERR_SHAPE;
            }
            out->trees_per_class[gid]++;
            ++idx;
        }
    }

    // ================= allocate the blob =================
    // nodes[total_nodes] + tree_roots[num_trees], one aligned_alloc(64)
    // (load-path-sanctioned; HotSwap/boot zoo precedent). 16B nodes keep the
    // roots array 4B-aligned for free at the tail.
    size_t nodes_bytes = (size_t)total_nodes * sizeof(FlatTreeNode);
    size_t roots_bytes = (size_t)num_trees * sizeof(int32_t);
    size_t blob_bytes  = ((nodes_bytes + roots_bytes + 63) / 64) * 64;
    FlatTreeNode* blob = (FlatTreeNode*)aligned_alloc(64, blob_bytes);
    if (!blob) {
        fprintf(stderr, "[walker] %s: aligned_alloc(%zu) OOM (ERR_OOM)\n", path, blob_bytes);
        return WALKER_ERR_OOM;
    }
    out->nodes      = blob;
    out->tree_roots = (int32_t*)((char*)blob + nodes_bytes);
    out->num_trees   = num_trees;
    out->total_nodes = total_nodes;

    // per-class root-slot layout: class c's trees occupy
    // tree_roots[class_tree_start[c] .. +count) in FILE ORDER (R5).
    {
        int cursor = 0;
        for (int c = 0; c < out->num_class; ++c) {
            out->class_tree_start[c] = cursor;
            out->class_tree_count[c] = out->trees_per_class[c];
            cursor += out->trees_per_class[c];
        }
    }

    // ================= PASS 2 — fill =================
    int fill_cursor[WALKER_MAX_CLASSES];              // next root slot per class
    for (int c = 0; c < out->num_class; ++c) fill_cursor[c] = out->class_tree_start[c];
    int node_base = 0;                                 // absolute index of the current tree's node block
    int tree_idx = 0;
    int max_depth = 1;
    auto ti_it = tree_info.begin();
    for (simdjson::dom::element t : trees) {
        int64_t gid = 0;
        if ((*ti_it).get(gid) != simdjson::SUCCESS) {
            fprintf(stderr, "[walker] %s: tree %d tree_info malformed in pass 2 (ERR_SHAPE)\n", path, tree_idx);
            TreeWalker_Free(out);
            return WALKER_ERR_SHAPE;
        }
        ++ti_it;

        simdjson::dom::array sc, si, lc, rc, dl;
        if (t["split_conditions"].get(sc) != simdjson::SUCCESS ||
            t["split_indices"].get(si)    != simdjson::SUCCESS ||
            t["left_children"].get(lc)    != simdjson::SUCCESS ||
            t["right_children"].get(rc)   != simdjson::SUCCESS ||
            t["default_left"].get(dl)     != simdjson::SUCCESS) {
            fprintf(stderr, "[walker] %s: tree %d node arrays missing (ERR_JSON)\n", path, tree_idx);
            TreeWalker_Free(out);
            return WALKER_ERR_JSON;
        }
        int nn = (int)lc.size();
        if ((int)sc.size() != nn || (int)si.size() != nn || (int)rc.size() != nn || (int)dl.size() != nn) {
            fprintf(stderr, "[walker] %s: tree %d ragged node arrays (ERR_SHAPE)\n", path, tree_idx);
            TreeWalker_Free(out);
            return WALKER_ERR_SHAPE;
        }

        auto sc_it = sc.begin(); auto si_it = si.begin();
        auto lc_it = lc.begin(); auto rc_it = rc.begin(); auto dl_it = dl.begin();
        for (int n = 0; n < nn; ++n) {
            double cond_d = 0.0; int64_t feat = 0, l = 0, r = 0, dflt = 0;
            int gets_ok = ((*sc_it).get(cond_d) == simdjson::SUCCESS)
                        & ((*si_it).get(feat)   == simdjson::SUCCESS)
                        & ((*lc_it).get(l)      == simdjson::SUCCESS)
                        & ((*rc_it).get(r)      == simdjson::SUCCESS)
                        & ((*dl_it).get(dflt)   == simdjson::SUCCESS);
            ++sc_it; ++si_it; ++lc_it; ++rc_it; ++dl_it;
            if (!gets_ok) {
                fprintf(stderr, "[walker] %s: tree %d node %d malformed element (ERR_JSON)\n", path, tree_idx, n);
                TreeWalker_Free(out);
                return WALKER_ERR_JSON;
            }

            FlatTreeNode* node = &out->nodes[node_base + n];
            int is_leaf = (l == -1);
            if (!is_leaf) {
                if (feat < 0 || feat >= out->num_feature) {
                    fprintf(stderr, "[walker] %s: tree %d node %d split_index %lld >= num_feature %d (ERR_FEATURE_IDX)\n",
                            path, tree_idx, n, (long long)feat, out->num_feature);
                    TreeWalker_Free(out);
                    return WALKER_ERR_FEATURE_IDX;
                }
                if (l < 0 || l >= nn || r < 0 || r >= nn) {
                    fprintf(stderr, "[walker] %s: tree %d node %d child OOB (ERR_SHAPE)\n", path, tree_idx, n);
                    TreeWalker_Free(out);
                    return WALKER_ERR_SHAPE;
                }
            }
            // leaf self-loop transform: children -> self, so the constant-iter
            // walk (leaf 2) runs max_depth steps unconditionally and parks on
            // the leaf. cond carries the LEAF VALUE at leaves (xgboost stores
            // it in split_conditions).
            node->cond = (float)cond_d;
            node->meta = (uint32_t)(is_leaf ? 0 : (uint32_t)feat & WALKER_META_FEAT_MASK)
                       | (dflt    ? WALKER_META_DEFAULT_LEFT : 0u)
                       | (is_leaf ? WALKER_META_IS_LEAF      : 0u);
            node->left  = node_base + (is_leaf ? n : (int)l);
            node->right = node_base + (is_leaf ? n : (int)r);
        }
        out->tree_roots[fill_cursor[gid]++] = node_base;

        // derive this tree's depth (root = node 0; children strictly deeper).
        // small trees, cold path: an O(nn) sweep with a parent-depth array on
        // the stack would need VLA; use the log2 bound instead — a BINARY tree
        // with nn nodes has depth <= nn (chain) but xgboost trees are proper
        // binaries: internal count = (nn-1)/2. Exact depth computed in leaf 2's
        // first walk-calibration is unnecessary — take the safe chain bound
        // capped by cap: depth <= (nn+1)/2 for proper binary chains.
        int depth_bound = (nn + 1) / 2;
        if (depth_bound > max_depth) max_depth = depth_bound;

        node_base += nn;
        ++tree_idx;
    }
    out->max_depth = max_depth;

    return WALKER_OK;
}
// [END_CODE]
// [END_FUNCTION]_[TreeWalker_ParseFromJson]
//======================================================================
