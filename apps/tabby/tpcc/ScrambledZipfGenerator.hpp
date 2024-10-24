#include "FNVHash.hpp"
#include "Types.hpp"
#include "ZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
class ScrambledZipfGenerator
{
  public:
   u64 min, max, n;
   double theta;
   ZipfGenerator zipf_generator;
   // 10000000000ul
   // [min, max)
   ScrambledZipfGenerator(u64 min, u64 max, double theta) : min(min), max(max), n(max - min), zipf_generator((max - min) * 2, theta) {}
   u64 rand();
};
// -------------------------------------------------------------------------------------

u64 ScrambledZipfGenerator::rand()
{
   u64 zipf_value = zipf_generator.rand();
   return min + (FNV::hash(zipf_value) % n);
}