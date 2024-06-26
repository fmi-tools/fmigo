/* ---------------------------------------------------------------------------*
 * fmuTemplate.c
 * Implementation of the FMI interface based on functions and macros to
 * be defined by the includer of this file.
 * If FMI_COSIMULATION is defined, this implements "FMI for Co-Simulation 2.0",
 * otherwise "FMI for Model Exchange 2.0".
 * The "FMI for Co-Simulation 2.0", implementation assumes that exactly the
 * following capability flags are set to fmi2True:
 *    canHandleVariableCommunicationStepSize, i.e. fmiDoStep step size can vary
 * and all other capability flags are set to default, i.e. to fmi2False or 0.
 *
 * Revision history
 *  07.03.2014 initial version released in FMU SDK 2.0.0
 *  02.04.2014 allow modules to request termination of simulation, better time
 *             event handling, initialize() moved from fmiEnterInitialization to
 *             fmiExitInitialization, correct logging message format in fmiDoStep.
 *
 * Author: Adrian Tirea
 * Copyright QTronic GmbH. All rights reserved.
 * ---------------------------------------------------------------------------*/

// macro to be used to log messages. The macro check if current
// log category is valid and, if true, call the logger provided by simulator.
#define FILTERED_LOG(instance, status, categoryIndex, message, ...) if (isCategoryLogged(instance, categoryIndex)) \
        instance->functions->logger(instance->functions->componentEnvironment, instance->instanceName, status, \
        logCategoriesNames[categoryIndex], message, ##__VA_ARGS__);

static fmi2String logCategoriesNames[] = {"logAll", "logError", "logFmiCall", "logEvent"};

// ---------------------------------------------------------------------------
// Private helpers logger
// ---------------------------------------------------------------------------

// return fmi2True if logging category is on. Else return fmi2False.
static fmi2Boolean isCategoryLogged(ModelInstance *comp, int categoryIndex) {
    if (categoryIndex < NUMBER_OF_CATEGORIES
        && (comp->logCategories[categoryIndex] || comp->logCategories[LOG_ALL])) {
        return fmi2True;
    }
    return fmi2False;
}

// ---------------------------------------------------------------------------
// Private helpers used below to validate function arguments
// ---------------------------------------------------------------------------

static fmi2Boolean invalidNumber(ModelInstance *comp, const char *f, const char *arg, int n, int nExpected) {
    if (n != nExpected) {
        comp->s.state = modelError;
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "%s: Invalid argument %s = %d. Expected %d.", f, arg, n, nExpected)
        return fmi2True;
    }
    return fmi2False;
}

static fmi2Boolean invalidState(ModelInstance *comp, const char *f, int statesExpected) {
    if (!comp)
        return fmi2True;
    if (!(comp->s.state & statesExpected)) {
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "%s: Illegal call sequence. got %i, expected %i", f, comp->s.state, statesExpected)
        comp->s.state = modelError;
        return fmi2True;
    }
    return fmi2False;
}

static fmi2Boolean nullPointer(ModelInstance* comp, const char *f, const char *arg, const void *p) {
    if (!p) {
        comp->s.state = modelError;
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "%s: Invalid argument %s = NULL.", f, arg)
        return fmi2True;
    }
    return fmi2False;
}

#ifndef HAVE_GENERATED_GETTERS_SETTERS
static fmi2Boolean vrOutOfRange(ModelInstance *comp, const char *f, fmi2ValueReference vr, fmi2ValueReference end) {
    if (vr >= end) {
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "%s: Illegal value reference %u.", f, vr)
        comp->s.state = modelError;
        return fmi2True;
    }
    return fmi2False;
}
#endif

static fmi2Status unsupportedFunction(fmi2Component c, const char *fName, int statesExpected) {
    ModelInstance *comp = (ModelInstance *)c;
    fmi2CallbackLogger log = comp->functions->logger;
    if (invalidState(comp, fName, statesExpected))
        return fmi2Error;
    if (comp->loggingOn) log(c, comp->instanceName, fmi2OK, "log", fName);
    FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "%s: Function not implemented.", fName)
    return fmi2Error;
}

fmi2Status fmi2SetString (fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2String value[]);
fmi2Status setString(fmi2Component comp, fmi2ValueReference vr, fmi2String value){
    return fmi2SetString(comp, &vr, 1, &value);
}

static void zeroState(state_t *s) {
    //zero s, except simulation pointer
#ifdef SIMULATION_TYPE
    memset(s, 0, sizeof(*s) - sizeof(s->simulation));
#else
    memset(s, 0, sizeof(*s));
#endif
}

