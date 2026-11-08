// Platforms without a GPU backend: the prefilter is never available.

#include "gpufilter_backend.h"

bool gpuBackendAvailable() { return false; }
GpuBackend *gpuBackendCreate() { return nullptr; }
