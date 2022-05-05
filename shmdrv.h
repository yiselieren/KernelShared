#ifndef SHMDRV_H
#define SHMDRV_H

/*
 * SHMDRV data structs
*/

#define LEN (64*1024)

struct Data1 {
    volatile uint32_t wp;       /* Write pointer (index in "d") */
    volatile uint32_t rp;       /* Read pointer (index in "d")  */
    volatile char     d[LEN];   /* FIFO data                    */
};

struct Data {
    struct Data1 in;
    struct Data1 out;
};

#endif
