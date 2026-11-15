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

---

# Phase 2: the switch, the Bifrost v6 gaps, a Windows build

State: 2026-11-15 23:41 EET. Five more commits on `sb-nir` (on top of d9185f0), nothing pushed:

- `c640c16` panfrost: texture level and sample counts, rasterizer sample count and sample
  positions by index before Valhall
- `bfab469` d3d10umd: the TGSI translator refuses what it cannot translate; icb base,
  clip/cull registers
- `0b188df` d3d10umd: vertex and pixel shaders through the NIR translator with
  C:\ProgramData\Starbite\nirshaders
- `cf67267` d3d10umd: the harness also runs the old path, and checks the vertex input form
- `bc37c61` d3d10umd: the harness compiles the dual-source variant of pixel shaders writing o1

Sections 4, 5 and 6 above describe phase 1; where phase 2 changed something it says so here.

## P2.1 The switch (Shader.cpp)

`C:\ProgramData\Starbite\nirshaders`, read with `malikm_switch("nirshaders")` once per process
at the first vertex or pixel shader creation. One line then:
`sbumd: shader translator: DXBC -> NIR (C:\ProgramData\Starbite\nirshaders)` or
`sbumd: shader translator: DXBC -> TGSI`.

With the file, and only on a driver that takes no TGSI (`supported_irs` without
`PIPE_SHADER_IR_TGSI`: Panfrost, not softpipe), `CreateVertexShader` and `CreatePixelShader`
call `Shader_nir_translate` with the signature copied from `pSignatures` and
`screen->nir_options[stage]`, then `create_*_state` with `PIPE_SHADER_IR_NIR`. The driver owns
the NIR afterwards; `state.ir.nir` is cleared. A screen's `finalize_nir` is called if it has
one (Panfrost has none).

A shader the translator refuses takes the TGSI path. The reason is logged the first time
(`sbumd: NIR translation refused, TGSI instead (shader #n of those): <reason>`, 16 distinct
reasons kept), later ones are counted at powers of two.

Geometry shaders stay on TGSI (Panfrost has no geometry stage: `create_gs_state` is not even
set there, so `CreateGeometryShader` calls a null pointer today; untouched). The empty shaders
(`CreateEmptyShader`) stay TGSI built with ureg; they do not depend on the switch.

### Every user of the TGSI tokens, and what happened to it

| Where | What it used the tokens for | Now |
|---|---|---|
| `Shader.cpp` `CreateVertexShader`, `CreatePixelShader` | `Shader_tgsi_translate` | NIR first when the switch is on, else / on refusal TGSI; a NULL result fails the creation (P2.2) |
| `Shader.cpp` `CreateGeometryShader`, `CreateGeometryShaderWithStreamOutput` | `Shader_tgsi_translate` | still TGSI; NULL fails the creation |
| `Shader.cpp` `CreatePixelShader`: `tgsi_scan_shader` for `writes_o1` | does the shader declare COLOR[1] | TGSI path unchanged; NIR path: `nir->info.outputs_written & FRAG_RESULT_DATA1` |
| `Shader.cpp` `SbCreateDualFs`: `tgsi_to_nir(state.tokens)` | a second NIR for the dual-source variant (0127) | NIR path: a `nir_shader_clone` taken at creation (only when the shader writes o1), used once, freed in `DestroyShader` if never used |
| `Shader.cpp` `DestroyShader`: `ureg_free_tokens` | free | unchanged; plus the unused clone |
| `Shader.cpp` `ShaderFindOutputMapping`: `!state.tokens` | "this shader has no code, the register is its own index" | `ShaderHasCode()` (tokens or `sb_nir`) |
| `Draw.cpp` `ResolveState`: `bound_vs->state.tokens` | the bound vertex shader has code | `ShaderHasCode()` |
| `Shader.cpp` `GsSetShader`: `!state.tokens` | stream-output-only geometry shader | unchanged (geometry shaders are always TGSI) |
| `DxgiFns.cpp` `sb_trace_draw` (0116 / 0157): prints `state.tokens` of the bound VS and PS | the name under which the shader text was dumped | `ShaderDumpId()`: the tokens, or the NIR shader's address |
| `ShaderTGSI.c` end: `tgsi_dump_str` to stderr when `sb_dump_on()` | the shader text in the log | NIR path: `nir_print_shader(nir, stderr)` between the same `sbumd: shader %p` / `sbumd: end shader %p` lines, also for `ST_DEBUG_TGSI` |
| `Shader.cpp` `CreateEmptyShader` | ureg | unchanged |

