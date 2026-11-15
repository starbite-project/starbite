/* Injected into a Wine D3D test file right after its queue_test() (renamed queue_test_impl by build.sh): each test
 * function gets its name, so one process can list them (SBWT_LIST=1) or run exactly one (SBWT_ONLY=name). */
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
