// ARM64X stretch only: lld-link 20.1.2 /machine:arm64x does not apply the /alternatename directives of
// ARM64EC objects (libcmt, libcpmt) to the EC symbol table. This object restates them:
// functions as ARM64EC tail jumps to the alternate, data as weak aliases.
.text
.p2align 3
.word 0
.globl "#__acrt_initialize"
"#__acrt_initialize":
  b "#__scrt_stub_for_acrt_initialize"
.weak_anti_dep __acrt_initialize
.set __acrt_initialize, "#__acrt_initialize"
.p2align 3
.word 0
.globl "#__acrt_thread_attach"
"#__acrt_thread_attach":
  b "#__scrt_stub_for_acrt_thread_attach"
.weak_anti_dep __acrt_thread_attach
.set __acrt_thread_attach, "#__acrt_thread_attach"
.p2align 3
.word 0
.globl "#__acrt_thread_detach"
"#__acrt_thread_detach":
  b "#__scrt_stub_for_acrt_thread_detach"
.weak_anti_dep __acrt_thread_detach
.set __acrt_thread_detach, "#__acrt_thread_detach"
.p2align 3
.word 0
.globl "#__acrt_uninitialize"
"#__acrt_uninitialize":
  b "#__scrt_stub_for_acrt_uninitialize"
.weak_anti_dep __acrt_uninitialize
.set __acrt_uninitialize, "#__acrt_uninitialize"
.p2align 3
.word 0
.globl "#__acrt_uninitialize_critical"
"#__acrt_uninitialize_critical":
  b "#__scrt_stub_for_acrt_uninitialize_critical"
.weak_anti_dep __acrt_uninitialize_critical
.set __acrt_uninitialize_critical, "#__acrt_uninitialize_critical"
.p2align 3
.word 0
.globl "#__scrt_exe_initialize_mta"
"#__scrt_exe_initialize_mta":
  b "#__scrt_stub_for_initialize_mta"
.weak_anti_dep __scrt_exe_initialize_mta
.set __scrt_exe_initialize_mta, "#__scrt_exe_initialize_mta"
.p2align 3
.word 0
.globl "#_is_c_termination_complete"
"#_is_c_termination_complete":
  b "#__scrt_stub_for_is_c_termination_complete"
.weak_anti_dep _is_c_termination_complete
.set _is_c_termination_complete, "#_is_c_termination_complete"
.weak __volatile_metadata
.set __volatile_metadata, __AbsoluteZero
