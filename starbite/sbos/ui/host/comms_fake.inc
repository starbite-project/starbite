/* SPDX-License-Identifier: MIT
 * helper I: a pretend network for calls and SMS in the previews (the platform side of shell.h's sh_dial ... sh_sms_*;
 * the phone's is kern_next.c on the modem session's kmodem_call_* / kmodem_sms_*). Included by host/main.c.
 *   - it works only while the pretend Cellular is on (sh_cellular(1), or the state comms=net:1);
 *   - a dial rings (ALERTING) after 0.8 s and is answered 2 s after the dial; the seconds count while connected;
 *     a hang-up ends it (ENDING, ENDED 0.3 s later, IDLE 1.3 s later); audio_ok follows comms=audio:v (0 by default:
 *     "No audio yet", as the phone is tonight);
 *   - "ring MS" in a film (or comms=incoming) is an incoming call from a made-up number MS ms later; Decline ends it;
 *   - a sent SMS is "sending" for 1 s, then "sent"; "sms TEXT" in a film receives one from a made-up number;
 *   - emergency numbers are refused with the kernel's words, as kmodem_call_dial does.
 * Every number here is a made-up one (+1 555 01xx: reserved for fiction). Nothing logs a number or a text. */

static sh_call_t FC;                     /* the pretend call */
static float fc_now, fc_t0, fc_end_t, fc_ring_at = -1, fc_conn_t;
static int fc_audio_ok;                  /* fc_net: host/main.c (the pretend Cellular) */
static struct { sh_sms_t m[SH_SMS_MAX]; float sent_at[SH_SMS_MAX]; int n, ids; unsigned gen; } FM;

static void fc_push(void) { sh_call(&FC); }
static void fm_bump(void) { FM.gen++; sh_sms_changed(FM.gen); }
static void fc_why(char *why, const char *s) { if (why) snprintf(why, SH_WHY, "%s", s); }
/* the kernel's rule: these (and any behind +CC / 00CC) are refused before anything is sent */
static int fc_emergency(const char *n)
{
    static const char *const E[] = { "112", "911", "999", "000", "110", "118", "119", "08" };
    char d[32];
    int k = 0;
    for (const char *p = n; *p && k < 31; p++) if ((*p >= '0' && *p <= '9') || *p == '+') d[k++] = *p;
    d[k] = 0;
    for (int s = -1; s < 3; s++) {          /* no prefix, then a country code of 1..3 digits */
        const char *r = d;
        if (s >= 0) {
            if (d[0] == '+') r = d + 1 + s + 1;
            else if (d[0] == '0' && d[1] == '0') r = d + 2 + s + 1;
            else break;
            if ((int)strlen(d) < (int)(r - d)) break;
        }
        for (unsigned i = 0; i < sizeof E / sizeof E[0]; i++) if (!strcmp(r, E[i])) return 1;
    }
    return 0;
}
static int fc_digits(const char *n) { int c = 0; for (; *n; n++) c += *n >= '0' && *n <= '9'; return c; }
static int fc_refuse(const char *num, char *why)
{
    if (!fc_net) { fc_why(why, "No network: Cellular is off or not registered"); return -11; }
    if (fc_emergency(num)) { fc_why(why, "Emergency calls are not supported on Starbite OS: use another phone"); return -1; }
    if (fc_digits(num) < 7) { fc_why(why, "Enter the whole number (at least 7 digits)"); return -1; }
    return 0;
}

