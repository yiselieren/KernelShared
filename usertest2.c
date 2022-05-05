#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "shmdrv.h"

#define PERR(args...) do { printf(args); ; return 1; } while(0)
#define RTMO_USEC  rand_range(10, 30000)
#define TX_CHUNK   rand_range(1, 15000)
#define VERBOSE_READ
#define VERBOSE_WRITE

int       verbose_write = 0;
int       verbose_read = 0;
pthread_t readThreadID;
pthread_t writeThreadID;
int       done = 0;

void usage(const char *s)
{
    printf("Usage:\n\t%s [SWITCHES] DEVICE\n\n", s);
    printf("Where switches are:\n");
    printf("\t-h - Print help message\n");
    printf("\t-w - Verbose write\n");
    printf("\t-r - Verbose read\n");
    printf("\n");
    exit(0);
}

static struct Data *data;
static uint64_t    total_wr = 0;
static uint64_t    total_rd = 0;

static char *humanReadableBytes(uint64_t cnt, const char *pref)
{
#define K  1024
#define M  (K*1024)
#define G  (M*1024)
#define T  (G*1024ULL)
#define BS 64
    static char b[64];

    if (cnt > T)
        snprintf(b, BS-1, "%.2f%sT", (double)cnt / (double)T, pref);
    else if (cnt > G)
        snprintf(b, BS-1, "%.2f%sG", (double)cnt / (double)G, pref);
    else if (cnt > M)
        snprintf(b, BS-1, "%.2f%sM", (double)cnt / (double)M, pref);
    else if (cnt > K)
        snprintf(b, BS-1, "%.2f%sK", (double)cnt / (double)K, pref);
    else
        snprintf(b, BS-1, "%lu%s", cnt, pref);
    return b;
}

static void finish(int rc)
{
    (void)rc;
    done = 1;
}


int rand_range(int low, int high)
{
    int rc = rand() % (high - low);
    return low + rc;
}

void *readThread(void *pfd)
{
    static uint8_t exp_char = 0;
    uint8_t        rd_char = 0;
    uint64_t       rd_bytes = 0;
    int            fd = *((int *)pfd);

    while (!done) {
        char c;
        // Comleted read means it is some data in a ring. Otherwise it hangs
        // till some data is available. The read itself doesn't return usefull info
        read(fd, &c, 1);

        for (;;) {
            if (data->in.rp == data->in.wp)
                // Empty
                break;
            rd_char = data->in.d[data->in.rp];
            rd_bytes++;
            data->in.rp = (data->in.rp + 1) % LEN;
            if (++exp_char >= 255)
                exp_char = 1;
            if (rd_char != exp_char) {
                printf("User read:  mismatch - 0x%02x expected, 0x%02x received\n", exp_char&0xff, rd_char&0xff);
                finish(0);
            }
        }
        total_rd += rd_bytes;
        if (verbose_read && rd_bytes) {
            printf("User read:  %ld bytes read (%s total)\n", rd_bytes, humanReadableBytes(total_rd, ""));
        }
    }
    return NULL;
}

void *writeThread(void *pfd)
{
    static uint8_t wr_char = 0;
    uint64_t       wr_bytes = 0;
    int            i;
    int            fd = *((int *)pfd);

    while (!done) {
        int l = TX_CHUNK;

        for (i = 0; i < l; i++) {
            uint32_t next_idx = (data->out.wp + 1) % LEN;
            if (next_idx == data->out.rp) {
                // Full
                printf("Output buffer full!\n");
                break;
            }
            if (++wr_char >= 255)
                wr_char = 1;
            data->out.d[data->out.wp] = wr_char;
            wr_bytes++;
            data->out.wp = next_idx;
        }
        if (wr_bytes)
            write(fd, "", 1);  // Fake write is a signal to kernel module about the new data
        total_wr += wr_bytes;
        if (verbose_write) {
            printf("User write: %ld bytes written (%s total)\n", wr_bytes, humanReadableBytes(total_wr, ""));
        }
        usleep(RTMO_USEC);
    }
    return NULL;
}

int main(int ac, char *av[])
{
    int         argcnt = 0;
    int         i, fd;
    char        *dev;

    if (ac < 2)
        usage(av[0]);
    for (i = 1; i < ac; i++) {
        if (*av[i] == '-') {
            switch (*++av[i]) {
            case 'h':
                usage(av[0]);
                break;
            case 'r':
                verbose_read = 1;
                break;
            case 'w':
                verbose_write = 1;
                break;
            default:
                PERR("\"%c\" is invalid command line switch\n", *av[i]);
            }
        } else {
            switch (argcnt++) {
            case 0:
                dev = av[i];
                break;
            default:
                PERR("Too many parameters\n");
            }
        }
    }
    if (argcnt < 1)
        PERR("Too few parameters\n");

    // shm device
    if ((fd=open(dev, O_RDWR))<0)
        PERR("Can't open \"%s\": %s\n", dev, strerror(errno));

    data = (struct Data*)mmap(0, sizeof(struct Data), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
        PERR("Can't mmap \"%s\": %s\n", dev, strerror(errno));

    signal(SIGINT, finish);
    pthread_create(&readThreadID, NULL, readThread, (void*)&fd);
    pthread_create(&writeThreadID, NULL, writeThread, (void*)&fd);

    pthread_join(readThreadID, NULL);
    pthread_join(writeThreadID, NULL);
    printf("\n\n");
    printf("Total read  %ld bytes (%s)\n", total_rd, humanReadableBytes(total_rd, ""));
    printf("Total write %ld bytes (%s)\n", total_wr, humanReadableBytes(total_wr, ""));

    close(fd);
    return(0);
}
