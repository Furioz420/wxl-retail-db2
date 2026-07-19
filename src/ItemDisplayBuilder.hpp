// Builds the equip extension's compact attachment/material index from retail DB2 relationships and SKINs.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "runtime/db2/ItemDisplayIndex.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wxl::scripts::retaildb2::displaybuilder
{
    struct DisplaySource
    {
        uint32_t id = 0;
        uint32_t inventoryType = 0;
        std::array<uint32_t, 2> modelResources{};
        std::array<uint32_t, 2> modelMaterials{};
    };

    struct ModelMaterialSource
    {
        uint32_t id = 0;
        uint32_t materialResource = 0;
        uint32_t textureType = 0;
        uint32_t modelIndex = 0;
    };

    using ReadAssetFn = bool (*)(const char* path, std::vector<uint8_t>& bytes);

    std::shared_ptr<wxl::runtime::db2::itemdisplay::Index> Build(
        const std::unordered_map<uint32_t, DisplaySource>& displays,
        const std::unordered_map<uint32_t, std::vector<uint32_t>>& modelFiles,
        const std::unordered_map<uint32_t, uint32_t>& componentModelPositions,
        const std::unordered_map<uint32_t, uint32_t>& textureFiles,
        const std::unordered_map<uint32_t, std::vector<ModelMaterialSource>>& modelMaterials,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& componentMaterials,
        const std::vector<uint32_t>& appearanceOrder,
        const std::unordered_map<uint32_t, std::string>& paths,
        ReadAssetFn readAsset);
}
