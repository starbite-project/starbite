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

# Phase 3: geometry shaders

State: 2026-11-16 00:28 EET. Branch `sb-nir`, seven commits on top of phase 2 (cf67267 and the
harness-only bc37c61), nothing pushed:

- `b4003da` poly: build for Panfrost, and on Windows
- `5b0383e` panfrost: lower geometry shaders with src/poly (pan_nir_lower_gs.c)
- `b062195` d3d10umd: the harness lowers and compiles geometry shaders
- `3a4a2af` panfrost: geometry shaders on Bifrost behind gsemu (rasterization only)
- `fd92c68` d3d10umd: geometry shaders through the NIR translator where the driver takes them
- `a658e21` panfrost: a geometry shader draw without a count is skipped
- `4c517c3` panfrost: the jobs of a geometry shader draw depend on each other

**Nothing of this has drawn anything.** What ran is the lowering and the Bifrost compiler on the
Linux host. The draw path, the switch and the frontend change are compiled only (P3.9).

Switches: `C:\ProgramData\Starbite\gsemu` on Windows (read once at screen creation, like
`noafbc`), `PAN_MESA_DEBUG=gsemu` on Linux. Off by default. Geometry shaders reach the driver
only as NIR, so `gsemu` does something only together with `nirshaders`.

## P3.1 What src/poly does, as asahi uses it (read)

`poly_nir_lower_gs` (src/poly/nir/poly_nir_lower_gs.c) turns one geometry shader into four:

| poly's shader | What it is | asahi runs it as |
|---|---|---|
| rasterization shader (`gs_copy`) | a vertex shader: runs the geometry shader for one input primitive and keeps one emitted vertex, chosen by vertex ID (and instance ID or an index buffer, by shape). It also does the stream output writes | the hardware vertex shader of the final draw |
| main | per input primitive: writes that primitive's part of the index buffer when the output topology depends on data | compute kernel |
| count | per input primitive: how many vertices/primitives it emits, when that is not static | compute kernel, then a prefix sum kernel over the counts |
| pre-GS | once per draw: sizes stream output, advances its offsets, adds to the statistics queries | compute kernel |

