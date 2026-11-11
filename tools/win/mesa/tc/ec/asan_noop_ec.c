/* lld-link 20.1.2 /machine:arm64ec: libcpmt.lib's ARM64EC objects reach
 * __sanitizer_annotate_contiguous_container through
 *   /alternatename:#X=#X_default  (X_default is the no-op in libcpmt's asan_noop.obj)
 * but lld does not apply an alternate name to the weak anti-dependency alias chain
 * X -> #X -> #X$exit_thunk that MSVC emits for ARM64EC, and reports X undefined.
 * This object gives the same no-op as libcpmt's default (we never build with ASan). */
void __cdecl __sanitizer_annotate_contiguous_container(const void *beg, const void *end,
                                                       const void *old_mid, const void *new_mid)
{
   (void)beg; (void)end; (void)old_mid; (void)new_mid;
}
