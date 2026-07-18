# Supported retail DB2 data

This module separates DB2 support into two levels. A table being catalog-loadable means WXL can validate,
decode, snapshot, and query it; it does not mean the 3.3.5a client automatically uses that table in native
gameplay systems.

## Compatibility target

The checked-in catalog targets retail build `12.1.0.68209` and contains:

| Coverage | Count | Meaning |
| --- | ---: | --- |
| Catalog schemas | 1,319 | Tables represented by the generated wowdbd schema graph |
| Catalog-declared loadable tables | 1,130 | Schemas with DB2 filename, WDC5 layout hash, and physical field metadata |
| Metadata-only schemas | 189 | Known schemas without complete filename, layout, and physical-field metadata |
| Logical columns | 10,339 | Named columns exposed through the catalog API |
| Physical WDC5 fields | 7,417 | Decoder fields used by the loadable definitions |
| Declared links | 2,914 | 2,913 resolvable parent/child links plus 1 preserved unresolved link |

Every load is checked against its generated layout hash. Data from another retail build fails closed when
the layout differs instead of being decoded with an incompatible schema.

The generated source of truth is `src/GeneratedSchemaCatalog.inc`. Use `catalog::Find(table)` and inspect
`Schema::available` when code needs to determine whether a table has a complete generated definition.
An available definition still requires the corresponding file to be mounted; the host verifies the actual
file and layout hash when loading it. The full list is kept generated rather than duplicated here so this
document cannot silently drift from the catalog.

Against the current local `12.1.0.68209` export, 1,129 of the 1,130 catalog-declared tables match their file
layout and physical field count. `CraftingQualityAtlasSet` is the sole mismatch (file layout `4FC8E439`,
catalog layout `9BD23E0F`) and therefore fails closed until either the file or generated schema is updated.

## Tables currently used by gameplay compatibility

These tables are loaded during the retail item/equipment bootstrap and already have purpose-built runtime
consumers:

- `Item`
- `ItemAppearance`
- `ItemModifiedAppearance`
- `ItemDisplayInfo`
- `ItemDisplayInfoMaterialRes`
- `ItemDisplayInfoModelMatRes`
- `ModelFileData`
- `TextureFileData`
- `ComponentModelFileData`

Together they provide retail-only item lookup, icons, inventory types, display relationships, models,
textures, collection geosets, attachments, and material overrides. Native WotLK DBC rows remain authoritative
when the same item or display already exists in the 3.3.5a client.

`ComponentTextureFileData` also has a focused declaration in `src/Schemas.hpp`, but the current item bootstrap
does not load or consume it. All ten focused declarations—the nine active tables above plus this declaration—
have been verified against the local `12.1.0.68209` export.

`TextureFilePath.db2` and `ModelFilePath.db2` are DB2Gen WDC1 path indexes used by the host FileDataID service.
They are infrastructure inputs rather than tables exposed through the WDC5 gameplay catalog.

## On-demand catalog support

Any of the 1,130 catalog-declared tables can be requested by another WXL script without adding another DB2
parser when its matching file is mounted:

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

Loading is on demand through the 64-bit host. Runtime receives the full requested table as an
architecture-neutral decoded snapshot; its consumer should derive compact indexes and then release the
table. The host caches requested decoded snapshots for its process lifetime, so modules should not bulk-load
all catalog tables. A new gameplay feature still requires explicit hooks and consumer logic; catalog
availability alone never replaces native client storage.

## Decoder boundary

- The host extension owns archive access, WDC1/WDC2/WDC3/WDC5 decoding, layout validation, caching, and
  FileDataID path snapshots.
- This module owns schema declarations, generated relationship metadata, item/display indexes, and focused
  client hooks.
- The 32-bit runtime does not mount retail archives or parse raw WDC files.
- `RelationIndex` in the shared host DB2 API provides reusable one-to-many joins for parent IDs or ordinary
  relationship fields.

## Updating the supported build

After replacing the retail DB2 export or wowdbd schema graph:

```powershell
node tools/GenerateSchemaCatalog.mjs `
  --input C:\wamp64\www\wowdbd\data\schema.json `
  --db2-root D:\path\to\retail-export-root

node tools/GenerateSchemaCatalog.mjs `
  --input C:\wamp64\www\wowdbd\data\schema.json `
  --check
```

Then build and run both validators:

```powershell
cmake --build build/dll --config Release --target wxl-db2-validate
cmake --build build/dll --config Release --target wxl-db2-schema-check
build/dll/Release/wxl-db2-schema-check.exe
```

Commit the regenerated catalog, update the counts and build number above, and test every purpose-built
gameplay consumer before declaring the new retail build supported.
