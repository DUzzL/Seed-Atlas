#include "gpufilter.h"

// Systems without a GPU implementation: the filter is never enabled.

struct GpuClimateFilter::Impl {};

bool GpuClimateFilter::enabled() { return false; }
GpuClimateFilter::GpuClimateFilter() : d(nullptr) {}
GpuClimateFilter::~GpuClimateFilter() {}
bool GpuClimateFilter::init(const std::vector<Test>&, bool) { return false; }
bool GpuClimateFilter::filter(uint64_t, uint32_t, std::vector<uint64_t>&) { return false; }