`output_mapping`: the NIR path fills a local table (0xff in unused entries) and copies it on
success, so a failed NIR translation leaves nothing behind for the TGSI translation.
Entry = the output's `driver_location`, as before entry = TGSI output index.

`State.h`: `struct Shader` has three more fields (`sb_nir`, `sb_id`, `sb_dual_nir`), set by
`SbInitShader()` at the top of all four creation functions (the object is runtime memory, not
zeroed).

`sb_retype_int_attribs` (pan_shader.c:343): read. It changes a `load_input` whose offset is
constant and whose type is float32, at slot `base + offset`, where the mask bit is the vertex
element = the D3D input register. The NIR path's inputs are `vec4` variables with
`driver_location` = register at `VERT_ATTRIB_GENERIC0 + register`, so `nir_lower_io` gives
exactly that. Ran: the harness checks these three properties on every `load_input` of every
vertex shader after `nir_lower_io` (67 corpus + 1 self test vertex shaders, no failure). The
pass itself (it is static in pan_shader.c, keyed on draw state) was not executed.

## P2.2 The TGSI path (ShaderTGSI.c)

- **Refusals.** `Shader_tgsi_translate` returns NULL for lod, gather4, samplepos, sampleinfo,
  RESERVED0 and every opcode past Shader Model 4.1, with
  `sbumd: shader refused (#n): no TGSI translation for opcode <name> (<number>)` (first 8, then
  powers of two). The four creation functions then leave the handle NULL and report
  `E_OUTOFMEMORY` through `SetError`: per the DDI documentation that is the one error a shader
  creation may report without the runtime treating it as fatal, and the runtime then neither
  uses nor destroys the handle. Before, the pipe driver was handed tokens containing
  `TGSI_OPCODE_LAST` or `ARL`. A D3D10.0 device is not sent 4.1 or D3D11 shaders by the
  runtime, so this is unreachable until the driver offers more.
- **icb base** (old bug 1): fixed, one line. But read P2.5: on Panfrost the dynamic index
  itself is lost later, in `tgsi_to_nir`.
- **Clip / cull registers** (old bugs 2 and 3): fixed locally. A register's CLIPDIST index is
  looked up by register in a two-entry table that cannot be overrun, for outputs and now for
  inputs (they always got 0); `NUM_CLIPDIST_ENABLED` / `NUM_CULLDIST_ENABLED` count distances
  instead of declarations. Not fixed, because not local: lanes are not reordered (gallium
  wants all clip distances before all cull distances; D3D can interleave them in a register),
  the FIXME stays.

Ran: the TGSI text (`--print-tgsi`) of all 265 corpus shaders that give TGSI, old translator
against new: identical except `NUM_CLIPDIST_ENABLED` 1 -> 2 in 2 vertex shaders and
`NUM_CLIPDIST_ENABLED` / `NUM_CULLDIST_ENABLED` 2/2 -> 4/4 in 2 others
(`harness-p2-tgsi-oldtranslator.txt` against `harness-p2-tgsi.txt`). 4 shaders are now
refused (2 gather4, 2 with D3D11 opcodes) that gave junk before.

## P2.3 Panfrost before Valhall (c640c16)

| Gap | Fix |
|---|---|
| `query_levels` | `PAN_SYSVAL_TEXTURE_LEVELS` (id = texture index), lowered in `pan_nir_lower_sysvals.c` next to `txs`, filled in `pan_cmdstream.c` from the bound sampler view: `last_level - first_level + 1`, 1 for a buffer, 0 for an empty slot |
| `texture_samples` | `PAN_SYSVAL_TEXTURE_SAMPLES`: `max(nr_samples, 1)`, 0 for an empty slot |
| `load_sample_pos_from_id` | `pan_nir_lower_sample_position.c`: the table read it does for the current sample, with the given index |
| rasterizer sample count | `PAN_SYSVAL_RASTERIZER_SAMPLES` from the batch's framebuffer, for `load_rasterization_samples_amd` (vendor name, but the only NIR intrinsic with that meaning) |
| texture size of an empty slot | `panfrost_upload_txs_sysval` dereferenced a NULL sampler view; now zeros (what D3D reports) |

Both texture sysvals mark `PAN_DIRTY_STAGE_TEXTURE` in `panfrost_analyze_sysvals`.

