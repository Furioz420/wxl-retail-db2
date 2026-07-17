# WXL retail DB2 compatibility

This module provides WarcraftXL with direct, relationship-aware access to Blizzard-like retail DB2 data.
Native 3.3.5a DBC records remain authoritative for existing client paths; focused compatibility consumers
use retail data when the old client has no equivalent record or field.

See [`SUPPORTED_DB2.md`](SUPPORTED_DB2.md) for the distinction between catalog-loadable tables and tables
already connected to 3.3.5a gameplay systems.

## Complete 12.1.x schema catalog

`shared/SchemaCatalog.hpp` exposes the complete graph generated from the local wowdbd dataset for build
`12.1.0.68209`:

- 1,319 table schemas
- 10,339 logical columns mapped to 7,417 physical WDC5 fields
- 2,914 declared parent links, with reverse child discovery
- 1,130 tables with catalog-declared filenames, layout hashes, and physical field metadata

Tables are loaded **on demand** through the 64-bit host decoder. They are not all copied into the 32-bit
client at startup. This keeps address-space use bounded and lets WXL modules request only the DB2 graphs
they actually consume.

The catalog API provides case-insensitive table and column lookup, typed column metadata (including DBD
storage width and signedness), raw 32/64-bit row access, decoded strings, parent/child link discovery,
layout validation, and construction of the existing `db2::Definition` used by the host transport.
Purpose-built modules can use it like this:

```cpp
#include "wxl-retail-db2/shared/SchemaCatalog.hpp"

namespace catalog = wxl::scripts::retaildb2::catalog;
wxl::runtime::db2::Table table;
std::string error;
if (catalog::Load("CreatureDisplayInfo", table, &error))
{
    const catalog::Schema* schema = catalog::Find("CreatureDisplayInfo");
    const catalog::Column* model = schema->FindColumn("ModelID");
    if (const auto* row = table.Find(123))
        const uint32_t modelId = schema->RawValue(*row, table, *model);
}
```

Regenerate the checked-in catalog after updating wowdbd:

```powershell
node tools/GenerateSchemaCatalog.mjs `
  --input C:\wamp64\www\wowdbd\data\schema.json
```

CI or local verification can detect a stale checked-in catalog without rewriting it:

```powershell
node tools/GenerateSchemaCatalog.mjs `
  --input C:\wamp64\www\wowdbd\data\schema.json `
  --check
