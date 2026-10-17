// The IRuntime for Azahar's core (runtime.h). Nothing here includes Azahar: the implementation
// files in this folder do.
#pragma once

#include <memory>

#include "runtime.h"

namespace App::Azahar {

class AzaharRuntime final : public IRuntime {
public:
    AzaharRuntime();
    ~AzaharRuntime() override;

    std::string UserDir() const override;
    void SetUserDir(const std::string& dir) override;
    std::string SdmcDir() const override;
    std::string GameOptionsPath() const override;
    std::string GameOptionsTemplate() const override;

    BuildInfo Build() const override;
    void StartLogging(const std::string& file_name, const std::string& filter) override;
    void StopLogging() override;
    void LogOptions() override;
};

} // namespace App::Azahar
