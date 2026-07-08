#pragma once

#include <platform/CommissionableDataProvider.h>
#include <lib/core/CHIPError.h>

/**
 * MacCommissionableDataProvider
 *
 * Gives every flashed ESP32 its own unique passcode and discriminator (QR
 * code), derived from a random seed that's generated once on first boot and
 * persisted in NVS (namespace "cdp_seed") — NOT from the device's MAC
 * address. An earlier version hashed the MAC directly, which is broadcast in
 * cleartext during BLE advertising/WiFi association and so let anyone who
 * observed it recompute the same passcode offline; that's why this is
 * NVS-backed now instead of purely reproducible from public info. The MAC
 * hash is kept only as a fallback if NVS itself is unavailable.
 *
 * Algorithm, given the seed:
 *   passcode = (seed % 89999998) + 10000000   → 8 digits, valid range
 *              re-rolled if it hits a forbidden value
 *   disc     = (seed >> 20) & 0xFFF           → 12 bits, 0–4095
 */
class MacCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t & discriminator) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t discriminator) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t & iterationCount) override;
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan & saltBuf) override;
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan & verifierBuf, size_t & verifierLen) override;
    CHIP_ERROR GetSetupPasscode(uint32_t & passcode) override;
    CHIP_ERROR SetSetupPasscode(uint32_t passcode) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    void     deriveSeed();
    bool     m_derived   = false;
    uint32_t m_seed      = 0;
    uint32_t m_passcode  = 0;
    uint16_t m_disc      = 0;
};
