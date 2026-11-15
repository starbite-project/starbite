# d3d10umd: DXBC -> NIR shader translator, phase 1

State: 2026-11-15 23:10 EET. Branch `sb-nir` in the VM worktree `~/sbmesa/src-nir`
(based on `sb-int` 018f6ef), two commits, nothing pushed:

- `fc9d48d` d3d10umd: DXBC to NIR shader translator, phase 1 (ShaderNIR.c)
- `d9185f0` d3d10umd: host harness for the DXBC to NIR translator

Nothing in the driver calls the new translator yet. `Shader.cpp` is untouched and still goes
through `Shader_tgsi_translate`. `ShaderNIR.c` is added to the d3d10umd library's source list,
so the next Windows build compiles it (checked: clang-cl with the flags of `build-int-a64`
compiles `ShaderNIR.c` and the changed `ShaderParse.c` to objects without errors; not linked
into a DLL, not run on Windows).

## 1. What there is

In `src/gallium/frontends/d3d10umd/`:

| File | What |
|---|---|
| `ShaderNIR.h`, `ShaderNIR.c` | `Shader_nir_translate(code, options, signature, output_mapping, &error)`: vertex, pixel and geometry shaders, Shader Model 4.0 and 4.1 |
| `ShaderParse.c/.h` | operand counts for `lod`, `gather4`, `samplepos`, `sampleinfo` (+ its return type), custom data other than the immediate constant buffer skipped, opcodes past 4.1 skipped by length, `Shader_opcode_name()`, host build switch `D3D10UMD_HOST_TEST` |
| `tests/dxbc_nir_harness.c` | the harness: corpus extraction, DXBC container and signature parsing, one child process per shader, Panfrost compile path, summary, `--stats` |
| `tests/dxbc_nir_selftest.c` | 80 hand-assembled token streams (`--selftest`) |
| `tests/HostCompat.h`, `tests/winapifamily.h` | what the parser needs instead of the DDK headers on Linux |
| `tests/meson.build` | target `dxbc_nir_harness` |

Outside the frontend: `meson.options` (new option `d3d10umd-nir-tests-tpf-dir`),
`src/gallium/meson.build` (subdir when the option is set), top `meson.build` (the option also
turns on `with_gfx_compute`, otherwise libnir is only a stub in a build without GL or Vulkan).

The SDK header `d3d11TokenizedProgramFormat.hpp` is not copied into the tree: the option names
the SDK's `um/` directory and meson copies that one file into the build directory.

### The API

```c
struct Shader_nir_signature_entry { unsigned system_value; unsigned reg; unsigned mask; };
struct Shader_nir_signature { const ... *inputs; unsigned num_inputs; const ... *outputs; unsigned num_outputs; };

struct nir_shader *
Shader_nir_translate(const unsigned *code,
                     const struct nir_shader_compiler_options *options,
                     const struct Shader_nir_signature *signature,   /* may be NULL */
                     unsigned *output_mapping,                       /* may be NULL */
                     char **error);                                  /* malloc'ed message on failure */
```

The signature entry has the three fields of `D3D10DDIARG_SIGNATURE_ENTRY` (SystemValue,
Register, Mask), so phase 2 copies `pSignatures` field by field. The result is a finalized
shader in the state `tgsi_to_nir()` returns one for a driver without a `finalize_nir` hook
(Panfrost has none), ready for `pipe_shader_state.type = PIPE_SHADER_IR_NIR`.

## 2. Design decisions

**Registers.** A D3D register is four untyped 32-bit lanes. Temporaries, indexable temporaries
(arrays) and one shadow per output register are local `uvec4` variables; `nir_lower_vars_to_ssa`
makes them SSA. Every opcode reads sources as four lanes after the swizzle, computes four lanes
and stores with the destination's write mask; scalar results (dot products, comparison
sampling) are replicated. The operand modifier (neg, abs) is float or integer by opcode; `mov`
and `movc` take it as float, which is how fxc uses them.

**Two passes.** Pass 1 reads only declarations and creates every variable (it has to see all
clip/cull declarations before it can lay them out). Pass 2 translates instructions.

**Inputs and outputs** are NIR variables with derefs, not lowered IO: that is what
`tgsi_to_nir` hands Panfrost, whose `create_*_state` runs `nir_lower_io` itself. A register is
a list of up to four pieces, because two signature elements can share one register (22 corpus
shaders do).

