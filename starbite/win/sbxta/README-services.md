# sbxta: per-boot service list (opt-in)

sbxta.sys (v2) can turn named services off on the big-core (Exynos M3) row and back on on the A55 row, the same way it
already does for XtaCache. It is OFF until one registry value is set; with the value absent or empty sbxta does exactly
what v1 did (XtaCache only) and only writes one extra log value, `State\Svc`.

## The value

| | |
|---|---|
| Key (online) | `HKLM\SYSTEM\CurrentControlSet\Services\sbxta\Parameters` |
| Key (offline SYSTEM hive) | `ControlSet001\Services\sbxta\Parameters` (the control set `Select\Current` names) |
| Name | `DisableOnM3Services` |
| Type | `REG_MULTI_SZ`, one service name per string |
| Suggested | `WpnUserService` `DiagTrack` `OneSyncSvc` `UsoSvc` `wuauserv` |

Online (either row, admin prompt):

    reg add HKLM\SYSTEM\CurrentControlSet\Services\sbxta\Parameters /v DisableOnM3Services /t REG_MULTI_SZ /d "WpnUserService\0DiagTrack\0OneSyncSvc\0UsoSvc\0wuauserv" /f

Offline (hivexsh, type 7 = MULTI_SZ, UTF-16LE, each string NUL-terminated plus one final NUL):

    python3 -c "print('hex:7:' + ','.join('%02x' % b for b in ('\0'.join(['WpnUserService','DiagTrack','OneSyncSvc','UsoSvc','wuauserv']) + '\0\0').encode('utf-16-le')))"

then in hivexsh: `cd \ControlSet001\Services\sbxta`, `add Parameters` (if missing), `cd Parameters`, `setval 1`,
`DisableOnM3Services`, the `hex:7:...` line, `commit`.

Limits: the first 16 names are used (more = logged "list cut"); names that are empty, longer than 80 characters or
contain `\` or `/` are ignored; the value must be under 4 KB, otherwise it counts as absent.

## What each row does (at system-start time, before the Service Control Manager reads service configs)

**Big-core (M3) row**, for each listed name that exists under `Services`:
- `Start` already 4: nothing (and nothing saved, so it is never "restored" later).
- otherwise: the current `Start` goes to `Services\sbxta\Saved\<name>` (REG_DWORD) unless a saved value already
  exists (a second M3 boot in a row never overwrites the real original with 4), then `Start` = 4. If the save fails
  the service is left alone.
- per-user templates (`Type` has bit 0x40: WpnUserService, OneSyncSvc): every existing instance key
  `<name>_<suffix>` (e.g. `WpnUserService_3a5f1`) is handled the same way, each saved under its own name. Windows
  creates or refreshes instances from the template at logon, so the template's `Start` = 4 is the part that matters;
  doing the existing instances too covers the ones already there.
- names that do not exist: logged "missing", nothing else.

**A55 row**: for every value in `Services\sbxta\Saved` (the list is not read at all):
- service `Start` is 4: set back to the saved value;
- service `Start` is not 4 (someone re-enabled it) or the key is gone: left as it is;
- in every case the saved value is deleted afterwards.

**Any other CPU**: nothing.

sbxta never creates or deletes a service key and never touches a service that is neither listed nor in `Saved`.

## Undo

Delete `DisableOnM3Services` (or empty it), then boot the **A55 row once**: it restores everything in `Saved` whether
or not the list still exists. Booting the M3 row first is harmless (with no list nothing is disabled or saved).
To keep the list but re-enable one service on the M3 row, remove its name from the list and boot the A55 row once.

## Log

`Services\sbxta\State\Svc` (REG_SZ), e.g.
`M3: WpnUserService 3->4 ok; WpnUserService_3a5f1 2->4 ok; DiagTrack 2->4 ok; OneSyncSvc 3->4 ok; UsoSvc 2->4 ok; wuauserv 3->4 ok; `
or `A55: WpnUserService 4->3 ok; ...`. `State\Build` ends in ` v2`. The other State values keep their v1 meaning.

## Risk notes

- **Windows Update (wuauserv, UsoSvc) is off on the M3 row only.** No updates, no Store installs there; the A55 row
  turns them back on every boot, so update from the A55 row. Do not run Windows Update on the M3 row by hand.
- **Order of rows matters for the saved value**: if Windows itself changes a listed service's Start on the A55 row
  (an update switches wuauserv 3 <-> 2, say), the next M3 boot saves that new value; that is intended.
- **Per-user instances** (`WpnUserService_*`, `OneSyncSvc_*`): new instances made at a later logon on the M3 row
  follow the template (Start 4). On the A55 row the template and the instances that were saved go back; instances
  created while the template was 4 were never saved, so sbxta leaves them as Windows made them. Whether Windows
  refreshes an existing instance's Start from the template at logon is not verified here: if a `<name>_<suffix>`
  instance stays at 4 on the A55 row, the next M3 boot will not save it (already 4); set it back by hand (3 for
  WpnUserService_* / OneSyncSvc_*) or delete that instance key so Windows recreates it.
- Notifications (WpnUserService), mail/calendar sync (OneSyncSvc) and telemetry (DiagTrack) do not work on the M3
  row while listed.
- A service listed by mistake that the system needs to boot (a driver, RpcSs, ...) would be disabled on the next M3
  boot; booting the A55 row restores it. Only list the services above unless there is a reason.