```

Pass `--db2-root <directory>` when the linked DB2 files are locally present. The generator will then verify
every WDC5 magic, layout hash, and physical-field count against the generated schema.

## WDC5 reader

`wxl-host-extension/host/db2/Wdc5.cpp` reads the PTR 12.1 WDC5 variant (including its 128-byte schema
name), packed fields, signed immediate fields, 64-bit values, section-relative strings/localized strings,
common data, pallets, pallet arrays, sparse variable-size rows with inline strings, non-inline IDs, copy
rows, and relationship data. The injected runtime receives decoded snapshots through the existing host
transport; it does not parse DB2 files or mount archives itself.

The focused declarations in `src/Schemas.hpp` remain the startup set used by item/equipment compatibility.
They use the same host-extension DB2 API and are validated independently alongside the generated global
catalog.

The tables currently consumed eagerly for item/equipment compatibility are:

- `Item`
- `ItemAppearance`
- `ItemModifiedAppearance`
- `ItemDisplayInfo`
- `ItemDisplayInfoMaterialRes`
- `ItemDisplayInfoModelMatRes`
- `ModelFileData`
- `TextureFileData`
- `ComponentModelFileData`

`ComponentTextureFileData` has a focused declaration but is not currently loaded by the item bootstrap.
All other catalog-declared tables can be loaded on demand when their corresponding file is mounted. The
reader validates each actual file and layout hash before decoding, so a retail update with a changed layout
fails closed instead of interpreting a new schema as the old one.

## Validator

`wxl-db2-validate` is a developer executable and is not deployed to the client:

```powershell
cmake --build build/dll --config Release --target wxl-db2-validate
build/dll/Release/wxl-db2-validate.exe item DB2/item.db2 225744
```

Decoded samples have been compared with the PTR CSV exports, including Item 225744 and
ItemDisplayInfo 64278.

The complete generated graph has a separate validator. With no arguments it checks all catalog metadata
and a synthetic 64-bit decode/snapshot regression; with a table and file it also decodes a real DB2 and
verifies the architecture-neutral snapshot round-trip:

```powershell
cmake --build build/dll --config Release --target wxl-db2-schema-check
build/dll/Release/wxl-db2-schema-check.exe
build/dll/Release/wxl-db2-schema-check.exe Map DB2/map.db2 0
```

## FileData paths

Retail item DB2 rows refer to assets by `FileDataID`. The host resolves those IDs through DB2Gen's canonical
WDC1 tables: `DBFilesClient\TextureFilePath.db2` and `DBFilesClient\ModelFilePath.db2`. It publishes one
neutral FileDataID-to-path snapshot to runtime consumers, so the injected side does not parse WDC1 either.
The compatibility layer no longer reads or deploys `WXLFileDataPaths.csv`.

The retail-item loader and FileDataID resolver intentionally consume the same host-owned canonical,
archive-backed files. Do not deploy newer client-only copies under alternate names.

DB2 decoding and relationship indexing run on a background thread. WXL currently applies fallback only
through the shared database accessor. An experiment that replaced the inline `Item` and
`ItemDisplayInfo` ID tables was rolled back after it caused a null-call crash shortly after character
selection. Inventory-type and right-click paths that bypass the accessor must be hooked narrowly; the
storage object's internal tables must not be replaced wholesale. Icon resolution now has two such narrow
hooks: `CGItem_C::GetInventoryArt(displayId)` supplies the retail basename after the native resolver returns
its question-mark fallback, and Lua `GetItemIcon(itemId)` supplies the full path when the inline native
`Item.dbc` range check rejects a retail-only item. Native WotLK rows remain authoritative in both paths.

For addons that explicitly want retail DB2 data without entering any native `Item.dbc` code path, the
reload-safe Lua API also exposes:

- `GetRetailItemIcon(itemIDOrLink)` -> `Interface\\Icons\\...` or `nil`
- `GetRetailItemInfo(itemIDOrLink)` -> the stock 3.3.5 `GetItemInfo` tuple with its icon replaced by
  the retail DB2 icon when available
- `GetRetailItemAppearance(itemIDOrLink)` -> `displayID, appearanceID, iconFileDataID` or `nil`
- `GetRetailFileDataPath(fileDataID)` -> the canonical path for FileDataIDs already indexed as dependencies
  of the loaded item/display DB2 rows, or `nil`
- `GetRetailDB2Status()` -> `ready, failed`, allowing addons to wait for background indexing

The item icon lookup retains the selected `ItemModifiedAppearance -> ItemAppearance` relationship. It does
not collapse the item to a display and select an arbitrary icon when several appearances share that display.
When no selected appearance supplies an icon, it falls back to `Item.IconFileDataID` from the same retail
DB2 snapshot.

The compatibility bootstrap also repairs the texture returned by both `GetContainerItemInfo` and the
table-returning `C_Container.GetContainerItemInfo`. This is necessary because the 3.3.5 container cache
reads its icon directly from `Item.dbc` and never calls the DB2-aware `GetItemIcon` hook.
Additional DB2 APIs should follow this purpose-built pattern instead of publishing synthetic native storage
tables or offering schema-blind raw-row access.

The module also publishes an immutable item-display index for runtime scripts. Attachments come from
`ItemDisplayInfo`, `ModelFileData`, `TextureFileData`, and the component material tables. The missing
old-client targeting information is derived directly from each referenced `.skin`: section IDs select the
slot geosets and batch-to-section mappings target `ItemDisplayInfoModelMatRes` layers. The equip extension
therefore no longer reads `WXLItemDisplayModels.csv` or `WXLItemDisplayModelMaterials.csv`.

Publication uses three immutable snapshots: direct object models first, then the complete model/geoset index,
and finally material/SKIN targeting with `materialsReady=true`. Each phase is swapped atomically; until a
phase is available, the equip extension retains the normal DBC path and retries the shared snapshot on the
next rebuild. Consumers never observe a snapshot while it is being constructed.