int sh_comms_ok(void) { return 1; }
int sh_dial(const char *number, char *why)
{
    int r = fc_refuse(number, why);
    fprintf(stderr, "comms: dial (the number is not logged): %s\n", r ? "refused" : "dialling");
    if (r) return r;
    if (FC.state != SH_CALL_IDLE && FC.state != SH_CALL_ENDED) { fc_why(why, "A call is already on"); return -11; }
    memset(&FC, 0, sizeof FC);
    FC.state = SH_CALL_DIALING; FC.outgoing = 1; FC.audio = SH_AUDIO_EARPIECE; FC.audio_ok = fc_audio_ok;
    snprintf(FC.number, sizeof FC.number, "%s", number);
    fc_t0 = fc_now;
    fc_push();
    return 0;
}
int sh_answer(char *why)
{
    fprintf(stderr, "comms: answer\n");
    if (FC.state != SH_CALL_INCOMING) { fc_why(why, "There is no call to answer"); return -1; }
    FC.state = SH_CALL_ACTIVE; FC.secs = 0; fc_conn_t = fc_now;
    fc_push();
    return 0;
}
int sh_hangup(void)
{
    fprintf(stderr, "comms: hang up\n");
    if (FC.state == SH_CALL_IDLE || FC.state == SH_CALL_ENDED || FC.state == SH_CALL_ENDING) return -1;
    snprintf(FC.why, sizeof FC.why, "%s", FC.state == SH_CALL_INCOMING ? "Declined" : "");
    FC.state = SH_CALL_ENDING; fc_end_t = fc_now;
    fc_push();
    return 0;
}
int sh_route(int audio, char *why)
{
    fprintf(stderr, "comms: route %s\n", audio == SH_AUDIO_SPEAKER ? "speaker" : "earpiece");
    if (FC.state != SH_CALL_ACTIVE && FC.state != SH_CALL_DIALING && FC.state != SH_CALL_ALERTING) { fc_why(why, "No call"); return -1; }
    FC.audio = audio;
    fc_push();
    return 0;
}
int sh_mute(int on, char *why)
{
    fprintf(stderr, "comms: mute %s\n", on ? "on" : "off");
    if (FC.state != SH_CALL_ACTIVE && FC.state != SH_CALL_DIALING && FC.state != SH_CALL_ALERTING) { fc_why(why, "No call"); return -1; }
    FC.muted = !!on;
    fc_push();
    return 0;
}
int sh_dtmf(char d)
{
    (void)d;
    fprintf(stderr, "comms: key tone (the key is not logged)\n");
    return FC.state == SH_CALL_ACTIVE ? 0 : -1;
}
int sh_sms_send(const char *number, const char *utf8, char *why)
{
    int r = fc_refuse(number, why);
    fprintf(stderr, "comms: SMS send (number and text not logged): %s\n", r ? "refused" : "queued");
    if (r) return r;
    if (FM.n == SH_SMS_MAX) { memmove(&FM.m[0], &FM.m[1], sizeof FM.m[0] * (SH_SMS_MAX - 1)); memmove(&FM.sent_at[0], &FM.sent_at[1], sizeof(float) * (SH_SMS_MAX - 1)); FM.n--; }
    sh_sms_t *m = &FM.m[FM.n];
    memset(m, 0, sizeof *m);
    m->id = ++FM.ids; m->incoming = 0; m->state = SH_SMS_SENDING; m->utc = 1794500000LL + (long long)(fc_now / 1000);
    snprintf(m->number, sizeof m->number, "%s", number);
    snprintf(m->text, sizeof m->text, "%s", utf8);
    FM.sent_at[FM.n] = fc_now;
    FM.n++;
    fm_bump();
    return 0;
}
int sh_sms_count(void) { return FM.n; }
int sh_sms_get(int i, sh_sms_t *m)
{
    if (i < 0 || i >= FM.n) return -1;
    *m = FM.m[i];
    return 0;
}

/* a received message (the film's "sms TEXT", the states' inbox) */
static void fm_receive(const char *number, const char *text, int quiet)
{
    if (FM.n == SH_SMS_MAX) { memmove(&FM.m[0], &FM.m[1], sizeof FM.m[0] * (SH_SMS_MAX - 1)); memmove(&FM.sent_at[0], &FM.sent_at[1], sizeof(float) * (SH_SMS_MAX - 1)); FM.n--; }
    sh_sms_t *m = &FM.m[FM.n];
    memset(m, 0, sizeof *m);
    m->id = ++FM.ids; m->incoming = 1; m->state = SH_SMS_RECEIVED; m->utc = 1794500000LL + (long long)(fc_now / 1000);
    snprintf(m->number, sizeof m->number, "%s", number);
    snprintf(m->text, sizeof m->text, "%s", text);
    FM.sent_at[FM.n] = -1;
    FM.n++;
    if (!quiet) fm_bump();
}
static void fm_inbox(void)
{
    if (FM.n) return;
    fm_receive("+15550100", "Are you bringing the phone tomorrow?", 1);
    fm_receive("+15550142", "The parcel is at the pick-up point. Code 4471.", 1);
    fm_receive("+15550100", "Yes, after lunch", 1);              /* one we sent (the stills' comms=failed marks it) */
    FM.m[FM.n - 1].incoming = 0;
    FM.m[FM.n - 1].state = SH_SMS_SENT;
    fm_receive("+15550100", "I can bring the cable if you need it", 1);
    fm_bump();
}
static void fc_incoming(void)
{
    if (FC.state != SH_CALL_IDLE && FC.state != SH_CALL_ENDED) return;
    memset(&FC, 0, sizeof FC);
    FC.state = SH_CALL_INCOMING; FC.audio = SH_AUDIO_EARPIECE; FC.audio_ok = fc_audio_ok;
    snprintf(FC.number, sizeof FC.number, "+15550123");
    fc_push();
}

