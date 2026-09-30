/*
 * title: TCP connection state machine over a simulated link
 * topic: networking
 * covers: RFC 793 states, simultaneous open, simultaneous close, TIME_WAIT, FIN retransmit, discrete-event queue
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum St { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTAB, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT, NST };
static const char *stname[NST] = {"CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                  "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"};
enum { F_SYN = 1, F_ACK = 2, F_FIN = 4, F_RST = 8 };
enum Ev { E_DELIVER, E_APP_LISTEN, E_APP_OPEN, E_APP_CLOSE, E_TIMER };

typedef struct { int flags; unsigned seq, ack; } Seg;
typedef struct { long t; long order; int type, ep; Seg seg; int gen; } Event;
typedef struct {
    char name;
    int st;
    unsigned iss, snd_nxt, rcv_nxt;
    int gen; /* timer generation, stale timers are ignored */
} Ep;

static Event heap[256];
static int hn;
static long now, order_ctr;
static Ep eps[2];
static unsigned long drop_mask; /* bit i set: the i-th segment sent is lost */
static int sent_count;
static int visited[NST];

static int less(const Event *a, const Event *b) { return a->t != b->t ? a->t < b->t : a->order < b->order; }
static void push(Event e) {
    e.order = order_ctr++;
    int i = hn++;
    heap[i] = e;
    while (i > 0 && less(&heap[i], &heap[(i - 1) / 2])) {
        Event tmp = heap[i]; heap[i] = heap[(i - 1) / 2]; heap[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}
static Event pop(void) {
    Event top = heap[0];
    heap[0] = heap[--hn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && less(&heap[l], &heap[m])) m = l;
        if (r < hn && less(&heap[r], &heap[m])) m = r;
        if (m == i) break;
        Event tmp = heap[i]; heap[i] = heap[m]; heap[m] = tmp;
        i = m;
    }
    return top;
}

static void fail(const char *m) { fprintf(stderr, "check failed: %s\n", m); exit(1); }
static void flagstr(int f, char *o) {
    o[0] = 0;
    if (f & F_SYN) strcat(o, "S");
    if (f & F_FIN) strcat(o, "F");
    if (f & F_RST) strcat(o, "R");
    if (f & F_ACK) strcat(o, ".");
}
static void set_state(Ep *e, int s, const char *why) {
    printf("t=%3ld %c: %-11s -> %-11s (%s)\n", now, e->name, stname[e->st], stname[s], why);
    e->st = s;
    visited[s] = 1;
}
static void send_seg(Ep *e, int flags, unsigned seq, unsigned ack) {
    char fs[8];
    flagstr(flags, fs);
    int idx = sent_count++;
    if ((drop_mask >> idx) & 1) {
        printf("t=%3ld %c: send %-3s seq=%u ack=%u  LOST\n", now, e->name, fs, seq, ack);
        return;
    }
    Event ev = {now + 5, 0, E_DELIVER, e == &eps[0] ? 1 : 0, {flags, seq, ack}, 0};
    push(ev);
    printf("t=%3ld %c: send %-3s seq=%u ack=%u\n", now, e->name, fs, seq, ack);
}
static void arm(Ep *e, long delay) {
    Event ev = {now + delay, 0, E_TIMER, e == &eps[0] ? 0 : 1, {0, 0, 0}, ++e->gen};
    push(ev);
}

static void on_segment(Ep *e, Seg s) {
    switch (e->st) {
    case LISTEN:
        if (s.flags & F_SYN) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_SYN | F_ACK, e->iss, e->rcv_nxt);
            e->snd_nxt = e->iss + 1;
            set_state(e, SYN_RCVD, "rcv SYN");
        }
        break;
    case SYN_SENT:
        if ((s.flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && s.ack == e->iss + 1) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            set_state(e, ESTAB, "rcv SYN+ACK");
        } else if (s.flags == F_SYN) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_SYN | F_ACK, e->iss, e->rcv_nxt);
            set_state(e, SYN_RCVD, "rcv SYN (simultaneous open)");
        }
        break;
    case SYN_RCVD:
        if ((s.flags & F_ACK) && s.ack == e->iss + 1) set_state(e, ESTAB, "rcv ACK of SYN");
        break;
    case ESTAB:
        if (s.flags & F_FIN) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            set_state(e, CLOSE_WAIT, "rcv FIN");
        }
        break;
    case FIN_WAIT_1: {
        int acked = (s.flags & F_ACK) && s.ack == e->snd_nxt;
        if ((s.flags & F_FIN) && acked) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            set_state(e, TIME_WAIT, "rcv FIN+ACK");
            arm(e, 40);
        } else if (s.flags & F_FIN) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            set_state(e, CLOSING, "rcv FIN (simultaneous close)");
        } else if (acked) {
            e->gen++;
            set_state(e, FIN_WAIT_2, "rcv ACK of FIN");
        }
        break;
    }
    case FIN_WAIT_2:
        if (s.flags & F_FIN) {
            e->rcv_nxt = s.seq + 1;
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            set_state(e, TIME_WAIT, "rcv FIN");
            arm(e, 40);
        }
        break;
    case CLOSING:
        if ((s.flags & F_ACK) && s.ack == e->snd_nxt) {
            e->gen++;
            set_state(e, TIME_WAIT, "rcv ACK of FIN");
            arm(e, 40);
        }
        break;
    case LAST_ACK:
        if ((s.flags & F_ACK) && s.ack == e->snd_nxt) {
            e->gen++;
            set_state(e, CLOSED, "rcv ACK of FIN");
        }
        break;
    case TIME_WAIT:
        if (s.flags & F_FIN) {
            send_seg(e, F_ACK, e->snd_nxt, e->rcv_nxt);
            printf("t=%3ld %c: retransmitted FIN in TIME_WAIT, restart 2MSL\n", now, e->name);
            arm(e, 40);
        }
        break;
    default:
        break;
    }
}