| D3D | NIR |
|---|---|
| VS input `vN` | `vec4`, `VERT_ATTRIB_GENERIC0 + N`, `driver_location = N` (InputAssembly.cpp numbers vertex elements by register) |
| varying register N (VS/GS out, GS/PS in) | `vec4`, `VARYING_SLOT_VAR0 + N`; PS: interpolation, centroid and sample from the declaration |
| SV_Position out / GS in | `VARYING_SLOT_POS` |
| SV_Position PS in | `nir_build_frag_coord`, `.w` replaced by its reciprocal (D3D gives w, the rasterizer 1/w) |
| SV_ClipDistance / SV_CullDistance | one compact float array at `VARYING_SLOT_CLIP_DIST0`, clip first, `clip_distance_array_size` / `cull_distance_array_size` set |
| SV_RenderTargetArrayIndex, SV_ViewportArrayIndex | flat `int` at `VARYING_SLOT_LAYER` / `VARYING_SLOT_VIEWPORT`, written and read |
| SV_PrimitiveID | GS out: `VARYING_SLOT_PRIMITIVE_ID`; PS / GS in: `load_primitive_id` |
| SV_VertexID | `load_vertex_id_zero_base` (as `TGSI_SEMANTIC_VERTEXID_NOBASE`; pan_shader.c lowers it) |
| SV_InstanceID, SV_IsFrontFace, SV_SampleIndex | `load_instance_id`, `load_front_face` (to all ones / zero), `load_sample_id` |
| PS `oN` | `vec4`, `FRAG_RESULT_DATA0 + N`, `fs.untyped_color_outputs` |
| `oDepth`, `oMask` | `FRAG_RESULT_DEPTH` float, `FRAG_RESULT_SAMPLE_MASK` int |

Outputs are stored exactly once, at the end of the shader, from the shadows
(`nir_lower_returns` runs first so every path gets there); a geometry shader stores them before
each `emit`.

**Clip / cull order.** D3D leaves the order inside each group open; the array index of a lane
is its rank by (register, lane), clip before cull. With the signature a pixel shader computes
this over all elements, not only the ones it declares, so it agrees with the vertex shader.

**The signature** is used for two things: that order, and the system value of a register the
token stream declares without one. The second happens: 2 corpus geometry shaders declare a
clip distance input with a plain `dcl_input`, and only ISGN says what it is. Without the
signature these two are the only corpus shaders whose NIR comes out different.

**Constant buffers.** `cbN[i]` is `load_ubo(block N, offset 16 * i)`, four lanes,
`align_mul 16`, range from the declared size for dynamic indices. Finalization runs
`nir_lower_uniforms_to_ubo` when the driver's options ask for it, exactly as `tgsi_to_nir`
does, which moves block N to N + 1: the gallium slot `Shader.cpp` binds cbN at (`SB_CB_SHIFT`).
The immediate constant buffer is a local array with a constant initializer.

**Resources and samplers.** `tN` is a texture variable with binding N, `sN` a bare (or bare
shadow) sampler variable with binding N; `nir_lower_samplers` turns the derefs into
`texture_index = N`, `sampler_index = N`. This is the shape of the 0114 patch in
`tgsi_to_nir`. The swizzle on the resource operand is applied to the result. Buffer resources
set `info.texture_buffers` (0118).

**Stage linkage** is by register number, as before: output register N of one stage and input
register N of the next share `VARYING_SLOT_VAR0 + N`; system values share their slot.
`output_mapping[driver_location] = register` for vertex and geometry outputs (the TGSI path:
index = TGSI output index, also declaration order).

**Control flow.** `if/else/endif` and `loop/endloop` are NIR ifs and loops (loops get an empty
continue construct, which this NIR needs for a continue jump). `break(c)`, `continue(c)`,
`ret(c)` are jumps; code after an unconditional jump up to the end of its block is dropped,
because NIR has no place for it. `switch` is a loop that runs once: each label opens
`if (fell_through || selector == label)`, `default` is taken when no label matches (the labels
are collected by looking ahead to the matching `endswitch`, so `default` may stand anywhere),
`break` is a NIR break. A `continue` inside a switch sets a flag, breaks out of the switch's
loop, and the code after the switch passes it on.

