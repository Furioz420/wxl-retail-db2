// Generated-schema catalog for Blizzard-like retail DB2 data.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "wxl-host-extension/shared/db2/Db2.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wxl::scripts::retaildb2::catalog
{
    namespace db2 = wxl::runtime::db2;

    enum class ValueKind : uint8_t
    {
        Unknown,
        Integer,
        Float,
        String,
        LocalizedString,
    };

    enum class ColumnSource : uint8_t
    {
        Field,
        RowId,
        ParentId,
    };

    struct Column
    {
        std::string_view name;
        std::string_view field;
        ValueKind kind = ValueKind::Unknown;
        std::string_view storage;
        ColumnSource source = ColumnSource::Field;
        uint16_t physicalField = 0;
        uint16_t element = 0;
        bool optional = false;
    };

    struct Link
    {
        std::string_view sourceTable;
        std::string_view sourceColumn;
        std::string_view relation;
        std::string_view targetTable;
        std::string_view targetColumn;
        bool targetExists = false;
    };

    struct Schema
    {
        std::string_view name;
        std::string_view filename;
        uint32_t layoutHash = 0;
        uint32_t rowCount = 0;
        uint64_t fileSize = 0;
        std::span<const db2::Field> fields;
        std::span<const db2::Relation> relations;
        std::span<const Column> columns;
        std::span<const Link> links;
        bool available = false;

        db2::Definition Definition(bool required = false) const noexcept;
        const Column* FindColumn(std::string_view column) const noexcept;
        uint32_t RawValue(const db2::wdc5::Row& row, const db2::Table& table,
                          const Column& column) const noexcept;
        uint64_t RawValue64(const db2::wdc5::Row& row, const db2::Table& table,
                            const Column& column) const noexcept;
        std::string_view StringValue(const db2::wdc5::Row& row, const db2::Table& table,
                                     const Column& column) const noexcept;
    };

    /** All 12.1.x wowdbd schemas, including tables absent from the mounted DB2 export. */
    std::span<const Schema> All() noexcept;

    /** Case-insensitive table lookup. */
    const Schema* Find(std::string_view table) noexcept;

    /** Loads one available table through the host WDC5 service. Tables are never globally preloaded. */
    bool Load(std::string_view table, db2::Table& out, std::string* error = nullptr);

    /** All declared outgoing/incoming links. The returned spans remain valid for process lifetime. */
    std::span<const Link* const> Parents(std::string_view table) noexcept;
    std::span<const Link* const> Children(std::string_view table) noexcept;

    /** Validates generated offsets, physical fields, and every relationship endpoint. */
    bool Validate(std::string* error = nullptr);
}
