#!/usr/bin/env python3
"""Host ASan+UBSan unit test for G4 1-bit boolean subgroup shuffle/broadcast normalization.

Extracts air_simd_shuffle() and air_quad_broadcast() directly from tools/mesa/air/air_simd.inc
and verifies:
  1. 1-bit boolean inputs are widened to 32-bit via nir_b2i32 before nir_shuffle/nir_quad_broadcast,
     then masked with 1 and compared != 0 to yield a normalized 1-bit boolean (preventing stale
     upper bits from inactive lanes in Mesa 26.2.5 ir3).
  2. Non-boolean (32-bit/16-bit) inputs pass directly to nir_shuffle/nir_quad_broadcast without
     extra widening or masking ops.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile

AIR_SIMD = pathlib.Path(__file__).resolve().parents[1] / "air" / "air_simd.inc"


def cut_func(src: str, name: str) -> str:
    m = re.search(rf"static nir_def \*\n{name}\([^)]*\)\n\{{", src)
    assert m, f"function {name} not found in {AIR_SIMD}"
    i = m.end() - 1
    depth = 0
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start() : i + 1]
        i += 1
    raise AssertionError(f"unterminated {name}")


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
   unsigned bit_size;
   const char *op;
   uint64_t imm;
   unsigned id;
} nir_def;

typedef struct {
   unsigned count;
   nir_def nodes[64];
} nir_builder;

static nir_def *
emit(nir_builder *b, unsigned bits, const char *op, uint64_t imm)
{
   assert(b->count < 64);
   nir_def *d = &b->nodes[b->count];
   d->bit_size = bits;
   d->op = op;
   d->imm = imm;
   d->id = b->count++;
   return d;
}

static nir_def *nir_b2i32(nir_builder *b, nir_def *v) { assert(v->bit_size == 1); return emit(b, 32, "b2i32", v->id); }
static nir_def *nir_shuffle(nir_builder *b, nir_def *v, nir_def *l) { (void)l; return emit(b, v->bit_size, "shuffle", v->id); }
static nir_def *nir_quad_broadcast(nir_builder *b, nir_def *v, nir_def *l) { (void)l; return emit(b, v->bit_size, "quad_broadcast", v->id); }
static nir_def *nir_iand_imm(nir_builder *b, nir_def *v, uint64_t imm) { assert(v->bit_size == 32); return emit(b, 32, "iand_imm", imm); }
static nir_def *nir_ine_imm(nir_builder *b, nir_def *v, uint64_t imm) { assert(v->bit_size == 32); return emit(b, 1, "ine_imm", imm); }

__FUNCS__

int
main(void)
{
   nir_builder b = { 0 };
   nir_def bool_val = { .bit_size = 1, .op = "in_bool", .id = 100 };
   nir_def i32_val = { .bit_size = 32, .op = "in_i32", .id = 101 };
   nir_def lane = { .bit_size = 32, .op = "lane", .id = 102 };

   /* 1-bit simd_shuffle: b2i32 -> 32-bit shuffle -> iand_imm(1) -> ine_imm(0) */
   nir_def *r1 = air_simd_shuffle(&b, &bool_val, &lane);
   assert(r1->bit_size == 1);
   assert(b.count == 4);
   assert(b.nodes[0].bit_size == 32 && b.nodes[0].op[0] == 'b');
   assert(b.nodes[1].bit_size == 32 && b.nodes[1].op[0] == 's');
   assert(b.nodes[2].bit_size == 32 && b.nodes[2].imm == 1);
   assert(b.nodes[3].bit_size == 1 && b.nodes[3].imm == 0);

   /* 32-bit simd_shuffle: direct shuffle only */
   b.count = 0;
   nir_def *r2 = air_simd_shuffle(&b, &i32_val, &lane);
   assert(r2->bit_size == 32 && b.count == 1 && b.nodes[0].op[0] == 's');

   /* 1-bit quad_broadcast: b2i32 -> 32-bit quad_broadcast -> iand_imm(1) -> ine_imm(0) */
   b.count = 0;
   nir_def *r3 = air_quad_broadcast(&b, &bool_val, &lane);
   assert(r3->bit_size == 1 && b.count == 4);
   assert(b.nodes[0].bit_size == 32 && b.nodes[0].op[0] == 'b');
   assert(b.nodes[1].bit_size == 32 && b.nodes[1].op[0] == 'q');
   assert(b.nodes[2].bit_size == 32 && b.nodes[2].imm == 1);
   assert(b.nodes[3].bit_size == 1 && b.nodes[3].imm == 0);

   /* 32-bit quad_broadcast: direct quad_broadcast only */
   b.count = 0;
   nir_def *r4 = air_quad_broadcast(&b, &i32_val, &lane);
   assert(r4->bit_size == 32 && b.count == 1 && b.nodes[0].op[0] == 'q');

   printf("test-air-simd-bool-shuffle: PASS\n");
   return 0;
}
"""


def main() -> int:
    src = AIR_SIMD.read_text()
    # Verify no bare nir_shuffle / nir_quad_broadcast remain outside the two helpers
    body = src[src.index("quad_reduce_pair(") :]
    assert "nir_shuffle(" not in body, "raw nir_shuffle() call remains in air_simd.inc"
    assert "nir_quad_broadcast(" not in body, "raw nir_quad_broadcast() call remains in air_simd.inc"

    funcs = cut_func(src, "air_simd_shuffle") + "\n\n" + cut_func(src, "air_quad_broadcast")
    code = HARNESS.replace("__FUNCS__", funcs)
    with tempfile.TemporaryDirectory(prefix="air-simd-bool-") as td:
        cpath = pathlib.Path(td) / "harness.c"
        exe = pathlib.Path(td) / "harness"
        cpath.write_text(code)
        subprocess.run(
            ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(cpath), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