// ---------------------------------------------------------------------------
// FMI functions: class methods not depending of a specific model instance
// ---------------------------------------------------------------------------

const char* fmi2GetTypesPlatform() {
    return fmi2TypesPlatform;
}

const char* fmi2GetVersion() {
    return fmi2Version;
}

#include "strlcpy.h"

fmi2Status fmi2SetDebugLogging(fmi2Component c, fmi2Boolean loggingOn, size_t nCategories, const fmi2String categories[]) {
    // ignore arguments: nCategories, categories
    size_t i;
    int j;
    ModelInstance *comp = (ModelInstance *)c;
    comp->loggingOn = loggingOn;

    for (j = 0; j < NUMBER_OF_CATEGORIES; j++) {
        comp->logCategories[j] = fmi2False;
    }
    for (i = 0; i < nCategories; i++) {
        fmi2Boolean categoryFound = fmi2False;
        for (j = 0; j < NUMBER_OF_CATEGORIES; j++) {
            if (strcmp(logCategoriesNames[j], categories[i]) == 0) {
                comp->logCategories[j] = loggingOn;
                categoryFound = fmi2True;
                break;
            }
        }
        if (!categoryFound) {
            comp->functions->logger(comp->componentEnvironment, comp->instanceName, fmi2Warning,
                logCategoriesNames[LOG_ERROR],
                "logging category '%s' is not supported by model", categories[i]);
        }
    }

    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetDebugLogging")
    return fmi2OK;
}

static int isHex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex2Int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    } else {
        return 10 + c - 'A';
    }
}

static fmi2Status unescapeFileURI(ModelInstance *comp, const char *uri, char *dest, size_t n) {
    const char *uriIn = uri;
#ifdef WIN32
    if (!strncmp(uri, "file:///", 8)) { // file:///C:/foo/bar -> C:/foo/bar
        uri += 8;
#else
    if (!strncmp(uri, "file://",  7)) { //leave leading slash intact, so file:///foo/bar -> /foo/bar
        uri += 7;
#endif
    } else {
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "Cannot unescape file URI %s", uri);
        return fmi2Error;
    }

    size_t i = 0;
    for (; i < n; i++) {
        char c = *uri;
        if (c == 0) {
            break;
        } else if (c == '%') {
            //expect two hex characters
            if (isHex(uri[1]) && isHex(uri[2])) {
                char c2 = 16*hex2Int(uri[1]) + hex2Int(uri[2]);
                dest[i] = c2;
                uri += 3;
            } else {
                FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "File URI contains illegal percent escape %3s (%s)", uri, uriIn);
                return fmi2Error;
            }
        } else {
            dest[i] = c;
            uri++;
        }
    }
    dest[i < n ? i : n-1] = 0;

    if (i >= n) {
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "Not enough space to unescape file URI %s -> %s", uriIn, dest);
        return fmi2Error;
    }

    return fmi2OK;
}

fmi2Component fmi2Instantiate(fmi2String instanceName, fmi2Type fmuType, fmi2String fmuGUID,
                            fmi2String fmuResourceLocation, const fmi2CallbackFunctions *functions,
                            fmi2Boolean visible, fmi2Boolean loggingOn) {
    // ignoring arguments: visible
    ModelInstance *comp;
    if (!functions->logger) {
        return NULL;
    }

    if (!functions->allocateMemory || !functions->freeMemory) {
        functions->logger(functions->componentEnvironment, instanceName, fmi2Error, "error",
                "fmi2Instantiate: Missing callback function.");
        return NULL;
    }
    if (!instanceName || strlen(instanceName) == 0) {
        functions->logger(functions->componentEnvironment, instanceName, fmi2Error, "error",
                "fmi2Instantiate: Missing instance name.");
        return NULL;
    }
    if (!fmuResourceLocation || strlen(fmuResourceLocation) == 0) {
        functions->logger(functions->componentEnvironment, fmuResourceLocation, fmi2Error, "error",
                "fmi2Instantiate: Missing resource location.");
        return NULL;
    }
    if (strcmp(fmuGUID, MODEL_GUID)) {
        functions->logger(functions->componentEnvironment, instanceName, fmi2Error, "error",
                "fmi2Instantiate: Wrong GUID %s. Expected %s.", fmuGUID, MODEL_GUID);
        return NULL;
    }
    comp = (ModelInstance *)functions->allocateMemory(1, sizeof(ModelInstance));
    if (comp) {
        size_t i;
        // UMIT: log errors only, if logging is on. We don't have to enable all of them,
        // to quote the spec: "Which LogCategories the FMU sets is unspecified."
        // fmi2SetDebugLogging should be called to choose specific categories.
        for (i = 0; i < NUMBER_OF_CATEGORIES; i++) {
            comp->logCategories[i] = 0;
        }
        comp->logCategories[LOG_ERROR] = loggingOn;
    }
    if (!comp) {

        functions->logger(functions->componentEnvironment, instanceName, fmi2Error, "error",
            "fmi2Instantiate: Out of memory.");
        return NULL;
    }
    strlcpy2(comp->instanceName,        instanceName,        sizeof(comp->instanceName));
    comp->type = fmuType;
    comp->functions = functions;
    comp->componentEnvironment = functions->componentEnvironment;
    comp->loggingOn = loggingOn;
    zeroState(&comp->s);
    comp->s.state = modelInstantiated;
#ifdef HAVE_DEFAULTS
    comp->s.md = defaults;
#endif

    if (unescapeFileURI(comp, fmuResourceLocation, comp->fmuResourceLocation, sizeof(comp->fmuResourceLocation)) != fmi2OK) {
        return NULL;
    }

    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2Instantiate: GUID=%s", fmuGUID)

#ifdef SIMULATION_INSTANTIATE
    if (SIMULATION_INSTANTIATE(comp) != fmi2OK) {
        return NULL;
    }
#endif

    return comp;
}

