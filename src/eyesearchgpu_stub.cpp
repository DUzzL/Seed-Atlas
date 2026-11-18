// Platforms without a GPU backend: the GPU eye search is never available.

#include "eyesearchgpu_backend.h"

bool gpuEyeBackendAvailable() { return false; }
GpuEyeBackend *gpuEyeBackendCreate() { return nullptr; }
