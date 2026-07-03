#pragma once

#include <platform/DeviceInfoProvider.h>

/**
 * Ws90DeviceInfoProvider
 *
 * esp-matter's newer per-cluster architecture backs the FixedLabel cluster
 * with chip::DeviceLayer::DeviceInfoProvider::IterateFixedLabel() — not an
 * esp_matter attribute — so distinguishing the WS90's five identical
 * FlowMeasurement endpoints (wind speed/direction, rain, UV, battery) in
 * Home Assistant requires a real FixedLabel entry per endpoint.
 *
 * HA's Matter integration only surfaces FixedLabel/UserLabel values whose
 * `label` field matches an allow-list keyed by (vendorId, productId) — see
 * home-assistant/core's VENDOR_LABELING_LIST. Our test VID/PID (0xFFF1 /
 * 0x8000) is on that list under the key "ha_entitylabel", so that's the
 * label name used here.
 *
 * UserLabel/locale/calendar are unused (no UserLabel cluster is ever
 * attached to any endpoint) — those overrides are inert stubs required only
 * because DeviceInfoProvider is a single monolithic interface.
 */
class Ws90DeviceInfoProvider : public chip::DeviceLayer::DeviceInfoProvider
{
public:
    // Called from matter_bridge's endpoint factory right after an endpoint is
    // created, to record the friendly name FixedLabel should report for it.
    static void RegisterFixedLabel(chip::EndpointId endpoint, const char *name);

    FixedLabelIterator *IterateFixedLabel(chip::EndpointId endpoint) override;
    UserLabelIterator *IterateUserLabel(chip::EndpointId endpoint) override;
    SupportedLocalesIterator *IterateSupportedLocales() override;
    SupportedCalendarTypesIterator *IterateSupportedCalendarTypes() override;

protected:
    CHIP_ERROR SetUserLabelLength(chip::EndpointId endpoint, size_t val) override;
    CHIP_ERROR GetUserLabelLength(chip::EndpointId endpoint, size_t &val) override;
    CHIP_ERROR SetUserLabelAt(chip::EndpointId endpoint, size_t index, const UserLabelType &userLabel) override;
    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId endpoint, size_t index) override;
};
