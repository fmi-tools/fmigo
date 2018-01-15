#ifndef MODELEXCHANGE_H
#define MODELEXCHANGE_H

#define FMILIB_BUILDING_LIBRARY
#include <fmilib.h>
#include <stdbool.h>
#include "gsl-interface.h"

typedef struct Backup
{
    double t;
    double h;
    double *dydt;
    fmi2_real_t* ei;
    fmi2_real_t* ei_b;
    fmi2_real_t* x;
    unsigned long failed_steps;
    fmi2_event_info_t eventInfo;
}Backup;

void restoreStates(cgsl_simulation *sim, Backup *backup);
void storeStates(cgsl_simulation *sim, Backup *backup);
void runIteration(cgsl_simulation *sim, double t, double dt);
void prepare(cgsl_simulation *sim, fmi2_import_t *FMU, enum cgsl_integrator_ids integrator);
void allocateBackup(Backup *backup, void *params);
void freeBackup(Backup *backup);
#endif