/* every film frame (the platform's clock): the pretend network's next step */
static void comms_fake_poll(float t)
{
    fc_now = t;
    if (fc_ring_at >= 0 && t >= fc_ring_at) { fc_ring_at = -1; fc_incoming(); }
    switch (FC.state) {
    case SH_CALL_DIALING:
        if (t - fc_t0 >= 800) { FC.state = SH_CALL_ALERTING; fc_push(); }
        break;
    case SH_CALL_ALERTING:
        if (t - fc_t0 >= 2000) { FC.state = SH_CALL_ACTIVE; FC.secs = 0; fc_conn_t = t; fc_push(); }
        break;
    case SH_CALL_ACTIVE: {
        int s = (int)((t - fc_conn_t) / 1000);
        if (s != FC.secs) { FC.secs = s; fc_push(); }
        break;
    }
    case SH_CALL_ENDING:
        if (t - fc_end_t >= 300) { FC.state = SH_CALL_ENDED; FC.secs = 0; fc_push(); }
        break;
    case SH_CALL_ENDED:
        if (t - fc_end_t >= 1600) { FC.state = SH_CALL_IDLE; fc_push(); }
        break;
    default: break;
    }
    int ch = 0;
    for (int i = 0; i < FM.n; i++)
        if (FM.m[i].state == SH_SMS_SENDING && FM.sent_at[i] >= 0 && t - FM.sent_at[i] >= 1000) { FM.m[i].state = SH_SMS_SENT; ch = 1; }
    if (ch) fm_bump();
}
/* the film's commands: "ring MS" (an incoming call MS ms from now), "sms TEXT" (a message from a made-up number) */
static void comms_fake_ring(float in_ms) { fc_ring_at = fc_now + in_ms; }
static void comms_fake_sms(const char *text) { fm_receive("+15550142", text, 0); }
/* the stills' states, comms=WHAT:v:
 *   net (v 1 on, 0 off: the pretend Cellular), audio (v: audio_ok), inbox (three made-up messages),
 *   incoming, dialing, alerting, active (v: seconds), ended, sending, failed (the newest sent message's state) */
static void comms_fake_state(const char *what, float v)
{
    if (strcmp(what, "net") && !fc_net) sh_cellular(1);
    if (!strcmp(what, "net")) { sh_cellular(v > 0); return; }
    if (!strcmp(what, "audio")) { fc_audio_ok = v > 0; FC.audio_ok = fc_audio_ok; if (FC.state) fc_push(); return; }
    if (!strcmp(what, "inbox")) { fm_inbox(); return; }
    if (!strcmp(what, "incoming")) { fc_incoming(); return; }
    if (!strcmp(what, "dialing") || !strcmp(what, "alerting") || !strcmp(what, "active") || !strcmp(what, "ended")) {
        memset(&FC, 0, sizeof FC);
        FC.outgoing = 1; FC.audio = SH_AUDIO_EARPIECE; FC.audio_ok = fc_audio_ok;
        snprintf(FC.number, sizeof FC.number, "+15550123");
        FC.state = what[0] == 'd' ? SH_CALL_DIALING : what[0] == 'a' && what[1] == 'l' ? SH_CALL_ALERTING :
                   what[0] == 'a' ? SH_CALL_ACTIVE : SH_CALL_ENDED;
        if (FC.state == SH_CALL_ACTIVE) FC.secs = (int)v;
        fc_t0 = fc_end_t = fc_now;
        fc_push();
        return;
    }
    if (!strcmp(what, "sending") || !strcmp(what, "failed")) {
        for (int i = FM.n - 1; i >= 0; i--)
            if (!FM.m[i].incoming) {
                FM.m[i].state = what[0] == 's' ? SH_SMS_SENDING : SH_SMS_FAILED;
                FM.sent_at[i] = -1;                      /* stays so (a still) */
                if (what[0] == 'f') snprintf(FM.m[i].why, sizeof FM.m[i].why, "No network");
                fm_bump();
                break;
            }
        return;
    }
}