void fmi2FreeInstance(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (!comp) return;
    if (invalidState(comp, "fmi2FreeInstance", modelTerminated))
        return;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2FreeInstance")
#ifdef SIMULATION_FREE
    SIMULATION_FREE(comp->s.simulation);
#endif
    comp->functions->freeMemory(comp);
}

fmi2Status fmi2SetupExperiment(fmi2Component c, fmi2Boolean toleranceDefined, fmi2Real tolerance,
                            fmi2Real startTime, fmi2Boolean stopTimeDefined, fmi2Real stopTime) {

    // ignore arguments: stopTimeDefined, stopTime
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetupExperiment", modelInstantiated))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetupExperiment: toleranceDefined=%d tolerance=%g",
        toleranceDefined, tolerance)

    comp->s.time = startTime;
#ifdef SIMULATION_SETUP_EXPERIMENT
    return SIMULATION_SETUP_EXPERIMENT(comp, toleranceDefined, tolerance, startTime, stopTimeDefined, stopTime);
#else
    return fmi2OK;
#endif
}

fmi2Status fmi2EnterInitializationMode(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2EnterInitializationMode", modelInstantiated))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2EnterInitializationMode")

    comp->s.state = modelInitializationMode;
#ifdef SIMULATION_ENTER_INIT
    return SIMULATION_ENTER_INIT(comp);
#else
    return fmi2OK;
#endif
}

fmi2Status fmi2ExitInitializationMode(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    fmi2Status ret = fmi2OK;
    if (invalidState(comp, "fmi2ExitInitializationMode", modelInitializationMode))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2ExitInitializationMode")

#ifdef SIMULATION_EXIT_INIT
    ret = SIMULATION_EXIT_INIT(comp);
#endif
#ifdef FMI_COSIMULATION
    comp->s.state = modelInitialized;
#else
    // ME FMUs enter event mode after initialization (FMI 2.0 spec, fig. 6)
    comp->s.state = modelEventMode;
#endif
    return ret;
}

fmi2Status fmi2Terminate(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2Terminate", modelInitialized|modelStepping|modelEventMode|modelContinuousTimeMode))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2Terminate")

    comp->s.state = modelTerminated;
    return fmi2OK;
}

fmi2Status fmi2Reset(fmi2Component c) {
    ModelInstance* comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2Reset", modelInitialized|modelStepping))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2Reset")

    zeroState(&comp->s);
    comp->s.state = modelInstantiated;
    return fmi2OK;
}

fmi2Status fmi2GetReal (fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Real value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetReal", MASK_fmi2GetX))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetReal", "vr[]", vr))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetReal", "value[]", value))
        return fmi2Error;
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    return generated_fmi2GetReal(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2GetReal", vr[i], NUMBER_OF_REALS))
            return fmi2Error;
        value[i] = comp->s.r[vr[i]];
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetReal: #r%u# = %.16g", vr[i], value[i])
    }
    return fmi2OK;
#endif
}

fmi2Status fmi2GetInteger(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Integer value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetInteger", MASK_fmi2GetX))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetInteger", "vr[]", vr))
            return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetInteger", "value[]", value))
            return fmi2Error;
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    return generated_fmi2GetInteger(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2GetInteger", vr[i], NUMBER_OF_INTEGERS))
            return fmi2Error;
        value[i] = comp->s.i[vr[i]];
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetInteger: #i%u# = %d", vr[i], value[i])
    }
    return fmi2OK;
