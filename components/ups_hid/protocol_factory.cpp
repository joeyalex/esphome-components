#include "protocol_factory.h"
#include "ups_hid.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace ups_hid {

// Forward declarations of each protocol's creator function. These are defined
// (not just declared) in protocol_apc.cpp / protocol_cyberpower.cpp /
// protocol_generic.cpp, and referenced from THOSE files' own
// REGISTER_UPS_PROTOCOL_FOR_VENDOR/REGISTER_UPS_FALLBACK_PROTOCOL macro
// invocations to build each protocol's self-registering static object.
//
// The problem: this component is built into a static archive (.a), and
// nothing outside protocol_apc.cpp/protocol_cyberpower.cpp/protocol_generic.cpp
// ever references any symbol those files define — their creator functions'
// addresses are only ever taken from *within* their own file, by their own
// registrar constructor. Static-library linking pulls in an object file from
// an archive only when something *outside* that archive member has an
// unresolved reference into it; with no such reference, the linker's archive
// scan never has a reason to extract these .o files at all, so their
// self-registering static objects — and therefore their whole protocol
// registration — silently never happen. This is a different (and earlier)
// problem than ordinary dead-code elimination: __attribute__((used)) alone
// does not fix it, because it only protects code that is already part of the
// link, and these files are never part of the link in the first place.
//
// The fix is the force_link_protocols array below: a genuine external
// reference, from this file (which the archive linker *does* need, since
// ups_hid.cpp and others call into it), into each protocol file. That forces
// the linker to pull in protocol_apc.o/protocol_cyberpower.o/protocol_generic.o
// after all, letting their registrar constructors run normally.
std::unique_ptr<UpsProtocolBase> create_apc_protocol(UpsHidComponent* parent);
std::unique_ptr<UpsProtocolBase> create_cyberpower_protocol(UpsHidComponent* parent);
std::unique_ptr<UpsProtocolBase> create_generic_protocol(UpsHidComponent* parent);

namespace {
// The array itself also needs __attribute__((used)): nothing reads it either,
// so without this it would just be link-time dead weight that --gc-sections
// is free to discard, silently undoing the whole point of having it.
using ProtocolCreatorFn = std::unique_ptr<UpsProtocolBase> (*)(UpsHidComponent*);
static const ProtocolCreatorFn force_link_protocols[] __attribute__((used)) = {
    &create_apc_protocol,
    &create_cyberpower_protocol,
    &create_generic_protocol,
};
}  // namespace

static const char *const FACTORY_TAG = "ups_hid.factory";

// Static registry implementations
std::unordered_map<uint16_t, std::vector<ProtocolFactory::ProtocolInfo>>& 
ProtocolFactory::get_vendor_registry() {
    static std::unordered_map<uint16_t, std::vector<ProtocolInfo>> vendor_registry;
    return vendor_registry;
}

std::vector<ProtocolFactory::ProtocolInfo>& 
ProtocolFactory::get_fallback_registry() {
    static std::vector<ProtocolInfo> fallback_registry;
    return fallback_registry;
}

void ProtocolFactory::ensure_initialized() {
    // Registries are initialized on first access due to static storage
    // (Meyer's singleton pattern in get_vendor_registry()/get_fallback_registry()).
    //
    // IMPORTANT: this function is called from protocol registrars' global
    // static constructors — running during do_global_ctors(), before
    // app_main() — via register_protocol_for_vendor()/register_fallback_protocol().
    // ESPHome's Logger is itself a separately-constructed global object, and
    // C++ gives no guarantee about initialization order between global
    // objects defined in different translation units. Calling into Logger
    // (even just ESP_LOGD) from here is unsafe: if Logger's internal
    // std::map hasn't been constructed yet, this crashes with LoadProhibited
    // (confirmed by a real device crash log — level_for() dereferencing an
    // uninitialized std::map's internal tree pointers).
    //
    // So: no logging here, ever. This function intentionally does nothing.
}

void ProtocolFactory::register_protocol_for_vendor(uint16_t vendor_id, 
                                                  const ProtocolInfo& info) {
    // NOTE: this runs during static initialization (called from protocol
    // registrars' global constructors, before app_main()/Logger exist) — see
    // the comment on ensure_initialized(). No ESP_LOG* calls in this
    // function, ever.
    ensure_initialized();
    
    auto& registry = get_vendor_registry();
    registry[vendor_id].push_back(info);
    
    // Sort by priority (higher first)
    std::sort(registry[vendor_id].begin(), registry[vendor_id].end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

void ProtocolFactory::register_fallback_protocol(const ProtocolInfo& info) {
    // NOTE: same static-initialization-time constraint as
    // register_protocol_for_vendor() above — no ESP_LOG* calls here.
    ensure_initialized();
    
    auto& registry = get_fallback_registry();
    registry.push_back(info);
    
    // Sort by priority (higher first)
    std::sort(registry.begin(), registry.end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_for_vendor(uint16_t vendor_id, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }
    
    // Try vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        ESP_LOGD(FACTORY_TAG, "Found %zu vendor-specific protocols for 0x%04X", 
                 vendor_it->second.size(), vendor_id);
        
        for (const auto& info : vendor_it->second) {
            ESP_LOGD(FACTORY_TAG, "Trying vendor protocol '%s' for 0x%04X", 
                     info.name.c_str(), vendor_id);
            
            auto protocol = info.creator(parent);
            if (protocol && protocol->detect()) {
                ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' for vendor 0x%04X", 
                         info.name.c_str(), vendor_id);
                return protocol;
            }
        }
    }
    
    // Try fallback protocols
    auto& fallback_registry = get_fallback_registry();
    ESP_LOGD(FACTORY_TAG, "Trying %zu fallback protocols for vendor 0x%04X", 
             fallback_registry.size(), vendor_id);
    
    for (const auto& info : fallback_registry) {
        ESP_LOGD(FACTORY_TAG, "Trying fallback protocol '%s' for 0x%04X", 
                 info.name.c_str(), vendor_id);
        
        auto protocol = info.creator(parent);
        if (protocol && protocol->detect()) {
            ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' for vendor 0x%04X", 
                     info.name.c_str(), vendor_id);
            return protocol;
        }
    }
    
    ESP_LOGW(FACTORY_TAG, "No suitable protocol found for vendor 0x%04X", vendor_id);
    return nullptr;
}

