# Shader disk cache on Windows (gap-list item 9)

2026-11-16 17:40 EET. GL kit built with a persistent on-disk shader cache; the D3D driver (sbumd) analysed, not
built. **Nothing here has run on Windows or on a phone**: the Windows file code is compiled only. Everything else
(the cache logic that Windows uses, the entry format, the driver identity, the GL path end to end) ran on the Linux
build host. Section 6 says exactly which is which.

| What | Where |
|---|---|
| Kit (ARM64EC, as `gd-kit-push.sh` wants it) + link maps + `SHA256SUMS` + `driver-id.txt` | `build/win/gd-kit-ec-cache/` |
| Patches (`git format-patch`, on top of src-tc 3d8ee0f = GL kit 0170) | `build/win/patchwork/cache-patches/0001-…0171….patch`, `0002-…0172….patch` |
| Build scripts, host test program and logs | `build/win/patchwork/cache-patches/host-test/` |
| Tree and branch (VM) | `~/sbmesa/src-cache`, branch `sb-cache` (e7fcee5 = 0171, 8909baf = 0172); not pushed |
| Build dirs (VM) | `build-cache-ec` (kit, 230 MB), `build-cache-host` (356 MB), `build-cache-hostposix` (32 MB), `cache-work` (20 MB) |

The numbers 0171 / 0172 are my guess at the next free sb-patches numbers; rename if another session took them.

## 1. What Mesa 26.2 has for Windows, and what it lacks