#endif
}

fmi2Status fmi2GetBoolean(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2Boolean value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetBoolean", MASK_fmi2GetX))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetBoolean", "vr[]", vr))
            return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetBoolean", "value[]", value))
            return fmi2Error;
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    return generated_fmi2GetBoolean(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2GetBoolean", vr[i], NUMBER_OF_BOOLEANS))
            return fmi2Error;
        value[i] = comp->s.b[vr[i]];
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetBoolean: #b%u# = %s", vr[i], value[i]? "true" : "false")
    }
    return fmi2OK;
#endif
}

fmi2Status fmi2GetString (fmi2Component c, const fmi2ValueReference vr[], size_t nvr, fmi2String value[]) {
#ifndef HAVE_GENERATED_GETTERS_SETTERS
    return unsupportedFunction(c, "fmi2GetString", MASK_fmi2GetX);
#else
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetString", MASK_fmi2GetX))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetString", "vr[]", vr))
            return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2GetString", "value[]", value))
            return fmi2Error;
    return generated_fmi2GetString(comp, &comp->s.md, vr, nvr, value);
#endif

}

fmi2Status fmi2SetReal (fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Real value[]) {
    fmi2Status ret;
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetReal", MASK_fmi2SetX | modelContinuousTimeMode))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2SetReal", "vr[]", vr))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2SetReal", "value[]", value))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetReal: nvr = %d", nvr)
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    ret = generated_fmi2SetReal(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2SetReal", vr[i], NUMBER_OF_REALS))
            return fmi2Error;
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetReal: #r%d# = %.16g", vr[i], value[i])
        comp->s.r[vr[i]] = value[i];
    }
    ret = fmi2OK;
#endif
#ifdef HAVE_INITIALIZATION_MODE
    //we could do something similar in other modes too
    //we just have to be careful what we use for states going into sync_out()
    if (comp->s.state == modelInitializationMode) {
        int N = get_initial_states_size(&comp->s);
        double *initials = calloc(N, sizeof(double));
        get_initial_states(&comp->s, initials);
        sync_out(0.0, 0.0, N, initials, &comp->s);
        free(initials);
    }
#endif
    return ret;
}

fmi2Status fmi2SetInteger(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Integer value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetInteger", MASK_fmi2SetX))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2SetInteger", "vr[]", vr))
        return fmi2Error;
    if (nvr > 0 && nullPointer(comp, "fmi2SetInteger", "value[]", value))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetInteger: nvr = %d", nvr)
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    return generated_fmi2SetInteger(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2SetInteger", vr[i], NUMBER_OF_INTEGERS))
            return fmi2Error;
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetInteger: #i%d# = %d", vr[i], value[i])
        comp->s.i[vr[i]] = value[i];
    }
    return fmi2OK;
#endif
}

fmi2Status fmi2SetBoolean(fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2Boolean value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetBoolean", MASK_fmi2SetX))
        return fmi2Error;
    if (nvr>0 && nullPointer(comp, "fmi2SetBoolean", "vr[]", vr))
        return fmi2Error;
    if (nvr>0 && nullPointer(comp, "fmi2SetBoolean", "value[]", value))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetBoolean: nvr = %d", nvr)
#ifdef HAVE_GENERATED_GETTERS_SETTERS
    return generated_fmi2SetBoolean(comp, &comp->s.md, vr, nvr, value);
#else
    size_t i;
    for (i = 0; i < nvr; i++) {
        if (vrOutOfRange(comp, "fmi2SetBoolean", vr[i], NUMBER_OF_BOOLEANS))
            return fmi2Error;
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetBoolean: #b%d# = %s", vr[i], value[i] ? "true" : "false")
        comp->s.b[vr[i]] = value[i];
    }
    return fmi2OK;
#endif
}

fmi2Status fmi2SetString (fmi2Component c, const fmi2ValueReference vr[], size_t nvr, const fmi2String value[]) {
#ifndef HAVE_GENERATED_GETTERS_SETTERS
    return unsupportedFunction(c, "fmi2SetString", MASK_fmi2SetX);
#else
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetString", MASK_fmi2SetX))
        return fmi2Error;
    if (nvr>0 && nullPointer(comp, "fmi2SetString", "vr[]", vr))
        return fmi2Error;
    if (nvr>0 && nullPointer(comp, "fmi2SetString", "value[]", value))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetString: nvr = %d", nvr)
    return generated_fmi2SetString(comp, &comp->s.md, vr, nvr, value);
#endif
}

