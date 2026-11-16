# Shader disk cache on Windows (gap-list item 9)

**Update 2026-11-16 20:35 EET: the kit ran on the phone (T89) and the cache stayed off. Section 10 at the end says
why, what changed (commit 0173, new kit) and what the log prints now; where it differs from sections 2-7 (log
lines, the driver id of the kit, `MESA_SHADER_CACHE_DIR`), section 10 is right.**

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

## 10. T89 on the phone and what changed

2026-11-16 20:35 EET. Kit rebuilt and delivered to the same place; commit 0173 (f2e5052) on `sb-cache`; patches
re-exported (three files now). Still nothing of mine has run on Windows: what follows is the phone's own log read
against the source, and host tests of the fix.

### What failed (from the logs in `build/win/results-t89/ours/pre/umdlog/`)

Runs A and B (pid 2884, 6472) have one line that the summary of T89 did not quote, directly before the "off" line:

```
Warning: Mesa single file cache selected but Mesa wasn't built with single cache file support. Shader cache will be disabled!
sbumd: shader cache: off (switched off, or its directory cannot be used); 20.9 s after process start
```

That text exists once in Mesa: `foz_prepare`, the Windows stub of the single-file (Fossilize) cache
(`src/util/fossilize_db.c:765`). It is reached only when the cache type is "single file", and the type comes from
the environment: `debug_get_bool_option("MESA_DISK_CACHE_SINGLE_FILE", false)` in `disk_cache_create`
(`src/util/disk_cache.c:255`), then `disk_cache_load_cache_index_foz` (`disk_cache.c:164`) fails and the cache
object comes back unusable. Run C (flag file) has no such line because a switched-off cache never gets as far as
its type. So:

- **Certain** (it is in the log): the game's process had `MESA_DISK_CACHE_SINGLE_FILE` set to true, Mesa picked a
  cache type that does not exist on Windows, and my code let that end in "no cache" with a message that named
  neither the variable nor the step. My directory, index and identity code was never reached (the identity was:
  it passed).
- **Inferred, not seen**: that Steam put the variable there. Steam hands its games a block of shader-cache
  variables on Linux (`MESA_DISK_CACHE_SINGLE_FILE=1` among them, with a cache directory and a size); the game
  was started by Steam. The next run prints the variables it finds, which settles it.
- The only other way to that warning is `MESA_DISK_CACHE_COMBINE_RW_WITH_RO_FOZ` (`disk_cache.c:329`), which
  makes a second, read-only cache of that type; then the main cache would have had to fail as well, silently.
  Possible, less likely; the new log distinguishes the two.
- **Not known**: whether the steps after the type work on Windows (directory, index file, mapping, writing
  entries). T89 never got there. The first run with this kit is the first test of them.

What T89 did confirm: stderr reaches the umdlog, the flag file works, the first-frame hook fires (21.5 s / 19.2 s
without cache, 13.8 s in run C), the PE identity can be computed in the x64 process.

### Every step of the enable path that could fail without saying why (T89 tree, 8909baf)

