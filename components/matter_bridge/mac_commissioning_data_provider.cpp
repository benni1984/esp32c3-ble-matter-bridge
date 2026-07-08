#include "mac_commissioning_data_provider.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include <crypto/CHIPCryptoPAL.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "mac_cdp";

// Seed is persisted here (once generated) rather than re-derived from the MAC
// on every boot -- see the security note on deriveSeed() below.
static const char *kSeedNvsNamespace = "cdp_seed";
static const char *kSeedNvsKey = "seed";

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

    // SECURITY: the seed for the setup passcode/discriminator must NOT be
    // derivable from public information alone. It used to be a straight
    // FNV-1a hash of the device's MAC address — but the MAC is broadcast in
    // cleartext in every BLE advertisement and WiFi association, so anyone
    // who observes it could recompute the exact same passcode/discriminator
    // offline, no brute force needed. That defeated the entire point of a
    // per-device commissioning secret.
    //
    // Instead: generate a real random seed once (esp_random(), the ESP32's
    // hardware TRNG) and persist it in NVS, so it's stable across reboots
    // but not reconstructable from anything an attacker can observe over
    // the air. The MAC is only used as a last-resort fallback if NVS itself
    // is unavailable (better a predictable device than a bricked one).
    bool have_seed = false;
    nvs_handle_t h;
    if (nvs_open(kSeedNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(h, kSeedNvsKey, &stored) == ESP_OK) {
            m_seed = stored;
            have_seed = true;
        } else {
            uint32_t fresh = esp_random();
            if (nvs_set_u32(h, kSeedNvsKey, fresh) == ESP_OK && nvs_commit(h) == ESP_OK) {
                m_seed = fresh;
                have_seed = true;
                ESP_LOGI(TAG, "Generated and stored a new random commissioning seed");
            }
        }
        nvs_close(h);
    }

    if (!have_seed) {
        uint8_t mac[6] = {};
        esp_base_mac_addr_get(mac);
        m_seed = fnv1a(mac, 6);
        ESP_LOGW(TAG, "Could not read/store commissioning seed in NVS — falling back to "
                      "MAC-derived seed (predictable from the device's broadcast MAC, "
                      "less secure than the normal random-seed path)");
    }

    // Discriminator: upper 12 bits of seed (0–4095)
    m_disc = (m_seed >> 20) & 0xFFF;

    // Passcode: map seed to valid 8-digit range [10000000, 99999998]
    // Roll forward on forbidden values
    m_passcode = (m_seed % 89999998u) + 10000000u;
    uint32_t attempts = 0;
    while (isForbidden(m_passcode) && attempts++ < 12) {
        // NOTE: parenthesize explicitly -- `%` binds tighter than `&` in C++,
        // so without the inner parens this silently masked against
        // (0xFFFFFFFF % 89999998u) instead of computing "mask to 32 bits,
        // then reduce mod 89999998u" as intended, badly narrowing the range.
        uint64_t mixed = m_passcode * 6364136223846793005ull + 1442695040888963407ull;
        m_passcode = (uint32_t)((mixed & 0xFFFFFFFFull) % 89999998u) + 10000000u;
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
    // Derive a deterministic 32-byte salt from the seed. (Spake2+ salts are
    // public commissioning data, not secret by design -- this only needs to
    // be stable across boots, not unpredictable, unlike the seed itself.)
    //
    // Previously this read `fnv1a((uint8_t*)&m_seed + (i % 4), 4)` for i up
    // to 7 -- for i=1,2,3,5,6,7 that reads past the 4-byte m_seed variable's
    // bounds (out-of-bounds stack read), and separately did `m_seed >> (32 -
    // i)` which is undefined behavior in C++ when i=0 (shift by the full
    // bit width). Both are gone below: each of the 8 output words is now a
    // hash of an explicit, fully in-bounds 5-byte block (seed bytes + the
    // word index), so there's no pointer arithmetic past a variable's size
    // and no shift-by-the-type-width.
    deriveSeed();
    uint8_t raw[32];
    for (int i = 0; i < 8; i++) {
        uint8_t block[5] = {
            (uint8_t)(m_seed >> 24), (uint8_t)(m_seed >> 16),
            (uint8_t)(m_seed >> 8),  (uint8_t)(m_seed >> 0),
            (uint8_t)i,
        };
        uint32_t v = fnv1a(block, sizeof(block));
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
    (void) GetSetupPasscode(passcode);
    (void) GetSpake2pIterationCount(iterations);

    uint8_t saltRaw[32];
    chip::MutableByteSpan saltSpan(saltRaw, sizeof(saltRaw));
    (void) GetSpake2pSalt(saltSpan);

    chip::Crypto::Spake2pVerifier verifier;
    CHIP_ERROR err = verifier.Generate(iterations, saltSpan, passcode);
    if (err != CHIP_NO_ERROR) return err;

    verifierLen = chip::Crypto::kSpake2p_VerifierSerialized_Length;
    if (verifierBuf.size() < verifierLen) return CHIP_ERROR_BUFFER_TOO_SMALL;
    return verifier.Serialize(verifierBuf);
}