fmi2Status fmi2GetFMUstate (fmi2Component c, fmi2FMUstate* FMUstate) {
#if CAN_GET_SET_FMU_STATE
    ModelInstance *comp = (ModelInstance *)c;
    *FMUstate = comp->functions->allocateMemory(1, sizeof(comp->s));

#ifdef SIMULATION_TYPE
#ifndef SIMULATION_GET
//assume GSL
#define SIMULATION_GET cgsl_simulation_get
#endif
    SIMULATION_GET( &(( ModelInstance *) c)->s.simulation );
#endif
    memcpy(*FMUstate, &comp->s, sizeof(comp->s));
    return fmi2OK;
#else
    return fmi2Error;
#endif
}

fmi2Status fmi2SetFMUstate (fmi2Component c, fmi2FMUstate FMUstate) {
#if CAN_GET_SET_FMU_STATE
    ModelInstance *comp = (ModelInstance *)c;
    memcpy(&comp->s, FMUstate, sizeof(comp->s));

#ifdef SIMULATION_TYPE
#ifndef SIMULATION_SET
//assume GSL
#define SIMULATION_SET  cgsl_simulation_set
#endif
    SIMULATION_SET( &(( ModelInstance *) c)->s.simulation );
#endif
    return fmi2OK;
#else
    return fmi2Error;
#endif
}

fmi2Status fmi2FreeFMUstate(fmi2Component c, fmi2FMUstate* FMUstate) {
    ModelInstance *comp = (ModelInstance *)c;
    comp->functions->freeMemory(*FMUstate);
    *FMUstate = NULL;
    return fmi2OK;
}

fmi2Status fmi2SerializedFMUstateSize(fmi2Component c, fmi2FMUstate FMUstate, size_t *size) {
    return fmi2Error; //disabled for now
    //ModelInstance *comp = (ModelInstance *)c;
    //*size = sizeof(comp->s);
    //return fmi2OK;
}

fmi2Status fmi2SerializeFMUstate (fmi2Component c, fmi2FMUstate FMUstate, fmi2Byte serializedState[], size_t size) {
    return fmi2Error; //disabled for now
    //memcpy(serializedState, FMUstate, size);
    //return fmi2OK;
}

fmi2Status fmi2DeSerializeFMUstate (fmi2Component c, const fmi2Byte serializedState[], size_t size, fmi2FMUstate* FMUstate) {
    return fmi2Error; //disabled for now
    //memcpy(*FMUstate, serializedState, size);
    //return fmi2OK;
}

fmi2Status fmi2GetDirectionalDerivative(fmi2Component c, const fmi2ValueReference vUnknown_ref[], size_t nUnknown,
                const fmi2ValueReference vKnown_ref[] , size_t nKnown, const fmi2Real dvKnown[], fmi2Real dvUnknown[]) {
#ifndef HAVE_DIRECTIONAL_DERIVATIVE
#error HAVE_DIRECTIONAL_DERIVATIVE not defined
#endif
#if HAVE_DIRECTIONAL_DERIVATIVE
    ModelInstance *comp = (ModelInstance *)c;
    size_t x, y;

    /**
     * Return partial derivatives of outputs wrt inputs, weighted by dvKnown
     */
    for (x = 0; x < nUnknown; x++) {
        dvUnknown[x] = 0;

        for (y = 0; y < nKnown; y++) {
            fmi2Real partial;
            fmi2Status status = getPartial(comp, vUnknown_ref[x], vKnown_ref[y], &partial);

            if (status != fmi2OK) {
                fprintf(stderr, "Tried to get partial derivative of VR %i w.r.t VR %i, which doesn't exist or isn't defined\n", vUnknown_ref[x], vKnown_ref[y]);
                return status;
            }

            dvUnknown[x] += dvKnown[y] * partial;
        }
    }

    return fmi2OK;
#else
    return fmi2Error;
#endif
}

// ---------------------------------------------------------------------------
// Functions for FMI for Co-Simulation
// ---------------------------------------------------------------------------
#ifdef FMI_COSIMULATION
/* Simulating the slave */
fmi2Status fmi2SetRealInputDerivatives(fmi2Component c, const fmi2ValueReference vr[], size_t nvr,
                                     const fmi2Integer order[], const fmi2Real value[]) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetRealInputDerivatives",
                     modelInstantiated|modelInitializationMode|modelInitialized|modelStepping)) {
        return fmi2Error;
    }
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetRealInputDerivatives: nvr= %d", nvr)
    FILTERED_LOG(comp, fmi2Error, LOG_ERROR, "fmi2SetRealInputDerivatives: ignoring function call."
        " This model cannot interpolate inputs: canInterpolateInputs=\"fmi2False\"")
    return fmi2Error;
}

