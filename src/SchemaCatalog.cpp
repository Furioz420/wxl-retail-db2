// Generic on-demand retail DB2 schema and relationship catalog.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "wxl-retail-db2/shared/SchemaCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wxl::scripts::retaildb2::catalog
{
    namespace
    {
        struct GeneratedTable
        {
            std::string_view name;
            std::string_view filename;
            uint32_t layoutHash;
            uint32_t rowCount;
            uint64_t fileSize;
            uint32_t fieldBegin;
            uint32_t fieldCount;
            uint32_t relationBegin;
            uint32_t relationCount;
            uint32_t columnBegin;
            uint32_t columnCount;
            uint32_t linkBegin;
            uint32_t linkCount;
            bool available;
        };

#include "GeneratedSchemaCatalog.inc"

        struct TransparentCaseHash
        {
            using is_transparent = void;
            size_t operator()(std::string_view value) const noexcept
            {
                size_t hash = 1469598103934665603ull;
                for (unsigned char ch : value)
                {
                    hash ^= static_cast<unsigned char>(std::tolower(ch));
                    hash *= 1099511628211ull;
                }
                return hash;
            }
        };

        struct TransparentCaseEqual
        {
            using is_transparent = void;
            bool operator()(std::string_view left, std::string_view right) const noexcept
            {
                return left.size() == right.size() && std::equal(
                    left.begin(), left.end(), right.begin(),
                    [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
            }
        };

        struct CatalogState
        {
            std::vector<Schema> schemas;
            std::unordered_map<std::string_view, size_t, TransparentCaseHash, TransparentCaseEqual> byName;
            std::unordered_map<std::string_view, std::vector<const Link*>,
                               TransparentCaseHash, TransparentCaseEqual> parents;
            std::unordered_map<std::string_view, std::vector<const Link*>,
                               TransparentCaseHash, TransparentCaseEqual> children;

            CatalogState()
            {
                schemas.reserve(std::size(kGeneratedTables));
                byName.reserve(std::size(kGeneratedTables));
                for (const GeneratedTable& table : kGeneratedTables)
                {
                    schemas.push_back({
                        table.name, table.filename, table.layoutHash, table.rowCount, table.fileSize,
                        std::span{kGeneratedFields}.subspan(table.fieldBegin, table.fieldCount),
                        std::span{kGeneratedRelations}.subspan(table.relationBegin, table.relationCount),
                        std::span{kGeneratedColumns}.subspan(table.columnBegin, table.columnCount),
                        std::span{kGeneratedLinks}.subspan(table.linkBegin, table.linkCount),
                        table.available,
                    });
                    byName.emplace(schemas.back().name, schemas.size() - 1);
                }
                parents.reserve(schemas.size());
                children.reserve(schemas.size());
                for (const Link& link : kGeneratedLinks)
                {
                    parents[link.sourceTable].push_back(&link);
                    children[link.targetTable].push_back(&link);
                }
            }
        };

        CatalogState& State()
        {
            static CatalogState state;
            return state;
        }

        bool Fail(std::string* error, std::string message)
        {
            if (error) *error = std::move(message);
            return false;
        }

        std::span<const Link* const> LinksFor(
            const std::unordered_map<std::string_view, std::vector<const Link*>,
                                     TransparentCaseHash, TransparentCaseEqual>& index,
            std::string_view table) noexcept
        {
            const auto it = index.find(table);
            return it == index.end() ? std::span<const Link* const>{} : std::span<const Link* const>{it->second};
        }
    }

    db2::Definition Schema::Definition(bool required) const noexcept
    {
        return {name, filename, layoutHash, fields, relations, required};
    }

    const Column* Schema::FindColumn(std::string_view column) const noexcept
    {
        const auto it = std::ranges::find_if(columns, [column](const Column& value) {
            return TransparentCaseEqual{}(value.name, column);
        });
        return it == columns.end() ? nullptr : &*it;
    }

    uint32_t Schema::RawValue(const db2::wdc5::Row& row, const db2::Table& table,
                              const Column& column) const noexcept
    {
        switch (column.source)
        {
            case ColumnSource::RowId: return row.id;
            case ColumnSource::ParentId: return row.parentId;
            case ColumnSource::Field: return table.Value(row, column.physicalField, column.element);
        }
        return 0;
    }

    uint64_t Schema::RawValue64(const db2::wdc5::Row& row, const db2::Table& table,
                                const Column& column) const noexcept
    {
        switch (column.source)
        {
            case ColumnSource::RowId: return row.id;
            case ColumnSource::ParentId: return row.parentId;
            case ColumnSource::Field: return table.Value64(row, column.physicalField, column.element);
        }
        return 0;
    }

    std::string_view Schema::StringValue(const db2::wdc5::Row& row, const db2::Table& table,
                                         const Column& column) const noexcept
    {
        if (column.kind != ValueKind::String && column.kind != ValueKind::LocalizedString)
            return {};
        if (column.source != ColumnSource::Field) return {};
        return table.String(row, column.physicalField, column.element);
    }

    std::span<const Schema> All() noexcept
    {
        return State().schemas;
    }

    const Schema* Find(std::string_view table) noexcept
    {
        CatalogState& state = State();
        const auto it = state.byName.find(table);
        return it == state.byName.end() ? nullptr : &state.schemas[it->second];
    }

    bool Load(std::string_view table, db2::Table& out, std::string* error)
    {
        out = {};
        if (error) error->clear();
        const Schema* schema = Find(table);
        if (!schema) return Fail(error, "DB2 catalog: unknown table " + std::string(table));
        if (!schema->available)
            return Fail(error, "DB2 catalog: table has no 12.1.x DB2/layout: " + std::string(schema->name));
        const db2::Definition definition = schema->Definition(false);
        return out.Load(definition, error);
    }

    std::span<const Link* const> Parents(std::string_view table) noexcept
    {
        return LinksFor(State().parents, table);
    }

    std::span<const Link* const> Children(std::string_view table) noexcept
    {
        return LinksFor(State().children, table);
    }

    bool Validate(std::string* error)
    {
        if (error) error->clear();
        const std::span<const Schema> schemas = All();
        if (schemas.empty()) return Fail(error, "DB2 catalog: no generated schemas");
        for (const Schema& schema : schemas)
        {
            if (schema.name.empty()) return Fail(error, "DB2 catalog: unnamed table");
            if (schema.available && (schema.filename.empty() || !schema.layoutHash || schema.fields.empty()))
                return Fail(error, "DB2 catalog: incomplete loadable table " + std::string(schema.name));
            for (const Column& column : schema.columns)
            {
                if (column.name.empty())
                    return Fail(error, "DB2 catalog: unnamed column in " + std::string(schema.name));
                if (column.source == ColumnSource::Field &&
                    (column.physicalField >= schema.fields.size() ||
                     column.element >= schema.fields[column.physicalField].elements))
                    return Fail(error, "DB2 catalog: invalid physical column " + std::string(schema.name) +
                                       "." + std::string(column.name));
            }
            for (const Link& link : schema.links)
            {
                if (link.sourceTable != schema.name || !schema.FindColumn(link.sourceColumn))
                    return Fail(error, "DB2 catalog: invalid source link in " + std::string(schema.name));
                const Schema* target = Find(link.targetTable);
                if (link.targetExists && !target)
                    return Fail(error, "DB2 catalog: missing target table " + std::string(link.targetTable));
                if (link.targetExists && target && link.targetColumn != "@id" &&
                    !target->FindColumn(link.targetColumn))
                    return Fail(error, "DB2 catalog: missing target column " + std::string(link.targetTable) +
                                       "." + std::string(link.targetColumn));
            }
        }
        return true;
    }
}
