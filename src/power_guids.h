// Windows processor power-setting identifiers (Microsoft Learn "Processor power management options"). Kept as
// plain data with the same layout as GUID so a native unit test can check them against the documented strings.
#pragma once
#include <cstdint>

namespace cbf {

struct GuidBytes {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};

constexpr GuidBytes kGuidProcessorSubgroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};  // 54533251-82be-4824-96c1-47b60b740d00
constexpr GuidBytes kGuidMinProcessorState = {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};   // 893dee8e-2bef-41e0-89c6-b55d0929964c
constexpr GuidBytes kGuidMaxProcessorState = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};   // bc5038f7-23e0-4960-96da-33abaf5935ec
constexpr GuidBytes kGuidCoreParkingMinCores = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}}; // 0cc5b647-c1df-4637-891a-dec35c318583
constexpr GuidBytes kGuidPerfBoostMode = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};       // be337238-0d82-4146-a960-4f3749d470c7

}  // namespace cbf