fmi2Status fmi2GetRealOutputDerivatives(fmi2Component c, const fmi2ValueReference vr[], size_t nvr,
                                      const fmi2Integer order[], fmi2Real value[]) {
    size_t i;
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetRealOutputDerivatives", modelInitialized|modelStepping|modelError))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetRealOutputDerivatives: nvr= %d", nvr)
    FILTERED_LOG(comp, fmi2Error, LOG_ERROR,"fmi2GetRealOutputDerivatives: ignoring function call."
        " This model cannot compute derivatives of outputs: MaxOutputDerivativeOrder=\"0\"")
    for (i = 0; i < nvr; i++) value[i] = 0;
    return fmi2Error;
}

fmi2Status fmi2DoStep(fmi2Component cc, fmi2Real currentCommunicationPoint,
                    fmi2Real communicationStepSize, fmi2Boolean noSetFMUStatePriorToCurrentPoint) {
    ModelInstance *comp = (ModelInstance *)cc;

    if (invalidState(comp, "fmi2DoStep", modelInitialized|modelStepping))
        return fmi2Error;

    // model is in stepping state
    comp->s.state = modelStepping;

    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2DoStep: "
        "currentCommunicationPoint = %g, "
        "communicationStepSize = %g, "
        "noSetFMUStatePriorToCurrentPoint = fmi%s",
        currentCommunicationPoint, communicationStepSize, noSetFMUStatePriorToCurrentPoint ? "True" : "False")

    if (communicationStepSize <= 0) {
        FILTERED_LOG(comp, fmi2Error, LOG_ERROR,
            "fmi2DoStep: communication step size must be > 0. Fount %g.", communicationStepSize)
        return fmi2Error;
    }

    doStep(&comp->s, currentCommunicationPoint, communicationStepSize, noSetFMUStatePriorToCurrentPoint);
    return fmi2OK;
}

fmi2Status fmi2CancelStep(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2CancelStep", modelStepping))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2CancelStep")
    FILTERED_LOG(comp, fmi2Error, LOG_ERROR,"fmi2CancelStep: Can be called when fmiDoStep returned fmiPending."
        " This is not the case.");
    return fmi2Error;
}

/* Inquire slave status */
static fmi2Status getStatus(char* fname, fmi2Component c, const fmi2StatusKind s) {
    const char *statusKind[3] = {"fmi2DoStepStatus","fmi2PendingStatus","fmi2LastSuccessfulTime"};
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, fname, modelInitialized|modelStepping))
            return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "$s: fmi2StatusKind = %s", fname, statusKind[s])

    switch(s) {
        case fmi2DoStepStatus: FILTERED_LOG(comp, fmi2Error, LOG_ERROR,
            "%s: Can be called with fmi2DoStepStatus when fmiDoStep returned fmiPending."
            " This is not the case.", fname)
            break;
        case fmi2PendingStatus: FILTERED_LOG(comp, fmi2Error, LOG_ERROR,
            "%s: Can be called with fmi2PendingStatus when fmiDoStep returned fmiPending."
            " This is not the case.", fname)
            break;
        case fmi2LastSuccessfulTime: FILTERED_LOG(comp, fmi2Error, LOG_ERROR,
            "%s: Can be called with fmi2LastSuccessfulTime when fmiDoStep returned fmiDiscard."
            " This is not the case.", fname)
            break;
        case fmi2Terminated: FILTERED_LOG(comp, fmi2Error, LOG_ERROR,
            "%s: Can be called with fmi2Terminated when fmiDoStep returned fmiDiscard."
            " This is not the case.", fname)
            break;
    }
    return fmi2Error;
}

fmi2Status fmi2GetStatus(fmi2Component c, const fmi2StatusKind s, fmi2Status *value) {
    return getStatus("fmi2GetStatus", c, s);
}

fmi2Status fmi2GetRealStatus(fmi2Component c, const fmi2StatusKind s, fmi2Real *value) {
    if (s == fmi2LastSuccessfulTime) {
        ModelInstance *comp = (ModelInstance *)c;
        *value = comp->s.time;
        return fmi2OK;
    }
    return getStatus("fmi2GetRealStatus", c, s);
}

fmi2Status fmi2GetIntegerStatus(fmi2Component c, const fmi2StatusKind s, fmi2Integer *value) {
    return getStatus("fmi2GetIntegerStatus", c, s);
}

fmi2Status fmi2GetBooleanStatus(fmi2Component c, const fmi2StatusKind s, fmi2Boolean *value) {
    if (s == fmi2Terminated) {
        ModelInstance *comp = (ModelInstance *)c;
        *value = comp->s.eventInfo.terminateSimulation;
        return fmi2OK;
    }
    return getStatus("fmi2GetBooleanStatus", c, s);
}

