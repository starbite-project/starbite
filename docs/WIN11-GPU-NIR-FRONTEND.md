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
