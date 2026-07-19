// Builds the equip extension's compact attachment/material index from retail DB2 relationships and SKINs.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ItemDisplayBuilder.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_set>

namespace wxl::scripts::retaildb2::displaybuilder
{
    namespace itemdisplay = wxl::runtime::db2::itemdisplay;

    namespace
    {
        constexpr std::string_view kObjectMarker = "item\\objectcomponents\\";
        constexpr std::string_view kCollectionMarker = "item\\objectcomponents\\collections\\";

        struct SkinBatch
        {
            uint16_t sectionId = 0;
            uint32_t batchIndex = 0;
        };

        struct SkinInfo
        {
            std::vector<uint16_t> sectionIds;
            std::vector<SkinBatch> batches;
        };

        struct ModelInfo
        {
            uint32_t fileId = 0;
            std::string path;
            std::string model;
            std::string folder;
            std::string family;
            std::string slot;
            std::string side;
            bool collection = false;
        };

        std::string Lower(std::string value)
        {
            for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return value;
        }

        std::string Normalize(std::string value)
        {
            std::replace(value.begin(), value.end(), '/', '\\');
            return Lower(std::move(value));
        }

        bool EndsWith(std::string_view value, std::string_view suffix)
        {
            return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
        }

        bool Contains(std::string_view value, std::string_view needle)
        {
            return value.find(needle) != std::string_view::npos;
        }