fmi2Status fmi2GetStringStatus(fmi2Component c, const fmi2StatusKind s, fmi2String *value) {
    return getStatus("fmi2GetStringStatus", c, s);
}

// ---------------------------------------------------------------------------
// Functions for FMI for Model Exchange
// ---------------------------------------------------------------------------
#else

// array of value references of states
#if NUMBER_OF_STATES>0
static const fmi2ValueReference vrStates[NUMBER_OF_STATES] = STATES;
static const fmi2ValueReference vrDerivatives[NUMBER_OF_STATES] = DERIVATIVES;
#endif

/* Enter and exit the different modes */
fmi2Status fmi2EnterEventMode(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2EnterEventMode", MASK_fmi2EnterEventMode))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2EnterEventMode")

#ifdef HAVE_MODELDESCRIPTION_STRUCT
    comp->s.state = modelEventMode;
#endif
    return fmi2OK;
}

fmi2Status fmi2NewDiscreteStates(fmi2Component c, fmi2EventInfo *eventInfo) {
    ModelInstance *comp = (ModelInstance *)c;
    int timeEvent = 0;
    if (invalidState(comp, "fmi2NewDiscreteStates", MASK_fmi2NewDiscreteStates))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2NewDiscreteStates")

    comp->s.eventInfo.newDiscreteStatesNeeded = fmi2False;
    comp->s.eventInfo.terminateSimulation = fmi2False;
    comp->s.eventInfo.nominalsOfContinuousStatesChanged = fmi2False;
    comp->s.eventInfo.valuesOfContinuousStatesChanged = fmi2False;

    if (comp->s.eventInfo.nextEventTimeDefined && comp->s.eventInfo.nextEventTime <= comp->s.time) {
        timeEvent = 1;
    }
    eventUpdate(comp, &comp->s.eventInfo);

    // copy internal eventInfo of component to output eventInfo
    eventInfo->newDiscreteStatesNeeded = comp->s.eventInfo.newDiscreteStatesNeeded;
    eventInfo->terminateSimulation = comp->s.eventInfo.terminateSimulation;
    eventInfo->nominalsOfContinuousStatesChanged = comp->s.eventInfo.nominalsOfContinuousStatesChanged;
    eventInfo->valuesOfContinuousStatesChanged = comp->s.eventInfo.valuesOfContinuousStatesChanged;
    eventInfo->nextEventTimeDefined = comp->s.eventInfo.nextEventTimeDefined;
    eventInfo->nextEventTime = comp->s.eventInfo.nextEventTime;

    return fmi2OK;
}

fmi2Status fmi2EnterContinuousTimeMode(fmi2Component c) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2EnterContinuousTimeMode", MASK_fmi2EnterContinuousTimeMode))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL,"fmi2EnterContinuousTimeMode")

    comp->s.state = modelContinuousTimeMode;
    return fmi2OK;
}

fmi2Status fmi2CompletedIntegratorStep(fmi2Component c, fmi2Boolean noSetFMUStatePriorToCurrentPoint,
                                     fmi2Boolean *enterEventMode, fmi2Boolean *terminateSimulation) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2CompletedIntegratorStep", MASK_fmi2CompletedIntegratorStep))
        return fmi2Error;
    if (nullPointer(comp, "fmi2CompletedIntegratorStep", "enterEventMode", enterEventMode))
        return fmi2Error;
    if (nullPointer(comp, "fmi2CompletedIntegratorStep", "terminateSimulation", terminateSimulation))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL,"fmi2CompletedIntegratorStep")
    *enterEventMode = fmi2False;
    *terminateSimulation = fmi2False;
    return fmi2OK;
}

/* Providing independent variables and re-initialization of caching */
fmi2Status fmi2SetTime(fmi2Component c, fmi2Real time) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetTime", MASK_fmi2SetTime))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetTime: time=%.16g", time)
    comp->s.time = time;
    return fmi2OK;
}

fmi2Status fmi2SetContinuousStates(fmi2Component c, const fmi2Real x[], size_t nx){
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2SetContinuousStates", MASK_fmi2SetContinuousStates))
        return fmi2Error;
    if (invalidNumber(comp, "fmi2SetContinuousStates", "nx", nx, NUMBER_OF_STATES))
        return fmi2Error;
#if NUMBER_OF_STATES==0
    return fmi2OK;
#else
    if (nullPointer(comp, "fmi2SetContinuousStates", "x[]", x))
        return fmi2Error;
#ifdef HAVE_MODELDESCRIPTION_STRUCT
    return generated_fmi2SetReal(comp, &comp->s.md, vrStates, nx, x);
#else
    size_t i;
    for (i = 0; i < nx; i++) {
        fmi2ValueReference vr = vrStates[i];
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2SetContinuousStates: #r%d#=%.16g", vr, x[i])
        assert(vr < NUMBER_OF_REALS);
        comp->s.r[vr] = x[i];
    }
    return fmi2OK;
#endif
#endif
}