static void app_close(Ep *e) {
    if (e->st == ESTAB || e->st == SYN_RCVD) {
        send_seg(e, F_FIN | F_ACK, e->snd_nxt, e->rcv_nxt);
        e->snd_nxt++;
        set_state(e, FIN_WAIT_1, "app close");
        arm(e, 12);
    } else if (e->st == CLOSE_WAIT) {
        send_seg(e, F_FIN | F_ACK, e->snd_nxt, e->rcv_nxt);
        e->snd_nxt++;
        set_state(e, LAST_ACK, "app close");
        arm(e, 12);
    } else {
        fail("close in bad state");
    }
}

static void run(const char *title, unsigned long drops, int a_open_t, int b_open_t, int a_close_t, int b_close_t, int passive_b) {
    printf("== %s\n", title);
    memset(eps, 0, sizeof eps);
    eps[0].name = 'A'; eps[1].name = 'B';
    eps[0].iss = 100; eps[1].iss = 900;
    eps[0].snd_nxt = 101; eps[1].snd_nxt = 901;
    hn = 0; now = 0; sent_count = 0; drop_mask = drops;
    Event ev;
    memset(&ev, 0, sizeof ev);
    if (passive_b) { ev.t = 0; ev.type = E_APP_LISTEN; ev.ep = 1; push(ev); }
    ev.t = a_open_t; ev.type = E_APP_OPEN; ev.ep = 0; push(ev);
    if (b_open_t >= 0) { ev.t = b_open_t; ev.type = E_APP_OPEN; ev.ep = 1; push(ev); }
    if (a_close_t >= 0) { ev.t = a_close_t; ev.type = E_APP_CLOSE; ev.ep = 0; push(ev); }
    if (b_close_t >= 0) { ev.t = b_close_t; ev.type = E_APP_CLOSE; ev.ep = 1; push(ev); }
    while (hn > 0) {
        ev = pop();
        now = ev.t;
        Ep *e = &eps[ev.ep];
        switch (ev.type) {
        case E_APP_LISTEN: set_state(e, LISTEN, "app listen"); break;
        case E_APP_OPEN:
            send_seg(e, F_SYN, e->iss, 0);
            set_state(e, SYN_SENT, "app connect");
            break;
        case E_APP_CLOSE: app_close(e); break;
        case E_DELIVER: on_segment(e, ev.seg); break;
        case E_TIMER:
            if (ev.gen != e->gen) break;
            if (e->st == TIME_WAIT) set_state(e, CLOSED, "2MSL expired");
            else if (e->st == LAST_ACK || e->st == FIN_WAIT_1) {
                send_seg(e, F_FIN | F_ACK, e->snd_nxt - 1, e->rcv_nxt);
                printf("t=%3ld %c: FIN retransmit timer\n", now, e->name);
                arm(e, 12);
            }
            break;
        }
    }
    if (eps[0].st != CLOSED || eps[1].st != CLOSED) fail("both ends must end CLOSED");
}

int main(void) {
    run("normal open, active close by A, passive close by B", 0, 0, -1, 30, 50, 1);
    run("A's FIN acked late: B's ACK lost, B replies FIN+ACK", 1ul << 4, 0, -1, 30, 36, 1);
    run("simultaneous open, simultaneous close", 0, 0, 0, 30, 30, 0);
    run("last ACK lost, FIN retransmitted into TIME_WAIT", 1ul << 6, 0, -1, 30, 40, 1);
    int all = 1;
    for (int i = 0; i < NST; i++) if (!visited[i]) { all = 0; printf("not visited: %s\n", stname[i]); }
    if (!all) fail("all eleven states visited");
    printf("all %d states visited\n", NST);
    return 0;
}
