# fixA: driver fixes from the first conformance run of the sb-nir tree (T82)

Written 2026-11-16 19:00 EET. Input: `build/win/results-t82/ours/pre/` (sbumd 63cb8404, sb-nir 430b803).
Nothing here was run on a phone. "Host" means the Linux harness in `~/sbmesa/build-nir-host`
(`dxbc_nir_harness`, Bifrost arch 6 compiler); "compiled only" means the ARM64X DLL builds and nothing executed
the change.

## Deliverables

| What | Where |
|---|---|
| DLL + link map | `build/win/umd-nir-fixA/sbumd.dll`, `sbumd.map`, `SHA256SUMS` (sbumd.dll a8a6dca8ea013065d73964b3633316f311cf4ea4db6f3992ccd249755a292152) |
| Patches | `build/win/patchwork/fixA-patches/0001..0008-*.patch` (`git format-patch 430b803..ea8a080`) |
| Branch | VM `~/sbmesa/src-nir`, `sb-nir`, HEAD `ea8a080`, 8 commits on 430b803, tree clean, nothing pushed |
| Build | `~/sbmesa/build-nir-x/build-all.sh`: exit 0; ARM64X verification `FAILS: 0`; `check-init-tables.py`: C++ initializers 0, C initializers 0 |
| Host runs | `~/sbmesa/build-nir-host/harness-fixA-final.txt` (NIR path + self tests), `harness-fixA-final-tgsi.txt` (old path); stderr files empty |
| Work scripts | `build/win/patchwork/fixA-work/` (the edit scripts, `run-host.sh`); `tlog.sh` (test -> driver log) is in the session scratch folder |

Host result, final tree: NIR path 471 of 471 in-scope shaders translated, validated and compiled (468 before,
plus 3 new self tests), 0 wrong results; old path unchanged line for line against `harness-p9-tgsi.txt`
(235 compiled, 23 geometry validated, 11 refused); `stream output bookkeeping: 35 checks, 0 failed`;
`restart-free draws: 15 checks, 0 failed` (new).

## The commits

| # | Commit | Path it changes | Verified |
|---|---|---|---|
| 1 | `422fd07` NIR translator records texture / sampler slots | `nirshaders` only | host: reproduced (94 of 468 shaders failed the new check) and fixed (0) |
| 2 | `0149fb5` sampler count covers samplers above the textures | both translators, only raises a count | host self test (failed before, passes now); effect on hardware not measured |
| 3 | `a0b8c94` TGSI refusal falls through to NIR | default path, only for shaders whose creation failed before | compiled only; the three shaders compile on host through NIR |
| 4 | `9a1c5d5` multisampling answered per format family | `msaa` only | compiled only; rule inferred |
| 5 | `feb4e17` Direct3D depth bias not doubled | default path, depth bias only | compiled only; all 437 failing lines match the model |
| 6 | `ea9a44a` buffer render target view no longer removes the device | default path, a call that removed the device before | compiled only |
| 7 | `e92ebbf` stream output / geometry capture vertex variants follow integer vertex elements | vertex shader stream output, `gsemu` | compiled only |
| 8 | `ea8a080` restart-free draws do not claim wrong index bounds | `gsemu` (indexed draws with a geometry shader) | host self test (2 of 15 checks failed before, 0 now) |

