#include "Types.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
class FNV
{
  private:
   static constexpr u64 FNV_OFFSET_BASIS_64 = 0xCBF29CE484222325L;
   static constexpr u64 FNV_PRIME_64 = 1099511628211L;

  public:
   static u64 hash(u64 val);
};
// -------------------------------------------------------------------------------------

u64 FNV::hash(u64 val)
{
   // from http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash
   u64 hash_val = FNV_OFFSET_BASIS_64;
   for (int i = 0; i < 8; i++) {
      u64 octet = val & 0x00ff;
      val = val >> 8;

      hash_val = hash_val ^ octet;
      hash_val = hash_val * FNV_PRIME_64;
   }
   return hash_val;
}