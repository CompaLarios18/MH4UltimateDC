// The registry of the five fragment features (N2-33). Written for this package: nothing is copied from Azahar.
#include "gpu/fragment/feature.h"

namespace mh4n::gpu::fragment {

FeatureRegistry& FeatureRegistry::Global() {
    static FeatureRegistry registry;
    return registry;
}

bool FeatureRegistry::Add(const FeatureModule& feature) {
    const std::string_view name = feature.Name();
    for (size_t i = 0; i < kFeatureNames.size(); ++i) {
        if (kFeatureNames[i] == name) {
            if (features_[i] != nullptr) {
                return false;
            }
            features_[i] = &feature;
            Rebuild();
            return true;
        }
    }
    return false;
}

const FeatureModule* FeatureRegistry::Find(std::string_view name) const {
    for (size_t i = 0; i < kFeatureNames.size(); ++i) {
        if (kFeatureNames[i] == name) {
            return features_[i];
        }
    }
    return nullptr;
}

void FeatureRegistry::Rebuild() {
    count_ = 0;
    for (const FeatureModule* feature : features_) {
        if (feature != nullptr) {
            ordered_[count_++] = feature;
        }
    }
}

size_t StubbedFeatures(const FragmentKey& key, const FeatureRegistry& features, std::span<const char*> out) {
    size_t n = 0;
    for (const FeatureModule* feature : features.All()) {
        if (const char* what = feature->Stubbed(key); what != nullptr && n < out.size()) {
            out[n++] = what;
        }
    }
    return n;
}

namespace detail {

FeatureRegistrar::FeatureRegistrar(const FeatureModule& feature) {
    FeatureRegistry::Global().Add(feature);
}

} // namespace detail

} // namespace mh4n::gpu::fragment