std::vector<ProtocolFactory::ProtocolInfo> 
ProtocolFactory::get_protocols_for_vendor(uint16_t vendor_id) {
    ensure_initialized();
    
    std::vector<ProtocolInfo> protocols;
    
    // Add vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        for (const auto& info : vendor_it->second) {
            protocols.push_back(info);
        }
    }
    
    // Add fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        protocols.push_back(info);
    }
    
    return protocols;
}

std::vector<std::pair<uint16_t, ProtocolFactory::ProtocolInfo>> 
ProtocolFactory::get_all_protocols() {
    ensure_initialized();
    
    std::vector<std::pair<uint16_t, ProtocolInfo>> all_protocols;
    
    // Add vendor-specific protocols
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        uint16_t vendor_id = vendor_pair.first;
        for (const auto& info : vendor_pair.second) {
            all_protocols.emplace_back(vendor_id, info);
        }
    }
    
    // Add fallback protocols (use 0x0000 as special vendor ID for fallbacks)
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        all_protocols.emplace_back(0x0000, info);
    }
    
    return all_protocols;
}

bool ProtocolFactory::has_vendor_support(uint16_t vendor_id) {
    ensure_initialized();
    
    auto& vendor_registry = get_vendor_registry();
    auto it = vendor_registry.find(vendor_id);
    
    // Has support if vendor-specific protocols exist OR fallback protocols exist
    bool has_vendor_specific = (it != vendor_registry.end() && !it->second.empty());
    bool has_fallback = !get_fallback_registry().empty();
    
    return has_vendor_specific || has_fallback;
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_by_name(const std::string& protocol_name, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }
    
    ESP_LOGD(FACTORY_TAG, "Creating protocol by name: %s", protocol_name.c_str());
    
    // Search through all registered protocols to find one with matching name
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        for (const auto& info : vendor_pair.second) {
            // Match protocol name (case-insensitive)
            std::string info_name_lower = info.name;
            std::string protocol_name_lower = protocol_name;
            std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
            std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
            
            if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
                ESP_LOGD(FACTORY_TAG, "Found matching protocol '%s' for name '%s'", 
                         info.name.c_str(), protocol_name.c_str());
                auto protocol = info.creator(parent);
                if (protocol) {
                    ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' by name", 
                             protocol->get_protocol_name().c_str());
                    return protocol;
                }
            }
        }
    }
    
    // Search through fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        std::string info_name_lower = info.name;
        std::string protocol_name_lower = protocol_name;
        std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
        std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
        
        if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
            ESP_LOGD(FACTORY_TAG, "Found matching fallback protocol '%s' for name '%s'", 
                     info.name.c_str(), protocol_name.c_str());
            auto protocol = info.creator(parent);
            if (protocol) {
                ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' by name", 
                         protocol->get_protocol_name().c_str());
                return protocol;
            }
        }
    }
    
    ESP_LOGE(FACTORY_TAG, "No protocol found with name containing '%s'", protocol_name.c_str());

    // --- Diagnostic dump: show exactly what IS in the registry at this point,
    // so we can tell apart "registry is empty" (registration never ran) from
    // "registry has entries but none matched" (a real logic bug) instead of
    // guessing from the absence of other log lines.
    ESP_LOGE(FACTORY_TAG, "Registry diagnostic: %zu vendor(s) registered, %zu fallback protocol(s) registered",
             vendor_registry.size(), fallback_registry.size());
    for (const auto& vendor_pair : vendor_registry) {
        ESP_LOGE(FACTORY_TAG, "  vendor 0x%04X: %zu protocol(s)", vendor_pair.first, vendor_pair.second.size());
        for (const auto& info : vendor_pair.second) {
            ESP_LOGE(FACTORY_TAG, "    - '%s'", info.name.c_str());
        }
    }
    for (const auto& info : fallback_registry) {
        ESP_LOGE(FACTORY_TAG, "  fallback: '%s'", info.name.c_str());
    }

    return nullptr;
}

} // namespace ups_hid
} // namespace esphome
