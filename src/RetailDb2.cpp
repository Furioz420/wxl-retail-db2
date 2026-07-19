// Retail item DB2 compatibility: native DBC first, real WDC5 DB2 fallback on lookup miss.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ItemDisplayBuilder.hpp"
#include "Schemas.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "game/Binding.hpp"
#include "game/io/Io.hpp"
#include "offsets/engine/Io.hpp"
#include "offsets/engine/Lua.hpp"
#include "offsets/game/DB2.hpp"
#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"
#include "runtime/db2/Db2.hpp"
#include "runtime/db2/ItemDisplayIndex.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wxl::scripts::retaildb2
{
    namespace
    {
        namespace db2 = wxl::offsets::game::db2;
        namespace io = wxl::game::io;
        namespace iooff = wxl::offsets::engine::io;
        namespace luaoff = wxl::offsets::engine::lua;
        namespace runtimeDb2 = wxl::runtime::db2;
        using Row = runtimeDb2::wdc5::Row;
        using Table = runtimeDb2::Table;

        constexpr size_t kWotlkItemRecordSize = 32;
        constexpr size_t kWotlkDisplayRecordSize = 100;
        constexpr char kEmpty[] = "";

        using LookupFn = db2::itemdisplayinfo::LookupFn;
        LookupFn g_originalLookup = nullptr;
        using InventoryArtFn = const char* (__cdecl*)(uint32_t displayId);
        InventoryArtFn g_originalInventoryArt = nullptr;
        using ItemInventoryArtFn = const char* (__fastcall*)(void* item, void* edx);
        ItemInventoryArtFn g_originalItemInventoryArt = nullptr;
        using ScriptGetItemIconFn = int (__cdecl*)(void* state);
        ScriptGetItemIconFn g_originalScriptGetItemIcon = nullptr;
        using GetInventoryTypeFn = uint32_t (__fastcall*)(void* item, void* edx);
        GetInventoryTypeFn g_originalGetInventoryType = nullptr;
        using CanGoInSlotFn = uint32_t (__fastcall*)(void* item, void* edx, uint32_t slot, uint32_t flags);
        CanGoInSlotFn g_originalCanGoInSlot = nullptr;
        std::atomic<bool> g_ready{false};
        // Item/ItemDisplayInfo relationships are complete before the slower SKIN/material build.
        // Glue character models may safely use those accessor fallbacks as soon as this is published.
        std::atomic<bool> g_lookupReady{false};
        std::atomic<bool> g_failed{false};

        constexpr uintptr_t kInventoryArt = 0x0070A910;
        constexpr uintptr_t kItemInventoryArt = 0x0070AA00;
        constexpr uintptr_t kScriptGetItemIcon = 0x00517020;
        constexpr uintptr_t kGetInventoryType = 0x00707280;
        constexpr uintptr_t kCanGoInSlot = 0x00708500;
        constexpr char kQuestionMarkIcon[] = "INV_Misc_QuestionMark";

        struct ItemBase
        {
            uint32_t classId = 0;
            uint32_t subclassId = 0;
            uint32_t soundOverride = 0;
            uint32_t material = 0;
            uint32_t inventoryType = 0;
            uint32_t sheatheType = 0;
            uint32_t displayId = 0;
            uint32_t appearanceId = 0;
            uint32_t iconFileDataId = 0;
            std::string icon;
        };

        struct Display
        {
            uint32_t id = 0;
            uint32_t flags = 0;
            uint32_t itemVisual = 0;
            uint32_t particleColor = 0;
            std::array<uint32_t, 6> geosets{};
            std::array<uint32_t, 2> helmetVis{};
            std::array<std::string, 2> models;
            std::array<std::string, 2> modelTextures;
            std::array<std::string, 2> icons;
            std::array<std::string, 8> componentTextures;
        };

        struct DisplayRaw
        {
            uint32_t id = 0;
            uint32_t itemVisual = 0;
            uint32_t particleColor = 0;
            uint32_t flags = 0;
            std::array<uint32_t, 2> modelResources{};
            std::array<uint32_t, 2> modelMaterials{};
            std::array<uint32_t, 6> geosets{};
            std::array<uint32_t, 2> helmetVis{};
        };

        struct Appearance
        {
            uint32_t displayId = 0;
            uint32_t iconFileDataId = 0;
            uint32_t displayType = 0;
            uint32_t uiOrder = 0;
        };

        uint32_t InventoryTypeFromDisplayType(uint32_t displayType)
        {
            switch (displayType)
            {
                case 0: return 1;  // Head
                case 1: return 3;  // Shoulder
                case 3: return 5;  // Chest
                case 4: return 6;  // Waist
                case 5: return 7;  // Legs
                case 6: return 8;  // Feet
                case 7: return 9;  // Wrist
                case 8: return 10; // Hands
                case 9: return 16; // Cloak
                default: return 0;
            }
        }

        int InventoryTypeRank(uint32_t inventoryType)
        {
            static constexpr std::array<uint32_t, 12> order{20, 5, 10, 7, 8, 6, 9, 4, 19, 3, 1, 16};
            const auto it = std::ranges::find(order, inventoryType);
            return it == order.end() ? static_cast<int>(order.size())
                                     : static_cast<int>(it - order.begin());
        }

        void PreferInventoryType(std::unordered_map<uint32_t, uint32_t>& types,
                                 uint32_t displayId, uint32_t inventoryType)
        {
            if (!displayId || !inventoryType) return;
            auto [it, inserted] = types.try_emplace(displayId, inventoryType);
            if (!inserted && InventoryTypeRank(inventoryType) < InventoryTypeRank(it->second))
                it->second = inventoryType;
        }

        std::unordered_map<uint32_t, ItemBase> g_items;
        std::unordered_map<uint32_t, Display> g_displays;
        std::unordered_map<uint32_t, std::string> g_fileDataPaths;
        void** g_itemIdTable = nullptr;
        void** g_displayIdTable = nullptr;
        uint8_t* g_itemRecords = nullptr;
        uint8_t* g_displayRecords = nullptr;

        bool ReadWholeFile(const char* path, std::vector<uint8_t>& bytes)
        {
            bytes.clear();
            auto readLoose = [&bytes](const std::string& loosePath) {
                std::ifstream stream(loosePath, std::ios::binary);
                if (!stream) return false;
                bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
                return !bytes.empty();
            };
            if (readLoose(path)) return true;
            std::string openPatchPath = "Data\\Patch-Z.MPQ\\";
            openPatchPath += path;
            if (readLoose(openPatchPath)) return true;

            void* handle = nullptr;
            if (!io::FileOpen(path, iooff::kOpenWholeFile, &handle) || !handle) return false;
            uint32_t high = 0;
            const uint32_t size = io::FileSize(handle, &high);
            if (!size || high)
            {
                io::FileClose(handle);
                return false;
            }
            bytes.resize(size);
            uint32_t got = 0;
            const bool ok = io::FileRead(handle, bytes.data(), size, &got) != 0 && got == size;
            io::FileClose(handle);
            if (!ok) bytes.clear();
            return ok;
        }

        bool LoadTable(const runtimeDb2::Definition& definition, Table& table)
        {
            std::string error;
            if (!table.Load(definition, &error))
            {
                WLOG_WARN("retail-db2: %.*s: %s", int(definition.name.size()), definition.name.data(),
                          error.c_str());
                return false;
            }
            WLOG_INFO("retail-db2: decoded %.*s rows=%zu layout=0x%08X",
                      int(definition.name.size()), definition.name.data(), table.Rows().size(),
                      table.LayoutHash());
            return true;
        }

        std::string Lower(std::string value)
        {
            for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return value;
        }

        std::string BasenameStem(std::string path, bool model)
        {
            std::replace(path.begin(), path.end(), '/', '\\');
            const size_t slash = path.find_last_of('\\');
            if (slash != std::string::npos) path.erase(0, slash + 1);
            const size_t dot = path.find_last_of('.');
            if (dot != std::string::npos) path.erase(dot);
            if (model) path += ".mdx";
            return path;
        }

        std::string ComponentStem(std::string path)
        {
            std::string stem = BasenameStem(std::move(path), false);
            const std::string lower = Lower(stem);
            if (lower.size() > 2 && (lower.ends_with("_m") || lower.ends_with("_f")))
                stem.resize(stem.size() - 2);
            return stem;
        }

        uint32_t ReadU32(const uint8_t* data)
        {
            uint32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        bool ParseWdc1PathTable(const char* filename, const std::unordered_set<uint32_t>& wanted,
                                std::unordered_map<uint32_t, std::string>& paths)
        {
            constexpr uint32_t kWdc1Magic = 0x31434457u;
            constexpr size_t kHeaderSize = 84;
            std::vector<uint8_t> bytes;
            std::string path = "DBFilesClient\\";
            path += filename;
            if (!ReadWholeFile(path.c_str(), bytes))
            {
                WLOG_WARN("retail-db2: %s is missing", path.c_str());
                return false;
            }

            if (bytes.size() < kHeaderSize || ReadU32(bytes.data()) != kWdc1Magic)
            {
                WLOG_WARN("retail-db2: %s is not a DB2Gen WDC1 path table", filename);
                return false;
            }

            const uint32_t recordCount = ReadU32(bytes.data() + 4);
            const uint32_t fieldCount = ReadU32(bytes.data() + 8);
            const uint32_t recordSize = ReadU32(bytes.data() + 12);
            const uint32_t stringSize = ReadU32(bytes.data() + 16);
            const uint32_t fieldStorageSize = ReadU32(bytes.data() + 68);
            if (fieldCount != 2 || recordSize != 8 || fieldStorageSize != 48)
            {
                WLOG_WARN("retail-db2: %s has an unsupported DB2Gen layout", filename);
                return false;
            }

            const size_t recordsOffset = kHeaderSize + static_cast<size_t>(fieldCount) * 4;
            if (recordCount > ((std::numeric_limits<size_t>::max)() - recordsOffset) / recordSize)
            {
                WLOG_WARN("retail-db2: %s record block overflows", filename);
                return false;
            }
            const size_t stringsOffset = recordsOffset + static_cast<size_t>(recordCount) * recordSize;
            if (stringsOffset > bytes.size() || stringSize > bytes.size() - stringsOffset ||
                fieldStorageSize > bytes.size() - stringsOffset - stringSize)
            {
                WLOG_WARN("retail-db2: %s is truncated", filename);
                return false;
            }

            const uint8_t* strings = bytes.data() + stringsOffset;
            size_t resolved = 0;
            for (uint32_t i = 0; i < recordCount; ++i)
            {
                const uint8_t* record = bytes.data() + recordsOffset + static_cast<size_t>(i) * recordSize;
                const uint32_t id = ReadU32(record);
                if (!wanted.contains(id)) continue;
                const uint32_t stringOffset = ReadU32(record + 4);
                if (stringOffset >= stringSize)
                {
                    WLOG_WARN("retail-db2: %s contains an invalid string offset", filename);
                    return false;
                }
                const char* begin = reinterpret_cast<const char*>(strings + stringOffset);
                const void* terminator = std::memchr(begin, '\0', stringSize - stringOffset);
                if (!terminator)
                {
                    WLOG_WARN("retail-db2: %s contains an unterminated path", filename);
                    return false;
                }
                const auto* end = static_cast<const char*>(terminator);
                if (paths.try_emplace(id, begin, static_cast<size_t>(end - begin)).second) ++resolved;
            }
            WLOG_INFO("retail-db2: decoded %s rows=%u matched=%zu", filename, recordCount, resolved);
            return true;
        }

        bool ParsePathIndex(const std::unordered_set<uint32_t>& wanted,
                            std::unordered_map<uint32_t, std::string>& paths)
        {
            // Read the same canonical DB2Gen tables the host resolver consumes. Keeping one archive-backed
            // snapshot is important: a newer loose client alias paired with older host tables can resolve
            // the same FileDataID differently across the IPC boundary and feed the renderer mismatched
            // model/skin/texture families.
            const bool textures = ParseWdc1PathTable("TextureFilePath.db2", wanted, paths);
            const bool models = ParseWdc1PathTable("ModelFilePath.db2", wanted, paths);
            WLOG_INFO("retail-db2: resolved FileData paths=%zu/%zu", paths.size(), wanted.size());
            return textures && models && !paths.empty();
        }

        const std::string& PathOrEmpty(const std::unordered_map<uint32_t, std::string>& paths, uint32_t id)
        {
            static const std::string empty;
            const auto it = paths.find(id);
            return it == paths.end() ? empty : it->second;
        }

        bool BuildIndexes()
        {
            std::string schemaError;
            if (!runtimeDb2::ValidateDefinitions(schemas::All, &schemaError))
            {
                WLOG_WARN("retail-db2: schema set is invalid: %s", schemaError.c_str());
                return false;
            }

            std::unordered_map<uint32_t, ItemBase> items;
            std::unordered_map<uint32_t, Appearance> appearances;
            std::unordered_map<uint32_t, std::pair<std::array<uint32_t, 2>, uint32_t>> itemAppearances;
            std::unordered_set<uint32_t> usedDisplayIds;
            std::unordered_map<uint32_t, DisplayRaw> displayRows;
            std::unordered_map<uint32_t, std::vector<uint32_t>> modelFiles;
            std::unordered_map<uint32_t, uint32_t> componentModelPositions;
            std::unordered_map<uint32_t, uint32_t> textureFiles;
            std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> componentMaterials;
            std::unordered_map<uint32_t, std::vector<displaybuilder::ModelMaterialSource>> modelMaterials;
            std::unordered_map<uint32_t, uint32_t> displayInventoryTypes;
            std::unordered_map<uint32_t, uint32_t> actualDisplayInventoryTypes;
            std::unordered_set<uint32_t> wantedModelResources;
            std::unordered_set<uint32_t> wantedTextureResources;
            std::unordered_set<uint32_t> wantedPaths;

            Table table;
            if (!LoadTable(schemas::Item, table)) return false;
            items.reserve(table.Rows().size());
            for (const Row& row : table.Rows())
            {
                ItemBase value;
                value.classId = table.Value(row, "ClassID");
                value.subclassId = table.Value(row, "SubclassID");
                value.material = table.Value(row, "Material");
                value.inventoryType = table.Value(row, "InventoryType");
                value.sheatheType = table.Value(row, "SheatheType");
                value.soundOverride = table.Value(row, "Sound_override_subclassID");
                value.iconFileDataId = table.Value(row, "IconFileDataID");
                if (value.iconFileDataId) wantedPaths.insert(value.iconFileDataId);
                items[row.id] = value;
            }

            table = {};
            if (!LoadTable(schemas::ItemAppearance, table)) return false;
            appearances.reserve(table.Rows().size());
            for (const Row& row : table.Rows())
            {
                appearances[row.id] = Appearance{
                    table.RelationKey(row, "display"), table.Value(row, "DefaultIconFileDataID"),
                    table.Value(row, "DisplayType"), table.Value(row, "UiOrder")
                };
                PreferInventoryType(displayInventoryTypes, table.RelationKey(row, "display"),
                                    InventoryTypeFromDisplayType(table.Value(row, "DisplayType")));
                if (const uint32_t icon = table.Value(row, "DefaultIconFileDataID")) wantedPaths.insert(icon);
            }

            table = {};
            if (!LoadTable(schemas::ItemModifiedAppearance, table)) return false;
            for (const Row& row : table.Rows())
            {
                const uint32_t itemId = table.RelationKey(row, "item");
                const uint32_t modifier = table.Value(row, "ItemAppearanceModifierID");
                const uint32_t appearanceId = table.RelationKey(row, "appearance");
                const uint32_t order = table.Value(row, "OrderIndex");
                const auto linkedAppearance = appearances.find(appearanceId);
                if (linkedAppearance != appearances.end() && linkedAppearance->second.displayId)
                    usedDisplayIds.insert(linkedAppearance->second.displayId);
                const std::array<uint32_t, 2> rank{modifier == 0 ? 0u : 1u, order};
                auto it = itemAppearances.find(itemId);
                if (it == itemAppearances.end() || rank < it->second.first)
                    itemAppearances[itemId] = {rank, appearanceId};
            }
            for (auto& [itemId, value] : items)
            {
                const auto selected = itemAppearances.find(itemId);
                if (selected == itemAppearances.end()) continue;
                const auto appearance = appearances.find(selected->second.second);
                if (appearance != appearances.end())
                {
                    value.appearanceId = selected->second.second;
                    value.displayId = appearance->second.displayId;
                    if (appearance->second.iconFileDataId)
                        value.iconFileDataId = appearance->second.iconFileDataId;
                }
            }
            for (const auto& [itemId, value] : items)
            {
                if (!value.displayId) continue;
                usedDisplayIds.insert(value.displayId);
                // A real Item row is more specific than ItemAppearance.DisplayType. Keep the same
                // deterministic priority used by the former CSV generator when displays are shared.
                PreferInventoryType(actualDisplayInventoryTypes, value.displayId, value.inventoryType);
            }

            table = {};
            if (!LoadTable(schemas::ItemDisplayInfo, table)) return false;
            displayRows.reserve(table.Rows().size());
            for (const Row& row : table.Rows())
            {
                DisplayRaw value;
                value.id = row.id;
                value.itemVisual = table.Value(row, "ItemVisual");
                value.particleColor = table.Value(row, "ParticleColorID");
                value.flags = table.Value(row, "Flags");
                for (size_t i = 0; i < 2; ++i)
                {
                    value.modelResources[i] = table.Value(row, "ModelResourcesID", i);
                    value.modelMaterials[i] = table.Value(row, "ModelMaterialResourcesID", i);
                    value.helmetVis[i] = table.Value(row, "HelmetGeosetVis", i);
                }
                for (size_t i = 0; i < 6; ++i) value.geosets[i] = table.Value(row, "GeosetGroup", i);
                for (uint32_t resource : value.modelResources) if (resource) wantedModelResources.insert(resource);
                for (uint32_t resource : value.modelMaterials) if (resource) wantedTextureResources.insert(resource);
                displayRows[row.id] = value;
            }

            table = {};
            if (!LoadTable(schemas::ItemDisplayInfoMaterialRes, table)) return false;
            for (const Row& row : table.Rows())
            {
                const uint32_t resource = table.RelationKey(row, "material");
                componentMaterials[table.RelationKey(row, "display")].push_back({
                    table.Value(row, "ComponentSection"), resource
                });
                if (resource) wantedTextureResources.insert(resource);
            }

            table = {};
            if (!LoadTable(schemas::ItemDisplayInfoModelMatRes, table)) return false;
            for (const Row& row : table.Rows())
            {
                const uint32_t resource = table.RelationKey(row, "material");
                const uint32_t displayId = table.RelationKey(row, "display");
                modelMaterials[displayId].push_back({
                    row.id, resource, table.Value(row, "TextureType"), table.Value(row, "ModelIndex")
                });
                if (resource) wantedTextureResources.insert(resource);
            }
            for (auto& [displayId, rows] : modelMaterials)
                std::ranges::sort(rows, {}, [](const displaybuilder::ModelMaterialSource& row) {
                    return std::tuple{row.textureType, row.id, row.materialResource, row.modelIndex};
                });

            table = {};
            if (!LoadTable(schemas::ModelFileData, table)) return false;
            for (const Row& row : table.Rows())
            {
                const uint32_t fileId = table.Value(row, "FileDataID");
                const uint32_t resourceId = table.Value(row, "ModelResourcesID");
                if (fileId && wantedModelResources.contains(resourceId))
                {
                    modelFiles[resourceId].push_back(fileId);
                    wantedPaths.insert(fileId);
                }
            }

            table = {};
            if (!LoadTable(schemas::ComponentModelFileData, table)) return false;
            componentModelPositions.reserve(table.Rows().size());
            for (const Row& row : table.Rows())
            {
                const uint32_t position = table.Value(row, "PositionIndex");
                if (position <= 1) componentModelPositions[row.id] = position;
            }

            table = {};
            if (!LoadTable(schemas::TextureFileData, table)) return false;
            for (const Row& row : table.Rows())
            {
                const uint32_t fileId = table.Value(row, "FileDataID");
                const uint32_t resourceId = table.Value(row, "MaterialResourcesID");
                if (fileId && wantedTextureResources.contains(resourceId))
                {
                    textureFiles[resourceId] = fileId;
                    wantedPaths.insert(fileId);
                }
            }

            std::unordered_map<uint32_t, std::string> paths;
            paths.reserve(wantedPaths.size());
            if (!ParsePathIndex(wantedPaths, paths)) return false;

            // Keep the selected ItemModifiedAppearance -> ItemAppearance icon on the item itself.
            // Resolving through displayId loses information when multiple appearances share a display.
            for (auto& [itemId, item] : items)
            {
                if (!item.iconFileDataId) continue;
                item.icon = BasenameStem(PathOrEmpty(paths, item.iconFileDataId), false);
            }

            std::unordered_map<uint32_t, uint32_t> displayIcons;
            for (const auto& [appearanceId, appearance] : appearances)
                if (appearance.displayId && appearance.iconFileDataId)
                    displayIcons.try_emplace(appearance.displayId, appearance.iconFileDataId);

            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> orderedAppearances;
            orderedAppearances.reserve(appearances.size());
            for (const auto& [appearanceId, appearance] : appearances)
                if (appearance.displayId)
                    orderedAppearances.emplace_back(appearance.uiOrder, appearanceId, appearance.displayId);
            std::ranges::sort(orderedAppearances);
            std::vector<uint32_t> appearanceOrder;
            appearanceOrder.reserve(orderedAppearances.size());
            std::unordered_set<uint32_t> orderedDisplays;
            for (const auto& [uiOrder, appearanceId, displayId] : orderedAppearances)
            {
                (void)uiOrder;
                (void)appearanceId;
                if (orderedDisplays.insert(displayId).second) appearanceOrder.push_back(displayId);
            }

            std::unordered_map<uint32_t, Display> displays;
            displays.reserve(displayRows.size());
            std::unordered_map<uint32_t, displaybuilder::DisplaySource> builderDisplays;
            builderDisplays.reserve(usedDisplayIds.size());
            for (const auto& [displayId, raw] : displayRows)
            {
                if (!usedDisplayIds.contains(displayId)) continue;
                builderDisplays.emplace(displayId, displaybuilder::DisplaySource{
                    displayId,
                    actualDisplayInventoryTypes.contains(displayId)
                        ? actualDisplayInventoryTypes.at(displayId)
                        : (displayInventoryTypes.contains(displayId) ? displayInventoryTypes.at(displayId) : 0),
                    raw.modelResources,
                    raw.modelMaterials,
                });
                Display out;
                out.id = displayId;
                out.flags = raw.flags;
                out.itemVisual = raw.itemVisual;
                out.particleColor = raw.particleColor;
                out.geosets = raw.geosets;
                out.helmetVis = raw.helmetVis;
                for (size_t i = 0; i < 2; ++i)
                {
                    const auto models = modelFiles.find(raw.modelResources[i]);
                    if (models != modelFiles.end())
                    {
                        uint32_t selectedFile = 0;
                        for (uint32_t fileId : models->second)
                        {
                            const auto position = componentModelPositions.find(fileId);
                            if (position != componentModelPositions.end() && position->second == i)
                            {
                                selectedFile = fileId;
                                break;
                            }
                        }
                        for (uint32_t fileId : models->second)
                        {
                            if (selectedFile && fileId != selectedFile) continue;
                            const std::string& path = PathOrEmpty(paths, fileId);
                            if (!path.empty() && Lower(path).ends_with(".m2"))
                            {
                                out.models[i] = BasenameStem(path, true);
                                break;
                            }
                        }
                    }
                    const auto texture = textureFiles.find(raw.modelMaterials[i]);
                    if (texture != textureFiles.end())
                        out.modelTextures[i] = BasenameStem(PathOrEmpty(paths, texture->second), false);
                }
                const auto icon = displayIcons.find(displayId);
                if (icon != displayIcons.end())
                {
                    const std::string stem = BasenameStem(PathOrEmpty(paths, icon->second), false);
                    out.icons[0] = stem;
                    out.icons[1] = stem;
                }
                const auto components = componentMaterials.find(displayId);
                if (components != componentMaterials.end())
                {
                    for (const auto& [section, resourceId] : components->second)
                    {
                        const auto texture = textureFiles.find(resourceId);
                        if (section < out.componentTextures.size() && texture != textureFiles.end())
                            out.componentTextures[section] = ComponentStem(PathOrEmpty(paths, texture->second));
                    }
                }
                displays.emplace(displayId, std::move(out));
            }

            // Publish the lookup-owned maps before the display builder starts emitting its phased model
            // snapshots. Otherwise Glue receives the first snapshot while LookupDetour still rejects every
            // retail display, consumes the refresh, and remains naked until a later login/logout rebuild.
            g_items = std::move(items);
            g_displays = std::move(displays);
            g_lookupReady.store(true, std::memory_order_release);

            auto itemDisplayIndex = displaybuilder::Build(
                builderDisplays, modelFiles, componentModelPositions, textureFiles,
                modelMaterials, componentMaterials,
                appearanceOrder, paths,
                &ReadWholeFile);
            if (!itemDisplayIndex)
            {
                WLOG_WARN("retail-db2: failed to derive item-display attachment/material index");
                return false;
            }
            runtimeDb2::itemdisplay::Publish(std::move(itemDisplayIndex));

            g_fileDataPaths = std::move(paths);
            return true;
        }

        uint32_t FillItem(uint32_t id, void* output)
        {
            const auto it = g_items.find(id);
            if (it == g_items.end() || !it->second.displayId || !output) return 0;
            const ItemBase& value = it->second;
            std::array<uint32_t, 8> record{
                id, value.classId, value.subclassId, value.soundOverride,
                value.material, value.displayId, value.inventoryType, value.sheatheType
            };
            std::memcpy(output, record.data(), kWotlkItemRecordSize);
            return 1;
        }

        void PutStringPointer(uint8_t* record, size_t offset, const std::string& value)
        {
            const char* pointer = value.empty() ? kEmpty : value.c_str();
            std::memcpy(record + offset, &pointer, sizeof(pointer));
        }

        uint32_t FillDisplay(uint32_t id, void* output)
        {
            const auto it = g_displays.find(id);
            if (it == g_displays.end() || !output) return 0;
            const Display& value = it->second;
            auto* record = static_cast<uint8_t*>(output);
            std::memset(record, 0, kWotlkDisplayRecordSize);
            std::memcpy(record, &id, sizeof(id));
            PutStringPointer(record, 0x04, value.models[0]);
            PutStringPointer(record, 0x08, value.models[1]);
            PutStringPointer(record, 0x0C, value.modelTextures[0]);
            PutStringPointer(record, 0x10, value.modelTextures[1]);
            PutStringPointer(record, 0x14, value.icons[0]);
            PutStringPointer(record, 0x18, value.icons[1]);
            for (size_t i = 0; i < 3; ++i) std::memcpy(record + 0x1C + i * 4, &value.geosets[i], 4);
            std::memcpy(record + 0x28, &value.flags, 4);
            for (size_t i = 0; i < 2; ++i) std::memcpy(record + 0x34 + i * 4, &value.helmetVis[i], 4);
            for (size_t i = 0; i < 8; ++i) PutStringPointer(record, 0x3C + i * 4, value.componentTextures[i]);
            std::memcpy(record + 0x5C, &value.itemVisual, 4);
            std::memcpy(record + 0x60, &value.particleColor, 4);
            return 1;
        }

        const char* RetailIconForDisplay(uint32_t displayId)
        {
            if (!g_ready.load(std::memory_order_acquire)) return nullptr;
            const auto display = g_displays.find(displayId);
            if (display == g_displays.end() || display->second.icons[0].empty()) return nullptr;
            return display->second.icons[0].c_str();
        }

        const char* RetailIconForItem(uint32_t itemId)
        {
            if (!g_ready.load(std::memory_order_acquire)) return nullptr;
            const auto item = g_items.find(itemId);
            if (item == g_items.end()) return nullptr;
            if (!item->second.icon.empty()) return item->second.icon.c_str();
            return item->second.displayId ? RetailIconForDisplay(item->second.displayId) : nullptr;
        }

        bool NativeItemExists(uint32_t itemId)
        {
            if (!itemId) return false;
            __try
            {
                const uint32_t minId = *reinterpret_cast<const uint32_t*>(db2::item::kMinId);
                const uint32_t maxId = *reinterpret_cast<const uint32_t*>(db2::item::kMaxId);
                void** table = *reinterpret_cast<void***>(db2::item::kIdTable);
                return table && itemId >= minId && itemId <= maxId && table[itemId - minId] != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        uint32_t ItemIdFromObject(void* item)
        {
            if (!item) return 0;
            __try
            {
                const auto objectData = *reinterpret_cast<const uint8_t* const*>(
                    static_cast<const uint8_t*>(item) + 0x08);
                return objectData ? *reinterpret_cast<const uint32_t*>(objectData + 0x0C) : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        uint32_t __fastcall GetInventoryTypeDetour(void* item, void* edx)
        {
            const uint32_t itemId = ItemIdFromObject(item);

            // UseContainerItem relies on this getter to choose auto-equip instead of item use. The stock
            // getter performs an inline Item.dbc lookup, making every DB2-only ID look non-equippable.
            // Preserve all native rows (including legitimate inventory type 0 rows) before falling back.
            if (!itemId || NativeItemExists(itemId) || !g_ready.load(std::memory_order_acquire))
                return g_originalGetInventoryType ? g_originalGetInventoryType(item, edx) : 0;

            const auto retail = g_items.find(itemId);
            if (retail != g_items.end()) return retail->second.inventoryType;
            return g_originalGetInventoryType ? g_originalGetInventoryType(item, edx) : 0;
        }

        bool RetailInventoryTypeFitsSlot(uint32_t inventoryType, uint32_t slot)
        {
            switch (inventoryType)
            {
                // CursorCanGoInSlot receives the zero-based PaperDoll slot IDs, not the one-based
                // inventory constants used by GetInventorySlotInfo.
                case 1: return slot == 0;   // head
                case 2: return slot == 1;   // neck
                case 3: return slot == 2;   // shoulder
                case 4: return slot == 3;   // shirt
                case 5: case 20: return slot == 4; // chest / robe
                case 6: return slot == 5;   // waist
                case 7: return slot == 6;   // legs
                case 8: return slot == 7;   // feet
                case 9: return slot == 8;   // wrist
                case 10: return slot == 9;  // hands
                case 11: return slot == 10 || slot == 11; // rings
                case 12: return slot == 12 || slot == 13; // trinkets
                case 13: return slot == 15 || slot == 16; // one-hand weapon
                case 14: return slot == 16; // shield
                case 15: case 25: case 26: case 28: return slot == 17; // ranged/relic
                case 16: return slot == 14; // cloak
                case 17: case 21: return slot == 15; // two-hand/main-hand
                case 18: case 27: return slot >= 19 && slot <= 22; // bag/quiver
                case 19: return slot == 18; // tabard
                case 22: case 23: return slot == 16; // off-hand/held in off-hand
                default: return false;
            }
        }

        uint32_t __fastcall CanGoInSlotDetour(void* item, void* edx, uint32_t slot, uint32_t flags)
        {
            if (!item || NativeItemExists(ItemIdFromObject(item)) || !g_ready.load(std::memory_order_acquire))
                return g_originalCanGoInSlot ? g_originalCanGoInSlot(item, edx, slot, flags) : 0;

            const auto retail = g_items.find(ItemIdFromObject(item));
            if (retail == g_items.end())
                return g_originalCanGoInSlot ? g_originalCanGoInSlot(item, edx, slot, flags) : 0;
            return RetailInventoryTypeFitsSlot(retail->second.inventoryType, slot) ? 1u : 0u;
        }

        uint32_t ItemIdFromLua(void* state)
        {
            const auto toNumber = wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
            const double numeric = toNumber(state, 1);
            if (numeric > 0.0 && numeric <= static_cast<double>((std::numeric_limits<uint32_t>::max)()))
                return static_cast<uint32_t>(numeric);

            // GetItemIcon also accepts item links. lua_tonumber intentionally rejects those, so recover
            // the numeric ID from the standard "item:<id>:..." payload before giving up.
            const auto toString = wxl::game::Native<luaoff::LuaToStringFn>(luaoff::kLuaToString);
            size_t length = 0;
            const char* text = toString(state, 1, &length);
            if (!text || !length) return 0;
            const char* end = text + length;
            const char needle[] = "item:";
            const char* begin = std::search(text, end, std::begin(needle), std::end(needle) - 1);
            if (begin == end) return 0;
            begin += sizeof(needle) - 1;
            uint64_t value = 0;
            while (begin != end && *begin >= '0' && *begin <= '9')
            {
                value = value * 10u + static_cast<unsigned>(*begin - '0');
                if (value > (std::numeric_limits<uint32_t>::max)()) return 0;
                ++begin;
            }
            return static_cast<uint32_t>(value);
        }

        const char* __cdecl InventoryArtDetour(uint32_t displayId)
        {
            const char* native = g_originalInventoryArt ? g_originalInventoryArt(displayId) : nullptr;
            if (native && _stricmp(native, kQuestionMarkIcon) != 0) return native;
            if (const char* retail = RetailIconForDisplay(displayId)) return retail;
            return native ? native : kQuestionMarkIcon;
        }

        const char* __fastcall ItemInventoryArtDetour(void* item, void* edx)
        {
            const char* native = g_originalItemInventoryArt
                ? g_originalItemInventoryArt(item, edx) : nullptr;
            if (native && _stricmp(native, kQuestionMarkIcon) != 0) return native;
            if (const char* retail = RetailIconForItem(ItemIdFromObject(item))) return retail;
            return native ? native : kQuestionMarkIcon;
        }

        int __cdecl ScriptGetItemIconDetour(void* state)
        {
            const uint32_t itemId = ItemIdFromLua(state);

            // The native function returns the question-mark texture (a successful one-value Lua result)
            // for retail-only IDs, so waiting for a zero return never reaches the DB2 fallback. Preserve
            // strict DBC-first behavior by taking the native path only when Item.dbc actually owns the ID.
            if (NativeItemExists(itemId))
                return g_originalScriptGetItemIcon ? g_originalScriptGetItemIcon(state) : 0;

            if (const char* icon = RetailIconForItem(itemId))
            {
                std::string path = "Interface\\Icons\\";
                path += icon;
                wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(state, path.c_str());
                return 1;
            }
            return g_originalScriptGetItemIcon ? g_originalScriptGetItemIcon(state) : 0;
        }

        void PushRetailIcon(void* state, const char* icon)
        {
            if (!icon)
            {
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
                return;
            }
            std::string path = "Interface\\Icons\\";
            path += icon;
            wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(state, path.c_str());
        }

        /** Lua: iconPath = GetRetailItemIcon(itemIDOrLink). Never consults Item.dbc. */
        int __cdecl ScriptGetRetailItemIcon(void* state)
        {
            PushRetailIcon(state, RetailIconForItem(ItemIdFromLua(state)));
            return 1;
        }

        /** Lua: displayID, appearanceID, iconFileDataID = GetRetailItemAppearance(itemIDOrLink). */
        int __cdecl ScriptGetRetailItemAppearance(void* state)
        {
            if (!g_ready.load(std::memory_order_acquire))
            {
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
                return 1;
            }
            const auto item = g_items.find(ItemIdFromLua(state));
            if (item == g_items.end() || !item->second.appearanceId)
            {
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
                return 1;
            }
            const auto pushNumber = wxl::game::Native<luaoff::LuaPushNumberFn>(luaoff::kLuaPushNumber);
            pushNumber(state, item->second.displayId);
            pushNumber(state, item->second.appearanceId);
            pushNumber(state, item->second.iconFileDataId);
            return 3;
        }

        /** Lua: path = GetRetailFileDataPath(fileDataID), for dependencies indexed from loaded item DB2s. */
        int __cdecl ScriptGetRetailFileDataPath(void* state)
        {
            const double numeric = wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber)(state, 1);
            if (!g_ready.load(std::memory_order_acquire) || numeric <= 0.0 ||
                numeric > static_cast<double>((std::numeric_limits<uint32_t>::max)()))
            {
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
                return 1;
            }
            const auto path = g_fileDataPaths.find(static_cast<uint32_t>(numeric));
            if (path == g_fileDataPaths.end() || path->second.empty())
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
            else
                wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(state, path->second.c_str());
            return 1;
        }

        /** Lua: ready, failed = GetRetailDB2Status(). */
        int __cdecl ScriptGetRetailDb2Status(void* state)
        {
            const auto pushBoolean = wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean);
            pushBoolean(state, g_ready.load(std::memory_order_acquire));
            pushBoolean(state, g_failed.load(std::memory_order_acquire));
            return 2;
        }

        bool InstallStorageOverlay(uintptr_t minAddress, uintptr_t maxAddress, uintptr_t tableAddress,
                                   size_t recordSize, bool itemTable)
        {
            const uint32_t nativeMin = *reinterpret_cast<const uint32_t*>(minAddress);
            const uint32_t nativeMax = *reinterpret_cast<const uint32_t*>(maxAddress);
            void** nativeTable = *reinterpret_cast<void***>(tableAddress);
            if (!nativeTable || nativeMax < nativeMin) return false;

            uint32_t fallbackMax = nativeMax;
            size_t missing = 0;
            if (itemTable)
            {
                for (const auto& [id, value] : g_items)
                    if (id >= nativeMin && value.displayId) fallbackMax = std::max(fallbackMax, id);
            }
            else
            {
                for (const auto& [id, value] : g_displays)
                    if (id >= nativeMin) fallbackMax = std::max(fallbackMax, id);
            }

            const size_t pointerCount = static_cast<size_t>(fallbackMax - nativeMin) + 1;
            auto** merged = static_cast<void**>(VirtualAlloc(
                nullptr, pointerCount * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!merged) return false;
            const size_t nativeCount = static_cast<size_t>(nativeMax - nativeMin) + 1;
            std::memcpy(merged, nativeTable, nativeCount * sizeof(void*));

            if (itemTable)
            {
                for (const auto& [id, value] : g_items)
                    if (id >= nativeMin && id <= fallbackMax && value.displayId && !merged[id - nativeMin]) ++missing;
            }
            else
            {
                for (const auto& [id, value] : g_displays)
                    if (id >= nativeMin && id <= fallbackMax && !merged[id - nativeMin]) ++missing;
            }

            auto* records = static_cast<uint8_t*>(VirtualAlloc(
                nullptr, missing * recordSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (missing && !records)
            {
                VirtualFree(merged, 0, MEM_RELEASE);
                return false;
            }
            size_t recordIndex = 0;
            if (itemTable)
            {
                for (const auto& [id, value] : g_items)
                {
                    if (id < nativeMin || id > fallbackMax || !value.displayId || merged[id - nativeMin]) continue;
                    uint8_t* record = records + recordIndex++ * recordSize;
                    if (!FillItem(id, record)) continue;
                    merged[id - nativeMin] = record;
                }
                g_itemIdTable = merged;
                g_itemRecords = records;
            }
            else
            {
                for (const auto& [id, value] : g_displays)
                {
                    if (id < nativeMin || id > fallbackMax || merged[id - nativeMin]) continue;
                    uint8_t* record = records + recordIndex++ * recordSize;
                    if (!FillDisplay(id, record)) continue;
                    merged[id - nativeMin] = record;
                }
                g_displayIdTable = merged;
                g_displayRecords = records;
            }

            // nativeMin is unchanged, so existing IDs remain valid as soon as the pointer swaps. Expanding max
            // last prevents readers from indexing the old table with a retail-only ID during publication.
            *reinterpret_cast<void***>(tableAddress) = merged;
            *reinterpret_cast<uint32_t*>(maxAddress) = fallbackMax;
            WLOG_INFO("retail-db2: overlaid %s missing=%zu range=%u..%u",
                      itemTable ? "Item.dbc" : "ItemDisplayInfo.dbc", missing, nativeMin, fallbackMax);
            return true;
        }

        bool InstallStorageOverlays()
        {
            return InstallStorageOverlay(db2::item::kMinId, db2::item::kMaxId, db2::item::kIdTable,
                                         db2::item::kRecordSize, true) &&
                   InstallStorageOverlay(db2::itemdisplayinfo::kMinId, db2::itemdisplayinfo::kMaxId,
                                         db2::itemdisplayinfo::kIdTable, kWotlkDisplayRecordSize, false);
        }

        uint32_t __fastcall LookupDetour(void* storage, void* edx, uint32_t id, void* output)
        {
            const uint32_t native = g_originalLookup ? g_originalLookup(storage, edx, id, output) : 0;
            if (native || !g_lookupReady.load(std::memory_order_acquire)) return native;
            if (storage == reinterpret_cast<void*>(db2::item::kStorageObject)) return FillItem(id, output);
            if (storage == reinterpret_cast<void*>(db2::itemdisplayinfo::kStorageObject)) return FillDisplay(id, output);
            return 0;
        }

        DWORD WINAPI LoadThread(LPVOID)
        {
            // Do not replace the client's inline ID tables. Several consumers retain internal pointers and
            // invariants beyond min/max/idTable; publishing a synthetic table caused a null-call crash shortly
            // after character selection. Keep the proven accessor-only fallback until those paths are hooked
            // individually.
            const bool ok = BuildIndexes();
            g_failed.store(!ok, std::memory_order_release);
            g_ready.store(ok, std::memory_order_release);
            if (!ok) WLOG_WARN("retail-db2: disabled because startup indexing failed");
            else WLOG_INFO("retail-db2: ready items=%zu displays=%zu", g_items.size(), g_displays.size());
            return 0;
        }

        void InstallModule()
        {
            // Purpose-built DB2 globals avoid passing retail IDs through native Lua functions whose
            // inline Item.dbc range/table checks run before WXL's accessor fallback can participate.
            wxl::runtime::lua::RegisterFunction("GetRetailItemIcon", &ScriptGetRetailItemIcon);
            wxl::runtime::lua::RegisterFunction("GetRetailItemAppearance", &ScriptGetRetailItemAppearance);
            wxl::runtime::lua::RegisterFunction("GetRetailFileDataPath", &ScriptGetRetailFileDataPath);
            wxl::runtime::lua::RegisterFunction("GetRetailDB2Status", &ScriptGetRetailDb2Status);
            wxl::runtime::lua::RegisterScript("wxl-retail-item-api", R"lua(
do
    -- Stock-compatible item information with the icon replaced only when the retail DB2 index owns it.
    function GetRetailItemInfo(item)
        local name, link, quality, itemLevel, requiredLevel, itemType, itemSubType,
              maxStack, equipSlot, icon, vendorPrice = GetItemInfo(item)
        local retailIcon = GetRetailItemIcon(item)
        return name, link, quality, itemLevel, requiredLevel, itemType, itemSubType,
               maxStack, equipSlot, retailIcon or icon, vendorPrice
    end

    -- 3.3.5's container API obtains its texture from the cached Item.dbc record and therefore never
    -- reaches GetItemIcon. Wrap it before compatibility addons capture the global, then also repair the
    -- table-returning C_Container facade after that addon has loaded.
    local nativeContainerInfo = GetContainerItemInfo
    if type(nativeContainerInfo) == "function" and not _G.WXL_RetailContainerInfoWrapped then
        _G.WXL_RetailContainerInfoWrapped = true
        GetContainerItemInfo = function(bag, slot)
            local icon, count, locked, quality, readable, lootable, link = nativeContainerInfo(bag, slot)
            if link then
                local retailIcon = GetRetailItemIcon(link)
                if retailIcon then icon = retailIcon end
            end
            return icon, count, locked, quality, readable, lootable, link
        end
    end
    local function wrapCContainer()
        if type(C_Container) ~= "table" or
           type(C_Container.GetContainerItemInfo) ~= "function" or
           C_Container.WXL_RetailIcons then return end
        local original = C_Container.GetContainerItemInfo
        C_Container.GetContainerItemInfo = function(bag, slot)
            local info = original(bag, slot)
            if info then
                local item = info.hyperlink or info.itemID
                local retailIcon = item and GetRetailItemIcon(item)
                if retailIcon then info.iconFileID = retailIcon end
            end
            return info
        end
        C_Container.WXL_RetailIcons = true
    end

    wrapCContainer()
    local watcher = CreateFrame and CreateFrame("Frame")
    if watcher then
        watcher:RegisterEvent("ADDON_LOADED")
        watcher:SetScript("OnEvent", wrapCContainer)
    end
end
)lua");
            wxl::core::hook::Install("RetailDb2::DBCFirstLookup", db2::itemdisplayinfo::kLookup,
                                     reinterpret_cast<void*>(&LookupDetour),
                                     reinterpret_cast<void**>(&g_originalLookup));
            wxl::core::hook::Install("RetailDb2::InventoryArt", kInventoryArt,
                                     reinterpret_cast<void*>(&InventoryArtDetour),
                                     reinterpret_cast<void**>(&g_originalInventoryArt));
            wxl::core::hook::Install("RetailDb2::ItemInventoryArt", kItemInventoryArt,
                                     reinterpret_cast<void*>(&ItemInventoryArtDetour),
                                     reinterpret_cast<void**>(&g_originalItemInventoryArt));
            wxl::core::hook::Install("RetailDb2::GetItemIcon", kScriptGetItemIcon,
                                     reinterpret_cast<void*>(&ScriptGetItemIconDetour),
                                     reinterpret_cast<void**>(&g_originalScriptGetItemIcon));
            wxl::core::hook::Install("RetailDb2::GetInventoryType", kGetInventoryType,
                                     reinterpret_cast<void*>(&GetInventoryTypeDetour),
                                     reinterpret_cast<void**>(&g_originalGetInventoryType));
            wxl::core::hook::Install("RetailDb2::CanGoInSlot", kCanGoInSlot,
                                     reinterpret_cast<void*>(&CanGoInSlotDetour),
                                     reinterpret_cast<void**>(&g_originalCanGoInSlot));
            if (HANDLE thread = CreateThread(nullptr, 0, &LoadThread, nullptr, 0, nullptr))
                CloseHandle(thread);
            else
                WLOG_WARN("retail-db2: failed to create indexing thread");
        }

        struct Registrar
        {
            Registrar() { wxl::runtime::modules::Register("wxl-retail-db2", &InstallModule); }
        } g_registrar;
    }
}