        std::string Basename(std::string_view path)
        {
            const size_t slash = path.find_last_of("\\/");
            return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        std::string WithoutExtension(std::string value)
        {
            const size_t slash = value.find_last_of("\\/");
            const size_t dot = value.find_last_of('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) value.resize(dot);
            return value;
        }

        bool IsRaceCode(std::string_view code)
        {
            static constexpr std::array codes{
                "be", "dr", "dt", "dw", "ed", "gn", "go", "hr", "hu", "kt", "mg",
                "na", "nb", "ni", "or", "pa", "sc", "ta", "tr", "vu", "wo", "za",
            };
            return std::ranges::find(codes, code) != codes.end();
        }

        std::string StripRaceGender(std::string value)
        {
            const size_t dot = value.find_last_of('.');
            const std::string extension = dot == std::string::npos ? "" : value.substr(dot);
            std::string stem = dot == std::string::npos ? value : value.substr(0, dot);
            std::string lower = Lower(stem);

            const size_t last = lower.find_last_of('_');
            if (last != std::string::npos && last + 2 == lower.size() &&
                (lower[last + 1] == 'm' || lower[last + 1] == 'f'))
            {
                const size_t raceSep = lower.find_last_of('_', last - 1);
                if (raceSep != std::string::npos && last - raceSep == 3 &&
                    IsRaceCode(std::string_view(lower).substr(raceSep + 1, 2)))
                    stem.resize(raceSep);
            }
            else if (lower.size() >= 4 && lower[lower.size() - 4] == '_' &&
                     (lower.back() == 'm' || lower.back() == 'f') &&
                     IsRaceCode(std::string_view(lower).substr(lower.size() - 3, 2)))
            {
                stem.resize(stem.size() - 4);
            }
            return stem + extension;
        }

        std::string Capitalize(std::string value)
        {
            if (!value.empty()) value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
            return value;
        }

        std::string ObjectFolder(std::string_view path)
        {
            const std::string lower = Normalize(std::string(path));
            const size_t marker = lower.find(kObjectMarker);
            if (marker == std::string::npos) return {};
            const size_t begin = marker + kObjectMarker.size();
            const size_t end = lower.find('\\', begin);
            if (end == std::string::npos || end == begin) return {};
            return Capitalize(lower.substr(begin, end - begin));
        }

        std::string AfterMarker(std::string_view path, std::string_view marker)
        {
            std::string normalized(path);
            std::replace(normalized.begin(), normalized.end(), '/', '\\');
            const std::string lower = Lower(normalized);
            const size_t at = lower.find(marker);
            return at == std::string::npos ? Basename(normalized)
                                          : normalized.substr(at + marker.size());
        }

        std::string NormalizeFamily(std::string value)
        {
            value = Lower(std::move(value));
            while (!value.empty() && value.front() == '_') value.erase(value.begin());
            while (!value.empty() && value.back() == '_') value.pop_back();
            const size_t last = value.find_last_of('_');
            if (last != std::string::npos)
            {
                const std::string_view tail(value.data() + last + 1, value.size() - last - 1);
                if ((tail == "l" || tail == "r") ||
                    (!tail.empty() && std::ranges::all_of(tail, [](char ch) { return ch >= '0' && ch <= '9'; })))
                    value.resize(last);
            }
            static constexpr std::array prefixes{
                "collections_", "chest_", "robe_", "shirt_", "tabard_", "belt_", "pant_",
                "leg_", "boot_", "glove_", "hand_", "bracer_", "wrist_",
            };
            for (std::string_view prefix : prefixes)
                if (value.starts_with(prefix)) return value.substr(prefix.size());
            return value;
        }

        std::pair<std::string, std::string> FamilySlot(std::string_view name)
        {
            std::string stem = Lower(WithoutExtension(Basename(StripRaceGender(std::string(name)))));
            static constexpr std::array<std::pair<std::string_view, std::array<std::string_view, 3>>, 8> slots{{
                {"shoulder", {"shoulder", "", ""}},
                {"bracer", {"bracer", "wrist", ""}},
                {"glove", {"glove", "hand", ""}},
                {"chest", {"chest", "robe", "shirt"}},
                {"belt", {"belt", "waist", "buckle"}},
                {"pant", {"pant", "pants", "leg"}},
                {"boot", {"boot", "feet", "foot"}},
                {"helm", {"helm", "helmet", "head"}},
            }};

            for (const auto& [slot, tokens] : slots)
            {
                for (std::string_view token : tokens)
                {
                    if (token.empty()) continue;
                    for (const std::string prefix : {std::string(token) + "_", "collections_" + std::string(token) + "_"})
                        if (stem.starts_with(prefix)) return {NormalizeFamily(stem.substr(prefix.size())), std::string(slot)};
                    const std::string marker = "_" + std::string(token);
                    size_t at = stem.find(marker + "_");
                    if (at == std::string::npos && EndsWith(stem, marker)) at = stem.size() - marker.size();
                    if (at > 0 && at != std::string::npos)
                        return {NormalizeFamily(stem.substr(0, at)), std::string(slot)};
                }
            }
            return {NormalizeFamily(stem), {}};
        }

        std::string ModelSide(std::string_view model)
        {
            const std::string stem = Lower(WithoutExtension(Basename(StripRaceGender(std::string(model)))));
            if (stem.starts_with("lshoulder_") || stem.starts_with("leftshoulder_")) return "l";
            if (stem.starts_with("rshoulder_") || stem.starts_with("rightshoulder_")) return "r";
            if (EndsWith(stem, "_l") || Contains(stem, "_shoulder_l")) return "l";
            if (EndsWith(stem, "_r") || Contains(stem, "_shoulder_r")) return "r";
            return {};
        }

        uint32_t ModelSlot(uint32_t inventoryType)
        {
            switch (inventoryType)
            {
                case 1: return 0; case 3: return 1; case 4: return 2; case 5: case 20: return 3;
                case 6: return 4; case 7: return 5; case 8: return 6; case 9: return 7;
                case 10: return 8; case 16: return 9; case 19: return 10;
                default: return static_cast<uint32_t>(-1);
            }
        }

        uint32_t Attach(uint32_t inventoryType, size_t modelIndex, bool collection,
                        std::string_view slot, std::string_view model)
        {
            static constexpr uint32_t none = static_cast<uint32_t>(-1);
            const uint32_t side = modelIndex == 0 ? 0 : 1;
            switch (inventoryType)
            {
                case 1: return side == 0 ? 11 : 55;
                case 3: return side == 0 ? 6 : 5;
                case 4: case 5: case 19: case 20: return collection ? 19 : 34;
                case 6: return (slot == "belt" || Contains(Lower(std::string(model)), "_belt")) ? 53 : (collection ? 19 : 53);
                case 7: return collection ? 19 : (side == 0 ? 9 : 10);
                case 8: return collection ? 19 : (side == 0 ? 47 : 48);
                case 9: return collection ? 19 : (side == 0 ? 3 : 4);
                case 10: return collection ? 19 : (side == 0 ? 1 : 2);
                case 16: return 12;
                default: return collection ? 19 : none;
            }
        }

        const std::vector<uint16_t>& TargetGroups(uint32_t inventoryType)
        {
            static const std::vector<uint16_t> empty;
            static const std::unordered_map<uint32_t, std::vector<uint16_t>> groups{
                {1,{27}}, {3,{4,26}}, {4,{10,11,22}}, {5,{11,22,10}}, {6,{8}},
                {7,{13,11}}, {8,{5,20}}, {9,{3,23}}, {10,{4,3}}, {19,{9,22}},
                {20,{12,13,11,22}},
            };
            const auto it = groups.find(inventoryType);
            return it == groups.end() ? empty : it->second;
        }

        bool ReadU32(const uint8_t* data, size_t size, size_t offset, uint32_t& value)
        {
            if (offset > size || sizeof(value) > size - offset) return false;
            std::memcpy(&value, data + offset, sizeof(value));
            return true;
        }

        SkinInfo ReadSkin(const ModelInfo& model, ReadAssetFn readAsset)
        {
            SkinInfo info;
            if (!readAsset || model.path.empty()) return info;
            std::string skinPath = model.path;
            const size_t dot = skinPath.find_last_of('.');
            if (dot != std::string::npos) skinPath.resize(dot);
            skinPath += "00.skin";
            std::vector<uint8_t> bytes;
            if (!readAsset(skinPath.c_str(), bytes) || bytes.size() < 52 ||
                std::memcmp(bytes.data(), "SKIN", 4) != 0) return info;

            uint32_t submeshCount = 0, submeshOffset = 0, batchCount = 0, batchOffset = 0;
            if (!ReadU32(bytes.data(), bytes.size(), 28, submeshCount) ||
                !ReadU32(bytes.data(), bytes.size(), 32, submeshOffset) ||
                !ReadU32(bytes.data(), bytes.size(), 36, batchCount) ||
                !ReadU32(bytes.data(), bytes.size(), 40, batchOffset)) return info;
            if (submeshCount > 65535 || batchCount > 65535 ||
                submeshOffset > bytes.size() || static_cast<uint64_t>(submeshCount) * 48 > bytes.size() - submeshOffset ||
                batchOffset > bytes.size() || static_cast<uint64_t>(batchCount) * 24 > bytes.size() - batchOffset) return info;

            std::vector<uint16_t> bySection;
            bySection.reserve(submeshCount);
            for (uint32_t i = 0; i < submeshCount; ++i)
            {
                uint32_t raw = 0;
                ReadU32(bytes.data(), bytes.size(), submeshOffset + static_cast<size_t>(i) * 48, raw);
                const uint16_t id = static_cast<uint16_t>(raw & 0xFFFFu);
                bySection.push_back(id);
                if (std::ranges::find(info.sectionIds, id) == info.sectionIds.end()) info.sectionIds.push_back(id);
            }
            for (uint32_t i = 0; i < batchCount; ++i)
            {
                uint16_t section = 0;
                const size_t offset = batchOffset + static_cast<size_t>(i) * 24 + 4;
                if (offset + sizeof(section) > bytes.size()) break;
                std::memcpy(&section, bytes.data() + offset, sizeof(section));
                if (section < bySection.size()) info.batches.push_back({bySection[section], i});
            }
            return info;
        }

        std::vector<uint16_t> TargetSections(const SkinInfo& skin, uint32_t inventoryType)
        {
            std::vector<uint16_t> out;
            const auto& groups = TargetGroups(inventoryType);
            for (uint16_t id : skin.sectionIds)
                if (id && std::ranges::find(groups, static_cast<uint16_t>(id / 100)) != groups.end() &&
                    std::ranges::find(out, id) == out.end()) out.push_back(id);
            return out;
        }

        std::string Join(const std::vector<uint16_t>& values)
        {
            std::string out;
            for (uint16_t value : values)
            {
                if (!out.empty()) out += ',';
                out += std::to_string(value);
            }
            return out;
        }

        std::string JoinBatches(const std::vector<SkinBatch>& batches,
                                const std::vector<uint16_t>* selected = nullptr)
        {
            std::string out;
            for (const SkinBatch& batch : batches)
            {
                if (selected && std::ranges::find(*selected, batch.sectionId) == selected->end()) continue;
                if (!out.empty()) out += ',';
                out += std::to_string(batch.batchIndex);
            }
            return out;
        }

        int VariantRank(std::string_view path)
        {
            static constexpr std::array suffixes{
                "_hu_m.m2", "_hu_f.m2", "_be_m.m2", "_be_f.m2", "_or_m.m2",
                "_or_f.m2", "_ed_m.m2", "_ed_f.m2", "_hr_m.m2", "_hr_f.m2",
            };
            const std::string lower = Normalize(std::string(path));
            for (size_t i = 0; i < suffixes.size(); ++i) if (EndsWith(lower, suffixes[i])) return static_cast<int>(i);
            return static_cast<int>(suffixes.size()) + 2;
        }

        ModelInfo MakeModelInfo(uint32_t fileId, std::string path)
        {
            ModelInfo out;
            out.fileId = fileId;
            out.path = std::move(path);
            const std::string lower = Normalize(out.path);
            out.collection = Contains(lower, kCollectionMarker);
            out.folder = ObjectFolder(out.path);
            std::string name = out.collection ? AfterMarker(out.path, kCollectionMarker) : Basename(out.path);
            name = StripRaceGender(name);
            const size_t dot = name.find_last_of('.');
            if (dot != std::string::npos) name.replace(dot, std::string::npos, ".mdx");
            else name += ".mdx";
            out.model = std::move(name);
            std::tie(out.family, out.slot) = FamilySlot(out.model);
            out.side = ModelSide(out.model);
            return out;
        }

        ModelInfo SelectModel(const std::vector<uint32_t>& ids,
                              const std::unordered_map<uint32_t, std::string>& paths,
                              const std::unordered_map<uint32_t, uint32_t>& componentModelPositions,
                              std::string_view preferredSide = {})
        {
            std::vector<ModelInfo> candidates;
            std::unordered_map<std::string, uint32_t> baseCounts;
            for (uint32_t id : ids)
            {
                const auto path = paths.find(id);
                if (path == paths.end() || !EndsWith(Normalize(path->second), ".m2")) continue;
                ModelInfo info = MakeModelInfo(id, path->second);
                const auto position = componentModelPositions.find(id);
                if (position != componentModelPositions.end())
                {
                    if (position->second == 0) info.side = "l";
                    else if (position->second == 1) info.side = "r";
                }
                ++baseCounts[Lower(info.model)];
                candidates.push_back(std::move(info));
            }
            if (candidates.empty()) return {};
            std::ranges::sort(candidates, [&](const ModelInfo& a, const ModelInfo& b) {
                const int aSide = preferredSide.empty() ? 0 :
                    (a.side == preferredSide ? 0 : (a.side.empty() ? 1 : 2));
                const int bSide = preferredSide.empty() ? 0 :
                    (b.side == preferredSide ? 0 : (b.side.empty() ? 1 : 2));
                return std::tuple{aSide, -static_cast<int64_t>(baseCounts[Lower(a.model)]), a.collection ? 0 : 1,
                                  VariantRank(a.path), a.fileId} <
                       std::tuple{bSide, -static_cast<int64_t>(baseCounts[Lower(b.model)]), b.collection ? 0 : 1,
                                  VariantRank(b.path), b.fileId};
            });
            return candidates.front();
        }

        std::string TextureName(std::string_view path)
        {
            const std::string lower = Normalize(std::string(path));
            const bool collection = Contains(lower, kCollectionMarker);
            std::string value = collection ? AfterMarker(path, kCollectionMarker) : Basename(path);
            value = WithoutExtension(std::move(value));
            const std::string l = Lower(value);
            if (l.size() > 2 && l[l.size() - 2] == '_' &&
                (l.back() == 'm' || l.back() == 'f' || l.back() == 'u')) value.resize(value.size() - 2);
            return value;
        }

        std::string TextureForResource(uint32_t resource,
                                       const std::unordered_map<uint32_t, uint32_t>& textureFiles,
                                       const std::unordered_map<uint32_t, std::string>& paths)
        {
            const auto file = textureFiles.find(resource);
            if (file == textureFiles.end()) return {};
            const auto path = paths.find(file->second);
            return path == paths.end() ? std::string{} : TextureName(path->second);
        }

        std::string TextureFolder(uint32_t resource,
                                  const std::unordered_map<uint32_t, uint32_t>& textureFiles,
                                  const std::unordered_map<uint32_t, std::string>& paths)
        {
            const auto file = textureFiles.find(resource);
            if (file == textureFiles.end()) return {};
            const auto path = paths.find(file->second);
            return path == paths.end() ? std::string{} : ObjectFolder(path->second);
        }

        void SetFilter(itemdisplay::GeosetFilter& filter, const std::vector<uint16_t>& ids)
        {
            filter.count = static_cast<uint32_t>(std::min(ids.size(), std::size(filter.ids)));
            for (uint32_t i = 0; i < filter.count; ++i) filter.ids[i] = ids[i];
        }

        std::shared_ptr<itemdisplay::Index> CloneModelSnapshot(const itemdisplay::Index& source)
        {
            auto snapshot = std::make_shared<itemdisplay::Index>();
            snapshot->models.reserve(source.models.size());
            snapshot->strings.reserve(source.strings.size());
            for (const auto& [displayId, entries] : source.models)
            {
                auto& copied = snapshot->models[displayId];
                copied.reserve(entries.size());
                for (const itemdisplay::ModelEntry& entry : entries)
                {
                    itemdisplay::ModelEntry clone = entry;
                    clone.folder = snapshot->Intern(entry.folder ? entry.folder : "");
                    clone.model = snapshot->Intern(entry.model ? entry.model : "");
                    clone.texture = snapshot->Intern(entry.texture ? entry.texture : "");
                    copied.push_back(clone);
                }
            }
            return snapshot;
        }
    }

    std::shared_ptr<itemdisplay::Index> Build(
        const std::unordered_map<uint32_t, DisplaySource>& displays,
        const std::unordered_map<uint32_t, std::vector<uint32_t>>& modelFiles,
        const std::unordered_map<uint32_t, uint32_t>& componentModelPositions,
        const std::unordered_map<uint32_t, uint32_t>& textureFiles,
        const std::unordered_map<uint32_t, std::vector<ModelMaterialSource>>& modelMaterials,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& componentMaterials,
        const std::vector<uint32_t>& appearanceOrder,
        const std::unordered_map<uint32_t, std::string>& paths,
        ReadAssetFn readAsset)
    {
        auto index = std::make_shared<itemdisplay::Index>();
        index->strings.reserve(262144);
        index->models.reserve(displays.size());

        std::unordered_map<uint32_t, std::array<ModelInfo, 2>> selectedModels;
        selectedModels.reserve(displays.size());
        std::map<std::tuple<std::string, std::string, std::string>, ModelInfo> collectionCatalog;
        std::unordered_map<std::string, SkinInfo> skinCache;
        auto skinFor = [&](const ModelInfo& model) -> const SkinInfo& {
            const std::string key = Normalize(model.path);
            auto [it, inserted] = skinCache.try_emplace(key);
            if (inserted) it->second = ReadSkin(model, readAsset);
            return it->second;
        };

        // Resolve one stable race/gender family per model resource and build a collection family catalog.
        std::unordered_map<uint32_t, std::array<ModelInfo, 2>> resolvedResources;
        resolvedResources.reserve(modelFiles.size());
        for (const auto& [resource, files] : modelFiles)
        {
            std::array<ModelInfo, 2> selected{
                SelectModel(files, paths, componentModelPositions, "l"),
                SelectModel(files, paths, componentModelPositions, "r")
            };
            if (!selected[0].model.empty() || !selected[1].model.empty())
                resolvedResources.emplace(resource, std::move(selected));
            for (uint32_t fileId : files)
            {
                const auto path = paths.find(fileId);
                if (path == paths.end() || !Contains(Normalize(path->second), kCollectionMarker) ||
                    !EndsWith(Normalize(path->second), ".m2")) continue;
                ModelInfo candidate = MakeModelInfo(fileId, path->second);
                if (!candidate.family.empty() && !candidate.slot.empty())
                    collectionCatalog.try_emplace(std::tuple{candidate.family, candidate.slot, candidate.side},
                                                  std::move(candidate));
            }
        }

        // Resolve both model columns before deriving overlay sources. A single ModelResourcesID often
        // owns left and right shoulder files; keeping a side-specific choice prevents mirroring the left
        // model onto the right attachment.
        for (const auto& [displayId, display] : displays)
        {
            auto& chosen = selectedModels[displayId];
            for (size_t modelIndex = 0; modelIndex < 2; ++modelIndex)
            {
                const auto resource = resolvedResources.find(display.modelResources[modelIndex]);
                if (resource != resolvedResources.end()) chosen[modelIndex] = resource->second[modelIndex];
            }
        }

        // Publish direct object models before doing any archive/SKIN work. This phase is intentionally
        // limited to normal attachments and dedicated helm/shoulder collection models: shared body
        // collection meshes require geoset filtering and are added by the complete model phase below.
        auto directSnapshot = std::make_shared<itemdisplay::Index>();
        directSnapshot->models.reserve(displays.size());
        directSnapshot->strings.reserve(65536);
        for (const auto& [displayId, display] : displays)
        {
            const auto selected = selectedModels.find(displayId);
            if (selected == selectedModels.end()) continue;
            for (size_t modelIndex = 0; modelIndex < 2; ++modelIndex)
            {
                const ModelInfo& info = selected->second[modelIndex];
                if (info.model.empty()) continue;
                const bool dedicatedCollection =
                    (display.inventoryType == 1 && info.slot == "helm") ||
                    (display.inventoryType == 3 && info.slot == "shoulder");
                if (info.collection && !dedicatedCollection) continue;

                itemdisplay::ModelEntry entry;
                entry.modelSlot = ModelSlot(display.inventoryType);
                entry.attachId = Attach(display.inventoryType, modelIndex, info.collection, info.slot, info.model);
                entry.modelFlags = 0xffffffffu;
                entry.textureFlags = 0xffffffffu;
                entry.folder = directSnapshot->Intern(info.folder);
                entry.model = directSnapshot->Intern(info.model);
                entry.texture = directSnapshot->Intern(
                    TextureForResource(display.modelMaterials[modelIndex], textureFiles, paths));
                directSnapshot->models[displayId].push_back(entry);
            }
        }
        WLOG_INFO("retail-db2: publishing direct item-display models=%zu modelDisplays=%zu strings=%zu",
                  [&] { size_t n=0; for (const auto& [id,v] : directSnapshot->models) n += v.size(); return n; }(),
                  directSnapshot->models.size(), directSnapshot->strings.size());
        itemdisplay::Publish(std::move(directSnapshot));

        struct CollectionSource
        {
            std::string family;
            std::string slot;
            ModelInfo model;
            std::string texture;
        };
        std::unordered_map<uint32_t, std::vector<CollectionSource>> collectionSources;
        for (const auto& [displayId, display] : displays)
        {
            const auto selected = selectedModels.find(displayId);
            if (selected == selectedModels.end()) continue;
            for (size_t modelIndex = 0; modelIndex < 2; ++modelIndex)
            {
                const ModelInfo& info = selected->second[modelIndex];
                if (!info.collection || info.family.empty()) continue;
                collectionSources[displayId].push_back({
                    info.family, info.slot, info,
                    TextureForResource(display.modelMaterials[modelIndex], textureFiles, paths),
                });
            }
        }
        std::unordered_map<uint32_t, size_t> appearancePosition;
        appearancePosition.reserve(appearanceOrder.size());
        for (size_t i = 0; i < appearanceOrder.size(); ++i)
            appearancePosition.try_emplace(appearanceOrder[i], i);

        for (const auto& [displayId, display] : displays)
        {
            auto& chosen = selectedModels[displayId];
            bool hasCollection = false;
            std::unordered_set<std::string> families;
            for (size_t modelIndex = 0; modelIndex < 2; ++modelIndex)
            {
                ModelInfo& info = chosen[modelIndex];
                if (info.model.empty()) continue;
                hasCollection |= info.collection;
                if (!info.family.empty()) families.insert(info.family);

                // Most direct models do not need SKIN-derived targeting. Avoid tens of thousands of
                // archive reads during the background build; materials inspect the SKIN lazily below.
                // A dedicated helm collection M2 is already the complete slot model. Filtering it by
                // body section groups can leave only eyes/glow visible (for example item 202542).
                // Shared body collection meshes still need the slot-specific SKIN filter.
                const bool dedicatedSlotModel =
                    (display.inventoryType == 1 && info.slot == "helm") ||
                    (display.inventoryType == 3 && info.slot == "shoulder");
                const std::vector<uint16_t> targets = info.collection && !dedicatedSlotModel
                    ? TargetSections(skinFor(info), display.inventoryType) : std::vector<uint16_t>{};

                itemdisplay::ModelEntry entry;
                entry.modelSlot = ModelSlot(display.inventoryType);
                entry.attachId = Attach(display.inventoryType, modelIndex, info.collection, info.slot, info.model);
                entry.modelFlags = 0xffffffffu;
                entry.textureFlags = 0xffffffffu;
                entry.folder = index->Intern(info.folder);
                entry.model = index->Intern(info.model);
                entry.texture = index->Intern(TextureForResource(display.modelMaterials[modelIndex], textureFiles, paths));
                SetFilter(entry.geoFilter, targets);
                index->models[displayId].push_back(entry);
            }

            // Collection-only body meshes sometimes live beside component textures rather than directly in
            // ItemDisplayInfo. Match their family/slot through the DB2 resource graph, replacing the old CSV
            // generator's nearby-row heuristic with a deterministic global family catalog.
            if (!hasCollection)
            {
                const auto components = componentMaterials.find(displayId);
                if (components != componentMaterials.end())
                {
                    for (const auto& [section, resource] : components->second)
                    {
                        (void)section;
                        const auto file = textureFiles.find(resource);
                        if (file == textureFiles.end()) continue;
                        const auto path = paths.find(file->second);
                        if (path == paths.end()) continue;
                        auto [family, slot] = FamilySlot(TextureName(path->second));
                        if (!family.empty()) families.insert(std::move(family));
                    }
                }

                std::string preferredSlot;
                switch (display.inventoryType)
                {
                    case 3: preferredSlot = "shoulder"; break; case 4: case 5: case 19: case 20: preferredSlot = "chest"; break;
                    case 6: preferredSlot = "belt"; break; case 7: preferredSlot = "pant"; break;
                    case 8: preferredSlot = "boot"; break; case 9: preferredSlot = "bracer"; break;
                    case 10: preferredSlot = "glove"; break; default: break;
                }
                if (!preferredSlot.empty())
                {
                    for (const std::string& family : families)
                    {
                        std::vector<ModelInfo> overlays;
                        std::string overlayTexture;
                        if (preferredSlot == "shoulder")
                        {
                            for (std::string_view side : {"l", "r", ""})
                            {
                                const auto it = collectionCatalog.find(std::tuple{family, preferredSlot, std::string(side)});
                                if (it != collectionCatalog.end()) overlays.push_back(it->second);
                            }
                        }
                        else
                        {
                            const auto it = collectionCatalog.find(std::tuple{family, preferredSlot, std::string{}});
                            if (it != collectionCatalog.end()) overlays.push_back(it->second);
                            if (overlays.empty())
                                for (const auto& [key, candidate] : collectionCatalog)
                                    if (std::get<0>(key) == family && std::get<1>(key) == preferredSlot)
                                    { overlays.push_back(candidate); break; }
                        }

                        // Retail component-only rows borrow the collection body mesh and texture from a
                        // nearby appearance in the same set. Appearance UiOrder is the stable set ordering
                        // used by the former converter; allow a chest source for non-belt slot overlays.
                        if (overlays.empty())
                        {
                            const auto center = appearancePosition.find(displayId);
                            if (center != appearancePosition.end())
                            {
                                const size_t begin = center->second > 12 ? center->second - 12 : 0;
                                const size_t end = std::min(appearanceOrder.size(), center->second + 13);
                                size_t bestDistance = (std::numeric_limits<size_t>::max)();
                                int bestSlotRank = 3;
                                const CollectionSource* best = nullptr;
                                for (size_t position = begin; position < end; ++position)
                                {
                                    const auto sources = collectionSources.find(appearanceOrder[position]);
                                    if (sources == collectionSources.end()) continue;
                                    for (const CollectionSource& source : sources->second)
                                    {
                                        if (source.family != family) continue;
                                        const int slotRank = source.slot == preferredSlot ? 0 :
                                            (preferredSlot != "belt" && source.slot == "chest" ? 1 : 2);
                                        if (slotRank >= 2) continue;
                                        const size_t distance = position > center->second
                                            ? position - center->second : center->second - position;
                                        if (std::tuple{slotRank, distance} <
                                            std::tuple{bestSlotRank, bestDistance})
                                        {
                                            best = &source;
                                            bestSlotRank = slotRank;
                                            bestDistance = distance;
                                        }
                                    }
                                }
                                if (best)
                                {
                                    overlays.push_back(best->model);
                                    overlayTexture = best->texture;
                                }
                            }
                        }
                        for (size_t overlayIndex = 0; overlayIndex < overlays.size(); ++overlayIndex)
                        {
                            const ModelInfo& info = overlays[overlayIndex];
                            const SkinInfo& skin = skinFor(info);
                            const auto targets = TargetSections(skin, display.inventoryType);
                            if (targets.empty()) continue;
                            itemdisplay::ModelEntry entry;
                            entry.modelSlot = ModelSlot(display.inventoryType);
                            entry.attachId = Attach(display.inventoryType, overlayIndex, true, info.slot, info.model);
                            entry.modelFlags = 0xffffffffu;
                            entry.textureFlags = 0xffffffffu;
                            entry.folder = index->Intern(info.folder);
                            entry.model = index->Intern(info.model);
                            uint32_t textureResource = display.modelMaterials[0];
                            if (!overlayTexture.empty())
                                entry.texture = index->Intern(overlayTexture);
                            else if (components != componentMaterials.end() && !components->second.empty())
                                textureResource = components->second.front().second;
                            if (!entry.texture[0])
                                entry.texture = index->Intern(TextureForResource(textureResource, textureFiles, paths));
                            SetFilter(entry.geoFilter, targets);
                            index->models[displayId].push_back(entry);
                        }
                        if (!overlays.empty()) break;
                    }
                }
            }
        }

        // Character select and CharacterModelFrame are created long before the material pass can
        // finish on a large retail dataset. Publish a self-contained model snapshot now; its interned
        // pointers remain owned by that immutable snapshot while this builder continues in the background.
        auto modelSnapshot = CloneModelSnapshot(*index);
        WLOG_INFO("retail-db2: publishing early item-display models=%zu modelDisplays=%zu strings=%zu",
                  [&] { size_t n=0; for (const auto& [id,v] : modelSnapshot->models) n += v.size(); return n; }(),
                  modelSnapshot->models.size(), modelSnapshot->strings.size());
        itemdisplay::Publish(std::move(modelSnapshot));

        // Material layers are direct ItemDisplayInfoModelMatRes links. Their target batches are derived from
        // the selected model's SKIN section map, exactly where the old CSV generator obtained them.
        index->materials.reserve(modelMaterials.size());
        for (const auto& [displayId, rows] : modelMaterials)
        {
            const auto display = displays.find(displayId);
            const auto chosen = selectedModels.find(displayId);
            if (display == displays.end() || chosen == selectedModels.end()) continue;
            // ModelMatRes.TextureType identifies the replaceable M2 texture type, not an
            // absolute texture-combo layer.  Keep a separate occurrence counter per type:
            // some displays legitimately provide more than one texture of the same type.
            // The equip-side patcher resolves this ordinal against each batch's combo.
            std::array<std::unordered_map<uint32_t, uint32_t>, 2> typeOccurrence;
            std::array<uint32_t, 2> untypedLayer{};
            for (const ModelMaterialSource& source : rows)
            {
                if (source.modelIndex >= 2) continue;
                const ModelInfo& model = chosen->second[source.modelIndex];
                if (model.model.empty()) continue;
                const auto textureFile = textureFiles.find(source.materialResource);
                if (textureFile == textureFiles.end()) continue;
                const auto texturePath = paths.find(textureFile->second);
                if (texturePath == paths.end()) continue;

                const SkinInfo& skin = skinFor(model);
                const auto targets = TargetSections(skin, display->second.inventoryType);
                const bool hide = !model.collection &&
                    (display->second.inventoryType == 1 || display->second.inventoryType == 3) &&
                    source.textureType == 3 && !targets.empty();

                itemdisplay::MaterialEntry entry;
                entry.modelIndex = source.modelIndex;
                entry.modelColumn = source.modelIndex;
                entry.layer = source.textureType == static_cast<uint32_t>(-1)
                    ? untypedLayer[source.modelIndex]++
                    : typeOccurrence[source.modelIndex][source.textureType]++;
                entry.textureType = source.textureType;
                entry.folder = index->Intern(ObjectFolder(texturePath->second));
                entry.model = index->Intern(model.model);
                entry.texture = index->Intern(hide ? "__hide__" : TextureName(texturePath->second));
                entry.skinSectionIds = index->Intern(Join(skin.sectionIds));
                entry.batchIndexes = index->Intern(JoinBatches(skin.batches));
                entry.targetSkinSectionIds = index->Intern(Join(targets));
                entry.targetBatchIndexes = index->Intern(JoinBatches(skin.batches, &targets));
                entry.targetMode = index->Intern(hide ? "HideSlotGeosets" :
                    (!targets.empty() ? "SlotGeosets" : (!skin.batches.empty() ? "SkinMapOnly" : "None")));
                index->materials[displayId].push_back(entry);
            }
        }

        index->materialsReady = true;

        WLOG_INFO("retail-db2: derived item-display index models=%zu modelDisplays=%zu materials=%zu materialDisplays=%zu strings=%zu",
                  [&] { size_t n=0; for (const auto& [id,v] : index->models) n += v.size(); return n; }(),
                  index->models.size(),
                  [&] { size_t n=0; for (const auto& [id,v] : index->materials) n += v.size(); return n; }(),
                  index->materials.size(), index->strings.size());
        return index;
    }
}