/* Evaluation of the model equations */
fmi2Status fmi2GetDerivatives(fmi2Component c, fmi2Real derivatives[], size_t nx) {
    ModelInstance* comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetDerivatives", MASK_fmi2GetDerivatives))
        return fmi2Error;
    if (invalidNumber(comp, "fmi2GetDerivatives", "nx", nx, NUMBER_OF_STATES))
        return fmi2Error;
#if NUMBER_OF_STATES == 0
    return fmi2OK;
#else
    if (nullPointer(comp, "fmi2GetDerivatives", "derivatives[]", derivatives))
        return fmi2Error;
#ifdef HAVE_MODELDESCRIPTION_STRUCT
    return generated_fmi2GetReal(comp, &comp->s.md, vrDerivatives, nx, derivatives);
#else
    size_t i;
    for (i = 0; i < nx; i++) {
        fmi2ValueReference vr = vrStates[i] + 1;
        derivatives[i] = getReal(comp, vr); // to be implemented by the includer of this file
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetDerivatives: #r%d# = %.16g", vr, derivatives[i])
    }
    return fmi2OK;
#endif
#endif
}

fmi2Status fmi2GetEventIndicators(fmi2Component c, fmi2Real eventIndicators[], size_t ni) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetEventIndicators", MASK_fmi2GetEventIndicators))
        return fmi2Error;
    if (invalidNumber(comp, "fmi2GetEventIndicators", "ni", ni, NUMBER_OF_EVENT_INDICATORS))
        return fmi2Error;
#if NUMBER_OF_EVENT_INDICATORS == 0
    return fmi2OK;
#else
    if (nullPointer(comp, "fmi2GetEventIndicateros", "eventIndicators[]", eventIndicators))
        return fmi2Error;
#ifdef HAVE_MODELDESCRIPTION_STRUCT
    return getEventIndicator(&comp->s.md, eventIndicators);
#else
    size_t i;
    for (i = 0; i < ni; i++) {
        eventIndicators[i] = getEventIndicator(comp, i); // to be implemented by the includer of this file
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetEventIndicators: z%d = %.16g", i, eventIndicators[i])
    }
    return fmi2OK;
#endif
#endif
}

fmi2Status fmi2GetContinuousStates(fmi2Component c, fmi2Real states[], size_t nx) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetContinuousStates", MASK_fmi2GetContinuousStates))
        return fmi2Error;
    if (invalidNumber(comp, "fmi2GetContinuousStates", "nx", nx, NUMBER_OF_STATES))
        return fmi2Error;
#if NUMBER_OF_STATES == 0
    return fmi2OK;
#else

    if (nullPointer(comp, "fmi2GetContinuousStates", "states[]", states))
        return fmi2Error;
#ifdef HAVE_MODELDESCRIPTION_STRUCT
    return generated_fmi2GetReal(comp, &comp->s.md, vrStates, nx, states);
#else
    size_t i;
    for (i = 0; i < nx; i++) {
        fmi2ValueReference vr = vrStates[i];
        states[i] = getReal(comp, vr); // to be implemented by the includer of this file
        FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetContinuousStates: #r%u# = %.16g", vr, states[i])
    }
    return fmi2OK;
#endif
#endif
}

fmi2Status fmi2GetNominalsOfContinuousStates(fmi2Component c, fmi2Real x_nominal[], size_t nx) {
    ModelInstance *comp = (ModelInstance *)c;
    if (invalidState(comp, "fmi2GetNominalsOfContinuousStates", MASK_fmi2GetNominalsOfContinuousStates))
        return fmi2Error;
    if (invalidNumber(comp, "fmi2GetNominalContinuousStates", "nx", nx, NUMBER_OF_STATES))
        return fmi2Error;
#if NUMBER_OF_STATES==0
    return fmi2OK;
#endif
    if (nullPointer(comp, "fmi2GetNominalContinuousStates", "x_nominal[]", x_nominal))
        return fmi2Error;
    FILTERED_LOG(comp, fmi2OK, LOG_FMI_CALL, "fmi2GetNominalContinuousStates: x_nominal[0..%d] = 1.0", nx-1)
    size_t i;
    for (i = 0; i < nx; i++)
        x_nominal[i] = 1;
    return fmi2OK;
}
#endif // Model Exchange
