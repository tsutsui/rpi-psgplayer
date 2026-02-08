/* psg_backend.h - PSG backend API */
#ifndef PSG_BACKEND_H
#define PSG_BACKEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct psg_backend psg_backend_t;

typedef struct {
    const char *id;

    /* process-local resources */
    int  (*init)(psg_backend_t *psgbe);
    void (*fini)(psg_backend_t *psgbe);

    /* external side effects boundary */
    int  (*enable)(psg_backend_t *psgbe);
    void (*disable)(psg_backend_t *psgbe);

    /* PSG operations (valid only while enabled) */
    int  (*reset)(psg_backend_t *psgbe);
    int  (*write_reg)(psg_backend_t *psgbe, uint8_t reg, uint8_t val);
} psg_backend_ops_t;

#define PSG_BACKEND_LAST_ERROR_MAXLEN 256

struct psg_backend {
    const psg_backend_ops_t *ops;
    void *ctx; /* owned by backend: allocated in init, freed in fini */

    /* last error message (set by backend on failure paths) */
    char last_error[PSG_BACKEND_LAST_ERROR_MAXLEN];
};

static inline const char *
psg_backend_last_error(const psg_backend_t *psgbe)
{
    if (psgbe == NULL)
        return "(psgbe is NULL)";

    return psgbe->last_error;
}

#endif /* PSG_BACKEND_H */
