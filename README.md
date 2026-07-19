# WXL retail DB2 compatibility

This module replaces the generated `WXLItemDisplayFallback` CSV experiment with direct runtime reads
of retail DB2 tables. Native 3.3.5a DBC records remain authoritative; the shared client database lookup
calls Blizzard's accessor first and considers retail data only when the native lookup misses.

## WDC5 reader

`src/runtime/db2/Wdc5.*` in WXL core reads the PTR 12.1 WDC5 variant (including its 128-byte schema name), packed fields,
signed immediate fields, common data, pallets, pallet arrays, non-inline IDs, copy rows, and relationship
data. Sparse WDC5 tables are rejected explicitly because none of the item tables selected below use the
sparse layout.

The current supported PTR 12.1.0.68209 layouts are:

- `Item`
- `ItemAppearance`
- `ItemModifiedAppearance`
- `ItemDisplayInfo`
- `ItemDisplayInfoMaterialRes`
- `ItemDisplayInfoModelMatRes`
- `ModelFileData`
- `TextureFileData`
- `ComponentModelFileData`
- `ComponentTextureFileData`

The reader validates each layout hash before decoding. A retail update with a changed layout therefore
fails closed instead of interpreting a new schema as the old one.

## Validator

`wxl-db2-validate` is a developer executable and is not deployed to the client:

```powershell
cmake --build build/dll --config Release --target wxl-db2-validate
build/dll/Release/wxl-db2-validate.exe item DB2/item.db2 225744
```

Decoded samples have been compared with the PTR CSV exports, including Item 225744 and
ItemDisplayInfo 64278.

## FileData paths

Retail item DB2 rows refer to assets by `FileDataID`. Runtime resolves those IDs through DB2Gen's canonical
WDC1 tables: `DBFilesClient\TextureFilePath.db2` and `DBFilesClient\ModelFilePath.db2`. Each table stores compact
`{ FileDataID, pathOffset }` records generated from the retail listfile, so the compatibility layer no longer
reads or deploys `WXLFileDataPaths.csv`.

The client retail-item loader and the host FileDataID resolver intentionally consume the same canonical,
archive-backed files. Do not deploy newer client-only copies under alternate names: mismatched listfile
snapshots can make the two sides resolve different model, skin, or texture families for one FileDataID.

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

Publication is atomic and happens only after every required table, FileData path, and derived SKIN entry is
ready. Until then the equip extension retains the normal DBC path and retries the shared snapshot on the
next rebuild; consumers never observe a partially populated index.