What changes with no flag file present (the desktop compositor's path): commits 3, 5, 6 and the count in 2.
3 and 6 only act where a creation call failed before (E_OUTOFMEMORY / device removed). 5 halves the depth
bias on fixed-point depth buffers, which DWM does not use. 2 can only make the sampler count of a shader
descriptor larger, never above what the code indexes.

File:line references in this report are to the tested tree (430b803) unless a new function is named.

## Priority 1: the 13 new-translator regressions (pass N vs pass O)

One cause. `ShaderNIR.c` never set `info.textures_used` / `textures_used_by_txf` / `samplers_used`.
`nir_lower_samplers` and `nir_shader_gather_info` do not record them (the GLSL linker and `tgsi_to_nir` do it
themselves; `tgsi_to_nir.c:1148-1153, 1479-1502`). Panfrost takes the shader descriptor's counts from it:
`pan_compiler.c:305` `info->sampler_count = info->texture_count = BITSET_LAST_BIT(s->info.textures_used)`,
written to the RSD at `src/panfrost/lib/pan_shader.h:194-195`. So every pixel shader said "0 textures, 0
samplers" and the draw wrote nothing (the tests read back the clear colour or the untouched target, e.g.
update_subresource `Got 0x800000ff`). The compiled code was identical in both translators (compared the Bifrost
disassembly of d3d10core.c:13021 through both). `info.texture_buffers`, which `sn_finalize` derived from
`textures_used`, was empty for the same reason (buffer_srv).

Fix: `sn_gather_textures_used()` (ShaderNIR.c, called from `sn_finalize` after the cleanup).

Host test added: the harness compares, for every shader it compiles for Bifrost,
`pan_shader_info.texture_count` / `sampler_count` and `info.texture_buffers` with the texture instructions
(`check_tex_slots`, dxbc_nir_harness.c). Before the fix: 94 of 468 NIR-path shaders failed it, 0 of 235 on the
old path. After: 0. Three self tests (`ps_texture_slot0`, `ps_texture_slot5_buffer7`,
`ps_sampler_above_textures`). The third found commit 2: a sampler numbered above every texture (t0 through s6)
got `sampler_count` 1; whether Bifrost enforces that count is not known.

## Priority 2: why `msaa` makes the runtime drop the adapter

What the logs show (pass M, every process, e.g. `d3d10core_test.exe-1016-401781.log`):

- After CreateDevice the runtime walks the formats family by family, the typeless format first (the order is
  visible in any pass O log: 1,2,3,4 / 5..8 / 9..14 / 15..18 / 19..22 / 23,24,25,89 / 27..32 / ...). For each
  format it calls CheckFormatSupport and then CheckMultisampleQualityLevels at 2, 4, 8, 16 samples.
- With `msaa` the last line is `CheckFormatSupport DXGI_FORMAT_D32_FLOAT_S8X24_UINT (20) -> ...: caps 0x1`.
  Without it the walk goes on to format 93. Families 1 to 18 passed (caps 0x1f; x4: 1).
- T76b (docs 6.101) stopped at `R32G32B32_FLOAT x4: 1`, where the family's typeless format had no multisample
  capability; 1e2a6c6 fixed that family and the walk now gets past it.
- Our answers for 19 and 20 were identical: capabilities 0x1 (CheckFormatSupport gave
  `D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET` only when the format is a colour render target,
  Device.cpp), and one quality level at 4x and 8x (CheckMultisampleQualityLevels has a depth clause, and
  Panfrost multisamples Z32_FLOAT_S8X24). So: a depth format with quality levels whose own capabilities, and
  whose typeless parent's, say "no multisampling".

Inferred rule (not documented; the WDK pages for both functions say nothing on it): a member's multisample
answer has to be backed by its family's typeless format, and levels above 0 go with the capability bit. A
reference that the runtime accepts, VirtualBox's WDDM driver (VBoxDX.cpp, vboxDXCheckFormatSupport /
vboxDXCheckMultisampleQualityLevels), keys both answers on one per-format bit, which its host sets for depth
formats too.

Fix (commit 4, Device.cpp `SbFormatFamily` / `SbFormatTargetSamples` / `SbFamilyMultisample`): one answer per
family and sample count (yes when every member that is a colour render target or a depth buffer takes it);
every member reports the same levels; the capability bit is on the typeless format, the colour render targets
and D32_FLOAT_S8X24_UINT, D32_FLOAT, D24_UNORM_S8_UINT, D16_UNORM, R32G8X24_TYPELESS, R24G8_TYPELESS.
The view-only members (R32_FLOAT_X8X24_TYPELESS, X32_TYPELESS_G8X24_UINT, R24_UNORM_X8_TYPELESS,
X24_TYPELESS_G8_UINT) and R10G10B10_XR_BIAS keep their capabilities and answer the family's levels if asked.

