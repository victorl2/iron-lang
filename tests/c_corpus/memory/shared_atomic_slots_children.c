/*
 * title: Children claim work items in shared memory with atomic operations
 * topic: memory
 * covers: shared anonymous mapping, atomic_fetch_add ticket dispenser, compare-exchange ownership claims, fork/waitpid
 * deps: posix
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define NCHILD 4
#define ITEMS 2000

typedef struct {
    _Atomic uint32_t ticket;               /* dispenser: each fetch_add hands out a unique item */
    _Atomic uint32_t owner[ITEMS];         /* CAS-claimed: 0 = free, else child + 1 */
    _Atomic uint32_t done_count[NCHILD];   /* items processed per child */
    _Atomic uint64_t total;                /* sum of results, added by every child */
    uint32_t result[ITEMS];                /* written once by whoever holds the ticket */
    _Atomic uint32_t cas_wins[NCHILD];
    _Atomic uint32_t cas_losses[NCHILD];
} Shared;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t work(uint32_t i) {
    uint32_t x = i + 0x1234567u;
    for (int k = 0; k < 20; k++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return x & 0xFFFFu;
}

int main(void) {
    Shared *sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(sh != MAP_FAILED, "mmap");
    memset(sh, 0, sizeof *sh); /* zero is a valid initial state for every atomic here */
    fflush(stdout);

    pid_t pids[NCHILD];
    for (int c = 0; c < NCHILD; c++) {
        pid_t pid = fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            /* phase 1: ticket dispenser */
            for (;;) {
                uint32_t i = atomic_fetch_add(&sh->ticket, 1);
                if (i >= ITEMS)
                    break;
                sh->result[i] = work(i);
                atomic_fetch_add(&sh->total, sh->result[i]);
                atomic_fetch_add(&sh->done_count[c], 1);
            }
            /* phase 2: every child tries to claim every item; exactly one may win */
            for (uint32_t i = 0; i < ITEMS; i++) {
                uint32_t expected = 0;
                if (atomic_compare_exchange_strong(&sh->owner[i], &expected, (uint32_t)c + 1))
                    atomic_fetch_add(&sh->cas_wins[c], 1);
                else
                    atomic_fetch_add(&sh->cas_losses[c], 1);
            }
            _exit(20 + c);
        }
        pids[c] = pid;
    }
    for (int c = 0; c < NCHILD; c++) {
        int st = 0;
        check(waitpid(pids[c], &st, 0) == pids[c], "waitpid");
        check(WIFEXITED(st) && WEXITSTATUS(st) == 20 + c, "child status");
    }
    printf("children finished: %d\n", NCHILD);

    uint32_t done = 0, wins = 0, losses = 0;
    for (int c = 0; c < NCHILD; c++) {
        done += atomic_load(&sh->done_count[c]);
        wins += atomic_load(&sh->cas_wins[c]);
        losses += atomic_load(&sh->cas_losses[c]);
    }
    printf("items processed: %u of %d\n", (unsigned)done, ITEMS);
    check(done == ITEMS, "every item processed once");
    check(atomic_load(&sh->ticket) >= ITEMS, "tickets exhausted");

    uint64_t want = 0;
    for (uint32_t i = 0; i < ITEMS; i++) {
        check(sh->result[i] == work(i), "result slot");
        want += work(i);
    }
    printf("total: %llu\n", (unsigned long long)atomic_load(&sh->total));
    check(atomic_load(&sh->total) == want, "atomic sum exact");

    printf("cas wins: %u, losses: %u\n", (unsigned)wins, (unsigned)losses);
    check(wins == ITEMS && losses == ITEMS * (NCHILD - 1), "exactly one winner per item");
    unsigned claimed = 0;
    for (uint32_t i = 0; i < ITEMS; i++) {
        uint32_t o = atomic_load(&sh->owner[i]);
        check(o >= 1 && o <= NCHILD, "owner set");
        claimed++;
    }
    printf("claimed items: %u\n", claimed);
    check(munmap(sh, sizeof *sh) == 0, "munmap");
    return 0;
}
