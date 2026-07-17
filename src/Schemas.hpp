// PTR 12.1 item/appearance DB2 declarations used by wxl-retail-db2.
// Field order and relationships mirror the matching wowdbd definitions for build 12.1.0.68209.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "wxl-host-extension/shared/db2/Db2.hpp"

#include <array>

namespace wxl::scripts::retaildb2::schemas
{
    namespace db2 = wxl::runtime::db2;

    inline constexpr std::array kItemFields{
        db2::Field{"ClassID"}, db2::Field{"SubclassID"}, db2::Field{"Material"},
        db2::Field{"InventoryType"}, db2::Field{"SheatheType"},
        db2::Field{"Sound_override_subclassID"}, db2::Field{"IconFileDataID"},
        db2::Field{"ItemGroupSoundsID"}, db2::Field{"ContentTuningID"},
        db2::Field{"ModifiedCraftingReagentItemID"}, db2::Field{"Field_12_0_0_63534_010"},
        db2::Field{"CraftingQualityID"}, db2::Field{"ItemSquishEraID"},
        db2::Field{"RecraftReagentCountPercentage"}, db2::Field{"OrderSource"},
    };
    inline constexpr db2::Definition Item{
        "Item", "item.db2", 0x996192AAu, kItemFields,
    };

    inline constexpr std::array kItemAppearanceFields{
        db2::Field{"DisplayType"}, db2::Field{"ItemDisplayInfoID"},
        db2::Field{"DefaultIconFileDataID"}, db2::Field{"UiOrder"},
        db2::Field{"TransmogPlayerConditionID"},
    };
    inline constexpr std::array kItemAppearanceRelations{
        db2::Relation{"display", db2::RelationSource::Field, "ItemDisplayInfoID", 0,
                      "ItemDisplayInfo", "@id"},
    };
    inline constexpr db2::Definition ItemAppearance{
        "ItemAppearance", "itemappearance.db2", 0x481C4281u,
        kItemAppearanceFields, kItemAppearanceRelations,
    };

    inline constexpr std::array kItemModifiedAppearanceFields{
        db2::Field{"ID"}, db2::Field{"ItemID"}, db2::Field{"ItemAppearanceModifierID"},
        db2::Field{"ItemAppearanceID"}, db2::Field{"OrderIndex"},
        db2::Field{"TransmogSourceTypeEnum"}, db2::Field{"Flags"},
    };
    inline constexpr std::array kItemModifiedAppearanceRelations{
        db2::Relation{"item", db2::RelationSource::Field, "ItemID", 0, "Item", "@id"},
        db2::Relation{"appearance", db2::RelationSource::Field, "ItemAppearanceID", 0,
                      "ItemAppearance", "@id"},
    };
    inline constexpr db2::Definition ItemModifiedAppearance{
        "ItemModifiedAppearance", "itemmodifiedappearance.db2", 0x03A6C979u,
        kItemModifiedAppearanceFields, kItemModifiedAppearanceRelations,
    };

    inline constexpr std::array kItemDisplayInfoFields{
        db2::Field{"GeosetGroupOverride"}, db2::Field{"ItemVisual"},
        db2::Field{"ParticleColorID"}, db2::Field{"ItemRangedDisplayInfoID"},
        db2::Field{"OverrideSwooshSoundKitID"}, db2::Field{"SheatheTransformMatrixID"},
        db2::Field{"StateSpellVisualKitID"}, db2::Field{"SheathedSpellVisualKitID"},
        db2::Field{"UnsheathedSpellVisualKitID"}, db2::Field{"Flags"},
        db2::Field{"ModelResourcesID", 2}, db2::Field{"ModelMaterialResourcesID", 2},
        db2::Field{"ModelType", 2}, db2::Field{"GeosetGroup", 6},
        db2::Field{"AttachmentGeosetGroup", 6}, db2::Field{"HelmetGeosetVis", 2},
    };
    inline constexpr std::array kItemDisplayInfoRelations{
        db2::Relation{"model0", db2::RelationSource::Field, "ModelResourcesID", 0,
                      "ModelFileData", "ModelResourcesID"},
        db2::Relation{"model1", db2::RelationSource::Field, "ModelResourcesID", 1,
                      "ModelFileData", "ModelResourcesID"},
        db2::Relation{"modelMaterial0", db2::RelationSource::Field, "ModelMaterialResourcesID", 0,
                      "TextureFileData", "MaterialResourcesID"},
        db2::Relation{"modelMaterial1", db2::RelationSource::Field, "ModelMaterialResourcesID", 1,
                      "TextureFileData", "MaterialResourcesID"},
    };
    inline constexpr db2::Definition ItemDisplayInfo{
        "ItemDisplayInfo", "itemdisplayinfo.db2", 0x9F3AB8A9u,
        kItemDisplayInfoFields, kItemDisplayInfoRelations,
    };