With these, in `ShaderNIR.c`: `resinfo` of a level the resource does not have returns zero
extents again (deviation of phase 1 section 4 removed), `sampleinfo` and `samplepos` of a
resource compile, `sampleinfo rasterizer` is translated (was refused), `samplepos rasterizer`
returns zero for a sample index past the count.

Ran: lowering and Bifrost compile (the harness compiles `pan_nir_lower_sysvals.c` itself).
Not run: the upload code in `pan_cmdstream.c`. It compiles for every architecture in the
Windows build; no draw was made with it.

## P2.4 Harness tables

NIR path, corpus (`harness-p2-corpus.txt`; before = section 3):

| | before | after |
|---|---|---|
| vertex + pixel: translated, validated, compiled | 227 | **244** (vs 67, ps 177) |
| failed in the backend | 17 | **0** |
| geometry: translated and validated | 23 | 23 |
| failed to translate or validate | 0 | 0 |
| out of scope (SM5 92 + cs_4_0 1; D3D11 opcodes in 4.x 2) | 95 | 95 |

16 of the pixel shaders write o1; their dual-source variant (what `SbCreateDualFs` builds from
the clone) compiles too. 11 vertex shaders without SV_Position give an empty binary, as before.

Self tests (`harness-p2.txt`): 80; 79 OK (71 value-checked, 8 translate + validate + compile),
1 refused as intended (`call`), 0 backend failures (before: 4, and 2 refused).

Old path (`--tgsi`: ShaderTGSI.c + tgsi_to_nir built without assertions like the release
driver, then the same Panfrost passes). 246 vertex + pixel shaders of 4.x, including the two
with D3D11 opcodes that the NIR runs count as out of scope:

| | old translator, old driver | old translator, fixed driver | new translator, fixed driver |
|---|---|---|---|
| compiled | 224 | 233 | 233 |
| backend: `query_levels` | 9 | 0 | 0 |
| `switch`: `tgsi_to_nir` prints "unknown TGSI opcode: SWITCH" and calls `abort()` | 7 | 7 | 7 |
| cull distances: "unhandled TGSI property 16", then undefined behaviour | 2 | 2 | 2 |
| gather4 / D3D11 opcodes | 4 (junk TGSI) | 4 (junk TGSI) | 4 (refused) |

So with the switch on, on this corpus, 11 more vertex/pixel shaders work than on the old
path (244 against 233), not counting the two with a dynamically indexed immediate constant
buffer that the old path compiles but gets wrong (P2.5).

## P2.5 Found in the old path (ran, unless noted)

1. **`switch` aborts the process.** `tgsi_to_nir` has no SWITCH / CASE / DEFAULT / ENDSWITCH
   (`tgsi_to_nir.c:1991-1992`, `2285-2287`). A D3D shader with a `switch` that fxc does not
   flatten ends in `abort()` inside `create_*_state`. 7 corpus pixel shaders.
2. **Dynamically indexed immediate constant buffer** (`static const` array indexed by a
   variable): `tgsi_to_nir.c:581-585` asserts `!indirect` for immediates and, in a release
   build, ignores the index and returns the element at the base. 2 corpus shaders compile and
   read the wrong element. The icb base fix in ShaderTGSI.c changes which wrong element
   (`icb[n]` instead of `icb[0]` for `icb[r + n]`); it is only right on a driver that takes
   TGSI itself.
3. **Cull distances**: `NUM_CULLDIST_ENABLED` is a property `tgsi_to_nir` does not know
   (`tgsi_to_nir.c:2587-2592`: message, then `UNREACHABLE`). 2 corpus vertex shaders.
4. `tgsi_to_nir.c:654`, `:650`: asserts that the fragment position / face are not declared as
   inputs when the driver wants system values; the frontend declares inputs. A release build
   goes on correctly (the variable it loads was made a system value); a build of the driver
   with assertions would stop at 69 + 2 corpus shaders.
5. Phase 1's old-path items 2 (clip mapping overrun) and 4 (invalid opcodes) were confirmed by
   their assertions in the first, assertion-enabled run of the old path.

## P2.6 The Windows build

Build directories (all new, `-j6` at most, one build at a time): `~/sbmesa/build-nir-a64`
(cross.ini), `~/sbmesa/build-nir-ec` (cross-ec.ini), `~/sbmesa/build-nir-x` (link). The meson
options are those of `build-int-a64` / `build-int-ec` (`meson-private/cmd_line.txt` identical;
`intro-buildoptions.json` differs only in the new, empty `d3d10umd-nir-tests-tpf-dir`).
`link-arm64x.sh`, `altnames_ec.S`, `tlsindex_ec.c`, `exports_a64.c`, `verify_arm64x.py` are
copies from `build-int-x` with `build-int-*` replaced by `build-nir-*`. Nothing under
`build-int-*` or `src-int` was written.

