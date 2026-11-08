# Upstream: wddm-mali-g52's maliwddm

`maliwddm/` (adapter.c, command.c, display.c, memory.c, maliwddm.h, maliwddm.inf, maliwddm_abi.h) and
`common/maligpu.{h,c}` were copied verbatim from github.com/gahingwoo/wddm-mali-g52 at commit e8bcd36
(drivers/maliwddm, drivers/common, include/maliwddm_abi.h), GPL-2.0 (`tools/win/mesa/g52/LICENSE`).
It is a WDDM 1.3 full driver for a Mali-G52 that ran Mesa's Panfrost as the hardware UMD on its board
(their M5.2 step c2, 2026-10-03). This tree's commits after the copy are the Galaxy S9 port:
docs/WIN11-GPU.md section 6.8.
