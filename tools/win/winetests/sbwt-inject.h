/* Injected into a Wine D3D test file right after its queue_test() (renamed queue_test_impl by build.sh): each test
 * function gets its name, so one process can list them (SBWT_LIST=1) or run exactly one (SBWT_ONLY=name). */
/* Where a crash is: the module, its base and the offset of the faulting address (the suite's own handler prints only
 * the address, and a driver DLL's base differs per boot). First chance, so at most 4 lines per process. */
static LONG CALLBACK sb_wt_veh(EXCEPTION_POINTERS *p)
{
    static LONG said;
    DWORD code = p->ExceptionRecord->ExceptionCode;
    if ((code == 0xc0000005 || code == 0xc000001d || code == 0xc0000094 || code == 0xc00000fd) &&
            InterlockedIncrement(&said) <= 4)
    {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        void *addr = p->ExceptionRecord->ExceptionAddress;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (const char *)addr, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        printf("SBWT-CRASH code %08lx at %p = %s + 0x%llx", code, addr, name,
                mod ? (unsigned long long)((char *)addr - (char *)mod) : 0ull);
        if (code == 0xc0000005 && p->ExceptionRecord->NumberParameters >= 2)
            printf(" (%s address %p)", p->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read of",
                    (void *)p->ExceptionRecord->ExceptionInformation[1]);
#if defined(_M_ARM64)
        {
            HMODULE lm = NULL;
            void *lr = (void *)p->ContextRecord->Lr;
            char ln[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (const char *)lr, &lm) && lm)
                GetModuleFileNameA(lm, ln, sizeof(ln));
            printf("; called from %s + 0x%llx", ln, lm ? (unsigned long long)((char *)lr - (char *)lm) : 0ull);
        }
#endif
        printf("\n");
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void sb_wt_init(const char **only, int *list)
{
    static int inited, l;
    static const char *o;
    if (!inited)
    {
        const char *e = getenv("SBWT_LIST");
        l = e && *e == '1';
        o = getenv("SBWT_ONLY");
        if (o && !*o)
            o = NULL;
        inited = 1;
        if (!l)
            AddVectoredExceptionHandler(1, sb_wt_veh);
    }
    *only = o;
    *list = l;
}

static int sb_wt_skip(const char *name)
{
    const char *only;
    int list;
    sb_wt_init(&only, &list);
    if (list)
    {
        printf("SBWT-TEST %s\n", name);
        fflush(stdout);
        return 1;
    }
    return only && strcmp(only, name);
}

static void sb_wt_queue(void (*t)(void), const char *name)
{
    if (!sb_wt_skip(name))
        queue_test_impl(t);
}

static void sb_wt_direct(void (*t)(void), const char *name)
{
    if (!sb_wt_skip(name))
        t();
}
#define queue_test(t) sb_wt_queue(t, #t)