State: compiled only. It removes the inconsistency at the format where the walk stopped and in the three
families after it that have the same shape (R32, R24G8, R16). Whether the runtime has a further rule cannot be
known from this log, because the driver printed only the first 24 quality-level answers; the limit is now 640,
so if pass M still falls back, the last lines of one process log name the format and count.
Not looked at: what feature levels 10_1 / 11_0 additionally require (4x on every render target, 8x on most);
by Panfrost's tile buffer arithmetic every format here has 4x.

## Priority 3 and 4: every test looked at

Result columns are T82. "limit" = documented limit, not chased.

### d3d10core

| Pass | Test | T82 | Cause | Fixed | Verified |
|---|---|---|---|---|---|
| N, G | buffer_srv, copy_subresource_region, depth_stencil_sampling, initial_texture_data, null_sampler, rtv_depth_slice, shader_input_registers_limits, swapchain_flip, texture1d, texture, texture_compressed_3d, unbind_shader_resource_view, update_subresource | FAIL (pass in O) | textures_used never set, ShaderNIR.c `sn_finalize`; pan_compiler.c:305 | commit 1 | host |
| N, G | cube_maps, sample_c_lz | FAIL (all lookups 0) | same | commit 1 | host |
| O | cube_maps, sample_c_lz, unbound_multisample_texture | CRASH in the test after `Failed to create pixel shader, hr 0x8007000e` | log: `shader refused (#1): no TGSI translation for opcode D3D10_SB_OPCODE_SWITCH`; ShaderTGSI.c:1381 refuses, Shader.cpp failed the creation | commit 3 (NIR is asked after a TGSI refusal) | compiled; shaders d3d10core.c:7843, 8403, 17805 compile through NIR on host |
| O, N, G | depth_bias | FAIL 437 of 40106 | pan_cmdstream.c `panfrost_z_depth_offset`: units x 2 for fixed-point depth; every failing line is the expected depth with twice the bias (script over all 437, D24 and D16; D32F passes) | commit 5 | compiled; arithmetic checked |
| G | index_buffer_offset | FAIL 16 (draws at index buffer offset 16, 32 capture (0,0,0,1)) | u_prim_restart.c `util_draw_vbo_without_prim_restart` stored index-buffer positions as `min_index`/`max_index` and set `index_bounds_valid`; Panfrost shaded vertices 4..7 / 8..11 | commit 8 | host self test |
| O, N | immediate_constant_buffer | FAIL in O, pass in N | limit of the old path: `tgsi_to_nir.c:581-585` ignores the index of a dynamically indexed immediate constant buffer (docs P2.5) | no | - |
| O, N, G | pipeline_statistics_query | FAIL | limit: Panfrost has no pipeline statistics query | no | - |
| O, N | stream_output | FAIL (`hr 0x8007000e`) | limit: needs `gsemu` (passes in G) | no | - |
| O, N, G | combined_clip_and_cull_distances; G: clip_distance | FAIL (unclipped) | limit: Panfrost on Bifrost has no clip / cull distances (`clip_planes` 0); needs a pixel shader lowering keyed on the vertex shader | no | - |
| O, N | clear_state, clip_distance, create_shader, geometry_shader, index_buffer_offset, layered_rendering, multiple_viewports, stream_output_resume | CRASH in the test after a geometry shader creation failed (`geometry shader refused: this GPU driver has no geometry stage`) | limit: geometry shaders need `gsemu`; all 8 run in G | no | - |
| G | layered_rendering, multiple_viewports | FAIL (layer / viewport index reads 0, only slice 0 drawn) | limit: layered rendering and viewport arrays not implemented (docs P6.2) | no | - |
| G | stream_output_resume | FAIL 12 (records 7..18 zero: capture does not resume after the geometry shader was unbound and bound again) | not found; fill-level bookkeeping in `panfrost_single_draw_gs` / `panfrost_so_prepare` suspected, not confirmed | no | - |

