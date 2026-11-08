#include "gpufilter.h"
#include "search.h"

#include <climits>

std::vector<GpuClimateFilter::Test> GpuClimateFilter::analyze(const ConditionTree& tree, int mc)
{
    std::vector<Test> tests;
    if (mc < MC_1_18 || tree.condvec.empty() || tree.references.empty())
        return tests;
    // The direct children of the root condition are combined with AND and
    // are tested at the origin, so each of them has to pass on its own.
    for (char b : tree.references[0])
    {
        const Condition& c = tree.condvec[(int)b];
        if (c.type != F_CLIMATE_NOISE)
            continue;
        // the area as in testCondAt() at the origin
        int x1 = c.x1, z1 = c.z1, x2 = c.x2, z2 = c.z2;
        if (c.rmax > 0)
        {
            x1 = z1 = -(c.rmax - 1);
            x2 = z2 = c.rmax - 1;
        }
        int rx1 = x1 >> 2, rz1 = z1 >> 2;
        int rx2 = x2 >> 2, rz2 = z2 >> 2;
        // the order of the CPU test, which puts the cheap noises first
        const int order[] = {
            NP_TEMPERATURE, NP_HUMIDITY, NP_WEIRDNESS, NP_EROSION, NP_CONTINENTALNESS,
        };
        for (int i : order)
        {
            if (c.limok[i][0] == INT_MIN && c.limok[i][1] == INT_MAX)
                continue;
            Test t;
            t.para = i;
            t.x = rx1;
            t.z = rz1;
            t.w = rx2 - rx1 + 1;
            t.h = rz2 - rz1 + 1;
            t.lmin = c.limok[i][0];
            t.lmax = c.limok[i][1];
            if (t.w > 0 && t.h > 0)
                tests.push_back(t);
        }
    }
    return tests;
}
