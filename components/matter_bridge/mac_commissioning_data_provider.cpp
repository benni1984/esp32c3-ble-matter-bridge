#include "mac_commissioning_data_provider.h"

#include "esp_mac.h"
#include "esp_log.h"

#include <crypto/CHIPCryptoPAL.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "mac_cdp";

// Forbidden Matter passcodes (spec §5.1.1.1)
static const uint32_t kForbidden[] = {
    00000000, 11111111, 22222222, 33333333,
    44444444, 55555555, 66666666, 77777777,
    88888888, 99999999, 12345678, 87654321
};

static bool isForbidden(uint32_t p) {
    for (auto f : kForbidden) if (p == f) return true;
    return false;
}

// FNV-1a 32-bit hash
static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void MacCommissionableDataProvider::deriveSeed()
{
    if (m_derived) return;

    // Always use the base MAC — available before WiFi init, always consistent
    uint8_t mac[6] = {};
    esp_base_mac_addr_get(mac);

    ESP_LOGI(TAG, "Device MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    m_seed = fnv1a(mac, 6);

    // Discriminator: upper 12 bits of seed (0–4095)
    m_disc = (m_seed >> 20) & 0xFFF;

    // Passcode: map seed to valid 8-digit range [10000000, 99999998]
    // Roll forward on forbidden values
    m_passcode = (m_seed % 89999998u) + 10000000u;
    uint32_t attempts = 0;
    while (isForbidden(m_passcode) && attempts++ < 12) {
        m_passcode = ((m_passcode * 6364136223846793005ull + 1442695040888963407ull) & 0xFFFFFFFF
                      % 89999998u) + 10000000u;
    }

    ESP_LOGI(TAG, "Derived passcode=%lu discriminator=%u", m_passcode, m_disc);
    m_derived = true;
}

CHIP_ERROR MacCommissionableDataProvider::GetSetupPasscode(uint32_t &passcode)
{
    deriveSeed();
    passcode = m_passcode;
    return CHIP_NO_ERROR;
}

CHIP_ERROR MacCommissionableDataProvider::GetSetupDiscriminator(uint16_t &discriminator)
{
    deriveSeed();
    discriminator = m_disc;
    return CHIP_NO_ERROR;
}

CHIP_ERROR MacCommissionableDataProvider::GetSpake2pIterationCount(uint32_t &iterationCount)
{
    iterationCount = 1000;
    return CHIP_NO_ERROR;
}

CHIP_ERROR MacCommissionableDataProvider::GetSpake2pSalt(chip::MutableByteSpan &saltBuf)
{
    // Derive a deterministic 32-byte salt from the MAC seed
    deriveSeed();
    uint8_t raw[32];
    for (int i = 0; i < 8; i++) {
        uint32_t v = fnv1a((uint8_t *)&m_seed + (i % 4), 4) ^ (m_seed << i) ^ (m_seed >> (32 - i));
        raw[i*4+0] = (v >> 24) & 0xFF;
        raw[i*4+1] = (v >> 16) & 0xFF;
        raw[i*4+2] = (v >>  8) & 0xFF;
        raw[i*4+3] = (v >>  0) & 0xFF;
    }
    if (saltBuf.size() < 32) return CHIP_ERROR_BUFFER_TOO_SMALL;
    memcpy(saltBuf.data(), raw, 32);
    saltBuf.reduce_size(32);
    return CHIP_NO_ERROR;
}

CHIP_ERROR MacCommissionableDataProvider::GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf,
                                                              size_t &verifierLen)
{
    // Generate verifier from passcode + salt using SPAKE2+ KDF
    deriveSeed();

    uint32_t passcode    = 0;
    uint32_t iterations  = 0;
    GetSetupPasscode(passcode);
    GetSpake2pIterationCount(iterations);

    uint8_t saltRaw[32];
    chip::MutableByteSpan saltSpan(saltRaw, sizeof(saltRaw));
    GetSpake2pSalt(saltSpan);

    chip::Crypto::Spake2pVerifier verifier;
    CHIP_ERROR err = verifier.Generate(iterations, saltSpan, passcode);
    if (err != CHIP_NO_ERROR) return err;

    verifierLen = chip::Crypto::kSpake2p_VerifierSerialized_Length;
    if (verifierBuf.size() < verifierLen) return CHIP_ERROR_BUFFER_TOO_SMALL;
    return verifier.Serialize(verifierBuf);
}