**Float semantics.** All float operations are built with `nir_fp_preserve_sz_inf_nan`; without
the `refactoringAllowed` global flag also with `nir_fp_exact`. `mad` may always be fused.
`ftoi` / `ftou` saturate and give 0 for NaN, `udiv` by zero gives all ones, `ne` is the only
comparison true for NaN, comparisons give all ones / zero.

**Finalization** (in `sn_finalize`, in this order): `nir_lower_continue_constructs`,
`nir_lower_returns`, output stores, validate, `nir_lower_variable_initializers`,
`nir_lower_vars_to_ssa`, `nir_split_var_copies`, `nir_lower_var_copies`,
`nir_lower_system_values`, `nir_lower_uniforms_to_ubo` (if `options->lower_uniforms_to_ubo`),
`nir_lower_samplers`, the optimization loop of `ttn_optimize_nir` (copied, it is static
there), `nir_shader_gather_info`, validate.

## 3. Coverage

Corpus: Wine 10.0 `d3d10core.c` (117 containers) and `d3d11.c` (245). Backend: the Panfrost
compiler for gpu id 0x62210000 (Mali-G72, arch 6), reached through the same passes as
`panfrost_create_shader_state` + `panfrost_shader_compile` with the default shader key
(`pan_nir_lower_sysvals.c` and `pan_nir_lower_res_indices.c` are compiled into the harness).

### Corpus (ran, output in `~/sbmesa/build-nir-host/harness-corpus.txt`)

| | all | vs_4_0 | vs_4_1 | ps_4_0 | ps_4_1 | gs_4_0 | gs_4_1 |
|---|---|---|---|---|---|---|---|
| translated, validated, compiled by Bifrost | 227 | 66 | 1 | 154 | 6 | - | - |
| translated and validated (geometry: Panfrost has no such stage) | 23 | - | - | - | - | 22 | 1 |
| failed to translate or validate | 0 | | | | | | |
| failed in the backend | 17 | 0 | 0 | 16 | 1 | - | - |
| **Shader Model 4.x in scope** | **267** | 66 | 1 | 170 | 7 | 22 | 1 |
| out of scope: Shader Model 5 (92) and one `cs_4_0` | 93 | | | | | | |
| out of scope: 4.x shaders using Direct3D 11 opcodes | 2 | | | 2 | | | |

- All 267 in-scope shaders translate and pass `nir_validate_shader` (which also runs after
  every pass of the finalization, this is a build with assertions).
- The 17 backend failures are one cause: the shader reads the mip level count (`resinfo` .w),
  which is `query_levels` in NIR, and Bifrost before Valhall has no lowering for it
  (`assert(b->shader->arch >= 9)`, bifrost_compile.c:3653). See section 5.
- 11 of the 227 are vertex shaders without an SV_Position output; Bifrost returns an empty
  binary for them, which the driver also accepts. The harness reports them as
  "OK: empty binary".
- The 2 "Direct3D 11 opcode" shaders are `ps_4_0` profiles compiled for a D3D11 device
  (d3d11.c:24566 uses a StructuredBuffer). The translator refuses them by opcode number; a
  D3D10 driver is never handed one.

### Self tests (ran, `--selftest`, in `harness-final.txt`)

80 hand-assembled shaders. 71 have only immediates as inputs and carry the constant render
target 0 must fold to (the harness scalarizes and unrolls a copy with NIR's own passes and
compares): this checks the meaning, not just the validity. The checker was tried with three
deliberately wrong expectations and reported all three.

| | count |
|---|---|
| result checked and right, and compiled by Bifrost | 71 |
| translate + validate + compile only (`ps41_textures`, `ps41_outputs`, `vs_dynamic`) | 3 |
| refused with the intended message (`call`, `sampleinfo` of the rasterizer) | 2 |
| failed in the backend (`sampleinfo` x2, `samplepos` of a resource: `texture_samples`; `samplepos` of the rasterizer: `load_sample_pos_from_id`) | 4 |

### What each opcode was verified by

"corpus": translated, validated and (vertex/pixel) compiled for real fxc output. "value": a
self test checks the folded result. "valid": a self test translates, validates and compiles it,
result not checked.