All crash addresses in every pass are inside the test programs (`d3d10core_test.exe + 0x...`, a null object
after a failed creation); none is in sbumd.dll, so the map was not needed. The one exception is dxgi
test_cursor_clipping, which dies inside dxgi.dll.

### d3d11 (pass N, Direct3D 10 level interface)

| Test | T82 | Cause | Fixed | Verified |
|---|---|---|---|---|
| texture1d, texture, cube_maps, depth_stencil_sampling, initial_texture_data, update_subresource, copy_subresource_region, copy_subresource_region_3d, swapchain_flip, null_sampler, shader_input_registers_limits, unbind_shader_resource_view, buffer_srv, texture_compressed_3d, rtv_depth_slice, fl9_draw, sample_attached_rtv, high_resource_count | FAIL | same symptom as d3d10core (lookups 0 / target untouched): commit 1. The last three were classified by symptom, not read line by line | commit 1 | host |
| depth_bias | FAIL 437 | as d3d10core | commit 5 | compiled |
| create_rendertarget_view | CRASH after `hr 0x887a0005` at d3d11.c:4303 | OutputMerger.cpp:99: a render target view of a buffer answered `SetError(E_NOTIMPL)` (log: `Render target view into buffer!`), which the runtime takes as device removal; the view object was left uninitialised and a buffer reference leaked | commit 6 (view created empty; draws into it are dropped, with a log line) | compiled |
| vertex_id | FAIL 24 | two things. (a) captured id is 0x3f800000 for 1: the stream output variant `vs->xfb` (pan_shader.c:711) is compiled once with `int_attribs` 0, so the R32_UINT element is converted to float; (b) the indexed draws are not captured without `gsemu` (limit) | (a) commit 7, also for `vs->gs_capture`; (b) no | compiled |
| create_shader, layered_rendering, geometry_shader, clip_distance, multiple_viewports | CRASH | limit: geometry shader without `gsemu` | no | - |
| fl10_stream_output_desc | FAIL | limit: same | no | - |
| create_blend_state, independent_blend, sample_c_lz, device_context_state | CRASH after `hr 0x80070057` | limit: the runtime refuses 10.1 / 11 features (independent blend, cube array shader, UAV buffer) on a 10.0 interface; no driver call is made | no | - |
| create_buffer, texture1d/2d/3d_interfaces | FAIL (`0x80070057`) | same kind (runtime, feature level); not each read | no | - |
| format_support | FAIL 10 | limit: formats the 10.0 DDI never asks about (B5G6R5 ...) report 0 | no | - |
| format_compatibility | FAIL 132 | tests 15-18 (R9G9B9E5 -> R32*) and 27, 29 (R16 -> D16): the CPU copy path (`util_resource_copy_region` via Resource.cpp:935) leaves the destination zero. Not found | no | - |
| vertex_formats | FAIL 1 | B8G8R8X8_UNORM as a vertex element format reads wrong; not looked into | no | - |
| pipeline_statistics_query, combined_clip_and_cull_distances | FAIL | limits as above | no | - |
| nv12 | FAIL (`0x80004005`) | limit: no video formats | no | - |

### dxgi (pass X)