Ahead of them asahi runs the vertex shader as a compute kernel with input assembly in software
(`poly_nir_lower_sw_vs`, one invocation per index and instance), and for an indirect draw a
set-up kernel that reads the draw on the GPU and allocates from a GPU-side heap
(`agx_launch_gs_prerast` in asahi's agx_state.c). Primitive restart in the input is unrolled
first by one more kernel. Everything the shaders need at run time is in two structures,
`struct poly_vertex_params` (64 bytes) and `struct poly_geometry_params` (316 bytes).

poly sorts the rasterization draw into four shapes (`enum poly_gs_shape`): three with a
topology known at compile time (no main kernel, no index buffer from the GPU) and one dynamic.
It only finds a static topology if the geometry shader is straight-line code up to its emits.

## P3.2 What Panfrost's job manager back end has (read)

- A vertex shader variant that only has side effects and runs as a compute-type job in the
  draw's own batch, attributes fetched by the hardware: the transform feedback variant
  (`pan_nir_lower_xfb`, `jm_launch_xfb`). No IDVS for it.
- Compute jobs can be added to the framebuffer's batch (`panfrost_launch_grid_on_batch`).
- 64-bit `load_global`/`store_global` compile on Bifrost; sysvals carry per-draw values.
- The order of jobs in a chain is **not** the order of execution; a job waits for the (at most
  two) jobs it names as dependencies (the rules in src/panfrost/lib/pan_jc.h).
- No indirect draws or dispatches on the GPU before Valhall, no GPU-side allocator, no layered
  rendering in the Gallium driver.

## P3.3 The design

The vertex shader stays a hardware job. That is the one departure from asahi, and it removes
the software input assembly: no index-driven compute kernel, no vertex fetch emulation.

| Piece | Made | Runs as | Size of the job |
|---|---|---|---|
| capture variant of the bound vertex shader (`pan_nir_lower_vs_for_gs`, key `vs.gs_capture`): every output is stored at `output_buffer + (instance * captured + raw vertex ID) * outputs * 16` | first draw with a geometry shader that uses this vertex shader | compute-type vertex job, as the transform feedback variant | the draw's vertex range (max index - min index + 1, or the count) x instances |
| index kernel (poly's main) | `create_gs_state`, dynamic topology only | compute job | one invocation per input primitive and instance |
| rasterization shader | `create_gs_state`, as an ordinary vertex shader CSO (variants, IDVS as any other) | the draw | by shape, below |
| count, pre-GS | made by poly, freed again | never | - |

`lower_gs_input` (pan_nir_lower_gs.c) replaces poly's input lowering: the geometry shader looks
its vertices up in the application's index buffer itself (8/16/32 bit without a branch),
subtracts the index the capture started at, clamps, and reads the captured outputs. Being
branch-free matters: with it poly proves a static topology for 18 of the 23 corpus shaders.
The capture's first index travels in the one unused word of `struct poly_vertex_params`.

The rasterization draw, by shape (corpus count):

| Shape | Draw |
|---|---|
| static per primitive (9) | not indexed, `max_indices` vertices, input primitives x instances as hardware instances |
| static per instance (9) | not indexed, `max_indices` x input primitives vertices, the draw's instances |
| static indexed (0) | 8-bit index buffer that is the shader's fixed topology (at most 64 entries), instances as per primitive |
| dynamic indexed (5) | 32-bit index buffer, `max_indices` x input primitives x instances entries, written by the index kernel and padded with restart indices; one instance; vertex IDs up to (instances << ceil(log2(input primitives))) x vertices_out rounded up to a power of two |

One draw with a geometry shader (`panfrost_single_draw_gs`, pan_cmdstream.c), all in the batch
of the framebuffer:

1. capture job;
2. index kernel, if any; it depends on 1;
3. vertex job (or indexed vertex job) of the rasterization draw; it depends on 2, or on 1 if
   there is no 2; the tiler job depends on its vertex job as always.

The dependencies are explicit job dependencies (`sb_gs.job_dep`), added in 4c517c3 after
reading pan_jc.h: the first version relied on the order of the chain, which orders nothing.

The rasterization shader and the index kernel are the geometry shader, so they read the
geometry stage's constant buffers, textures and samplers: those are swapped into the stage the
job runs under for its duration (`panfrost_gs_swap_stage`).

Memory per draw, all from the batch's transient pool (freed with the batch, nothing allocated
from the kernel per draw, nothing the CPU reads back):

- the two parameter structures, 64 + 316 bytes;
- the capture: captured vertices x instances x vertex shader outputs x 16 bytes
  (3000 vertices with 4 outputs: 192,000 bytes);
- dynamic topology: 4 bytes x `max_indices` x input primitives x instances;
- the rasterization draw's varyings as for any draw, for the vertex count of the table above.

### Where the CPU reads, waits or flushes

| Draw with a geometry shader | What happens |
|---|---|
| not indexed | nothing: no flush, no readback, no wait |
| indexed, no restart | the min/max index scan of the index buffer that every indexed draw has (`panfrost_get_index_buffer_bounded`, cached) |
| indexed with primitive restart | `util_draw_vbo_without_prim_restart`: the index buffer is mapped for reading and scanned (index count, every draw, no cache), and the draw is split at the restart indices into one geometry shader draw per run. The map flushes and waits only if a batch writes the index buffer. **d3d10umd sets `primitive_restart` on every indexed draw (Draw.cpp:223 and 328), so every indexed draw from Direct3D 10 takes this.** |
| indirect | `util_draw_indirect`: the indirect buffer is mapped and read; a flush and a wait if the GPU wrote it. d3d10umd issues none |
| count from a stream output target (DrawAuto) | dropped with a `perf_debug` line (a658e21; it read a null pointer before). Doing it would be a readback of the count: flush and wait |
| any | descriptors are emitted three times instead of once (`panfrost_dirty_state_all` between the jobs); the capture variant is compiled at the first such draw of a vertex shader |

No draw with a geometry shader flushes the batch or starts another one between its jobs.

## P3.4 Implemented, and not

Implemented (compiled; lowering and shader compilation ran):

- `gsemu` switch; with it, for Bifrost (arch 6 and 7) only: geometry stage caps (NIR only,
  1024 output vertices and components, no side effects), `create_gs_state`, `bind_gs_state`,
  `delete_gs_state`, the draw path above for all four shapes, with instancing, indexed draws
  and the geometry stage's own resources.
- Frontend: with `nirshaders` and a driver that takes NIR geometry shaders, both
  `CreateGeometryShader` and `CreateGeometryShaderWithStreamOutput` go through
  `Shader_nir_translate`; otherwise TGSI as before.

Not implemented, on purpose rather than by half:

- **Stream output.** A geometry shader created with stream output declarations rasterizes
  only (one `mesa_logw` line at creation, a `perf_debug` line per draw with targets bound).
  A shader state without code (Direct3D 10's stream output of the vertex shader) is refused
  with its own warning and nothing is bound. poly does stream output in the rasterization
  shader plus the count kernel, a prefix sum and the pre-GS kernel. Here that needs: NIR
  transform feedback info built from `pipe_stream_output_info`, the rasterization shader run a
  second time as a side-effect job (a vertex job with IDVS may not write memory), poly's
  prefix sum kernel on Bifrost, stream output offsets that live on the GPU where Panfrost
  keeps them on the CPU, and a readback for DrawAuto. The count and pre-GS kernels compile
  (harness) and are thrown away by the driver.
- Pipeline statistics for the geometry stage (poly's query addresses are lowered to nothing).
  The primitives-generated count is taken from the rasterization draw's vertex count and is
  wrong with a geometry shader.
- Render target layer and viewport index written by a geometry shader: stored as varyings,
  not acted on (the Gallium driver has no layered rendering on these GPUs; read, not run).
- A geometry shader in TGSI: `create_gs_state` returns NULL with a warning, draws then run
  without it. (Without `gsemu` the hooks do not exist at all, as before.)
- Midgard and Valhall: `panfrost_device_has_gs()` is false there.

## P3.5 Harness: geometry shaders (ran at 4c517c3)

`~/sbmesa/build-nir-host/harness-p3.txt` (corpus + self tests: 346 OK, 1 refused as it must
be, 0 failures) and `harness-p3-corpus.txt` (267 OK: 67 vertex, 177 pixel, 23 geometry).
For each geometry shader the harness runs `panfrost_nir_lower_gs`, validates the pieces,
lowers a generated pass-through vertex shader writing the geometry shader's inputs with
`pan_nir_lower_vs_for_gs` (not the test's own vertex shader), and compiles all five results
for Bifrost arch 6 (gpu id 0x62210000). Bytes of Bifrost code:

| Shape, indices per input primitive | Shaders | vs capture | rast | index kernel | count | pre-GS |
|---|---|---|---|---|---|---|
| static per primitive, 4 | 8 (7 gs_4_0, 1 gs_4_1) | 544-880 | 1904-3616 | 0 | - | 544 |
| static per primitive, 3 | 1 | 544 | 3488 | 0 | - | 544 |
| static per instance, 1 | 6 | 880-1488 | 1840-2400, one 0 | 0 | - | 528 |
| static per instance, 3 | 3 | 544-880 | 3056-5280 | 0 | - | 528 |
| dynamic indexed, 16 | 2 | 544 | 3328 | 2528 | 208 | 640 |
| dynamic indexed, 8 | 2 | 544 | 3408 | 1376 | 208 | 640 |
| dynamic indexed, 4 | 1 | 544 | 2992 | 800 | 208 | 640 |
| all | 23 of 23 lower and compile | | | | | |

The one rasterization shader of 0 bytes is d3d11.c:27485, which writes no SV_Position (it
exists for stream output). A static topology has no count shader and an empty index kernel.
The static indexed shape does not occur in the corpus: its draw path has no shader behind it
even at this level.

The OpenCL helpers (`src/poly/cl`, built with `mesa_clc` and `vtn_bindgen2` from
`~/sbmesa/native`, which both build directories already used as system tools) build on the
host and in both Windows cross configurations. One source change was needed for Windows:
`uint` in src/poly/geometry.h (b4003da). Nothing was installed or downloaded.

## P3.6 What tessellation could reuse

libpoly and libpoly_nir in the Panfrost build, including Windows; the parameter structures
reached through a sysval (`struct poly_vertex_params` is also what poly's tessellation
lowering reads); the capture variant as the source of control point data; kernels in the
framebuffer's batch with explicit job dependencies; the stage swap for running one stage's
shader under another job type; the harness path that compiles lowered pieces. What it adds
and geometry shaders did not need: poly's tessellator kernel produces an amount of geometry
known only on the GPU, and these GPUs have no indirect draw, so each such draw costs either a
readback or a worst-case allocation.

## P3.7 Risks, most serious first

1. No job of this has run on a GPU. In particular: the capture job with more than one
   instance (it is the transform feedback job's layout, which upstream has; unrun here), the
   explicit dependencies, a compute job between draws of one batch.
2. Every rasterized vertex runs the whole geometry shader (poly's design). A shader that
   emits 16 vertices runs 16 times per input primitive, and for a dynamic topology the vertex
   IDs are padded to powers of two.
3. Indexed draws from d3d10umd always go through the CPU restart scan (P3.3). Setting
   `primitive_restart` only for strip topologies in the frontend would remove it for lists; that
   is a frontend change in behaviour, so not done.
4. The first-index word reuses a padding field of poly's structure; a Mesa update that gives
   the field a meaning breaks it (a `_pad` rename fails to compile, which is the alarm).
5. Three full state emissions per draw, and transient memory proportional to vertices x
   outputs: many small geometry shader draws are much more expensive than ordinary ones.
6. With `gsemu` but without `nirshaders`, a geometry shader is silently not applied.

## P3.8 Switch off: what differs from cf67267

Read from the diff, not run on Windows. Nothing in behaviour:

- No geometry caps, no geometry hooks: `create_gs_state`, `bind_gs_state` and
  `delete_gs_state` stay unset on Panfrost. d3d10umd therefore still calls a null pointer in
  `CreateGeometryShader` and `GsSetShader` on Panfrost, exactly as before (phase 2 noted it;
  left alone because the instruction was no change with the switch off).
- Code that now runs and decides nothing: a test of `sb_gs.bound` per `draw_vbo` and of
  `sb_gs.rast_indices` per indexed draw (arch 6/7 builds), `sb_gs.job_dep`, which is 0, where
  three `pan_jc_add_job` calls had a literal 0, a test of `key->vs.gs_capture` (false) per
  vertex shader compile.
- Data: `struct panfrost_context` and `struct panfrost_uncompiled_shader` grew; the vertex
  shader key has one more field that is always false; two sysval numbers exist and are not used.
- Frontend: `SbTranslateNir` is now asked for geometry shaders too and returns NULL at its
  first test (no `nirshaders`, or a geometry stage without NIR, which is what Panfrost
  reports without `gsemu`); the TGSI path follows as before. `GsSetShader` and
  `output_resolved` ask `ShaderHasCode()` where they asked `state.tokens`: the same for a
  TGSI shader.
- The DLL is 144,384 bytes larger (libpoly), and `gsemu` is a new name in `PAN_MESA_DEBUG`.
- The build of Panfrost now needs `mesa_clc` and `vtn_bindgen2`.

Found and left alone: on Panfrost `DrawAuto` without a geometry shader passes a NULL draw list
to `panfrost_multi_draw_direct` (d3d10umd Draw.cpp:379), with or without these changes.

## P3.9 Verified by running / only compiled / reasoned

Ran (Linux host, assertions on): `panfrost_nir_lower_gs` on all 23 corpus geometry shaders
with NIR validation of every piece; the Bifrost arch 6 compile of every piece and of the
capture lowering on pass-through vertex shaders; the whole harness again (no change in the
vertex and pixel numbers of phase 2); native builds of libpoly and of Panfrost for every
architecture; ARM64 and ARM64EC builds of the driver; the ARM64X link and its verification;
`strings` on the DLL.

Compiled only, never executed: everything in pan_cmdstream.c, pan_jm.c, pan_shader.c and
pan_screen.c of this phase (the draw path, the job dependencies, the hooks, the caps, the
switch), the capture variant as the driver compiles it (`panfrost_shader_compile` with the
key; the harness calls the lowering pass itself), the sysval upload, and the frontend change.
No lowered shader was executed either, so that the lowered code computes the right thing is
by reading: nothing here checks an output vertex.

Reasoned from source: the meaning of job dependencies, the flush and wait behaviour of the
buffer maps in P3.3, that an empty rasterization shader skips rasterization
(`panfrost_batch_skip_rasterization`), that layer and viewport outputs are not acted on.

## P3.10 The Windows build

`~/sbmesa/build-nir-x/build-all.sh`, from 4c517c3: ARM64 and ARM64EC builds exit 0, the link
log is empty, `verify.txt` has 35 `ok` lines and `FAILS: 0`.

```
/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir-gs/
  sbumd.dll  27372032 bytes  sha256 e9e9d06723a0eeb6e7c3d66ebd7c0c8c320f4dc9475f662bf250799cbc6e89e9
  sbumd.map   6734469 bytes  sha256 6ff3a476f67848882421049886fa0cbd4fad9f89bcf8d45d2f710e50be8820b9
```

`build/win/umd-nir/sbumd.dll` is untouched (still 98e9e6a6...). `strings` finds `gsemu`,
`sbumd: geometry shader emulation on (C:\ProgramData\Starbite\gsemu)` and the warnings of
P3.4 in the new DLL.

## P3.11 Rerun

```sh
export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
cd ~/sbmesa && ninja -j6 -C build-nir-host src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
cd build-nir-host
W=/Users/dev/project-starbite/build/win/winetests/src
H=./src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
$H --selftest --stats $W/d3d10core.c $W/d3d11.c > harness-p3.txt
grep "gs_4_"  harness-p3.txt          # one line per geometry shader: shape and code sizes
~/sbmesa/build-nir-x/build-all.sh     # Windows; result in build-nir-x/build-all.exit, verify.txt
```

To try it on a device: install the DLL, create the empty files
`C:\ProgramData\Starbite\nirshaders` and `C:\ProgramData\Starbite\gsemu`, and look for the
`geometry shader emulation on` line in the umdlog. Start with a draw that is not indexed and
has a static topology (a point-to-quad shader): it is the shortest path, two jobs and no
index buffer. On Linux the switch is `PAN_MESA_DEBUG=gsemu`.

# Phase 4: Shader Model 5

State: 2026-11-16 01:04 EET. Branch `sb-nir`, three commits on top of phase 3 (4c517c3),
nothing pushed:

- `f453dd8` d3d10umd: Shader Model 5.0 in the token parser and the NIR translator
- `9606fa1` d3d10umd: self tests for Shader Model 5
- `dbf9ef4` d3d10umd: a hull shader input that is a clip or cull distance is refused

Translator and harness only. `Shader.cpp` is untouched: nothing in the driver hands the
translator a Shader Model 5 stream, there is no Direct3D 11 DDI, and no switch changed. No
DLL was built for delivery (P4.9 says what was compiled for Windows).

## P4.1 What there is

| File | What |
|---|---|
| `ShaderParse.c/.h` | operand counts for every opcode up to Direct3D 11.1; chained extended opcode tokens (sample controls, resource dimension, return type); extended operand tokens on destinations (minimum precision); the declarations of 11 (streams, control point counts, tessellator, phase instance counts, thread group, UAVs, raw and structured resources, shared memory, GS instance count); all global flags; `sync` flags; the precise mask; 64-bit immediates (skipped). An instruction in a form the parser does not read is skipped by its length and marked (`opcode->unsupported`). Every difference is behind `major_version >= 5` |
| `ShaderNIR.c/.h` | Shader Model 5.0 for vertex, pixel, geometry, compute, hull and domain shaders; `cs_4_0`; the signature gained the patch constant list and a stream per entry |
| `tests/dxbc_nir_harness.c` | all of the corpus in scope, PCSG / PSG1 chunks and OSG5 streams read, tables by stage and model, statistics over all opcodes |
| `tests/dxbc_nir_selftest.c` | 38 more hand-assembled shaders |

## P4.2 Binding model

Chosen so that a Direct3D 11 frontend binds by register number, without a table per shader:

| HLSL register | Gallium | Note |
|---|---|---|
| `cbN` | constant buffer slot N + 1 | as in Shader Model 4 (slot 0 is the driver's) |
| `sN` | sampler slot N | |
| `tN`, typed | sampler view slot N | |
| `tN`, raw or structured buffer | sampler view slot N, **as a buffer view of format R32_UINT** | read with `txf`, one word per lane: element (byte offset / 4) + lane |
| `uN`, typed (texture or typed buffer) | shader image slot N | index-based image intrinsics, format NONE (Panfrost has `image_load_formatted`); atomics use R32_UINT / R32_SINT |
| `uN`, raw or structured buffer | shader buffer slot N | `load_ssbo` / `store_ssbo` / `ssbo_atomic` with byte offsets |
| counter of structured `uN` | shader buffer slot N + 8, word 0 | `imm_atomic_alloc` / `imm_atomic_consume`; the frontend owns a 4-byte buffer per view and copies the count in and out |
| `gN` (thread group shared memory) | NIR shared memory, the blocks one after the other from offset 0 | `info.shared_size`, explicit layout |

Why raw and structured shader resources are buffer textures and not shader buffers: tN and
uN are two namespaces, and Panfrost gives the vertex and geometry stages no shader buffers at
all (`allow_side_effects` in pan_screen.c sets `max_shader_buffers` and `max_shader_images`
to 0 there, 16 and 64 for pixel and compute). A buffer texture keeps tN = sampler view N in
every stage. Its limit on Bifrost is 2^27 elements (`pan_get_max_texel_buffer_elements`),
which is Direct3D 11's own limit in words.

Limits that follow: eight UAV slots (`SN_MAX_UAVS`, Direct3D 11.0's number; 11.1's 64 would
need a table, since counters and buffers share gallium's 32 shader buffer slots, of which
Panfrost has 16); UAVs in vertex, geometry, hull and domain shaders (11.1) translate but
Panfrost cannot run them; a structured stride must be a multiple of four bytes.

Reads and writes outside a buffer are not made safe here: Direct3D wants zero from such a
read and the write dropped; NIR leaves both to the driver.

## P4.3 Tessellation: the NIR shape

A hull shader is one NIR tessellation control shader in the GLSL form:

```
main() {
   control point phase                      // per invocation = per output control point
   cp_out[invocation].* = its o registers
   barrier(shader_out)                      // only if there are fork or join phases
   if (invocation == 0) {
      fork phases, each instance; join phases, each instance   // in stream order
      patch outputs, tess levels = the patch constant registers
   }
}
```

- Each phase is translated as a function with its own temporaries (a `ret` ends the phase,
  not the shader) and inlined; an instanced phase is called once per instance with the
  instance number as a constant, so `o[vForkInstanceID + n]` folds to one register per copy.
- Control point outputs: `vec4[output control points]` at VARYING_SLOT_VAR0 + register (POS
  for SV_Position). Patch constants: `patch vec4` at VARYING_SLOT_PATCH0 + register. The
  tessellation factors go to `gl_TessLevelOuter/Inner` (compact float arrays) by their system
  value: Direct3D names an edge by the coordinate that is constant along it, GL numbers the
  edges the same way, so quad and triangle factors map index for index; the two isoline
  factors swap (density is outer[0], detail outer[1]).
- `vicp` and the control point phase's `v[][]` are per-vertex inputs, `vocp` reads the
  per-vertex outputs (hence the barrier), `vpc` in a join phase reads the patch constant
  registers the fork phases wrote, `vOutputControlPointID` is the invocation ID.
- A hull shader without a control point phase copies input to output control points; which
  registers those are is only in the input signature.
- `dcl_hs_max_tessfactor`: the factors are clamped from above with `fmin` at the store.
- `info.tess`: `tcs_vertices_out`, and from the hull shader's declarations the primitive mode,
  spacing, winding and point mode. The domain shader only knows the domain.

A domain shader is a tessellation evaluation shader: `vDomain` is `load_tess_coord`,
`vcp[][]` per-vertex inputs, `vpc` patch inputs or the tessellation level arrays, outputs
as a vertex shader's.

Why this fits `poly_nir_lower_tess.c` (read): `poly_nir_lower_tcs` runs the whole shader once
per output control point as a compute invocation and lowers exactly these things:
`load_invocation_id` (the local invocation), per-vertex input loads, per-vertex and patch
output stores and loads to its patch buffer, and a `barrier` whose modes include
`nir_var_shader_out` (dropped or narrowed to a subgroup barrier). `poly_nir_lower_tes` takes
`load_tess_coord`, patch and per-vertex input loads, the level loads and
`info.tess._primitive_mode`. Neither was run on these shaders: they validate, that is all.

## P4.4 What a Direct3D 11 frontend has to pass and do

- The signatures, as `struct Shader_nir_signature`: input and output (with the stream of
  each output for a geometry shader: OSG5), and the patch constant signature (PCSG) of a hull
  or domain shader. The token stream names the tessellation factors itself, so PCSG only
  matters for a register declared without a name. **The input signature is required for a
  hull shader without a control point phase.**
- Bind by the table of P4.2: R32_UINT buffer views for raw and structured SRVs, shader
  buffers for raw and structured UAVs, a counter buffer per structured UAV at slot N + 8.
- Copy `info.tess` (spacing, winding, point mode) from the hull shader's NIR to the domain
  shader's when the two are paired, as Vulkan drivers merge the two stages' info.
- Geometry: the translator leaves every stream's outputs in the shader (`var->data.stream`,
  `active_stream_mask`); which stream is rasterized (RasterizedStream) is the driver's
  business, and pan_nir_lower_gs.c rasterizes stream 0. Instancing is `info.gs.invocations`;
  the `gsemu` caps say 1 today.
- Compute needs nothing beyond `create_compute_state` with the NIR; the thread group is
  `info.workgroup_size`.

## P4.5 Refused, with the message

| What | Message contains |
|---|---|
| Shader Model 5.1 and later | `Shader Model 5.1 is not supported` |
| 64-bit floats: the global flag, the `d*` opcodes, a 64-bit immediate | `enableDoublePrecisionFloatOps`, `opcode DADD (64-bit floats are not supported)`. Panfrost has no `doubles` cap, and the Bifrost options only name `nir_lower_dmod` with a TODO |
| interfaces (`dcl_function_body`, `dcl_function_table`, `dcl_interface`, `fcall`) | `opcode INTERFACE_CALL is not supported` (parsed by length only) |
| what came after Direct3D 11.1: the feedback and clamp sample opcodes of tiled resources, `check_access_fully_mapped` | `an opcode the parser does not know` |
| an operand with three indices or a 64-bit index | `an operand with three indices or a 64-bit index` |
| UAV slot 8 and up | `unordered access view slot 8 out of range` |
| a hull shader input that is a clip or cull distance | `clip or cull distance input of a hull shader` (as a control point *output* and a domain shader input it is plain data and works) |
| a hull shader without a control point phase and without a signature | `needs its input signature` |
| clip or cull distance in a geometry stream other than 0 | `clip or cull distance in geometry stream` |
| `sync` of the threads of a group outside a compute shader, a store to a read-only buffer, an atomic on a float UAV, raw/structured mixed up | named in the message |
| subroutines (`label`, `call`), as before | `opcode CALL is not supported` |

Accepted and ignored: `abort`, `debug_break`, the minimum precision of operands, the
skipOptimization and shader-extension global flags.

## P4.6 Harness tables (ran at dbf9ef4, Bifrost arch 6)

Corpus only, `~/sbmesa/build-nir-host/harness-p4-corpus.txt`:

| | 4.x all | vs_4_0 | vs_4_1 | ps_4_0 | ps_4_1 | gs_4_0 | gs_4_1 | cs_4_0 |
|---|---|---|---|---|---|---|---|---|
| OK, compiled | 270 | 66 | 1 | 172 | 7 | 22 | 1 | 1 |
| anything else | 0 | | | | | | | |

| | 5.0 all | vs_5_0 | ps_5_0 | gs_5_0 | hs_5_0 | ds_5_0 | cs_5_0 |
|---|---|---|---|---|---|---|---|
| OK, compiled | 83 | 1 | 57 | 3 | - | - | 22 |
| OK, validated only | 9 | - | - | - | 5 | 4 | - |
| failed or refused | 0 | | | | | | |

362 containers, all in scope, 362 translate and validate, 353 compile, 0 fail. The three
4.x shaders that were out of scope before (`cs_4_0` and two ps_4_0 with Direct3D 11 opcodes)
are in the 270.

With the self tests, `harness-p4.txt`: 480 containers; 4.x 349 OK + 1 refused as intended;
5.0 108 compiled, 11 validated only (6 hull, 5 domain), 11 refused as intended; 0 failures,
0 wrong results.

Shader Model 4 did not move: every 4.x line of `harness-p4.txt` equals its line in
phase 3's `harness-p3.txt`, apart from the three shaders that came into scope. The old path
(`--tgsi`, `harness-p4-tgsi.txt`) is line for line phase 2's: 233 compiled, 23 geometry
shaders validated, 13 failures, the same 13.

The three gs_5_0 shaders go through `panfrost_nir_lower_gs` like the 4.x ones: one static per
primitive, two dynamic (one with 32 indices per primitive and no SV_Position: it has four
streams for stream output). The self test with two streams and two instances is the first
shader with the static indexed shape.

What the corpus uses of Shader Model 5 (shaders/uses, from `--stats`): every UAV, raw,
structured and atomic opcode, both shared memory declarations, `sync`, `bufinfo`, the three
new gather4 forms, the bit field and first-bit instructions, `swapc`, the half conversions,
`emit_stream`, a GS instance count, hull shaders with fork phases (one with a control point
phase). Not in the corpus, covered by self tests only: `rcp`, `uaddc`, `usubb`, `msad`, the
`eval_*` instructions, the x derivatives, `cut_stream` and `emitthencut_stream`, a join
phase, `dcl_hs_max_tessfactor`, an instanced phase with an indexed output, atomics on shared
memory.

## P4.7 Self tests

Value checks (the shader's inputs are immediates, NIR constant folding stands in for running
it): `rcp`, `f32tof16` (two tests), `f16tof32`, `uaddc` and `usubb` (both results),
`countbits`, `firstbit_hi`, `firstbit_lo`, `firstbit_shi` (two tests), `ubfe`, `ibfe` (two
tests), `bfi`, `bfrev`, `swapc` (both results, and with destinations that are its sources),
`msad`.

Translate, validate and compile: a pixel shader with the three `eval_*`, the four
derivatives, SV_Coverage in, conservative depth out, early depth-stencil; a geometry shader
with two streams sharing a register, all three stream opcodes and an instance count; a
compute shader with the four thread system values, raw and structured shared memory with
loads, stores and atomics, a structured UAV with its counter, an atomic on a typed UAV, two
kinds of `sync`.

Translate and validate: a hull shader with a control point phase, an instanced fork phase,
a fork phase reading `vocp`, a join phase reading `vpc`, a maximum factor (the NIR was read:
outer levels 4, 4, 4, inner level min(4+4+4, 8) = 8, the barrier, invocation 0); a domain
shader.

Refusals: eleven shaders, each expecting the message of its row in P4.5 (64-bit float flag
and opcode, interface call and function body, a feedback opcode, Shader Model 5.1, UAV slot
8, `sync` of threads in a pixel shader, a compute shader without a thread group, a store to
a read-only buffer, a hull shader without control point phase or signature).

## P4.8 Where I was not sure

The Direct3D 11 functional specification was not available here; these are from memory and
from how other translators do it. In each case the self test checks what the translator
does, not that it is what Direct3D does.

1. `f32tof16` rounding: round to nearest even is what is implemented and tested.
2. `firstbit_hi` / `firstbit_shi` count from the most significant bit (the HLSL intrinsics
   count from the least; the compiler subtracts from 31). `firstbit_shi` of 0 and of -1 is
   all ones. Fairly sure.
3. `ubfe` / `ibfe` / `bfi`: width and offset are the low five bits; a width of 0 extracts 0;
   width + offset >= 32 is a plain shift right by the offset. Fairly sure.
4. `msad`: no saturation of the sum assumed.
5. `swapc`: first destination gets the *second* value where the condition is non-zero.
6. `imm_atomic_consume` returns the counter after the decrement, `imm_atomic_alloc` before
   the increment.
7. Isoline factors: detail (segments per line) is GL's outer[1], density outer[0].
8. Winding: `triangle_cw` is `info.tess.ccw = false`, as `spirv_to_nir` stores Vulkan's
   VertexOrderCw; this assumes the consumer treats the domain as Vulkan's default
   upper-left origin, which is Direct3D's. A consumer with GL's convention must flip it.
9. Power-of-two partitioning is given equal spacing: NIR has nothing closer. The factors are
   then not rounded up to a power of two.
10. The maximum tessellation factor is applied as an upper clamp only.
11. `eval_snapped`: four signed bits per lane, sixteenths of a pixel from the centre.
    `gather4_po`: six signed bits per lane.
12. SV_DepthGreaterEqual is GL's `depth_greater` layout, SV_DepthLessEqual `depth_less`.
13. fxc pads `sample_pos` of a texture with one word (two corpus shaders): for Shader Model 5
    the parser accepts an instruction shorter than its length, never a longer one.
14. Operand type 41 (the stencil reference output of Direct3D 11.3) is not in the SDK header
    the build uses; one corpus shader has it. It is translated to FRAG_RESULT_STENCIL.

## P4.9 Verified by running / only compiled / reasoned

Ran (Linux host, assertions on): parser and translator over all 362 corpus containers and
118 self tests; NIR validation of every result; the Bifrost arch 6 compile of every vertex,
pixel and compute shader and, through `panfrost_nir_lower_gs`, of every piece of every
geometry shader; the value checks; the old path over the same corpus.

Compiled only: `ShaderParse.c` and `ShaderNIR.c` in the ARM64 and ARM64EC Windows
configurations at dbf9ef4 (both builds exit 0 and link `libgallium_d3d10.dll`; no ARM64X DLL
was linked or delivered, because the Shader Model 4 path is the same code under
`major_version < 5` and nothing calls the new paths).

Not run, not checkable here: any shader on a GPU; what the resource instructions do at run
time (their NIR was read for the compute self test: slots, offsets and atomics are as P4.2
says); hull and domain shaders past validation (Panfrost has no tessellation, the poly
passes were not run on them); UAVs in vertex or geometry shaders on Panfrost (the caps say
no); geometry shader stream output and instancing in the driver.

## P4.10 Rerun

```sh
export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
cd ~/sbmesa && ninja -j6 -C build-nir-host src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
cd build-nir-host
W=/Users/dev/project-starbite/build/win/winetests/src
H=./src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
$H --selftest --stats $W/d3d10core.c $W/d3d11.c > harness-p4.txt     # everything
$H $W/d3d10core.c $W/d3d11.c > harness-p4-corpus.txt                 # corpus only
$H --tgsi $W/d3d10core.c $W/d3d11.c > harness-p4-tgsi.txt            # old path, 4.x only
$H --selftest --no-fork --no-compile --print-nir --only sm5_hull_phases   # one shader's NIR
~/sbmesa/build-nir-x/compile-check.sh    # Windows: ARM64 and ARM64EC, result in compile-check.exit
```

# Phase 5: the Direct3D 11 DDI

State: 2026-11-16 01:26 EET. Branch `sb-nir`, on top of phase 4 (dbf9ef4), nothing pushed:

- `1b3e560` 0166 cherry-picked (geometry shaders refused cleanly without a geometry stage)
- `114a42e` 0167 cherry-picked (written BOs named to the shim, MSAA behind `msaa`)
- `08ac11c` d3d10umd: the Direct3D 11.0 DDI behind the flag file d3d11ddi
- `789393a` d3d10umd: the query types of Direct3D 11
- `1c0a515` d3d10umd: Shader Model 5 never reaches the old translator; fl11 needs msaa

**None of this has run.** There is no Windows here: both Windows configurations compile and
link, the ARM64X verification passes, and everything about how the runtime behaves is read
from `d3d10umddi.h` (10.0.26100.0) or reasoned. Where my memory and the header disagreed the
header won; those places are marked.

## P5.1 Part 0: the merge

Both commits applied without conflict. 0166 merged into the phase 2/3 structure of
`Shader.cpp` by itself; read afterwards: the NIR path of the two geometry create functions is
only reached when the driver reports a NIR geometry stage (`gsemu`), every other path reaches
0166's test of `pipe->create_gs_state` before the hook is called, `GsSetShader`,
`DestroyShader` and `ResolveState` test their hooks. So with `gsemu` off a geometry shader is
refused with E_OUTOFMEMORY and a log line, never a call through a null pointer.

0167 and the geometry shader draw path: a draw with a geometry shader issues **no submit of
its own**. Its capture job, index kernel and rasterization jobs are added to the framebuffer
batch's vertex/tiler chain and go out with that batch, which is submitted by `jm_submit_jc`
(the only place in the driver with the submit ioctl; called once for the vertex/tiler chain
and once for the fragment job). Both calls are preceded by `malikm_submit_writes` with the
batch's written BOs. What the geometry path itself writes is memory of the batch's pool,
which is the device's own and, as 0167's comment says, needs no listing.

Harness after the merge: identical to phase 4, both paths.

## P5.2 Switches

| Flag file | Effect | Needs |
|---|---|---|
| `d3d11ddi` | the Direct3D 11.0 DDI is offered; pipeline level 10_0 | `nirshaders`; without it a log line says the DDI is NOT offered |
| `msaa` (0167) | with `d3d11ddi`: pipeline level 10_1 as well | |
| `fl11` | pipeline level 11_0 as well | `d3d11ddi` and `msaa` (11_0 contains 10_1; logged if `msaa` is missing) |

All are read once per process. With none of them the adapter lists what it listed, answers
the caps with zeros as before, and no Direct3D 11 function can be reached.

## P5.3 Adapter and caps (Adapter.cpp, D3D11.cpp)

- `GetSupportedVersions` appends `D3D11_0_DDI_SUPPORTED` and `D3D11_0_7_DDI_SUPPORTED`.
- `OpenAdapter10` / `CreateDevice` accept the two interface versions; the device size is the
  same `Device`.
- `GetCaps`: `D3D11DDICAPS_THREADING` = 0 (not free-threaded, no driver command lists: the
  runtime serializes and emulates deferred contexts); `D3D11DDICAPS_SHADER` = 0 (no doubles;
  compute below 11_0 is optional and not offered); `D3D11DDICAPS_3DPIPELINESUPPORT` from the
  switches. Every other caps type (the 11.1 and later ones, which belong to DDI versions that
  are not listed) is answered with zeros as before.
- `CreateDevice` reads the level from the flags (`D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS`),
  refuses a level that was not offered, and checks the screen (`SbLevelPossible`).
- The core layer callbacks of Direct3D 11 start with the Direct3D 10 ones (header), so the
  device keeps using its copy of those. The DXGI base table gets its eighth entry by the
  header's `IS_DXGI1_1_BASE_FUNCTIONS`.

Header against the task text: the deferred context and command list entry points are members
of the *device* function table, not adapter functions.

## P5.4 Pipeline levels: what 10_1 and 11_0 need

10_1 over 10_0, and where each stands:

| Requirement | State |
|---|---|
| Shader Model 4.1 (`gather4`, `lod`, `sample_pos`, `sample_info`, per-sample inputs) | in the NIR translator (phase 1/2); `d3d11ddi` requires `nirshaders` |
| 4x multisampling for every render target format, sample-rate shading, multisample depth reads | behind `msaa`, not yet checked on the phone: **the gate** |
| cube map arrays | driver cap, checked at CreateDevice |
| blend state per render target | `CreateBlendState1` exists; driver caps checked at CreateDevice |
| the 10.1 shader resource view (cube arrays) | `CreateShaderResourceView1` exists |

Not checked anywhere, so possibly missing: the exact list of formats 10_1 requires
multisampling for, and the BC4/BC5 and 16-bit formats it makes mandatory.

11_0 over 10_1: Shader Model 5 (translator, phase 4), compute, UAVs, indirect draws (this
phase), tessellation (**refused**), 8x multisampling and its format list (**not checked**),
stream output with four streams (**refused**: nothing does stream output), 16384 textures
and the other raised limits (**not checked against Panfrost's**). `fl11` is therefore a
switch for trying things, not a claim of conformance.

## P5.5 The device function table (Device.cpp)

`D3D11DDI_DEVICEFUNCS` has 150 members (static-asserted against the header; `D3D10PSGP` is
not defined). They are set **by member name**; the table is cleared first, then looked at as
150 pointers, and a device whose table has an empty member is not created (the count is
logged at every CreateDevice).

| Members | What serves them |
|---|---|
| 89 | the Direct3D 10 function itself (the member's type is a `PFND3D10DDI_` one) |
| 4 | the Direct3D 10.1 function (blend state, ResourceConvert, ResourceConvertRegion) |
| 10 | Direct3D 11's form of an existing function, below |
| 47 | new in Direct3D 11, below |

Changed form:

| Function | What it does |
|---|---|
| `SetRenderTargets` | Direct3D 10's, then the pixel stage's UAVs for the changed range |
| `RelocateDeviceFuncs` | nothing, as before |
| `CalcPrivateResourceSize`, `CreateResource` | the 11 structure extends the 10 one (static-asserted member by member); the stride and misc flags are kept; `D3D11_DDI_BIND_UNORDERED_ACCESS` becomes `PIPE_BIND_SHADER_IMAGE | SHADER_BUFFER`; a structured stride that is 0 or not a multiple of 4 is refused |
| `CreateShaderResourceView` (+Calc) | raw or structured buffer: an R32_UINT buffer view (the phase 4 binding model); anything else the 10.1 function |
| `CreateDepthStencilView` (+Calc) | Direct3D 10's; the read-only flags have nothing to do in gallium |
| `CreateGeometryShaderWithStreamOutput` (+Calc) | one stride, stream 0 only, rasterized stream 0: Direct3D 10's; otherwise **refused** by name |

New:

| Function | State |
|---|---|
| `CreateComputeShader`, `CsSetShader`, `CsSetConstantBuffers/ShaderResources/Samplers` | implemented: NIR translator, `create_compute_state`, the stage helpers Direct3D 10's stages use |
| `Dispatch` | implemented (`launch_grid`) |
| `DispatchIndirect`, `DrawInstancedIndirect`, `DrawIndexedInstancedIndirect` | implemented by reading the arguments on the CPU and calling the direct function |
| `CalcPrivateUnorderedAccessViewSize`, `CreateUnorderedAccessView`, `DestroyUnorderedAccessView` | implemented: buffers (raw, structured, typed, with append/counter) and 1D/2D/3D textures; a format without image support in the driver, a counter on a buffer that is not structured, a range past the buffer are **refused** by name |
| `CsSetUnorderedAccessViews`, UAVs of `SetRenderTargets` | implemented for slots 0 to 7; a view in a higher slot is logged and NOT bound |
| `ClearUnorderedAccessViewUint/Float` | implemented (`clear_buffer`, `clear_texture`) |
| `CopyStructureCount` | implemented (copy of the counter buffer) |
| `SetResourceMinLOD` | **stub**: logged, not applied (gallium views have no minimum level; the DDI has no error for it) |
| `CreateHullShader`, `CreateDomainShader` | **refused** (E_OUTOFMEMORY, logged) until tessellation exists; `CalcPrivateTessellationShaderSize` is real |
| `Hs*` and `Ds*` set functions (8) | state kept in the device, not handed to the driver |
| `*SetShaderWithIfaces` (6) | set the shader; class instances are logged and NOT applied (a shader with interfaces is refused at creation, so none can arrive) |
| deferred context and command list functions (13) | sizes 0, creations **fail** (E_OUTOFMEMORY, logged); the threading caps keep the runtime from calling them |

Queries (shared `CreateQuery` / `QueryGetData`): the Direct3D 11 pipeline statistics (eleven
counters) and the per-stream stream output statistics and overflow predicates map to the
pipe query with the stream as index. Whether Panfrost has those queries is what it was for
Direct3D 10: a type the driver has no query for returns zeros.

`CheckFormatSupport`: unchanged. The header has no format support bits for the 11.0 DDI
beyond Direct3D 10's (UAV writes, gather and the others are `D3D11_1DDI_` bits).

Map, unmap and the UpdateSubresource entries have Direct3D 10's types in the 11.0 table and
are the Direct3D 10 functions.

## P5.6 Binding model, and no remap

What phase 4 defined is what the frontend binds: cbN at constant buffer N + 1, tN at sampler
view N (raw and structured as R32_UINT buffer views), sN at sampler N, typed uN at image N,
raw and structured uN at shader buffer N, the counter of uN at shader buffer N + 8.

No remap was needed: the Direct3D 11.0 DDI has unordered access only in the compute stage
and at the output merger (pixel stage), which are exactly the two stages Panfrost gives
images and shader buffers. UAVs in the other stages are Direct3D 11.1.

## P5.7 Where it reads back or flushes

- Indirect draws and `DispatchIndirect`: the argument buffer is mapped for reading. If the GPU
  wrote it (a compute shader filling the arguments), that flushes the batch that writes it
  and waits.
- `Dispatch`: Panfrost's `launch_grid` flushes all batches before and after every dispatch
  (upstream behaviour).
- A UAV's initial count and counter creation write four bytes through `pipe_buffer_write`.

## P5.8 Left out or unfinished, with the reason

1. Tessellation: refused (next phase).
2. Stream output with streams, or without a rasterized stream: refused; plain Direct3D 10
   stream output is as (not) functional as it was.
3. `SetResourceMinLOD`: not applied.
4. Shader Model 5 through the old translator: now refused up front (`Shader_tgsi_translate`
   returns NULL for anything but Shader Model 4 vertex, pixel, geometry), so a Direct3D 11
   shader the NIR translator refuses fails its creation instead of reaching a translator that
   does not know its opcodes.
5. The signatures given to the translator carry no stream and no patch constants (the
   Direct3D 10 signature structure has neither): a multi-stream geometry shader's outputs in
   streams above 0 get no system value names.
6. Level 10_1 and 11_0 requirements that are not checked (P5.4).
7. No host-side unit harness: the function table, the caps and the size functions are built
   from DDK types, and a native build of them would need the Windows headers on Linux. In
   its place: the static assertions (table size, the create structures' layouts), the
   by-name fill that the compiler type-checks member by member, and the count of empty
   members at every CreateDevice.

## P5.9 All new switches absent: what differs from after Part 0

Read from the diff, not run:

- One more log line at the first `GetSupportedVersions` (`Direct3D 11 DDI not offered`).
- `Device`, `Resource` and `Shader` are larger (the runtime allocates them by the Calc
  functions, which return the new sizes).
- `DestroyShader` looks at two more fields; `CreateQuery` computes an index that is 0;
  the resource bind flag translation knows one more bit, which only the Direct3D 11 runtime
  sets; `Shader_tgsi_translate` checks the version first.
- Nothing else: the Direct3D 10 table is filled by the same code into the same place.

## P5.10 The Windows build

`~/sbmesa/build-nir-x/build-all.sh`: ARM64 and ARM64EC exit 0, link log empty, 35 `ok`
lines, `FAILS: 0`, built from 1c0a515:

```
/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir-d3d11/
  sbumd.dll  27480064 bytes  sha256 1572d243c328158a3461623e50683281fbcb342d02a455235d8332bea8e2f81d
  sbumd.map   6840919 bytes  sha256 060387187ccaf873cfa2a59ed866a94475837d11b2ff7688f817515cad4b8775
```

The other three directories are untouched (`umd-nir` 98e9e6a6..., `umd-nir-gs` e9e9d067...).

`strings` finds `d3d11ddi`, `fl11`, `msaa`, `nirshaders`, `gsemu` and the log lines of P5.2.

## P5.11 What the Direct3D 11 conformance run is likely to hit first

Reasoned, in the order I would expect them:

1. Without `fl11` every test that asks for feature level 11_0 gets a 10_x device or skips;
   with `gsemu` off every geometry shader creation fails.
2. Hull and domain shader creation (refused), then everything that draws with them.
3. Stream output: declarations with streams refused, the rest not functional.
4. Typed UAV formats: a format the driver has no image support for is refused at view
   creation, where the test expects success.
5. `Dispatch` cost and ordering: every dispatch flushes.
6. Query results that are zeros (pipeline statistics, stream output statistics).
7. `SetResourceMinLOD`, class instances: accepted and not applied.
8. Multisampling at 8x and the format lists of 10_1 and 11_0.
9. Anything of mine that is plainly wrong because it never ran: the UAV range handling in
   `SetRenderTargets`, the counter semantics, the compute stage's resource binding.

## P5.12 What tessellation must add on the frontend side

- `CreateHullShader` / `CreateDomainShader`: translate with the tessellation signatures
  (`D3D11DDIARG_TESSELLATION_IO_SIGNATURES` has the patch constant list; the translator's
  signature structure already takes it), keep the NIR, and copy the hull shader's
  `info.tess` into the domain shader's when the two are bound (P4.4).
- Hand the kept hull and domain stage state (`Device::sb_tess`) to the driver: constant
  buffers, views and samplers of two more stages.
- The patch list topologies of `IaSetTopology` (1 to 32 control points), and the draw path
  that runs the tessellator.
- Pipeline statistics for the two stages.

## P5.13 What 11.1 and WDDM 1.3 would take

- 11.1 (`D3D11_1_DDI_SUPPORTED`): `D3D11_1DDI_DEVICEFUNCS` has 158 members: the 150, of
  which several change type (constant buffers with offsets, UpdateSubresource and CopyRegion
  with flags, blend state with logic ops, rasterizer state with a forced sample count), plus
  `AssignDebugBinary`, `CheckDirectFlipSupport`, `ClearView`, `Discard`,
  `DynamicConstantBufferMapNoOverwrite`. Caps `D3D11_1DDICAPS_D3D11_OPTIONS`,
  `ARCHITECTURE_INFO` and `SHADER_MIN_PRECISION_SUPPORT` must be answered. The DXGI table
  becomes `DXGI1_2_DDI_BASE_FUNCTIONS` (`IS_DXGI1_2_BASE_FUNCTIONS`): `Blt1`,
  `OfferResources`, `ReclaimResources`, and the multiplane overlay functions
  (`GetMultiplaneOverlayCaps`, `GetMultiplaneOverlayFilterRange`,
  `CheckMultiplaneOverlaySupport`, `PresentMultiplaneOverlay`). UAVs in every stage arrive
  here, which is where Panfrost's vertex and geometry stages would need a remap or would
  have to refuse.
- WDDM 1.3: `D3DWDDM1_3DDI_DEVICEFUNCS` (tiled resources, markers), caps
  `D3DWDDM1_3DDICAPS_D3D11_OPTIONS1` and `MARKER`, and `DXGI1_3_DDI_BASE_FUNCTIONS`
  (`Present1`, `CheckPresentDurationSupport`, a changed multiplane overlay set).

# Phase 6: conformance fixes

State: 2026-11-16 01:37 EET. Branch `sb-nir`, seven commits on top of phase 5 (1c0a515), one
topic each, nothing pushed. Input: the d3d10core run on the user's driver (old translator,
Direct3D 10.0 DDI, no geometry stage, no MSAA): 74 pass, 17 fail, 20 crash.

**Not finished.** Fixed: the causes behind 7 of the 17 wrong results and 4 of the 20 crashes
(one crash fixed, three aborts turned into refused shaders; as far as reading can tell,
nothing ran). Classified without a fix: the missing-feature
tests. Not found or not looked at: listed at the end. No fix special-cases a value.

## P6.1 Fixed

| Test | Cause | Fix | Confidence | Translator |
|---|---|---|---|---|
| test_multiple_render_targets, test_color_mask, test_independent_blend, test_instanced_draw (second target) | `pipe_blend_state::max_rt` was never set (0). Panfrost takes the number of blended targets from it (`pan_cmdstream.c`: `rt_count = blend->max_rt + 1`), so only target 0 was written | `079eabc` (OutputMerger.cpp, both blend create functions) | high: read on both sides; the symptom (targets 1 to 7 keep the clear colour) is exactly this | both |
| test_instanced_draw (per-instance data of the second draw) | Panfrost divided the base instance by the step rate (`base * stride / divisor`); the element is base + instance / divisor | `6969a97` (pan_cmdstream.c) | high for the cause: the old formula gives the elements the log shows for step rate 2 (2, 2, 3, 3 where 4, 4, 5, 5 are expected) | both |
| test_face_culling (the SV_IsFrontFace half; the culling half passed) | the old translator maps the face with a compare meant for the float +1/-1 face; Panfrost's is an integer system value (all ones / 0), on which the compare swaps front and back | `6d50e08` (ShaderTGSI.c, `Shader_tgsi_driver` set by CreateDevice) | high: the harness shows the NIR of the test's shader before (inverted) and after | old only (NIR was right) |
| test_copy_subresource_region | a source box of zero depth was handed to the driver, which copied the whole region; Direct3D copies nothing | `458207f` (Resource.cpp) | high: the last copy of the test has that box and the readback is the whole source | both |
| test_update_subresource (crash) | a box that ends before it starts (4,4,0 - 3,1,1 in the test) became a negative width and height in the map | `aec5da4` (Resource.cpp) | high | both |
| test_cube_maps, test_sample_c_lz, test_unbound_multisample_texture (abort) | their shaders have a `switch`; tgsi_to_nir aborts on TGSI SWITCH | `139d0c4`: on a NIR-only driver the old translator refuses the shader. The tests then fail cleanly on the old translator and need `nirshaders` to pass (their shaders translate and compile there) | high for the cause (harness: the same three shaders, "unknown TGSI opcode: SWITCH") | old: refused; NIR: works |
| test_depth_bias | the rasterizer state set the bias values but none of `offset_tri/line/point`; Panfrost's `depth_bias_enable` is `offset_tri`, so no bias was ever applied | `91faa99` (Rasterizer.cpp) | high for the cause; whether every format then matches is not checked | both |

## P6.2 Classified, not fixed

| Test | What it is |
|---|---|
| test_create_shader, test_clear_state, test_index_buffer_offset, test_multiple_viewports | the access violation at address 0 is the call through the missing `create_gs_state` hook: the last test seen is the line before a geometry shader creation in each (read). Refused cleanly since 0166; real with `gsemu` + `nirshaders` (never run) |
| test_layered_rendering, test_geometry_shader, test_stream_output, test_stream_output_resume, test_stream_output_vs, test_clip_distance | same family by their subject and, for test_clip_distance, by the address 0; not each read. Stream output and layered rendering stay missing features even with `gsemu` |
| test_combined_clip_and_cull_distances (illegal instruction) | the old translator's cull distance output (harness: its vertex shader ends tgsi_to_nir with an out-of-range opcode). The NIR translator has it; the old path is not fixed |
| test_alpha_to_coverage | needs multisampling (`msaa`) |
| test_pipeline_statistics_query | Panfrost has no pipeline statistics query: zeros. A missing feature, left failing |
| test_immediate_constant_buffer | the old translator's immediate constant buffer base, fixed in phase 2 on this branch (not in the tree that ran) |
| test_vs_input_relative_addressing | old translator; expected to need `nirshaders`, not verified |

## P6.3 Probable, unverified

- test_texture, test_texture1d (same crash address), test_resinfo_instruction: a size query
  on a slot with nothing bound. `panfrost_upload_txs_sysval` dereferenced the missing view;
  phase 2's c640c16 made it answer zeros. Low to medium confidence: I did not match the
  crashing iteration to a test case.

## P6.4 Not found, or not looked at

- test_shader_input_registers_limits (crash after its last check passed, during the
  releases): checked the array bounds for t127 and s15 in the old translator, tgsi_to_nir
  and Panfrost's empty-slot handling, all fine; cause not found.
- test_shader_stage_input_output_matching (crash in a draw with two render targets): not
  found.
- test_fragment_coords, test_depth_stencil_sampling, test_clear_render_target_view_2d,
  test_format_compatibility, test_compressed_format_compatibility,
  test_texture_compressed_3d: not looked at.
- The mistakes I listed in phases 1 and 2 for paths the suite does not crash on: not
  revisited in this phase.

## P6.5 Verification

- Harness: the NIR path is identical to phase 5 (`harness-p6.txt`). The old path
  (`harness-p6-tgsi.txt`) differs in the seven `switch` shaders, which are now refused
  instead of ending tgsi_to_nir, and in the NIR of the front face shader.
- The blend, copy, update and depth bias fixes are in Windows-only files: compiled, not run.
  No host test was written for them.
- Windows build from 91faa99: ARM64 and ARM64EC exit 0, 35 `ok`, `FAILS: 0`.

```
/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir-fix1/
  sbumd.dll  27475968 bytes  sha256 90fe47b0fe01de2092b4ab0f8a7fab0087acd3a75c94df98143ca68f8322edbb
  sbumd.map   6844636 bytes  sha256 c8f8afd383572f7de928bc0209a84014090cf6a0572a197e82f19bd467e4d211
```

## P6.6 Continued (second round): what was done and what was not

This round stopped early: I ran out of room to investigate further in this session. One fix,
one narrowing, the rest of the list is open.

| Item | Result |
|---|---|
| DrawAuto with a NULL draw list (my phase 3/5 note) | **fixed**, `b908e17` (pan_cmdstream.c `panfrost_draw_vbo`): a draw whose count comes from a stream output target is turned into a direct draw of the vertices Panfrost has counted for that target on the CPU (`pan_so_target()->offset`). Confidence: high that the null read is gone; that the count is right for Direct3D is reasoned (it is the count GL transform feedback keeps). Compiled natively for arch 6, 9 and 10. Both translators |
| test_fragment_coords | **narrowed, not fixed.** Both translators give the same NIR for the test's shader (`flt(cb0.xy, load_frag_coord.xy)`, printed with the harness, d3d10core.c:9504), so the cause is below the translators: Panfrost's fragment coordinate for this draw or the constant buffer. The log's colour (black everywhere) says both compares are false at every pixel. Not found |
| test_clear_render_target_view_2d, test_depth_stencil_sampling, test_format_compatibility, test_compressed_format_compatibility, test_texture_compressed_3d | not looked at |
| test_shader_stage_input_output_matching, test_shader_input_registers_limits | not looked at again |
| test_texture, test_texture1d, test_resinfo_instruction | still "probable" (P6.3); the iteration was not matched |
| test_vs_input_relative_addressing, the rest of test_face_culling | not looked at. For test_face_culling: the four failing lines (13643, 13648, 13664, 13670) are all in the SV_IsFrontFace part of the test, which 6d50e08 addresses; the culling loop before it passed |
| the other open mistakes from phases 1 and 2 (cube array `resinfo` reciprocal, `ld_ms` on the old path) | not done |
| stream output (design and implementation) | **not started**, not even the design |

Windows build from b908e17: in the phase 6 report.

---

# Phase 6, round 3

State: 2026-11-16 02:16 EET. Branch `sb-nir`, nine commits on top of b908e17, one topic each,
nothing pushed, tree clean at `1e2a6c6`. Input: the T74 run (old tree) for everything up to
R3.7. The T76b logs arrived at 02:12, after that work was done: they are in R3.8 (the run did
not test our driver at all). No T77 log existed at 02:16.

## R3.0 The crash addresses of T74 can be resolved

The T74 log only gives absolute addresses. The DLL's load base follows from one crash whose
function is known: test_combined_clip_and_cull_distances dies in `tgsi_to_nir` (P6.2). Trying
every 64 KiB-aligned base that puts all addresses inside `.text` of
`stage/sbwddm-t74/maps/sbumd.map`, exactly one puts that address in a tgsi_to_nir function
(`ttn_compile_init+0xe48`): **base 0x7ff96e5f0000** for that boot. With it:

| Test | Address | Function (T74 map) |
|---|---|---|
| test_texture, test_texture1d | 7FF96E82A160 | `panfrost_emit_const_buf+0x348` (pan_cmdstream.c) |
| test_update_subresource | 7FF96E9B0438 | `memcpy` (the negative box, fixed in aec5da4: consistent) |
| test_shader_stage_input_output_matching | 7FF96E826F5C | `panfrost_draw_vbo+0x72c` |
| test_shader_input_registers_limits | 7FF96E719C68 | `panfrost_blitter_save+0x30c` (pan_blit.c) |
| test_resinfo_instruction | 7FF96E92D8FC | `emit_cf_list+0x8f4` (bifrost_compile.c) |

The instructions at those addresses were read with `llvm-objdump-20 -d` on
`stage/sbwddm-t74/sbumd.dll` (RVA = address - base). The little script is not in the tree
(scan all bases, print the function of each address); it is twenty lines of Python over the
map's `0001:` lines.

## R3.1 Fixed

| Test | Cause | Fix | Confidence | Translator |
|---|---|---|---|---|
| test_shader_input_registers_limits (crash) | `u_blitter.h`: `saved_sampler_views[PIPE_MAX_SAMPLERS]` has 32 entries; Panfrost saves all its bound fragment views before a blit and has 128 slots (`PIPE_MAX_SHADER_SAMPLER_VIEWS`). The test binds a view at t127, draws, and the readback copy is a blit: `util_blitter_save_fragment_sampler_views` unreferenced whatever lay behind the array. The faulting instruction is the load of the old entry's reference count in that loop. (Not "in the releases": the last line seen is the draw, the crash is in the `check_texture_color` after it.) | `c4018b1` (the array has 128 entries) | high: address, disassembly and source agree | both |
| test_shader_stage_input_output_matching (crash) | The test never calls IASetInputLayout. 0155's empty vertex-elements state is only bound when `velems_changed` is set, which only IaSetInputLayout / IaSetVertexBuffers did; it started FALSE. Panfrost's `panfrost_emit_vertex_data` then read `ctx->vertex->nr_bufs` through NULL (offset 0x348, the same site 0155's comment names) | `3a2befe` (Device.cpp: a new device starts with `velems_changed = TRUE`) | high | both |
| test_fragment_coords; test_texture_compressed_3d | Panfrost from Bifrost on reports `fs_coord_pixel_center_integer` only, and its compiler makes `load_frag_coord.xy` from the integer pixel coordinate (`nir_lower_frag_coord_to_pixel_coord`, whose comment says the frontend has to have applied the centre; GL's state tracker does). Neither translator did: SV_Position.xy was 0.5 too small in every pixel shader. That is exactly the four failing lines (320.0 > 320 false, 18 > 16 true, `frac` = 0), and the one in test_texture_compressed_3d (its shader samples at `position / size`: y = 9 gives texel row 4.8 instead of 5.07, the neighbouring row's value 0x40) | `5220fb5`: `Shader_tgsi_driver.pixel_center_integer` from the caps (integer without half-integer); both translators add (0.5, 0.5), the NIR one the sample position instead when SV_Position is declared with a sample interpolation mode | high. Harness: NIR of both test shaders on both paths | both |
| test_clear_render_target_view_2d | `util_blitter_clear_render_target` draws once, into the first layer, when the driver has no layered rendering from the vertex shader (`has_layered` false on Panfrost): a clear through a view of layers 0-4 cleared layer 0, one of layers 1-3 layer 1. Layers 3 and 4 stayed 0 | `7634345`: one draw per layer through a surface of that layer; the same in `util_blitter_clear_depth_stencil` | high for the cause; the fix is compiled only | both |
| test_depth_stencil_sampling | `FormatTranslate` gives `X24_TYPELESS_G8_UINT` and `X32_TYPELESS_G8X24_UINT` the combined depth-stencil format, and that was the sampler view's format: it samples the depth. The values in the log are the bits of the depth clear value read as an integer (0.2f = 0x3e4ccccd = 1.045e9; 0.3f; 1.0f) | `fdc4c64` (Shader.cpp `SbStencilView`): `PIPE_FORMAT_X24S8_UINT` / `X32_S8X24_UINT`, stencil swizzled to green. Panfrost has both (pan_format.c; the Z32F_S8 stencil is the separate stencil resource) and replicates x for depth/stencil views | high for the cause, medium-high for the fix (Windows only code, not run) | both |
| test_format_compatibility (test 24), test_compressed_format_compatibility (0x46 -> 0x1) | Not the copies under test: the tests' **readback** of an `R32G32_TYPELESS` / `R32G32B32A32_TYPELESS` texture. Those resources are `R32G32_UNORM` / `R32G32B32A32_UNORM` here, and a same-format 2D copy is a blit (`sb_resource_copy_region`, 0105): a 32-bit normalized texel through a 32-bit float. The values read are bit-exact copies of earlier contents, at unshifted positions, and in the second test only one of fifteen identical cases fails: the staging texture was not written at all and showed recycled memory. Why the draw into a 32-bit UNORM target leaves nothing is **not established** (GL has no such render target, so that row of pan_format.c may never have run); a float round trip could not be exact in any case | `385d0de` (pan_resource.c): the blit uses the UINT format of the same layout for both views for R32/R32G32/R32G32B32A32 UNORM and SNORM. UINT blits of these sizes pass in the same test (tests 22, 23) | medium: the reading of the evidence is firm, the fix avoids the unexplained path rather than explaining it | both |
| test_vs_input_relative_addressing | old path only: ShaderTGSI.c emits `IN[TEMP[0].x+1]` correctly, `tgsi_to_nir` dropped the index ("Indirection on input arrays isn't supported by TTN") and read `IN[1]` | `c973330` (tgsi_to_nir.c): a compare-and-select over the declared input registers, as the NIR translator does | high. Harness: before/after NIR of the test's shader on `--tgsi`, compiled; the NIR path was already right | old (NIR was right) |
| (no test) `resinfo_rcpFloat` of a cube array, old path | `texture_dim_from_tgsi_target` had no cube array case | `a499371` | high, trivial | old |

## R3.2 Now certain, no new change needed

| Test | What it is |
|---|---|
| test_texture, test_texture1d | The faulting code is `ldr x11, [ctx->sampler_views[stage][id & 0x7f]]; ldr w9, [x11, #0xc]`: the `txs` sysval upload on an empty slot, x11 = NULL. The last check seen (7672 / 6753) is the end of an iteration; the next table entry is the first one with a NULL texture and `ps_ld`, whose shader calls GetDimensions. Fixed by c640c16 (phase 2: zeros for an empty slot) |
| test_resinfo_instruction | A crash in the Bifrost compiler (`emit_cf_list`), inside the first CreatePixelShader of the loop (the last check seen is the one before the loop): `query_levels` on Bifrost v6, the backend failure the harness showed for 9 corpus shaders on the old driver. Fixed by c640c16 (`PAN_SYSVAL_TEXTURE_LEVELS`) |
| test_format_compatibility, test_compressed_format_compatibility: the copy rules themselves | The runtime filters the copies Direct3D 10.0 forbids before the DDI (all "must not copy" cases pass). Between formats of one family `areResourcesCompatible` is false for differing channel types and the copy goes through `util_resource_copy_region` on the CPU, which is exact. All five BC families are reported by this GPU (`CheckFormatSupport ... caps 0x1` for formats 70 to 84 in the T74 umdlog), so nothing the feature level requires is missing |

## R3.3 Looked at, not done

- **`ld_ms` sample index on the old path.** `tgsi_to_nir` (0114) has `SAMPLE_I_MS` with the index
  in src2.x, and emitting it from ShaderTGSI.c is ten lines; I wrote them and took them out
  again: the only corpus shaders with `ld_ms` on Shader Model 4 also have a `switch` and are
  refused on the old path, so nothing here would have executed the new code, and it is the
  default path of the installed driver. With `msaa` off every multisampled texture has one
  sample, so sample 0 is right there. Needs `nirshaders` with `msaa`.
- **Why a draw into a 32-bit UNORM target writes nothing** (R3.1). Candidates, none checked:
  the blend descriptor's fixed-function conversion for `RG32_UNORM` / `RGBA32_UNORM` memory
  formats on the G72; `pan_pack` in pan_nir_lower_framebuffer.c scales by 2^32-1 in a float
  and converts with `f2u32`, which overflows for 1.0. Either way no Direct3D view format is a
  32-bit UNORM, so after 385d0de nothing renders to one.
- Float formats copied by blit (R32_FLOAT for `R32_TYPELESS`, R16G16B16A16_FLOAT...) go through
  float registers; the tests' patterns (including 0xffffffff) survive on this GPU. Signalling
  NaN patterns are not tested by the suite and I did not reason about them.
- `Tex3D.WSize = -1` and similar "all remaining" values in view creation (OutputMerger.cpp
  `last_layer = WSize - 1 + FirstW`): the tests pass, so the runtime resolves them before the
  DDI; not checked against the DDI documentation.

## R3.4 What changes with all flag files absent

Bug fixes only, but three of them are visible beyond the tests:

1. SV_Position.xy in every pixel shader is now x + 0.5, y + 0.5 (was x, y). Shaders that use
   it for `Load` coordinates are unaffected (same texel); shaders that compare it, take its
   fraction or scale it into texture coordinates change by half a pixel, to what Direct3D
   specifies. The old translator's shaders have one more MOV and ADD and the property
   `FS_COORD_PIXEL_CENTER INTEGER`.
2. Clears of array and 3D views clear every layer (one draw and one framebuffer state per
   layer).
3. Copies between two `R32G32B32A32_TYPELESS` / `R32G32_TYPELESS` textures are bit-exact.

`struct blitter_context` grew by 96 pointers. Nothing else differs in size or call order.

## R3.5 Verification

- Harness at a499371 (`harness-r3.txt`, `harness-r3-tgsi.txt`, both `.err` empty): the
  summary tables and every per-shader result line are identical to `harness-p6.txt` and
  `harness-p6-tgsi.txt` (NIR path: 468 in scope, 457 compiled, 11 validated only, 12 refused
  as intended, 0 failures, self tests as before; old path: 233 compiled, 13 refused). The TGSI
  text against phase 2's differs in the 62 pixel shaders that read SV_Position (property, MOV,
  ADD, renumbered immediates) and in the `switch` shaders refused since 139d0c4.
- Ran on the host: the two translators on the shaders of test_fragment_coords and
  test_vs_input_relative_addressing (`--print-nir`, both paths, Bifrost compile); host
  compiles of u_blitter.c, pan_resource.c, pan_cmdstream.c (all architectures), tgsi_to_nir.c.
- Reasoned and compiled only (Windows code or draw-time code, nothing here can draw): the
  blitter array, the initial input layout, the layered clears, the stencil views, the integer
  copy views. Resolved from the T74 binaries: the five crash sites.
- Windows build from 1e2a6c6 (a499371 plus the one-line Format.cpp change of R3.8): ARM64
  and ARM64EC exit 0, link without warnings, 35 `ok`, `FAILS: 0`.

```
/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir-fix3/
  sbumd.dll  27484160 bytes  sha256 35bae4d5c4b9d6aa45d612adf7e8a0d423029c3b6ff03562b498a02a59043001
  sbumd.map   6845052 bytes  sha256 3133885ef6636d85ec324b54e5aee7c584d6c7c3d8cdd40ea66a692b63c7566d
```

## R3.6 What the old-tree list looks like now

Of T74's 17 failures and 20 crashes, by expected state with this DLL (none of it run):

- fixed for both translators: test_multiple_render_targets, test_color_mask,
  test_independent_blend, test_instanced_draw, test_copy_subresource_region,
  test_update_subresource, test_depth_bias, test_fragment_coords,
  test_clear_render_target_view_2d, test_depth_stencil_sampling, test_format_compatibility,
  test_compressed_format_compatibility, test_texture_compressed_3d, test_texture,
  test_texture1d, test_resinfo_instruction, test_shader_stage_input_output_matching,
  test_shader_input_registers_limits, test_face_culling, test_vs_input_relative_addressing,
  test_immediate_constant_buffer (base; a dynamic index still needs `nirshaders`, P2.5);
- need `nirshaders`: test_cube_maps, test_sample_c_lz, test_unbound_multisample_texture
  (refused cleanly on the old path), test_combined_clip_and_cull_distances (**still an illegal
  instruction on the old path**: tgsi_to_nir's unknown property, not touched);
- need `gsemu` + `nirshaders`: test_create_shader, test_clear_state, test_index_buffer_offset,
  test_multiple_viewports, test_geometry_shader, test_clip_distance;
- missing features: stream output (test_stream_output, _resume, _vs: design below),
  test_layered_rendering, test_alpha_to_coverage (`msaa`), test_pipeline_statistics_query.

## R3.7 Stream output: design (nothing implemented)

### What Direct3D 10 asks for

- `CreateGeometryShaderWithStreamOutput`: up to 64 declaration entries (output slot 0-3,
  register, component mask, or a gap), one stride. Either a real geometry shader, or **no
  shader code**: the application passed vertex shader bytecode (or only its signature), and
  the entries name vertex shader output registers. d3d10umd keeps that as a `Shader` with
  `stream_output` and no code (`bound_empty_gs`); `ResolveState` (Draw.cpp) remaps the
  register numbers through the bound vertex shader's `output_mapping` and re-creates the
  geometry CSO, which is the draw module's convention ("a geometry shader state without
  tokens carries the stream output of the stage before it").
- `SoSetTargets`: up to 4 buffers, each with a **byte** offset, or -1 to continue where the
  last stream output into that buffer stopped.
- Output is always a list of whole primitives (strips are unrolled, adjacency dropped);
  writing stops when the next whole primitive does not fit in any one bound buffer;
  rasterization of stream 0 goes on unless there is no pixel shader and no depth/stencil.
- `DrawAuto`: draw the buffer bound at vertex slot 0, vertex count = bytes filled by the last
  stream output into it / the vertex stride, no CPU-visible count.
- Queries: SO_STATISTICS (primitives written, primitives that would have been written with
  room), SO_OVERFLOW_PREDICATE.

### What is there

- Panfrost's GL transform feedback (`so->xfb`, `panfrost_launch_xfb`, `pan_nir_lower_xfb`,
  `PAN_SYSVAL_XFB`): a second variant of the **vertex shader** that only writes memory, run as
  a compute-type job over the draw's vertices before the draw, a barrier in the job chain.
  Record index = instance x vertices + raw vertex ID: right for **non-indexed point, line and
  triangle lists**, which is all GLES allows. The fill level is a CPU-side vertex count per
  target (`panfrost_streamout_target::offset`), advanced by the draw's vertex count; no clamp
  to the buffer size (GL makes overflow an error before the draw). It is built only when the
  NIR has `xfb_info`; `pipe_shader_state::stream_output` alone does nothing.
  `set_stream_output_targets` takes its `offsets` as that vertex count, not bytes.
  PRIMITIVES_EMITTED / GENERATED are CPU counters; SO_STATISTICS and the overflow predicate
  fall into `default`.
- Phase 3's geometry path: capture job, optional index kernel, rasterization draw. poly's
  rasterization shader contains the stream output writes (poly_nir_lower_gs.c:770-840:
  `poly_xfb_vertex_copy_in_strip`, `poly_previous_xfb_primitives`, bounded by
  `xfb_verts[stream]`), and poly makes a count kernel, expects a prefix sum over the counts
  (`info->prefix_sum`) and a pre-GS kernel that sizes the output and advances the offsets in
  GPU memory. The driver drops `xfb_info` before lowering (pan_nir_lower_gs.c:247) and frees
  the count and pre-GS kernels.
- b908e17: DrawAuto as a direct draw of the CPU-side count.

### Tier 1: vertex shader only, non-indexed lists (test_stream_output_vs is a point list)

1. **Frontend, shader.** The stream output belongs to the vertex shader as far as the driver
   is concerned. Keep what is needed to build a second vertex CSO: on the NIR path a
   `nir_shader_clone` taken lazily from a retained clone (as `sb_dual_nir` does for dual
   source), on the old path the tokens (`tgsi_dup_tokens`). In `ResolveState`, when
   `bound_empty_gs` and a vertex shader with code are bound and the driver has no stage that
   takes a code-less geometry state, look up (vertex shader, stream output shader) in a small
   cache on the vertex `Shader` (the pair changes rarely; key on the SO shader pointer and
   invalidate in `DestroyShader` of either) and bind the variant; bind the plain vertex
   shader again when the pair goes away. `output_resolved` and the delete/create of the
   geometry CSO stay for drivers with a geometry stage (softpipe/llvmpipe).
2. **Frontend, xfb info.** The variant's `pipe_shader_state::stream_output` is the SO
   shader's, with `register_index` remapped (already done) - Panfrost needs it for the
   strides. For NIR it also needs `nir->xfb_info`: a helper `SbNirSetStreamOutput(nir, info)`
   that builds `nir_xfb_info` (buffer strides in bytes, per output: buffer, byte offset,
   `location` of the output variable whose `driver_location` is `register_index`,
   `component_offset`, `component_mask`), sets `info.has_transform_feedback_varyings` and
   `info.xfb_stride[]`. For the old path the same has to happen after `tgsi_to_nir`, inside
   Panfrost's `panfrost_create_shader_state` (it has `cso->stream_output` and the NIR): put
   the helper in Panfrost (`pan_shader.c`, "xfb info from pipe_stream_output_info when the
   NIR has none") so both paths share it; `output_mapping` gives `driver_location` =
   TGSI output index on both.
3. **Offsets in bytes.** Change `panfrost_streamout_target::offset` to bytes (GL passes 0 or
   -1, so GL is unaffected; `panfrost_xfb_offset` then adds it unscaled and
   `panfrost_update_streamout_offsets` adds count x stride of the bound variant). d3d10umd
   already passes Direct3D's byte offsets through.
4. **Whole primitives, buffer end.** In `panfrost_launch_xfb`: vertices that fit =
   min over bound targets of (buffer_size - offset) / stride, rounded down to whole
   primitives of the draw's mode; run the job over that many, advance the offsets by that
   many, and add written / needed to two CPU counters. All on the CPU, no readback.
5. **Queries.** `PIPE_QUERY_SO_STATISTICS` = (written, needed) deltas of those counters;
   `SO_OVERFLOW_PREDICATE` = needed > written. Query.cpp already maps both.
6. **DrawAuto.** b908e17's count becomes offset / the vertex buffer stride of slot 0 (bytes
   now). d3d10umd sets `draw_so_target` from the buffer bound at slot 0.
7. Refuse the rest of the vertex-only case cleanly until tier 2: indexed draws, strips,
   adjacency and instanced draws with an SO-only shader log once and draw without capture
   (as today), rather than writing vertices in the wrong order.

Cost: no extra flush, one compute-type job per captured draw (it exists in GL). Risk: the
xfb variant's attribute fetch uses `offset_start`; with `StartVertexLocation` != 0 check
`cfg.compute.attribute_offset` / `load_raw_vertex_offset_pan` on arch 6 (GL exercises it).

### Tier 2: everything through the geometry path

A vertex-only stream output of a strip, an indexed draw or an instanced draw is the same
problem as a geometry shader's: the k-th output vertex is not the k-th input vertex. Do not
grow a second mechanism: synthesize a pass-through geometry shader (one per input primitive
type: point, line, triangle, and the two adjacency types; it copies the captured vertex
outputs to its outputs and emits one primitive) in NIR in Panfrost when an SO-only shader is
bound with such a draw, and run the geometry path with stream output.

For the geometry path itself:

1. Keep `xfb_info` on the geometry shader (built as in tier 1, from the SO declaration on
   the geometry `Shader`); let poly put the writes into the rasterization shader.
2. **A side-effect run of the rasterization shader.** A vertex job with IDVS may not write
   memory, so compile an `is_xfb`-style variant of the rasterization shader (no IDVS, no
   varyings) and run it as a compute-type job over the same vertex range as the draw, after
   the capture job and the index kernel and before the draw's vertex job, with an explicit
   job dependency (`sb_gs.job_dep`). The draw's own variant has the xfb stores removed.
3. **Static output count** (18 of 23 corpus shaders): vertices per input primitive are known
   at compile time, so the count, the clamp to the buffer end and the new offsets are CPU
   arithmetic as in tier 1 (`xfb_verts[stream]` and the base addresses go into
   `struct poly_geometry_params`). No count kernel, no prefix sum, no readback; DrawAuto and
   the queries work from CPU counters.
4. **Dynamic output count** (5 of 23): needs poly's count kernel per input primitive, a
   prefix sum over the counts (`poly_previous_xfb_primitives` reads it), and the total. The
   prefix sum kernel poly ships is written for asahi's compute model; on Bifrost it is one
   more compute job (serial in one invocation is enough for a first version: one invocation
   that walks the count buffer). The total is only on the GPU, and three things want it on
   the CPU: the next append into the same buffer (its base address), DrawAuto (the vertex
   count of a direct draw; no indirect draws before Valhall), and the statistics query. So:
   after such a draw mark the target "fill level on the GPU"; the next use of that level
   flushes the batch, waits and reads one word. That is the single readback of the design,
   and only for geometry shaders whose emit count depends on data. Keeping offsets in GPU
   memory (asahi's pre-GS kernel) would save the readback for appends but not for DrawAuto
   on this hardware, so it is not worth a second representation.
5. Statistics with a geometry shader: PrimitivesStorageNeeded is the count total (static:
   CPU; dynamic: the same readback).
6. Up to 4 buffers: Direct3D 10 allows several buffers only with one element each; poly's
   per-buffer strides cover it. `RasterizedStream` other than 0 and multiple streams
   (Direct3D 11) stay refused as in D3D11.cpp.

### Order of work, and what each step unlocks

1. Tier 1 steps 1-6: test_stream_output_vs, the SO statistics of vertex-only draws, DrawAuto
   after them. About 250 lines (frontend variant cache 80, xfb helper 60, Panfrost byte
   offsets, clamp, counters and queries 110).
2. Geometry path, static count (tier 2 steps 1-3, 5): test_stream_output_resume
   (its shader emits a fixed two triangles), most real uses (particle systems expanding
   points). The side-effect variant is the new mechanism; everything else exists.
3. Pass-through shader for vertex-only strips/indexed/instanced.
4. Dynamic count with the readback.

test_stream_output itself is mostly creation checks done by the runtime; it crashed in T74 at
the first `create_gs_state` and needs only that creation with stream output succeeds, which
steps 1 and 2 give.

### Open points to check before writing code

- `nir_io_add_intrinsic_xfb_info` needs `xfb_info` before `nir_lower_io`; Panfrost calls it
  at pan_shader.c:171 on already lowered IO, as GL's does - confirm that the helper's
  `location` / `component_offset` are what it matches against for our `vec4` outputs at
  `VARYING_SLOT_VAR0 + register` and for SV_Position.
- Whether the xfb variant may run when the vertex shader has no SV_Position (11 corpus
  vertex shaders compile to an empty binary without it): the variant only stores to memory,
  so it should, but the "empty binary" shortcut has to be looked at.
- The SO buffer bound as a vertex buffer in the same batch as the capture: compute-type jobs
  are added as barriers, so a later vertex job waits; confirm in pan_jc.h for a draw that
  both captures and reads (Direct3D forbids the same buffer as input and output of one draw).

## R3.8 T76b: the run did not reach our driver (read 02:12-02:16)

`results-t76b/ours/pre/umdlog/sbwt-d3d10core-A.log` and `-B.log` both say 110 pass, 1 fail
(test_alpha_to_coverage). **Neither pass tested sbumd**: every test's output has
`Adapter: L"Microsoft Basic Render Driver", 1414:008c`. So T76b says nothing about either
translator, and its A/B comparison is void.

What happened, from the per-process driver logs (450 of them, two device creations per
process, all alike): the runtime opens the adapter, calls CreateDevice, asks
CheckFormatSupport / CheckMultisampleQualityLevels format by format, and stops after

```
sbumd: CheckFormatSupport DXGI_FORMAT_R32G32B32_TYPELESS (5) -> PIPE_FORMAT_R32G32B32_UNORM: caps 0x1
sbumd: CheckMultisampleQualityLevels DXGI_FORMAT_R32G32B32_TYPELESS x4: 0
sbumd: CheckFormatSupport DXGI_FORMAT_R32G32B32_FLOAT (6) -> PIPE_FORMAT_R32G32B32_FLOAT: caps 0x1f
sbumd: CheckMultisampleQualityLevels DXGI_FORMAT_R32G32B32_FLOAT x2: 0
sbumd: CheckMultisampleQualityLevels DXGI_FORMAT_R32G32B32_FLOAT x4: 1
```

Nothing from the driver follows (no format 7, no x8), and the test's `create_device` falls
back to WARP. In T74 (same runtime, MSAA compiled out) the runtime walks all formats. The
T76 driver is cf67267, where MSAA (0165) is on unconditionally.

Reading: the runtime drops a device whose driver reports multisampling for a format but not
for the typeless format of its family. In T74's full list of answers that family (R32G32B32)
is the only one whose typeless stand-in (`R32G32B32_UNORM`, not a Direct3D format, not
renderable on Panfrost) differs from its members (FLOAT, UINT, SINT: render targets), and it
is exactly where the runtime stops. Not shown: the rule itself. The other candidate is that
the runtime does not accept multisampling on R32G32B32_FLOAT at all; the fix below does not
cover that.

Fix: `1e2a6c6` (Format.cpp): `R32G32B32_TYPELESS` is created as `R32G32B32_FLOAT`, as
`R32_TYPELESS` already is `R32_FLOAT`. With the flags off this changes one answer:
CheckFormatSupport of format 5 becomes that of format 6 (0x7 instead of 0x1). Confidence:
medium-low; compiled only.

Consequences for tonight's queue:

- **T77** is built from 1c0a515, where MSAA is behind the `msaa` flag file. Its passes without
  `msaa` should reach the driver; every pass with `msaa` (and `fl11`, which needs it) will
  most likely end on the Basic Render Driver in the same way. Check the `Adapter:` line of
  any test output before reading a T77 score; `wt-compare.py` does not look at it.
- A rerun of the A/B comparison needs either a driver with `msaa` off or this fix; the DLL in
  `umd-nir-fix3` has both (MSAA off without the flag file, and 1e2a6c6).
- `CheckMultisampleQualityLevels` logs only its first 24 calls per process (`said < 24`), so
  the log alone cannot show a later format being refused once this one passes.

# Phase 7: stream output

State: 2026-11-16 03:02 EET. Branch `sb-nir`, on top of round 3 (1e2a6c6), nothing pushed:

- `057f691` poly: stream output only from vertices that were emitted; static counts exported
- `68c7bce` panfrost: stream output with Direct3D semantics, from vertex and geometry shaders
- `c87a244` d3d10umd: the harness lowers and compiles stream output, and checks its bookkeeping
- `4008acb` d3d10umd: every shader starts without a compute NIR (**a fault in 1e2a6c6, see P7.8**)
- `0dc3e0d` panfrost, poly: two warnings of the Windows build
- `ee1da6b` d3d10umd: stream output is wired to the driver

**Nothing of this has run on a GPU, and no draw path has run at all.** What ran is on the Linux
host: the lowering, NIR validation, the Bifrost arch 6 compile of every piece, and 35 checks of
the bookkeeping arithmetic. P7.9 lists it.

No new switch. Stream output from a vertex shader for lists that are not indexed works without
any flag file (it did not exist before: the creation failed). Everything through the geometry
path needs `gsemu` and `nirshaders` as before.

## P7.1 Where the design of R3.7 was changed, and why

1. **The rasterization shader has to know which vertex IDs are real.** poly's stream output
   writes sit in the rasterization shader and run for whatever vertex ID the shader is run for
   (poly_nir_lower_gs.c, `create_gs_rast_shader`): copy 0 of a vertex is written
   unconditionally (`poly_xfb_vertex_copy_in_strip` in src/poly/cl/geometry.cl: `copy == 0 ||`).
   asahi only shades the IDs an index buffer names. A Mali job shades a range, which also
   holds IDs of strips too short for a primitive, IDs no primitive uses, and for a dynamic
   topology the IDs the range is padded with to powers of two: each would have written a record
   over somebody else's. 057f691 adds a flag per shaded vertex (part of a complete emitted
   primitive of its stream) and a test of the primitive ID against the count. asahi's result is
   unchanged (the test is always true there). R3.7 did not see this.
2. **Vertex-shader-only stream output beyond tier 1 captures through a pass-through geometry
   shader but is rasterized by its ordinary draw.** R3.7 wanted to draw through the geometry
   path as well. Capturing only (`sb_gs.xfb_only`) leaves the picture of such a draw exactly
   what it is today, and the pass-through shader exists in src/poly
   (`poly_nir_passthrough_gs`). Cost: the vertex shader runs twice, as in tier 1.
3. **Dynamic counts use poly's pre-GS kernel as it is**, offsets and statistics in GPU memory,
   instead of reading a total back per draw: consecutive draws into the same targets need no
   readback at all, only the first use of a fill level or a statistic on the CPU does (P7.5).
   The prefix sum is a serial loop put in front of the pre-GS kernel
   (pan_nir_lower_gs.c:279 `prepend_prefix_sum`), not a kernel of its own.
4. **Later vertex jobs depend on the last stream output job of the batch** (`sb_so_dep`,
   pan_job.h; `sb_job_dep()` pan_jm.c:31). R3.7's open point said compute-type jobs are added
   as barriers; what the barrier bit orders is not written down anywhere I could read, and
   phase 3 found that the chain orders nothing. DrawAuto right after the capture, in the same
   batch, is the normal Direct3D use, so the dependency is explicit.
5. Multiple streams and a rasterized stream other than 0 are done (poly has both), not refused.

## P7.2 Bookkeeping (pan_so.h, pan_context.c, pan_cmdstream.c)

- `panfrost_streamout_target::offset` is bytes filled from the start of the target. Upstream
  counted vertices and advanced **every bound target on every draw**, capturing or not, by the
  draw's vertex count (`panfrost_update_streamout_offsets`, removed); it never looked at the
  buffer size. Now only a capturing draw moves the level, by what it wrote.
- `set_stream_output_targets`: an offset replaces the level, -1 keeps it (pan_context.c:1088).
- Whole primitives: `pan_so_clamp` gives, per stream, min(needed, room in each bound buffer of
  that stream), room being poly's own formula (the last vertex needs space up to its last
  captured component, not a whole stride). A buffer the shader writes with no target bound
  takes nothing and limits nothing, unless the stream has no bound buffer: then nothing is
  written and everything is still counted as needed (d3d11.c:6653 expects written 0, needed 5
  with no targets).
- Statistics: `ctx->sb_so.needed[4]`, `written[4]`, per stream. `PIPE_QUERY_SO_STATISTICS`
  (index = stream), `SO_OVERFLOW_PREDICATE` (needed > written) and `SO_OVERFLOW_ANY_PREDICATE`
  are served from them; they returned zeros before.
- DrawAuto (pan_cmdstream.c:4360 `panfrost_draw_vbo`): count = (fill level - offset of vertex
  buffer 0) / stride of the first vertex element that reads vertex buffer 0. b908e17 took the
  level as a vertex count. No vertex element on slot 0 means no draw.

## P7.3 Vertex shader stream output

Frontend (Shader.cpp, Draw.cpp): on a driver whose geometry stage does not take TGSI
(`SbVsStreamOutput`, Shader.cpp:236: Panfrost with or without `gsemu`) the shader of
`CreateGeometryShaderWithStreamOutput` without code is only its declaration
(`Shader::sb_so_only`, no pipe object, an id that is never reused). `ResolveState`
(Draw.cpp:90) binds, in place of the bound vertex shader, a pipe vertex shader created from the
same code with `pipe_shader_state::stream_output` (`SbVsStreamOutputVariant`, Shader.cpp:284;
one kept per vertex shader, for the declaration it was last drawn with). Old translator: the
kept tokens. NIR: the token stream and signature are kept per vertex shader
(`SbKeepVertexShaderSource`) and translated again, which is cheaper than keeping a clone of
every vertex shader's NIR. Drivers with the draw module (softpipe, llvmpipe) keep the geometry
state without code, unchanged.

Driver: `panfrost_create_shader_state` builds `nir->xfb_info` from the declaration when the NIR
has none (`panfrost_nir_set_stream_output`, pan_nir_lower_gs.c:323; `register_index` is the
`driver_location` of the output variable, on both translators), and the upstream transform
feedback variant does the rest.

| Draw | What happens |
|---|---|
| point, line, triangle list, not indexed, one instance | the transform feedback variant, clamped to whole primitives that fit (`panfrost_launch_xfb`, pan_cmdstream.c:3438) |
| the same, instanced | the same; when not everything fits, capture stops at a **whole instance** (the records of an instance are contiguous and a partial instance would need a second job with another instance ID): fewer primitives than Direct3D writes, `perf_debug` line. With `gsemu` this case goes the next row's way and is exact |
| indexed, strips, fans, adjacency, instanced, with `gsemu` | captured by the pass-through geometry shader of the primitive class (`panfrost_get_so_gs`, pan_shader.c:1012; one per class, cached on the vertex shader), capture only; then the ordinary draw without capture |
| the same without `gsemu`, or a vertex shader writing clip or cull distances (no pass-through for those) | **nothing is captured**; counted as needed, not written; one `mesa_logw` line |

Refused at creation, with the reason in the log: an entry naming a system value that is one
integer (primitive ID, render target or viewport index), a clip or cull distance, a register
the shader does not write (NIR path), a component mask with a hole, a stream other than 0
without a geometry shader. A declaration the driver cannot express fails the variant
(`stream output of a vertex shader NOT set up`): the draws go on without capture, counted as
nothing.

Not done: with no rasterized stream (Direct3D 11's `D3D11_SO_NO_RASTERIZED_STREAM`) and no
geometry shader, rasterization is **not** switched off (the frontend has no rasterizer discard
variant; the rasterizer state object keeps only its handle). Logged at creation. It makes a
difference only when a pixel shader and a render target or depth buffer are bound.

## P7.4 Geometry shader stream output (gsemu)

`panfrost_create_gs_state` builds the xfb info from the declaration and lowers with it
(`panfrost_gs_from_nir`, pan_shader.c:890). From poly's rasterization shader two shaders are
made (pan_nir_lower_gs.c:257 `keep_only`): the draw's, without the stream output stores, and
the writer, without outputs, a vertex shader CSO that compiles without IDVS because it writes
memory and has no outputs (pan_shader.c:174).

One draw (`panfrost_single_draw_gs`, pan_cmdstream.c:4114), each job depending on the one
before:

1. capture job (as before);
2. counts on the GPU only: count kernel (one invocation per input primitive and instance),
   then pre-GS with its prefix sum (one invocation);
3. index kernel, only if the draw rasterizes and the topology is dynamic;
4. the writer, a compute-type vertex job over **every** vertex ID the rasterization draw can
   use: its vertex count for the two shapes that are not indexed, `vertex_id_stride x streams`
   for a static index buffer, `(instances << primitives_log2) x vertex_id_stride x streams` for
   a dynamic one. Skipped when nothing can be written;
5. the rasterization draw, unless the shader was created with no rasterized stream, has no
   SV_Position, or the draw only captures.

Counts known at compile time (19 of the 26 corpus shaders with the harness's declarations):
`panfrost_gs_stream_output` (pan_cmdstream.c:4013) does the arithmetic on the CPU and puts
final addresses and vertex limits into `struct poly_geometry_params`. Counts that depend on
data (7): the structure gets the buffer bases and sizes, the address of each target's fill
level word and of the counters; the pre-GS kernel does the same arithmetic on the GPU.

Streams: the vertex ID carries the stream in its low bits (poly); the vertex bound is
multiplied by the stream multiplier (phase 3 did not: its bound was wrong for a multi-stream
shader, which it could not draw usefully anyway). The rasterized stream is a compile-time
constant of the CSO (new `sb_rasterized_stream` / `sb_no_rasterized_stream` in
`pipe_stream_output_info`, zero meaning stream 0 for every other frontend).

Frontend: both DDIs go through `SbCreateGeometryShaderWithStreamOutput` (Shader.cpp:1304).
The Direct3D 11 one (Shader.cpp:2210) no longer refuses streams, several strides or a
rasterized stream. An entry is resolved to the vec4 output variable of its register **and
stream** (`SbNirSoLocation`, Shader.cpp:251); the first-match lookup of `output_mapping` was
wrong for a register shared by two streams or by a varying and a system value.

Old translator: a geometry shader with code never reaches Panfrost as TGSI (the driver takes
NIR only). With `gsemu` and without `nirshaders` its creation now **fails** with a log line
(`geometry shader refused by the GPU driver`); it used to succeed and be ignored (P3.7 item 6).

## P7.5 Every place a draw now reads back, waits or flushes

| When | What | Cost |
|---|---|---|
| vertex shader stream output; geometry shader stream output with static counts | nothing | none: CPU arithmetic, one more job |
| a draw whose geometry shader has data-dependent counts and stream output | nothing at the draw: two more kernels | none, as long as the next draws into the same targets are of the same kind |
| first `pipe_buffer_write` of a target's fill level before such a draw (`panfrost_so_prepare`, pan_cmdstream.c:3194) | 4 bytes into a 64-byte buffer | a flush and wait only if an earlier batch still writes that buffer (an explicit offset set while a dynamic draw is in flight) |
| after such a draw, the first of: a draw with CPU-side counts into the same target, DrawAuto from it (`panfrost_so_target_fill`, pan_context.c:1045) | reads one word | **flush of the batch that wrote it, and a wait**; then nothing until the next dynamic draw |
| after such a draw, the first `begin_query` or `end_query` of a stream output statistics query or predicate (`panfrost_so_sync_counters`, pan_context.c:1066) | reads 16 words | **flush and wait**; `end_query` therefore blocks when a dynamic draw happened inside the query |
| indexed draws with a pass-through shader | as every geometry shader draw from d3d10umd: the CPU restart scan of P3.3 | as before |

All readbacks happen before the draw asks for its batch.

## P7.6 Harness tables (ran at cd7d8a9, Bifrost arch 6, `harness-p8*.txt`)

The declarations of the tests are C arrays the harness does not read: every shader gets one
made from its own outputs (every vec4 output whole, each stream to the buffer of its number),
in the form Shader.cpp hands the driver.

Geometry shaders, created with stream output (bytes of Bifrost code; "rast" is the draw's
shader, unchanged from P3.5 / P4.6 in every row):

| Shape, indices | Shaders | Counts | writer | rast | count kernel | pre-GS + prefix sum |
|---|---|---|---|---|---|---|
| static per primitive, 4 | 9 | CPU | 3168-4432 | 1904-3616 | - | - |
| static per primitive, 3 | 1 | CPU | 4272 | 3488 | - | - |
| static per instance, 1 | 6 | CPU | 2608-3952 | 1840-2400, one 0 | - | - |
| static per instance, 3 | 3 | CPU | 4048-4352 | 3056-5280 | - | - |
| dynamic indexed, 4 / 8 / 16 | 1 / 2 / 3 | GPU | 3904-6928 | 2992-3408 | 272 | 2384-2400 |
| dynamic indexed, 32, four streams (d3d11.c, gs_5_0) | 1 | GPU | 8944 | 0 (no SV_Position) | 640 | 6320 |
| all | 26 of 26 lower, validate and compile | | | | | |

Vertex shaders, created with stream output (68 in the corpus):

| Piece | Shaders | Bytes |
|---|---|---|
| transform feedback variant | 68 | 336-1104 |
| capture variant of the shader itself (phase 3 only compiled a generated one) | 62 | 544-1424 |
| pass-through writer: points / lines / triangles / lines adj. / triangles adj. | 62 each | 1888-3952 / 2640-5568 / 3584-7424 / 2576-5472 / 3472-7296 |
| no pass-through: the shader writes clip or cull distances | 6 | - |

Through the old translator (`--tgsi`) 64 vertex shaders get the same three kinds of pieces and 4
have no pass-through. Every pass-through shader comes out with one primitive per input
primitive and static counts (checked).

Bookkeeping, `--selftest`: `stream output bookkeeping: 35 checks, 0 failed`: offsets and
append, room in a buffer (points, lines, triangles, a full buffer, an offset past the end, a
trailing gap), **the CPU formula against a transcription of poly's `setup_xfb_buffer` over
1.7 million combinations**, several buffers and streams, a sequence with overflow and its
statistics, DrawAuto counts, whole instances, folding GPU counters.

Totals: `harness-p8.txt` 468 containers in scope, 468 compile, 12 refused as intended, 0
failures (hull and domain shaders now compile, phase 8: they were "validated only");
`harness-p8-corpus.txt` 362 of 362; `harness-p8-tgsi.txt` 233 compiled, 23 validated, the same
13 failures. Stripped of the new notes, every line equals round 3's except the pre-GS size of
geometry shaders **without** stream output (544 to 592, 640 to 704, 528 to 576: that kernel is
never run without stream output; its statistics pointers are now a word of the parameter
structure instead of address 0).

## P7.7 All flag files absent: what differs from 1e2a6c6

Read from the diff, not run on Windows. In behaviour, for applications that do not use stream
output: nothing. For those that do, it now exists.

- `CreateGeometryShaderWithStreamOutput` **without code now succeeds** on Panfrost (it failed
  with E_OUTOFMEMORY: no geometry stage), and non-indexed list draws with it capture. This is
  the feature; it is not behind a flag because nothing worked there before. With code it
  still fails the same way without `gsemu`.
- SO statistics queries and overflow predicates return counts instead of zeros; DrawAuto draws
  bytes / stride instead of a vertex count that was always 0.
- A stream output target bound across draws that do not capture no longer has its fill level
  advanced by them.
- Code that runs and decides nothing: in `ResolveState` one comparison of two pointers that are
  both NULL; in `panfrost_draw_vbo` a test of `vs->xfb`; `sb_job_dep()` returning 0 where
  `sb_gs.job_dep` (0) was passed.
- Data: `Shader`, `Device`, `panfrost_context`, `panfrost_batch`, `panfrost_query`,
  `panfrost_streamout_target`, `panfrost_uncompiled_shader`, `panfrost_gs`,
  `pipe_stream_output_info` (2 bytes) and `poly_gs_info` grew.
- With `nirshaders`: every vertex shader keeps a copy of its token stream and signature.
- `DestroyShader` clears `bound_vs` / `bound_empty_gs` when they point at the shader destroyed
  (they could dangle; nothing read them then).
- The bug fix of P7.8.

## P7.8 Found on the way

- **`Shader::sb_cs_nir` was read uninitialised** (fixed, 4008acb). `DestroyShader` tests and
  `ralloc_free`s it for a shader of any stage (since 08ac11c, phase 5), only
  `CreateComputeShader` set it. For every vertex, pixel and geometry shader it was whatever
  the runtime left in the object's memory, which is not zeroed: the fault of sb-patches/0128
  (LogonUI, T33) again. **The DLL staged for T77 (35bae4d5) has it.** If T77 shows a crash in
  `DestroyShader` / `ralloc_free`, this is it; `umd-nir-so` does not have it.
- Phase 3's vertex bound for multi-stream geometry shaders ignored the stream multiplier (P7.4).
- `output_mapping` first-match for stream output entries (P7.4).
- Upstream Panfrost advances transform feedback offsets by `u_stream_outputs_for_vertices` of
  a vertex count that includes `abs(index_bias)`; gone with the rewrite.

## P7.9 Verified by running / only compiled / reasoned

Ran (Linux host, assertions on): the three harness runs of P7.6; poly's lowering with stream
output on 26 geometry shaders and on the pass-through shaders of 62 vertex shaders x 5
classes, NIR validation of every piece, the Bifrost compile of every piece; the 35 bookkeeping
checks; native Panfrost for every architecture builds; ARM64 and ARM64EC builds, the ARM64X
link, the verifier.

Compiled only, never executed: everything in pan_cmdstream.c, pan_context.c, pan_jm.c and
pan_shader.c of this phase (jobs, dependencies, readbacks, queries, DrawAuto), and the whole
frontend part. No shader was executed: that a record lands where it should is by reading
poly's code and mine. The most likely places to be wrong, in my order: the writer job's vertex
and instance IDs for the per-primitive shape (hardware instances = primitives x instances,
taken from a compute-type job's coordinates as the transform feedback job does); the emitted
flag for points; `StartVertexLocation` != 0 with the transform feedback variant (R3.7's open
point, still open); the meaning of the barrier bit, which the explicit dependency makes
irrelevant if dependencies work as phase 3 read them.

Reasoned: that Direct3D leaves a buffer alone when a stream has an unbound buffer (I could not
find the rule; the choice is in pan_so.h); that a write to the sink address is dropped on
Bifrost (upstream uses it for the same purpose).

## P7.10 The Windows build

`~/sbmesa/build-nir-x/build-all.sh` at cd7d8a9: ARM64 and ARM64EC exit 0, link log empty, 35
`ok` lines, `FAILS: 0`.

```
/Users/dev/project-starbite/.claude/worktrees/win-mali-gpu-87c7b0/build/win/umd-nir-so/
  sbumd.dll  27554304 bytes  sha256 24ec17d933a25d57298ce7c70fb8e511c186413365825057274ff7a92ac3855c
  sbumd.map   6862968 bytes  sha256 b9bcca605ea9a080f514f69c7341d3ec3041f63c779185ea87d33ac13dcb9dd8
```

`umd-nir` (98e9e6a6...), `umd-nir-gs` (e9e9d067...), `umd-nir-d3d11` (1572d243...) are
untouched. `strings`: no new flag file name; the flag names are the same nine; new log lines
`stream output of a vertex shader NOT set up`, `geometry shader refused by the GPU driver`,
`is done through the geometry path (gsemu)`, `WITHOUT tessellation`.

To try on a device: nothing to create for a non-indexed list with a vertex-shader-only
declaration (test_stream_output_vs); `nirshaders` + `gsemu` for the rest. First test:
test_stream_output_vs, then test_stream_output_resume (static counts, DrawAuto), then the
multi-stream statistics test of d3d11.c:6610 (dynamic counts, the readbacks of P7.5).

## P7.11 Rerun

```sh
export PATH=~/sbmesa/native:~/sbmesa/venv/bin:$PATH
cd ~/sbmesa && ninja -j6 -C build-nir-host src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
cd build-nir-host
W=/Users/dev/project-starbite/build/win/winetests/src
H=./src/gallium/frontends/d3d10umd/tests/dxbc_nir_harness
$H --selftest --stats $W/d3d10core.c $W/d3d11.c > harness-p8.txt
$H $W/d3d10core.c $W/d3d11.c > harness-p8-corpus.txt
$H --tgsi $W/d3d10core.c $W/d3d11.c > harness-p8-tgsi.txt
grep "SO " harness-p8-corpus.txt            # one note per vertex and geometry shader
grep "bookkeeping" harness-p8.txt
~/sbmesa/build-nir-x/build-all.sh
```

# Phase 8: tessellation (designed, not implemented)

State: 2026-11-16 03:02 EET. `6678707` (harness) and `cd7d8a9` (a log line). **There is no
tessellation in the driver.** Hull and domain shader creation is refused as before
(E_OUTOFMEMORY, logged), there is no `tessemu` flag file, and the `fl11` log line now says that
11_0 is offered without tessellation. What exists is the design below and harness evidence for
its three shader pieces.

## P8.1 How asahi drives it (read: agx_state.c:4500-4720, src/asahi/libagx, src/poly)

1. vertex shader as a compute kernel with software input assembly, outputs to memory;
2. hull shader as a compute kernel, a workgroup of one thread per output control point, writing
   per patch: tessellation factors, patch constants, control points (`poly_nir_lower_tcs`);
3. tessellator, count pass: one thread per patch, writes the patch's index count;
4. prefix sum over the counts; allocates the index buffer from a GPU-side heap and writes an
   indirect draw;
5. tessellator, second pass: allocates the patch's domain points from the heap, writes them
   and the indices (vertex ID = patch x 8192 + point);
6. an indexed **indirect** draw whose vertex shader is the domain shader
   (`poly_nir_lower_tes`): it finds patch and point from its vertex ID and reads the hull
   shader's output from memory.

The tessellator is Microsoft's reference tessellator in OpenCL C (src/poly/cl/tessellator.h).
It is **not** in libpoly: asahi and kosmickrisp each build it into their own kernel library.

## P8.2 What Bifrost with the job manager lacks, and what follows

No indirect draw, no GPU-side allocator, and a vertex job (or the position storage of an IDVS
job) covers a **range** of vertex IDs. Two amounts are known only after the tessellator ran:
the number of indices and the number of domain points. Three consequences:

- the draw's index count and vertex range must be known on the CPU when the draw is emitted;
- vertex IDs of patch x 8192 + point cannot be used: the range would be 8192 IDs per patch
  (131 KB of positions per patch before varyings). IDs have to be dense;
- everything the second batch of a readback design needs must outlive the first batch (the
  batch pool does not).

Two designs; neither needs anything Bifrost cannot do.

**B, worst case, no readback (do this first).** The hull shader declares its maximum factor
(`dcl_hs_max_tessfactor`, 64 if absent), which bounds points and indices per patch:

| Domain at factor n | points | indices |
|---|---|---|
| quad | (n+1)^2: 4225 at 64, 289 at 16 | 6n^2: 24576 at 64, 1536 at 16 |
| triangle | 1 + 3(n/2)(n/2+1): 3169 at 64 | 4.5n^2: 18432 at 64 |
| isoline | n(n+1): 4160 at 64 | 2n^2: 8192 at 64 |

Jobs of one draw, in the framebuffer's batch, each depending on the one before:

1. capture job of the vertex shader (exists);
2. hull kernel: grid = patches x instances, **one thread per patch** (P8.3);
3. tessellator, second pass only: the count pass is replaced by a count buffer the CPU fills
   with the fixed stride (i + 1) x max indices, so every patch writes its indices at
   patch x max indices; the heap is a region of patches x max points x 8 bytes;
4. the draw: domain shader as the vertex shader, 32-bit indices, count = patches x max
   indices, vertex range = patches x max points.

Buffers, all from the batch pool: capture (as phase 3), hull output (patches x 72 to 216 bytes
in the corpus), point allocations (4 bytes per patch), points, indices. The index buffer is
cleared by the CPU first so that unused entries are degenerate triangles of vertex 0. poly
changes: the 8192 becomes a field of `struct poly_tess_params` (three uses), nothing else.
Cost: memory and tiler work for the declared maximum whatever the factors are: about 200 KB
per quad patch at 64 plus its varyings, 13 KB at 16. No flush, no readback, no wait.

**A, exact, one readback per draw (when B's memory is too much).** Jobs 1 to 3 with the real
count pass and a prefix sum kernel (the serial loop of phase 7 will do); then **flush the
batch, wait, read one word** (the total index count, which also bounds the points); in the
next batch the second pass and the draw. The framebuffer is reloaded for the second batch.
The hull output and the counts have to live in a buffer of the context, not in the batch pool.
Dense vertex IDs need the tessellator to number points from the patch's allocation and a
point-to-patch word per point (about 30 lines of OpenCL in tessellator.h and
tessellation.cl). The readback is unavoidable here: it is the price of not allocating for the
worst case, on a GPU that cannot be told the count of a draw by another job.

**With a geometry shader after the domain shader:** the domain shader runs as a capture job
(`poly_nir_lower_tes(tes, false)`, the form asahi uses ahead of a geometry shader) and
`panfrost_single_draw_gs` takes the tessellator's index buffer where it takes the
application's. Design B's padding does not work there (a geometry shader would emit geometry
for a degenerate triangle), so this case needs design A or a pad index the geometry path
skips. Open.

**Stream output after tessellation** is the pass-through geometry shader of phase 7 behind the
domain capture: same remark.

## P8.3 The hull shader as one thread per patch

poly runs the hull shader with one thread per output control point and relies on a barrier
inside a workgroup for the patch constant phases (which read every control point). Here the
body is wrapped in a loop over the output control point **from N-1 down to 0**
(`serialize_hull_invocations` in the harness): ShaderNIR.c puts the patch constant phases
under `invocation == 0`, so counting down they run last, after every control point is in
memory. No barrier, no assumption about how a compute job groups its threads, grid =
(patches, instances) like the geometry kernels. Its control point inputs would be fetched
through the index buffer from the capture, as `lower_gs_input` does for a geometry shader
(patch class: vertex = patch x control points + i); the harness uses poly's own fetch.

## P8.4 Harness (ran at cd7d8a9, Bifrost arch 6)

Hull shader: `nir_lower_io`, the loop of P8.3, `poly_nir_lower_tcs`, compiled as a compute
kernel. Domain shader: `nir_lower_io`, `poly_nir_lower_tes(tes, true)`, compiled as a vertex
shader. Pairs by the test source (line numbers in d3d11.c):

| Test | Hull | bytes | control points out | bytes per patch | Domain | bytes |
|---|---|---|---|---|---|---|
| test_device_context_state (7124 / 7144) `simple_hs` / `simple_ds` | 7124 | 2688 | 3 | 72 | 7144 | 2032 |
| test_clear_state (12101 / 12121) `simple_hs` / `simple_ds` | 12101 | 2688 | 3 | 72 | 12121 | 2032 |
| test_quad_tessellation, ccw and cw hull with one domain (26282, 26336 / 26307) | 26282, 26336 | 6320 each | 4 | 88 | 26307 | 2496 |
| test_clip_distance (29890 / 29926) `hs_code` / `ds_code` | 29890 | 11408 | 4 | 216 | 29926 | 4544 |
| all | 5 of 5 | | | | 4 of 4 | |

The six hull and five domain self tests (control point phase, instanced fork phase, `vocp`,
join phase, maximum factor) compile the same way. The summary tables therefore show hull and
domain shaders under "OK" where they were "validated, not compiled".

The tessellator, with `-Dd3d10umd-nir-tests-tessellator=true` (off by default: vtn_bindgen2
makes 1.1 GB of C++ from it), as NIR builder functions (`tests/sb_tess_kernels.cl`), one
compute invocation per patch:

| Domain | count pass | points and indices |
|---|---|---|
| isoline | 1824 bytes | 6224 bytes |
| triangle | 3360 bytes | 80128 bytes |
| quad | **fails before the compiler**: the generated builder code leaves a control flow list that `nir_validate` rejects (`collect_blocks: Invalid CF node type`) | the same |

So the Bifrost compiler takes the tessellator (global atomics, private arrays, its loops) for
two of three domains; the quad failure is in the form I built it in, not in Bifrost. The form
to use in the driver is the one Panfrost already has for kernels: entry points in
src/panfrost/libpan (as `indirect_dispatch.cl`), compiled per architecture at build time by
`panfrost_compile`, which is also how asahi and kosmickrisp build theirs. Not tried.

## P8.5 What the implementation consists of (nothing below exists)

Driver: `PAN_DBG_TESSEMU` / flag file `tessemu` (needs `gsemu`'s machinery and `nirshaders`);
tessellation stage caps (NIR only), `max_patch_vertices` 32; `create_tcs_state` /
`create_tes_state` and binds; a sysval for the tessellation parameters;
`load_vs_outputs_poly` from `struct poly_vertex_params`; the three tessellator kernels in
libpan; `panfrost_single_draw_tess` (design B) for `MESA_PRIM_PATCHES`; the domain shader's
resources swapped into the vertex stage as the geometry stage's are. Frontend: the list of
P5.12 (creation through `Shader_nir_translate` with the patch constant signature, the hull
and domain stage state handed to the driver, the patch topologies of `IaSetTopology`, the
hull shader's `info.tess` copied into the domain shader's). `fl11` would then require
`tessemu`.

## P8.6 Verified by running / reasoned

Ran: the harness table of P8.4 and the tessellator compile of P8.4, nothing else. Reasoned
from source: everything in P8.1 and P8.2, the worst-case formulas (from the reference
tessellator's structure, not checked against its output), that counting control points down
is equivalent to the barrier for what ShaderNIR.c generates.