| Opcodes / features | corpus | self test |
|---|---|---|
| add, mul, div, mad, min, max, dp3, dp4, frc, round_z | yes | value |
| dp2, exp, log, sqrt, rsq, round_ne, round_ni, round_pi, sincos, eq, ne | no | value |
| lt, ge | yes | value (with NaN) |
| ftoi, ftou (saturation, NaN), itof, utof | yes | value |
| and, xor, not, iadd, ishl, ieq, ige, ilt, uge, ult | yes | value |
| or, ineg, imax, imin, umax, umin, ishr, ushr, ine, imad, umad, imul, umul, udiv (incl. by zero) | no | value |
| mov, movc; write masks, swizzles | yes | value |
| saturate, operand abs | no | value |
| operand neg (float and integer) | yes | value |
| if / else / endif, loop, break, breakc, continuec, ret, retc | yes | value (retc, continue in a loop) |
| unconditional continue; continue inside a switch | no | value |
| switch / case / default / endswitch (fallthrough, default in the middle, code after break) | yes (7 shaders) | value |
| discard | yes | valid |
| deriv_rty / deriv_rtx | yes / no | valid |
| sample, sample_b, sample_l, sample_c, sample_c_lz, ld, ld_ms, resinfo, texel offset | yes | valid |
| sample_d, ld_ms on an array, sample_c on a 2D array, lod, gather4 with offset | gather4 only | valid |
| sampleinfo, samplepos | no | translate + validate only (backend fails) |
| immediate constant buffer, constant and dynamic index | yes | value |
| indexable temps, dynamic index | no | value |
| dynamic index: constant buffer, inputs (incl. GS vertex index) | yes | valid |
| dynamic index: outputs (`dcl_indexRange`) | no | valid |
| clip / cull distance outputs, layer and viewport in and out, vertex id, instance id, front face | yes | valid |
| clip distance as a PS input, primitive id and sample index as PS inputs, oDepth, oMask, centroid and sample interpolation | no (oDepth: not counted) | valid |
| emit, cut (geometry) | yes, validated only | - |
| emitthencut, nop, `dcl_output_sgv` | no | - (reasoned only) |

`--stats` prints the full per-opcode usage of the corpus (end of `harness-final.txt`).

### Not verified by anything here

- That the compiled shaders render correctly: there is no interpreter and no hardware in this
  work. Constant folding covers ALU and control flow semantics; texture, IO and linkage
  semantics are reviewed NIR, not executed code.
- That a vertex and a pixel shader translated separately link on Panfrost (same slots as the
  TGSI path by construction; not run).
- Geometry shaders beyond validation; stream output; `output_mapping` in use.
- The harness configured without the Panfrost driver (the `#ifndef HAVE_PANFROST_COMPILER`
  branch compiles nowhere yet).

## 4. Not done, and deviations

| What | Why |
|---|---|
| `call`, `callc`, `label` | Subroutines: fxc does not emit them for SM4 without interfaces; needs NIR functions. Translation fails with "opcode CALL is not supported" |
| `sampleinfo rasterizer` | NIR has no portable intrinsic for the render target's sample count (`load_rasterization_samples_amd` is a vendor one). Translation fails with a message |
| `samplepos` of a resource | Translated as a lookup in the D3D10.1 standard patterns (1, 2, 4, 8, 16 samples) by `texture_samples`. Right when the driver uses those patterns: Panfrost does (compared with `src/panfrost/lib/pan_samples.c` by reading). With a vendor pattern it would be wrong |
| `resinfo` of a level the resource does not have | D3D wants zero extents; this returns what the driver's `txs` gives. The check needs the level count for every `resinfo`, which Bifrost v6 cannot provide (section 5), so it was taken out. DXVK and vkd3d do not check either (from memory, not looked up) |
| Level count of a buffer or multisample resource in `resinfo` | Constant 1 (as the 0114 path) |
| Out-of-bounds constant buffer / indexable temp access | No robustness code (D3D: reads give 0). Same as the TGSI path |
| Driver-side clipping by SV_ClipDistance | Translator emits the standard compact array. Panfrost reports `clip_planes = 0` and the Bifrost compile accepts the output; whether anything is clipped was not tested. Unchanged from the TGSI path |
| Shader Model 5, Direct3D 11 opcodes in 4.x shaders | Phase 3. Rejected with a message naming the opcode number |
| Dual-source blending (o1 as second source) | Stays in Shader.cpp (0127): it re-targets `FRAG_RESULT_DATA1` in a second variant; works the same on this NIR |

## 5. Found on the way

### Driver gaps on Bifrost v6 (they limit both translators)