```sh
~/sbmesa/build-nir-x/build-all.sh     # a64, ec, link; exit status in build-nir-x/build-all.exit
```

Result, built from the tree at `cf67267` (`bc37c61` only touches the harness):

- `/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir/sbumd.dll`
  sha256 `98e9e6a68770d8b310cf6fec8240a2e03906062aba01af47b5b601def8b68275` (27227648 bytes)
- `.../umd-nir/sbumd.map` sha256 `f9285b15e5ff1191399da47c5ef96faf8dbeeb4d541e3561e2f08a7674cddb0a`
- `verify_arm64x.py`: `FAILS: 0` (35 `ok` lines), `lld-link` without warnings.
- `strings`: `nirshaders`, `DXBC -> NIR (C:\ProgramData\Starbite\nirshaders)`,
  `sbumd: shader translator: %s`, `opcode 0x%x is a Direct3D 11 opcode`,
  `sbumd: NIR translation refused, TGSI instead (shader #%u of those): %s`,
  `sbumd: shader refused (#%u): no TGSI translation for opcode %s (%u)`.

The DLL was not loaded anywhere.

## P2.7 With the switch OFF: what differs from 018f6ef

The intent is no change except the refusals. Exactly:

1. One more log line per process (`sbumd: shader translator: DXBC -> TGSI`) and one
   `GetFileAttributesA` at the first vertex or pixel shader creation.
2. `struct Shader` is three fields larger (`CalcPrivate*Size` returns `sizeof`). The creation
   functions set `handle = NULL` and the 0127 fields before translating instead of after.
3. Shaders with lod, gather4, samplepos, sampleinfo or post-4.1 opcodes fail their creation
   with `E_OUTOFMEMORY` (P2.2). Not reachable from a D3D10.0 device.
4. `icb[r + n]`: TGSI now carries the base `n` (P2.5 item 2 for what Panfrost makes of it).
5. Clip / cull distances: the two properties count distances; a second distance register read
   by a pixel or geometry shader gets CLIPDIST[1]. On Panfrost a vertex shader with
   `float2`..`float4` clip distances now gets a clip distance array of that many floats where
   it got one per declaration. Shaders with one `float` clip distance are unchanged.
6. The Panfrost changes of P2.3 apply to both paths: `resinfo` mip count and sample count
   queries compile, a size query on an empty slot gives zeros, three new sysval kinds exist.
   A shader that asks for the size and the level count of many textures uses more of the 32
   sysval slots (`MAX_SYSVAL_COUNT`, an assert) than before.
7. From phase 1: the parser reads the operands of the four 4.1 opcodes and skips unknown
   custom data. With item 3 the TGSI translator no longer looks at those operands.

Reasoned, not run: items 1 to 3 (they are Windows code); 4 to 6 are covered by the TGSI text
comparison and the harness as far as the compiler goes, not by a draw.

## P2.8 Verified by running / only reasoned (phase 2)

Ran (Linux host, assertions on unless said): both translators over the corpus into the Bifrost
compiler; the self tests; the dual-source variants; the vertex input form check; old against
new TGSI text; the refusal message of the TGSI translator; ARM64 and ARM64EC compiles of the
whole driver; the ARM64X link and its verification; `strings` on the DLL.

Not run: the driver on Windows or on the phone, so: the switch file and its log line, the
fallback counting, `SetError` on a refused shader, the NIR print in the draw dump, the sysval
upload at draw time, vertex/pixel linkage of NIR-path shaders, `sb_retype_int_attribs` itself,
stream output (not functional on Panfrost in either path: no geometry stage), softpipe (keeps
TGSI by design).

## P2.9 Rerun

```sh
export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
cd ~/sbmesa && ninja -j6 -C build-nir-host src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
cd build-nir-host
W=/Users/dev/project-starbite/build/win/winetests/src
H=./src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
$H --selftest --stats $W/d3d10core.c $W/d3d11.c > harness-p2.txt        # NIR path
$H --print-tgsi $W/d3d10core.c $W/d3d11.c > harness-p2-tgsi.txt         # old path, with TGSI text
```

To try it on a device: install the DLL, create the empty file
`C:\ProgramData\Starbite\nirshaders`, and look for the `shader translator` line and any
`NIR translation refused` lines in the umdlog.