- **meson**: `shader-cache` is refused outright on Windows (`.require(host_machine.system() != 'windows', 'Shader
  Cache does not currently work on Windows')`), and without zlib or zstd it is an `error('Shader Cache requires
  compression')`. Options: `-Dshader-cache` (feature), `-Dshader-cache-default` (bool), `-Dshader-cache-max-size`
  (string, default for `MESA_SHADER_CACHE_MAX_SIZE`; Mesa's own default is 1 GB).
- **`src/util/disk_cache_os.c` / `.h`**: on Windows the whole file layer is the comment `TODO: implement disk cache
  support on windows`. Missing: where the directory is (XDG_CACHE_HOME / HOME / getpwuid), making it, the mmap'ed
  `index` file, the file lock (`flock`/`fcntl`), the atomic write (`open` + `flock` + `rename`), the size
  accounting (`st_blocks`, `statvfs`), the LRU eviction (`opendir`/`fstatat`/atime). `struct disk_cache` itself is
  not even declared. The one Windows function that exists, `disk_cache_get_function_identifier`, hashes the
  *last-write time of the DLL file*.
- **`disk_cache.c`** (the OS-independent part) includes `ftw.h`, `sys/file.h`, `sys/mman.h`, `dirent.h`.
- **Other cache types**: the database cache (`mesa_cache_db.c`) is `#if DETECT_OS_WINDOWS == 0`, the single-file
  cache (`fossilize_db.c`) needs `HAVE_FLOCK`; both have Windows stubs that fail. So only the multi-file cache
  (Mesa's default) is worth porting.
- **No fork anywhere.** The async queue (`util_queue`, thread "disk$") is plain Mesa threading and already runs on
  Windows (the threaded context uses the same queue code).
- **Compression**: `subprojects/zlib.wrap` exists but only names a download (zlib 1.3.1 from zlib.net / wrapdb);
  nothing is vendored, there is no zstd wrap. Not needed: Mesa already has an uncompressed mode
  (`compression_disabled`, used by its unit tests), and `util_hash_crc32` has its own table when zlib is absent.
- **Panfrost** (`pan_disk_cache.c`) takes the driver id from the ELF build-id note
  (`build_id_find_nhdr_for_addr`), which does not exist for a PE image: with the option on, the file does not
  compile for Windows.
- **GL state tracker** (`st_shader_cache.c`, `compiler/glsl/shader_cache.cpp`): OS-independent; it uses whatever
  `screen->get_disk_shader_cache()` returns.

## 2. What was added (commit 0171)

| File | Change |
|---|---|
| `meson.build` | the option is allowed on Windows; no compression is a message, not an error |
| `src/util/disk_cache_os_portable.c` (new) | the multi-file cache for Windows on ~12 file primitives (`dc_*`): Win32 W calls on UTF-8 paths, and a POSIX twin of the primitives used only by the unit tests (`-DSB_DISK_CACHE_PORTABLE_TEST`) |
| `src/util/disk_cache_os.c`, `.h` | the POSIX file code is fenced with `#if !DISK_CACHE_OS_PORTABLE`; the two functions that define the bytes of an entry are shared by both backends; `disk_cache_pe_image_identifier`, `disk_cache_get_module_identifier` |
| `src/util/disk_cache.c` | POSIX includes fenced; one queue thread on Windows; entries uncompressed without zlib/zstd; the size is counted again at every start |
| `src/gallium/drivers/panfrost/pan_disk_cache.c`, `pan_context.[ch]`, `meson.build` | driver id for Windows; a cache that is off is dropped; log lines; BIFROST_MESA_DEBUG read before the key flags are taken |
| `src/gallium/targets/wgl/wgl.c` | flag file `glnocache` / `SB_GL_SHADER_CACHE=0` |
| `src/util/tests/cache_test.cpp` | 8 new tests (section 6) |

Same directory layout and same entry bytes as on Linux (`<dir>/index`, `<dir>/xx/<62 hex digits>`). What the
Windows backend does differently from Mesa's POSIX code, on purpose:

- **A bad entry is deleted when it is read** (cut short, damaged, zero-filled after a power loss), so the shader is
  stored again at the next compile. Mesa's POSIX code leaves it, and that entry stays a miss for ever.
- **The total size is counted again once per process**, on the cache's own thread, not on the game's. The counter
  lives in the mapped `index` file; a phone that is switched off or crashes loses its last updates, and those
  losses would add up to a cache larger than its limit says. Stale `.tmp` files (older than 1 h) go at the same
  time.
- **The time of last use is written by the driver** (at most once a day per entry), because Windows may not keep
  last-access times; times are compared at 100 ns, so entries of one start-up burst still have an order.
- **Write protocol**: the entry is written to `<name>.tmp` opened with share mode 0 (that is the lock: a second
  writer fails and leaves it to the first), then renamed without replace. Only complete files ever get the final
  name. The rename is retried 3 times at 20 ms on a sharing violation (a scanner holding the new file).
- A file counts as its size rounded up to 4 KiB.

## 3. The key, and what makes a cached shader invalid

Every key is `blake3(driver_keys_blob + what is being looked up)`. `driver_keys_blob` is: cache format version,
**driver id**, GPU name (`Mali-G72 MC… (Panfrost)`), pointer size, driver flags (`PAN_MESA_DEBUG` bits, which
include the `noafbc` flag file, and `BIFROST_MESA_DEBUG` bits). The blob is also written at the start of every
entry and compared on every read.

Two levels are cached, both in the same directory:

1. **Linked GLSL program** (state tracker): key adds attribute / frag-data bindings, transform feedback varyings,
   SSO, API and GLSL version, `MESA_EXTENSION_OVERRIDE`, the driconf hash and the hash of every shader source.
   Value: the linked program's metadata and NIR. A hit skips the GLSL compiler and linker.
2. **Compiled shader binary** (Panfrost): key is the hash of the serialized NIR plus the variant key
   (`panfrost_shader_key`). A hit skips the Bifrost compiler.

**Driver id on Windows.** `blake3` of
- the PE headers of the loaded `libgallium_wgl.dll`: link time stamp, SizeOfCode, SizeOfInitializedData,
  SizeOfImage, CheckSum, and name / virtual size / address / raw size of every section, and
- the string `26.2.6 (git-<commit>)`.

lld-link writes the link time into the time stamp at every link (this build: no `/Brepro`; the 0170 kit has
0x6AFB0303 = 13:20:03 UTC, its link time), so **every relink is a new id** even when no source changed, and a
change that is committed changes the string as well. Fields the loader rewrites in memory (ImageBase; for ARM64X
the machine and the entry point) are left out, so the id is the same in every process and equals the id computed
from the file.

Why not Mesa's existing Windows function (file write time): the kit is pushed again after every test cycle
(`gd-kit-push`), which would give the same binary a new file time and throw a good cache away each time. The file
time is still mixed in if a DLL ever has no usable link stamp (0 or 0xFFFFFFFF).

For this kit: `driver id d5d2e100048f45bd…74d4bb (link stamp 0x6afb2377, Mesa 26.2.6 (git-8909baf50e))`
(`gd-kit-ec-cache/driver-id.txt`, computed from the DLL file by `host-test/sbpeid.c`). The log line on the phone
should show exactly this; the link stamp can also be checked with `llvm-readobj --file-headers`.

**A corrupt or truncated file is rejected**: header blob compared, CRC32 over the payload, and the stored length
must equal the payload length (checked before the length is trusted). The index file is not trusted either: a
wrong size starts again as zeroes, its counter is recounted.

Holes I know of:
- A DLL **patched in place** without a relink keeps its id (lld writes no checksum). Use `glnocache` or delete the
  directory for such an experiment.
- Two links within the same second from different uncommitted sources with identical section sizes would share an
  id. Not a realistic sequence.
- Anything that changes generated code but is in neither the NIR, the variant key nor the driver flags would be
  served stale. I found none in the GL tree (audit of `getenv` / flag files in Panfrost and the Bifrost compiler);
  `BIFROST_MESA_DEBUG` was read too late to be in the key and is now read first (host run: a warm cache gives 100
  misses on the first `BIFROST_MESA_DEBUG=noopt` start). **A new A/B switch that changes shader code must go into
  `driver_flags` in `panfrost_disk_cache_init`.**
- A build with zlib would not read this build's entries (and the other way round); it rejects them, it does not
  misread them. The driver id differs anyway.

## 4. Location, size, switches, log

- **Directory**: `%LOCALAPPDATA%\Starbite\shader-cache\mesa_shader_cache` (read with the W call: a user name need
  not fit the ANSI code page). `MESA_SHADER_CACHE_DIR` (UTF-8) overrides it, as in Mesa. Missing parents are
  made. If the directory cannot be made, or `index` cannot be created or sized (read-only, disk full), there is no
  cache for that process and nothing else changes.
- **Size limit: 64 MiB** (`-Dshader-cache-max-size=64M` in `cache-ec-build.sh`; `MESA_SHADER_CACHE_MAX_SIZE`
  overrides). Mesa's mechanism: before an entry is written, while the total would pass the limit, the tenth of
  the entries that was unused longest in one random `xx` directory is deleted (up to 8 rounds). On top of the
  limit: the `index` file, a fixed 2 MiB. Entries of old driver builds are not deleted at once; they are the
  oldest, so they go first when the limit is reached. Host numbers: 604 entries for 300 programs = 3.0 MB
  (about 6.4 KiB per program in 4 KiB blocks), so one driver build of a game with ~100 programs is well under
  1 MiB.
- **Off**: flag file `C:\ProgramData\Starbite\glnocache`, or `SB_GL_SHADER_CACHE=0` (the environment wins, like
  the other GL switches), or Mesa's `MESA_SHADER_CACHE_DISABLE=true`. With the cache off the screen has no cache
  object at all, so the state tracker takes exactly the path of the builds without the option.
- **Log** (stderr, i.e. the umdlog file when `C:\ProgramData\Starbite\umdlog` exists):

  ```
  sbumd: shader cache: on, <dir>, limit 64 MiB, driver id <64 hex> (link stamp 0x6afb2377, Mesa 26.2.6 (git-8909baf50e)), flags 0x…; 3.2 s after process start
  sbumd: shader cache: so far, 5.1 s after process start: 96 lookups, 96 hits, 0 misses; compiled shaders: 64 hits (12 ms reading), 0 misses (0 ms compiling); 0 entries written
  sbumd: shader cache: first frame, 6.0 s after process start: …same fields…
  sbumd: shader cache: process end, 93.4 s after process start: …same fields…
  ```
  or, off: `sbumd: gl shader cache: off (C:\ProgramData\Starbite\glnocache)`, `sbumd: shader cache: off (switched
  off, or its directory cannot be used); 3.2 s after process start`, `sbumd: shader cache: off; first frame, 9.8 s
  after process start`.

  "lookups / hits / misses" are all lookups of the process (programs + binaries); "compiled shaders" are the
  binaries alone, with the time hits took to read and the time misses took in the Bifrost compiler. "so far"
  comes at every 32nd binary lookup; "first frame" at the first `PIPE_FLUSH_END_OF_FRAME` (the first SwapBuffers);
  "process end" from `atexit` (a killed process prints none). The numbers in the example are made up.

## 5. A bug in Mesa that the cache exposed (commit 0172)

`st_link_glsl_to_nir` returns early for a program that comes from the cache and never calls
`pipe_context::link_shader`. Panfrost learns the vertex shader's varying layout only there. So for a cached
program the fragment shader was compiled without it: the general form (other code than a start without the
cache runs), under another key than at the first start. Measured on the host with 48 programs: second start
96 hits / 48 misses, every fragment shader compiled again; only the third start was all hits. With 0172 the hook
is called on both paths: second start 144 hits / 0 misses, with and without `PAN_THREADED=1`. It is a separate
commit so it can be judged or dropped on its own; without it the cache still works, one start later and with the
general fragment shaders.

## 6. What was tested where

**Compiled only (ARM64EC, clang-cl / lld-link 20): never executed.**
The Win32 primitives in `disk_cache_os_portable.c` (`CreateFileW`, `FindFirstFileW`, `MoveFileExW`,
`CreateFileMappingW`, `SetFileTime`, `GetDiskFreeSpaceExW`, `GetEnvironmentVariableW`…),
`disk_cache_get_module_identifier` (`GetModuleHandleExW`), the Windows parts of `pan_disk_cache.c` (log lines,
`atexit`, process time) and the `wgl.c` switch. No compiler warning comes from a new line (the changed files
give three, all in code that was there before). Kit checks: 24 added imports, all `KERNEL32` file calls plus `rewind` and `_time64` from the
CRT; no new DLL (15 as before); 51 / 381 exports unchanged; opengl32.dll imports unchanged;
`libgallium_wgl-gd.dll` 19 518 464 bytes (0170: 19 493 376).

**Ran on the Linux host (aarch64 VM), release + NDEBUG, no zlib/zstd like the kit** (`build-cache-host`,
`-DSB_DISK_CACHE_PORTABLE_TEST`: the Windows backend on POSIX primitives, so everything above the primitives is
the code Windows runs):

- Mesa's own `Cache.*` unit tests + mine: 17 passed, 1 skipped (`DeleteOldCache`: a database-cache feature).
  Whole `util_tests`: 261 passed, 2 skipped.
- New tests: `SbDamagedEntryRejected` (9 kinds of damage: cut in half, last byte missing, empty, all zeroes, a
  bit flipped in payload / CRC / length / header, a byte appended: never handed out, file removed, stored again),
  `SbDriverIdChangeMisses` (two driver ids, GPU name, flags; even the other build's own key is refused),
  `SbUnusableDirectoryFailsSoft`, `SbSizeCountedAgainAtStart` (three wrong counters, stale and fresh `.tmp`),
  `SbLimitHolds` (600 entries against 1 MiB), `SbPeIdentity.HeaderFields` (each of 10 link-dependent fields
  changes the id; ImageBase, machine, entry point do not), `SbPeIdentity.NotAnImage` (every truncation and broken
  header refused), `SbPeIdentity.RealBuildsDiffer` (12 real DLLs from `~/sbmesa`: 12 different ids; the stamps of
  the 0170 kit and of this kit match llvm-readobj; `sbumd-arm64x.dll` and its x64 view `ecview.dll` give one id).
- Mutation check: with the CRC comparison disabled, `SbDamagedEntryRejected` fails (payload and CRC cases), so
  the test does test it.
- **GL end to end** (`sbcachegl.c`: GLES 3 program on Panfrost as a Mali-G72 over the no-op drm-shim; N programs
  compiled, linked, drawn; prints a digest of link status, info logs, attributes, uniforms with locations and
  the program binaries). `glscenarios.log`, all OK, the digest equals the cache-off run in every line:
  cold 44 hits / 100 misses, second start 144 / 0; all 100 entries damaged: no damaged entry is a hit, next start
  144 / 0 again; index file random bytes / deleted; directory read-only, below a file, missing; two processes on
  an empty cache at once; a process killed 10 times while filling the cache; a 256 KiB limit (43 entries,
  256 KiB on disk after three starts); a Mali-G52 and `PAN_MESA_DEBUG=nocrc` on the G72's cache find nothing of
  it.
- Time on the host (Apple M3, not the phone), 300 programs, start of process to all programs drawn:
  off 362 ms, first start with cache 280 ms, second and third start 63 ms; with `PAN_THREADED=1`: 304 / 263 / 59.
  Only the ratio means anything for the S9.

**Ran on the Linux host, Mesa as upstream builds it** (`build-cache-hostposix`: POSIX backend, zlib): the same
tests, 17 passed, 1 skipped (the recount test is Windows-backend only); whole `util_tests` 261 passed. So the
POSIX path is not broken by the fences.

Not tested anywhere: a full disk; a virus scanner holding files; a non-ASCII user name; the real game.

## 7. On-device A/B (for whoever has the phone; I touched no phone)

Install with `tools/win/gd-kit-push.sh build/win/gd-kit-ec-cache` and make sure `C:\ProgramData\Starbite\umdlog`
exists. Same game state for every start (e.g. start, wait for the menu, start one level that shows the effects,
quit normally so the "process end" line is written).

1. **Off**: `--flag glnocache`, start. Expect the two "off" lines and `off; first frame, T_off s`.
2. **First start with the cache**: `--no-flag glnocache`, start. Expect `on, C:\Users\…\AppData\Local\Starbite\
   shader-cache\mesa_shader_cache, limit 64 MiB, driver id d5d2e100…`, mostly misses, "entries written" > 0.
3. **Second start**: expect `first frame` with **0 misses** (if it has misses, note how many and whether "compiled
   shaders" has them: that is section 5 or a key that is not stable), and T_on well below T_off.
4. Third start: same as the second.

What to write down per start: the `on`/`off` line (time of screen creation), the `first frame` line (time,
lookups / hits / misses, ms compiling vs ms reading), the `process end` line (misses that came later = effects
that first appeared in play; with a warm cache it should show 0 ms compiling), and by eye whether the hitch when a
new effect first appears is gone. Also once: `dir /s` of the cache directory (count and bytes), and after
`gd-kit-push` of the **same** kit a start must still be all hits (the id does not depend on the file time),
while a rebuilt kit must start with all misses and a new driver id.

If anything looks wrong on the phone: the flag file `glnocache` gives the old behaviour without changing the kit.

## 8. sbumd (D3D10/11): finding and patch outline (not built)

**Finding.** d3d10umd hands Panfrost TGSI (`create_vs_state` / `create_fs_state` with `state.tokens`; the
dual-source variant hands NIR). `panfrost_create_shader_state` runs `tgsi_to_nir(tokens, screen, false)` and then
`panfrost_alloc_shader(nir)`, which hashes the serialized NIR for every shader whatever the frontend. Each
variant then goes through `panfrost_shader_get` → `panfrost_disk_cache_retrieve(screen->disk_cache, …)`. So
**Panfrost's own binary cache is hit by d3d10umd's shaders as they are, as soon as the screen has a cache**:
nothing in the frontend has to change for that. What a hit saves is the Bifrost compile only; DXBC → TGSI,
`tgsi_to_nir` and Panfrost's NIR passes at CSO creation still run at every start (there is no state-tracker
level, that is GL only).

**Does the patch apply?** `git apply --check` of 0171 against `src-int` (b8730f0): everything applies except the
`wgl.c` hunk, which sbumd does not need. The util files and `pan_disk_cache.c` are identical in both trees.

**Outline.**
1. Apply 0171 without `wgl.c` to `sb-int`; configure both halves (`build-int-a64`, `build-int-ec`) with
   `-Dshader-cache=enabled -Dshader-cache-max-size=64M` (zlib/zstd stay off).
2. **Key**: `src-int` has a switch the GL tree does not: `sb_no_idvs()` (`PAN_NO_IDVS` / flag file `noidvs`) sets
   `inputs.no_idvs` in `panfrost_shader_compile`. It must go into `driver_flags` in `panfrost_disk_cache_init`
   (one spare bit), or a cache filled with IDVS would serve IDVS shaders to a no-IDVS run. Repeat the audit for
   any later switch (`grep -n "getenv\|GetFileAttributesA\|malikm_switch" src/gallium/drivers/panfrost
   src/panfrost/compiler`).
3. **Identity**: one ARM64X DLL, two views. The PE identity is the same for both (tested on the host with
   `sbumd-arm64x.dll` and `ecview.dll`), so ARM64 and x64 processes would share entries. A shared entry carries
   `struct pan_shader_info` and the sysvals as raw bytes; the two halves should lay them out identically, but
   that is an assumption nobody has checked. Cheap and safe: mix one byte for the half into the id
   (`#ifdef _M_ARM64EC`), at the cost of two sets of entries. The x86 driver is a separate DLL (and pointer size)
   and gets its own entries anyway.
4. **Switch, in `d3d10_gdi.c` before `panfrost_drm_screen_create`** (the wgl.c block, with `malikm_switch`):
   I would start **opt-in** (`d3dcache` turns it on) and keep it off by name (`malikm_exe()`) for dwm.exe,
   LogonUI, winlogon and csrss until it has run in ordinary apps: a bad cached shader in DWM is a black screen,
   in an app it is one app. Later invert to `d3dnocache`.
5. **Many processes, several accounts.** `%LOCALAPPDATA%` gives each account its own directory (DWM runs as
   `Window Manager\DWM-n`, services as SYSTEM), which is what is wanted: a directory writable by every user and
   read by DWM would let any process plant GPU code for a more privileged one. Per process the cache costs one
   thread, a 2 MiB mapped view, and the recount at start (a directory walk); with every D3D process doing that,
   recount only when the last one is older than a day (a time stamp in the index file) before turning it on
   widely. Concurrent writers of one directory are handled by the write protocol (host-tested with two
   processes on the POSIX primitives; the Win32 share-mode version is compile-only).
6. **ARM64X specifics**: the new code is C with no dynamic initializers (one zero-initialised static struct, one
   `thread_local` integer; sbumd already has TLS data from `os_misc.c`). `link-arm64x.sh` needs no change.
7. **Log**: the same lines come out of `pan_disk_cache.c`; "first frame" needs a `PIPE_FLUSH_END_OF_FRAME` flush,
   which d3d10umd does not send, so call `panfrost_disk_cache_first_frame` from its present path if the line is
   wanted.
8. **Later, for more than the Bifrost compile**: `tgsi_to_nir(…, allow_disk_cache = true)` already knows how to
   cache the NIR by the TGSI tokens in the same disk cache (`tgsi_to_nir.c`, `load_nir_from_disk_cache`). It
   would skip the TGSI → NIR translation; Panfrost's CSO-time passes run after it either way. Measure first
   ("ms compiling" in the log line says how much the first level already saves).

## 9. Left in the VM

`~/sbmesa/cache-ec-build.sh` (the kit: `tc-ec-build.sh` with the cache options, `-j4`, `LINK=/MAP` for the
maps), `~/sbmesa/cache-host-build.sh portable|posix`, `~/sbmesa/cache-work/` (test program, scripts, logs, scratch
cache directories; the patch exports there are `patches-final2`, the older `patches*` directories are stale
copies I could not delete from here). No existing tree or build directory was touched; `git apply --check` on
`src-int` was read-only.

The kit in `build/win/gd-kit-ec-cache/` is the reference (its SHA256SUMS matched the VM's files when copied).
`build-cache-ec` will relink at its next run (I recompiled a few objects after the copy to list their warnings),
and **any relink is a new link stamp and so a new driver id**: copy DLLs, maps and `driver-id.txt` together.