1. **`query_levels` is not lowered** in the gallium driver for arch < 9
   (`pan_nir_lower_sysvals.c:206` only takes `txs`; `pan_nir_lower_tex.c` handles queries on
   Valhall only). A shader that reads the mip count hits `assert(arch >= 9)` at
   `bifrost_compile.c:3653`; a release build goes on into `bi_emit_tex_valhall` on Bifrost.
   Ran: 17 corpus shaders with the new translator. Read, not run: `tgsi_to_nir.c:1630-1642`
   (0114) emits the same `query_levels` for SVIEWINFO whenever `.w` is used, so the shipping
   TGSI path has this bug for the same shaders (for example `GetDimensions(0, w, h, levels)`).
2. `texture_samples` (sampleinfo, samplepos of a resource): same missing lowering. Ran.
3. `load_sample_pos_from_id` (samplepos of the rasterizer): "Unhandled intrinsic",
   `bifrost_compile.c:2233`. Ran. `pan_nir_lower_sample_position.c` only lowers
   `load_sample_pos` for the current sample; the same table read with a given index would do.
4. `load_layer_id` asserts `arch >= 9` (`bifrost_compile.c:2174`). Ran, then avoided: a pixel
   shader reads SV_RenderTargetArrayIndex as the flat varying `VARYING_SLOT_LAYER`, which
   compiles.

### The old translator (ShaderTGSI.c at 018f6ef; read, not run, unless noted)

1. `ShaderTGSI.c:1038-1045`: a dynamically indexed immediate constant buffer drops its base.
   `reg.Index += imm` is computed and then `ureg_src_indirect(sx->imms, ...)` is used, so
   `icb[r0.x + 2]` reads `icb[r0.x]`. No corpus shader has a non-zero base (counted), fxc can
   emit one.
2. `ShaderTGSI.c:2054-2058` with `239-242` and `325-332`: `clip_distance_mapping` has two
   entries and gets one per clip/cull *declaration*. A shader with more than two such
   declarations (d3d10core.c:17063 has four) writes entries 2 and 3 (into the neighbouring
   `cull_distance_mapping`, so no memory damage) and `translate_semantic_index` only looks at
   entries 0 and 1: the later declarations get the wrong CLIPDIST index, or the assert.
3. `ShaderTGSI.c:645-648` (and 567-569): a pixel or geometry shader input declared as clip or
   cull distance always gets semantic index 0, so the second distance register reads the
   first.