    inline constexpr std::array kItemDisplayInfoMaterialResFields{
        db2::Field{"ComponentSection"}, db2::Field{"MaterialResourcesID"},
    };
    inline constexpr std::array kItemDisplayInfoMaterialResRelations{
        db2::Relation{"display", db2::RelationSource::ParentId, {}, 0, "ItemDisplayInfo", "@id"},
        db2::Relation{"material", db2::RelationSource::Field, "MaterialResourcesID", 0,
                      "TextureFileData", "MaterialResourcesID"},
    };
    inline constexpr db2::Definition ItemDisplayInfoMaterialRes{
        "ItemDisplayInfoMaterialRes", "itemdisplayinfomaterialres.db2", 0xAA462C0Eu,
        kItemDisplayInfoMaterialResFields, kItemDisplayInfoMaterialResRelations,
    };

    inline constexpr std::array kItemDisplayInfoModelMatResFields{
        db2::Field{"MaterialResourcesID"}, db2::Field{"TextureType"}, db2::Field{"ModelIndex"},
    };
    inline constexpr std::array kItemDisplayInfoModelMatResRelations{
        db2::Relation{"display", db2::RelationSource::ParentId, {}, 0, "ItemDisplayInfo", "@id"},
        db2::Relation{"material", db2::RelationSource::Field, "MaterialResourcesID", 0,
                      "TextureFileData", "MaterialResourcesID"},
    };
    inline constexpr db2::Definition ItemDisplayInfoModelMatRes{
        "ItemDisplayInfoModelMatRes", "itemdisplayinfomodelmatres.db2", 0x52510D63u,
        kItemDisplayInfoModelMatResFields, kItemDisplayInfoModelMatResRelations,
    };

    inline constexpr std::array kModelFileDataFields{
        db2::Field{"GeoBox", 6}, db2::Field{"FileDataID"}, db2::Field{"Flags"},
        db2::Field{"LodCount"}, db2::Field{"ModelResourcesID"},
    };
    inline constexpr db2::Definition ModelFileData{
        "ModelFileData", "modelfiledata.db2", 0x2AE4E788u, kModelFileDataFields,
    };

    inline constexpr std::array kTextureFileDataFields{
        db2::Field{"FileDataID"}, db2::Field{"UsageType"}, db2::Field{"MaterialResourcesID"},
    };
    inline constexpr db2::Definition TextureFileData{
        "TextureFileData", "texturefiledata.db2", 0xBD7C74C2u, kTextureFileDataFields,
    };

    inline constexpr std::array kComponentModelFileDataFields{
        db2::Field{"GenderIndex"}, db2::Field{"ClassID"}, db2::Field{"RaceID"},
        db2::Field{"PositionIndex"},
    };
    inline constexpr db2::Definition ComponentModelFileData{
        "ComponentModelFileData", "componentmodelfiledata.db2", 0xAD90D87Au,
        kComponentModelFileDataFields, {}, false,
    };

    inline constexpr std::array kComponentTextureFileDataFields{
        db2::Field{"GenderIndex"}, db2::Field{"ClassID"}, db2::Field{"RaceID"},
    };
    inline constexpr db2::Definition ComponentTextureFileData{
        "ComponentTextureFileData", "componenttexturefiledata.db2", 0xB32B030Au,
        kComponentTextureFileDataFields, {}, false,
    };

    inline constexpr std::array All{
        Item, ItemAppearance, ItemModifiedAppearance, ItemDisplayInfo,
        ItemDisplayInfoMaterialRes, ItemDisplayInfoModelMatRes,
        ModelFileData, TextureFileData, ComponentModelFileData, ComponentTextureFileData,
    };
}