| # | Step | Where | In T89 |
|---|---|---|---|
| 1 | cache type from the environment: single file / database are failing stubs on Windows | `disk_cache.c:255`, `:257` → `:164` → `fossilize_db.c:763` | **this one** |
| 2 | `MESA_DISK_CACHE_MULTI_FILE=false`: `disk_cache_create` returns NULL | `disk_cache.c:265` | silent |
| 3 | read-only Fossilize cache next to the main one | `disk_cache.c:329` | prints the same warning |
| 4 | `MESA_SHADER_CACHE_DISABLE`, `MESA_GLSL_CACHE_DISABLE`, `MESA_GLSL_DISABLE_IO_OPT` | `disk_cache_os.c:1238`, `:1252` | silent |
| 5 | `MESA_SHADER_CACHE_DIR` from the environment decides the directory (read as ANSI, used as UTF-8) | `disk_cache_os_portable.c:1040` | silent; a launcher's directory would have been used |
| 6 | `MESA_SHADER_CACHE_MAX_SIZE` from the environment replaces the 64 MiB | `disk_cache.c:271` | silent; not a failure, but a launcher's "5G" would have lifted the bound |
| 7 | `%LOCALAPPDATA%` not set or 260+ characters | `disk_cache_os_portable.c:446`, `:449` | silent, no other place tried |
| 8 | directory cannot be made | `disk_cache_os_portable.c:696`, `:1055` | printed, without the error code |
| 9 | index file: `CreateFileW`, `SetEndOfFile`, `CreateFileMappingW`, `MapViewOfFile` | `disk_cache_os_portable.c:386`, `:406`, `:411`, `:413` | silent |
| 10 | cache thread cannot start | `disk_cache.c:189` | silent |
| 11 | driver identity | `disk_cache_os.c:165`, `pan_disk_cache.c:295` | printed without the error; passed on the phone |
| 12 | one message for every cause | `pan_disk_cache.c:342` | "switched off, or its directory cannot be used" |
| 13 | the GL switch said something only when off | `wgl.c:118`-`124` | no line in runs A, B |

Not candidates: `__normal_user()` is always true on Windows; `-Dshader-cache-default` is on (no
`SHADER_CACHE_DISABLE_BY_DEFAULT` in the build).

### What changed (0173)

1. **The cache type** (`disk_cache.c`): where the multi-file cache is the only one
   (`disk_cache_os_only_multi_file()`: Windows), `disk_cache_create` takes it whatever the environment asks for,
   notes what it ignored, and makes no read-only Fossilize cache. `MESA_DISK_CACHE_MULTI_FILE=false` still means
   no cache, and says so.
2. **Launcher variables** (`wgl.c`): the Mesa cache variables found in the environment are listed in the decision
   line. `MESA_SHADER_CACHE_DIR`, `MESA_GLSL_CACHE_DIR`, `MESA_SHADER_CACHE_MAX_SIZE`, `MESA_GLSL_CACHE_MAX_SIZE`
   are removed from the process before the cache is made, so the directory and the 64 MiB are ours. For tests:
   `SB_GL_SHADER_CACHE_DIR`, `SB_GL_SHADER_CACHE_MAX`. `MESA_SHADER_CACHE_DISABLE=true` is still honoured ("off"
   counts whoever says it). The flag file no longer goes through the environment: it is a reason handed to
   Panfrost, which makes no cache at all.
3. **Reasons** (`disk_cache_os_note`, util): every step in the table writes which step, which path and the
   system error; Panfrost prints them.
4. **Directory stand-ins** (`dc_default_root`, `disk_cache_generate_cache_dir`): tried in order, and a place is
   taken only when a file can be created in it (the index file, made right there):
   `%LOCALAPPDATA%\Starbite\shader-cache`, `%USERPROFILE%\AppData\Local\Starbite\shader-cache`,
   `C:\ProgramData\Starbite\shader-cache-<user name>` (`GetUserNameW`, else `%USERNAME%`, else `user`; characters
   that cannot be in a file name become `_`). Then `\mesa_shader_cache` as before. The third place is writable by
   every account of the machine: fine for one user on a phone, not something to rely on for sbumd (section 8.5).
   A directory given by name (`SB_GL_SHADER_CACHE_DIR`) has no stand-in.
5. **One decision line per process, always**, from `pan_disk_cache.c`; every cache line now starts
   `sbumd: gl shader cache:` (was `sbumd: shader cache:`; `grep "shader cache:"` finds both).
6. `MapViewOfFile` asks for `FILE_MAP_WRITE` instead of `FILE_MAP_ALL_ACCESS` (the same read-write view, the
   plainer request).

### What the next run prints

Exactly one of these, once, when the screen is made:

```
sbumd: gl shader cache: on, dir C:\Users\<u>\AppData\Local\Starbite\shader-cache\mesa_shader_cache (from %LOCALAPPDATA%), limit 64 MiB, driver id 78f187c7…795c46 (link stamp 0x6afb4c74, Mesa 26.2.6 (git-f2e5052c2c)), flags 0x…; passed over: MESA_DISK_CACHE_SINGLE_FILE in the environment ignored; environment at start: MESA_DISK_CACHE_SINGLE_FILE=1 … (type, directory and size from it are not used); 20.9 s after process start
sbumd: gl shader cache: OFF: switched off by C:\ProgramData\Starbite\glnocache; …
sbumd: gl shader cache: OFF: switched off by SB_GL_SHADER_CACHE=0; …
sbumd: gl shader cache: OFF: switched off by MESA_SHADER_CACHE_DISABLE in the environment; …
sbumd: gl shader cache: OFF: no build identity for this module (its PE headers cannot be read, error N); …
sbumd: gl shader cache: OFF: <one note per place tried, joined by "; ">; …
```

`passed over:` and `environment at start:` appear only when there is something to say. With a stand-in directory
the "on" line reads e.g. `dir C:\ProgramData\Starbite\shader-cache-<u>\mesa_shader_cache (from
C:\ProgramData\Starbite) … passed over: %LOCALAPPDATA% cannot be read (error 203); cannot create
C:\Users\<u>\AppData\Local\Starbite (error 5)`. The notes a step can write:

| Note | Step |
|---|---|
| `%LOCALAPPDATA% cannot be read (error N)`, same for `%USERPROFILE%` | variable not set (203) or too long (111) |
| `cannot create <dir> (error N)` | the first directory of the chain that `CreateDirectoryW` refused (5 = access denied, 267 = a file has the name) |
| `cannot create a file in <dir> (error N)` | the directory is there but the index file cannot be created or opened for writing |
| `cannot open <index> (CreateFile error N)`, `cannot make <index> 2097160 bytes long (… SetEndOfFile error N)` (112 = disk full), `cannot map <index> (CreateFileMapping error N)` / `(MapViewOfFile error N)` | the index file |
| `the thread that writes the cache cannot be started` | the queue |
| `switched off by … in the environment`, `MESA_DISK_CACHE_… in the environment ignored` | switches |
| `the cache came back unusable and no step said why` | a hole in the above; should never appear |

The other lines are as in section 4 with the new prefix (`so far`, `first frame`, `process end`,
`off; first frame`). For the kit as delivered: `gd-kit-ec-cache/driver-id.txt`
(`driver id 78f187c7160f523e…28795c46`, link stamp `0x6afb4c74`).

### Tests after the change

- Compiled only (ARM64EC), as before, now including `GetUserNameW` (the one new import, from `ADVAPI32.dll`,
  which the DLL already used: 286 imports from 15 DLLs, exports unchanged), the wgl hook and the decision line. No
  new warning.
- Host, Windows backend on POSIX primitives: `Cache.*` + `SbPeIdentity.*` 20 passed, 1 skipped; whole
  `util_tests` 264 passed. Host, Mesa's POSIX backend with zlib: 18 passed, 3 skipped; whole `util_tests` 262
  passed.
- New unit tests: `SbLauncherEnvironmentDoesNotSwitchOff` (the Steam variables with the "only multi-file" rule
  on: cache on, multi-file, a hit at the second start, the note is there; database type the same;
  `MULTI_FILE=false` is off with its reason; without the rule the environment is obeyed as upstream does),
  `SbOffSaysWhy` (switch; directory below a file → `cannot create … (error 20)`; read-only → `cannot create a
  file in … (error 13)`; index name taken by a directory → `(error 21)`; a working cache has no notes),
  `SbDirectoryStandIns` (first place cannot be made, second cannot be written, third taken, with both reasons
  and the source; all three unusable → off with three reasons; a named directory has no stand-in).
- GL scenarios 9 and 10 added to `host-test/glscenarios.sh` (the Steam environment; the stand-in chain): second
  start 144 hits / 0 misses in both, digest equal to the cache-off run; scenarios 1-8 unchanged, all OK.

What the host cannot show: that `only multi-file` is what Windows needs beyond T89's log line (the stub itself
is Windows code), and the Win32 calls of the stand-in chain.
