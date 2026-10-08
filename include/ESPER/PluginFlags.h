#pragma once
#include <cstdint>

namespace esper {

// Packs a 4-character record signature into a uint32_t (little-endian byte
// order), so a signature read off disk as a LE U32 compares == Sig4("NPC_").
constexpr uint32_t Sig4(const char s[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0]))
         | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

// Common record / group signatures. (Any signature can be formed inline with
// Sig4("XXXX"); these named constants cover the frequently compared ones.)
namespace sig {
    constexpr uint32_t TES4 = Sig4("TES4");
    constexpr uint32_t GRUP = Sig4("GRUP");
    constexpr uint32_t NPC_ = Sig4("NPC_");
    constexpr uint32_t RACE = Sig4("RACE");
    constexpr uint32_t ARMO = Sig4("ARMO");
    constexpr uint32_t ARMA = Sig4("ARMA");
    constexpr uint32_t HDPT = Sig4("HDPT");
    constexpr uint32_t TXST = Sig4("TXST");
    constexpr uint32_t WRLD = Sig4("WRLD");
    constexpr uint32_t CELL = Sig4("CELL");
    constexpr uint32_t REFR = Sig4("REFR");
    constexpr uint32_t ACHR = Sig4("ACHR");
    constexpr uint32_t LAND = Sig4("LAND");
    constexpr uint32_t NAVM = Sig4("NAVM");
    constexpr uint32_t LTEX = Sig4("LTEX");
}

// Record header flag bits (RecHeader::flags).
namespace rec_flag {
    constexpr uint32_t kMaster     = 0x00000001;
    constexpr uint32_t kDeleted    = 0x00000020;
    constexpr uint32_t kLocalized  = 0x00000080;
    constexpr uint32_t kLight      = 0x00000200;
    constexpr uint32_t kCompressed = 0x00040000;
}

} // namespace esper