4. `ShaderTGSI.c:2244-2277`: for an opcode marked `TGSI_LOG_UNSUPPORTED` a release build
   (assert compiled out) still calls `ureg_insn` with `TGSI_OPCODE_LAST`, an opcode that does
   not exist. That is what lod, gather4, samplepos and sampleinfo produce. For opcodes past
   4.1 the table entry is zero-filled (`ShaderTGSI.c:73`, the array is sized by the D3D11
   header's `D3D10_SB_NUM_OPCODES`), which is `TGSI_OPCODE_ARL` without operands.
5. `ShaderTGSI.c:1400-1404`: `ld_ms` ignores the sample index (known).
6. `ShaderTGSI.c:604-611`: the upper-left origin / half-integer pixel centre properties are
   only set for SV_Position declared with `dcl_input_ps_sgv`; fxc declares it with
   `dcl_input_ps_siv` (`ShaderTGSI.c:636-663`), so they are never set. Harmless as long as
   the driver ignores `fs.origin_upper_left`.
7. `ShaderTGSI.c:1119-1139`: `texture_dim_from_tgsi_target` has no cube array case
   (assert, returns 1), so `resinfo_rcpFloat` of a cube array only inverts the width.
8. `ShaderTGSI.c:269-277`, `308-311`: centroid and sample interpolation fall back to plain,
   SV_SampleIndex becomes GENERIC[0].
9. `tgsi_to_nir.c:1533` + `1597` (0114): comparison sampling sets `is_shadow` without
   `is_new_style_shadow`, so the result has four components and `nir_replicate` asserts in a
   build with assertions (ran into the same assertion in the new translator before setting
   the flag; the release driver takes `.xxxx`, which is right).

## 6. Open questions for phase 2 (wiring into Shader.cpp)

1. **Call site.** `Create{Vertex,Pixel,Geometry}Shader`: build the signature from
   `pSignatures`, `options = pDevice->screen->nir_options[stage]`, then
   `state.type = PIPE_SHADER_IR_NIR; state.ir.nir = nir`. The driver takes ownership of the
   shader. If a screen has `finalize_nir`, call it before (Panfrost has none).
2. **Fallback.** Keep `Shader_tgsi_translate` for a shader the new translator refuses
   (`error != NULL`), and a switch (flag file, as the other sb switches) to choose the path
   per boot. Log the message: it names the opcode.
3. **o1 / dual source (0127).** `writes_o1` comes from `tgsi_scan_shader` today: with NIR it
   is `nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA1)`. `SbCreateDualFs` calls
   `tgsi_to_nir(state.tokens)`: it needs its own copy (`nir_shader_clone` before the first
   create, or translate again).
4. **Dumps.** `sb_dump_on()` prints TGSI text and Draw.cpp's draw dump (0157) refers to it;
   the NIR path needs `nir_print_shader` there. `ST_DEBUG_TGSI` likewise.
5. **Stream output.** `ShaderFindOutputMapping` needs `output_mapping` and
   `pipe_stream_output_info.register_index` to mean the output's `driver_location`. That is
   what this translator fills in; how Panfrost consumes it for a NIR shader
   (`so->stream_output`, `nir->xfb_info`, pan_shader.c:675) was not followed through.
6. **Integer vertex attributes.** `sb_retype_int_attribs` (pan_shader.c) works on `load_input`
   after `nir_lower_io`; the inputs here are `vec4` at the same locations, so it should apply
   unchanged (reasoned).
7. **Signature required** for pixel and geometry shaders that read clip or cull distances;
   always pass it.
8. **Driver work to schedule with it**: section 5, items 1-3 (a sysval each in
   `pan_nir_lower_sysvals.c`), a rasterizer sample count for `sampleinfo rasterizer`.
9. **Performance.** Most corpus shaders have no `refactoringAllowed` flag and so get exact
   float math (no fused multiply-add except `mad`, no reassociation). Real applications'
   shaders usually carry the flag. If a title turns out slower, look there first.
10. **Other drivers** (softpipe is in the build): they may want sysvals as varyings
    (`nir_lower_sysvals_to_varyings`) and have no `lower_uniforms_to_ubo`; only Panfrost's
    options were used here.
11. **The parser change and the TGSI path.** lod / gather4 / samplepos / sampleinfo now have
    operands, so the TGSI translator's fall-through for them (item 4 above) translates those
    operands before emitting its invalid opcode. Broken before and after; if the TGSI path
    stays as fallback it should refuse these shaders outright.
12. **glsl type singleton.** The translator builds array and texture types;
    `targets/d3d10umd/d3d10_gdi.c:247` already takes the reference (read).

## 7. Rebuild and rerun

Everything in the VM (`limactl shell starbite bash -c '...'`). The build directory is
`~/sbmesa/build-nir-host`; it is a native Linux configuration with only the Panfrost gallium
driver, no GL, no Vulkan. Its setup and build logs are in it (`build-nir-host.*.log`).

```sh
export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
cd ~/sbmesa

# once (already done; meson reconfigures by itself after that)
meson setup build-nir-host src-nir -Dbuildtype=debugoptimized -Dplatforms= \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dllvm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dvideo-codecs= -Dzstd=disabled -Dshader-cache=disabled -Dbuild-tests=false \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dd3d10umd-nir-tests-tpf-dir=/Users/dev/sbwin/kit/Include/10.0.26100.0/um

# build (about 2 minutes from scratch with ccache, seconds after an edit)
ninja -j6 -C build-nir-host src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness

# run: corpus + self tests + usage statistics (about 1 minute)
cd build-nir-host
W=/Users/dev/project-starbite/build/win/winetests/src
./src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness --selftest --stats \
  $W/d3d10core.c $W/d3d11.c > harness-final.txt 2> harness-final.err
sed -n '/^Summary/,/^What the/p' harness-final.txt
```

Other switches: `--no-compile` (stop at validated NIR), `--no-signature`, `--only TEXT`
(matches "file:line name"), `--print-nir`, `--no-fork` (for gdb), `--gpu-id ID`.
One shader: `... --only "d3d10core.c:6417" --print-nir $W/d3d10core.c`.

Nothing was installed in the VM; the build used what was there (meson from the venv,
`mesa_clc` and `vtn_bindgen2` from `~/sbmesa/native`).
