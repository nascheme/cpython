#include "Python.h"
#include <stdbool.h>

static _Bool frozen_modules_initialized = false;
static _Bool frozen_modules_disable = false;
static PyObject* frozen_code_objects = NULL;


void _PyFrozenModules_Init(void) {
}

void _PyFrozenModules_Finalize(void) {
}

void _PyFrozenModules_Disable(void) {
    frozen_modules_disable = true;
}

void _PyFrozenModules_Enable(void) {
    frozen_modules_disable = false;
}


PyObject* _PyFrozenModule_Lookup(PyObject* name) {
    return NULL;
}

int _PyFrozenModules_ImportBootstrap(void)
{
    return 0;
}