| Test | T82 | Cause | Fixed |
|---|---|---|---|
| create_swapchain, swapchain_parameters, swapchain_window_styles, resize_target, default_fullscreen_target_output (CRASH); swapchain_present, set_fullscreen, output_ownership, gamma_control, window_association, resize_target_wndproc, swapchain_window_messages (FAIL) | `hr 0x887a0005` | each of the 12 has `sbumd: SetDisplayMode(...): 0x80004005` in its driver log: the runtime's SetDisplayMode callback fails, DxgiFns.cpp:499 returns DXGI_DDI_ERR_UNSUPPORTED and DXGI reports the device removed. Exclusive full-screen / mode changes are a display (KMD) gap (FULL-DRIVER.md line 31), not a user-mode driver bug | no |
| cursor_clipping (CRASH inside dxgi.dll + 0x8948) | | not resolved (no SetDisplayMode line in its log; the test output has nothing before the crash) | no |
| find_closest_matching_mode, output | FAIL | one mode (2960x1440) on the panel | no |
| query_video_memory_info, video_memory_budget_notification | FAIL (`0x8876086a`) | WDDM 2 memory queries the KMD does not answer | no |
| check_interface_support | FAIL (`0x887a0004` from CheckInterfaceSupport) | probable, not verified: DXGI reports the user-mode driver's file version, and sbumd.dll has no version resource (the link script adds none). A `VERSIONINFO` in the link would be the fix; not done here because it changes the ARM64X link | no |

## What the next on-device run should show

If each fix does what the evidence says (d3d10core has 111 tests, d3d11 163):

| Pass | T82 | Expected | Notes |
|---|---|---|---|
| O d3d10core (no flags) | 95 / 5 / 11 | 99 pass, 4 fail, 8 crash | + cube_maps, sample_c_lz, unbound_multisample_texture (log line `the shader the TGSI translator refused goes through DXBC -> NIR`), depth_bias. Left: immediate_constant_buffer, pipeline_statistics_query, stream_output, combined_clip_and_cull; 8 geometry shader crashes |
| N d3d10core (`nirshaders`) | 84 / 19 / 8 | 100 pass, 3 fail, 8 crash | the 13 regressions, cube_maps, sample_c_lz, depth_bias |
| N d3d11 | 122 / 31 / 10 | about 142 pass, 12 fail, 9 crash | 18 texture tests, depth_bias, create_rendertarget_view; vertex_id still fails but with integer ids |
| G d3d10core (`nirshaders gsemu`) | 88 / 23 / 0 | 105 pass, 6 fail | 15 texture tests, depth_bias, index_buffer_offset. Left: layered_rendering, multiple_viewports, clip_distance, combined_clip_and_cull, pipeline_statistics_query, stream_output_resume |
| M (`+msaa`) | Basic Render Driver | our adapter, if the family rule is the whole story | otherwise the process log's last `CheckMultisampleQualityLevels` / `CheckFormatSupport` lines name the next format |
| D, F, T | not tested | no prediction | first real run of the Direct3D 11 DDI |
| X dxgi | 37 / 12 / 6 | unchanged | display side |

The weakest predictions: pass M (inferred rule), and the three pass O tests now created through NIR while their
vertex shaders stay TGSI (the two translators link by register number by construction; this mix has not run).

## What remains, with reasons

- **Old path, dynamic immediate constant buffer index** (test_immediate_constant_buffer in O): can be routed
  to NIR like `switch` (refuse in ShaderTGSI.c when the driver is NIR-only; commit 3 then takes over). Not done:
  unlike `switch`, such a shader is created today and draws something, so this would move working desktop
  shaders onto a translator whose texture path has not yet run correctly on the phone. Do it after pass N
  confirms commit 1.
- **Clip / cull distances** (4 tests): needs `nir_lower_clip_fs`-style discard in the pixel shader with the
  vertex shader's distance count in the key, and culling has no exact pixel-shader equivalent. A feature.
- **Pipeline statistics**: a feature (CPU counts for IA / VS are possible, PS invocations are not).
- **stream_output_resume with gsemu**, **format_compatibility 15-18 / 27 / 29**, **vertex_formats B8G8R8X8**:
  cause not found.
- **Layered rendering, viewport arrays**: not implemented in the geometry path.
- **dxgi**: SetDisplayMode failing in the display driver; DLL version resource (probable cause of
  CheckInterfaceSupport).
- **Geometry shader creation without gsemu** crashes 8 d3d10core and 5 d3d11 tests in their own code after a
  clean failure; nothing to fix short of making gsemu the default.
- Not run: the sanitizer script `~/sbmesa/build-nir-asan/p9-asan-run.sh`.
