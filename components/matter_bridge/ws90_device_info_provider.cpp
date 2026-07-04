#include "ws90_device_info_provider.h"

#include "esp_log.h"

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

static const char *TAG = "ws90_info";

constexpr size_t kMaxRegisteredLabels = 12;
constexpr const char *kLabelKey = "ha_entitylabel";

struct FixedLabelEntry {
    EndpointId endpoint = kInvalidEndpointId;
    const char *name = nullptr;
};

FixedLabelEntry s_labels[kMaxRegisteredLabels];
size_t s_label_count = 0;

const char *find_label(EndpointId endpoint)
{
    for (size_t i = 0; i < s_label_count; i++) {
        if (s_labels[i].endpoint == endpoint) return s_labels[i].name;
    }
    return nullptr;
}

// FixedLabel values are compile-time string literals (static storage
// duration) registered via RegisterFixedLabel(), so CharSpans referencing
// them stay valid for the process lifetime — no per-iterator buffer needed.
class FixedLabelIteratorImpl : public DeviceInfoProvider::FixedLabelIterator
{
public:
    FixedLabelIteratorImpl(EndpointId endpoint, const char *name) : mName(name)
    {
        (void)endpoint;
    }
    size_t Count() override { return mName ? 1 : 0; }
    bool Next(DeviceInfoProvider::FixedLabelType &output) override
    {
        if (!mName || mIndex != 0) return false;
        output.label = CharSpan::fromCharString(kLabelKey);
        output.value = CharSpan::fromCharString(mName);
        mIndex++;
        ESP_LOGI(TAG, "IterateFixedLabel::Next served label=%s value=%s", kLabelKey, mName);
        return true;
    }
    void Release() override { chip::Platform::Delete(this); }

private:
    const char *mName;
    size_t mIndex = 0;
};

template <typename T>
class EmptyIteratorImpl : public DeviceInfoProvider::Iterator<T>
{
public:
    size_t Count() override { return 0; }
    bool Next(T & /*item*/) override { return false; }
    void Release() override { chip::Platform::Delete(this); }
};

} // namespace

void Ws90DeviceInfoProvider::RegisterFixedLabel(EndpointId endpoint, const char *name)
{
    if (s_label_count >= kMaxRegisteredLabels) return;
    s_labels[s_label_count++] = { endpoint, name };
    ESP_LOGI(TAG, "RegisterFixedLabel: ep%u -> %s (registered %u total)",
             endpoint, name, (unsigned)s_label_count);
}

DeviceInfoProvider::FixedLabelIterator *Ws90DeviceInfoProvider::IterateFixedLabel(EndpointId endpoint)
{
    const char *name = find_label(endpoint);
    ESP_LOGI(TAG, "IterateFixedLabel called for ep%u -> %s", endpoint, name ? name : "(none registered)");
    return chip::Platform::New<FixedLabelIteratorImpl>(endpoint, name);
}

DeviceInfoProvider::UserLabelIterator *Ws90DeviceInfoProvider::IterateUserLabel(EndpointId /*endpoint*/)
{
    return chip::Platform::New<EmptyIteratorImpl<DeviceInfoProvider::UserLabelType>>();
}

DeviceInfoProvider::SupportedLocalesIterator *Ws90DeviceInfoProvider::IterateSupportedLocales()
{
    return chip::Platform::New<EmptyIteratorImpl<CharSpan>>();
}

DeviceInfoProvider::SupportedCalendarTypesIterator *Ws90DeviceInfoProvider::IterateSupportedCalendarTypes()
{
    return chip::Platform::New<EmptyIteratorImpl<DeviceInfoProvider::CalendarType>>();
}

// UserLabel is never attached to any endpoint in this app, so these are never
// actually invoked by the Matter stack — safe not-implemented stubs.
CHIP_ERROR Ws90DeviceInfoProvider::SetUserLabelLength(EndpointId /*endpoint*/, size_t /*val*/)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR Ws90DeviceInfoProvider::GetUserLabelLength(EndpointId /*endpoint*/, size_t &val)
{
    val = 0;
    return CHIP_NO_ERROR;
}

CHIP_ERROR Ws90DeviceInfoProvider::SetUserLabelAt(EndpointId /*endpoint*/, size_t /*index*/,
                                                  const UserLabelType & /*userLabel*/)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR Ws90DeviceInfoProvider::DeleteUserLabelAt(EndpointId /*endpoint*/, size_t /*index*/)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}
