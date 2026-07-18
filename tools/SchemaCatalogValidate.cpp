// Standalone structural validator for the generated retail DB2 catalog.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "wxl-retail-db2/shared/SchemaCatalog.hpp"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// SchemaCatalog.cpp also provides the runtime Load/RawValue entry points. This metadata-only validator
// does not connect to the host, so satisfy those two unused Table methods without linking client IPC.
namespace wxl::runtime::db2
{
    bool Table::Load(const Definition&, std::string*) { return false; }
    uint32_t Table::Value(const wdc5::Row&, size_t, size_t) const noexcept { return 0; }
    uint64_t Table::Value64(const wdc5::Row&, size_t, size_t) const noexcept { return 0; }
    std::string_view Table::String(const wdc5::Row&, size_t, size_t) const noexcept { return {}; }
}

namespace
{
    namespace catalog = wxl::scripts::retaildb2::catalog;
    namespace wdc5 = wxl::runtime::db2::wdc5;

    template <class T>
    void Store(std::vector<uint8_t>& bytes, size_t offset, T value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    bool Validate64BitRoundTrip()
    {
        // One-record, one-field WDC5 fixture. A field_meta bit count of -32 yields a 64-bit
        // uncompressed scalar (32 - -32), exercising both decoder words and snapshot v3.
        constexpr uint32_t layout = 0x64DB2001u;
        constexpr uint64_t expected = 0xFEDCBA9876543210ull;
        constexpr size_t recordOffset = 204 + 40 + 4 + 24;
        std::vector<uint8_t> bytes(recordOffset + sizeof(expected));
        Store<uint32_t>(bytes, 0, 0x35434457u); // WDC5
        Store<uint32_t>(bytes, 4, 1u);
        constexpr char schema[] = "wxl_schema_selftest_64";
        std::memcpy(bytes.data() + 8, schema, sizeof(schema) - 1);
        Store<uint32_t>(bytes, 136, 1u); // records
        Store<uint32_t>(bytes, 140, 1u); // fields
        Store<uint32_t>(bytes, 144, 8u); // record size
        Store<uint32_t>(bytes, 152, 0x64DB2002u);
        Store<uint32_t>(bytes, 156, layout);
        Store<int32_t>(bytes, 160, 0);
        Store<int32_t>(bytes, 164, 0);
        Store<uint32_t>(bytes, 168, 1u); // locale
        Store<uint16_t>(bytes, 172, 0u); // inline id
        Store<uint16_t>(bytes, 174, 0u);
        Store<uint32_t>(bytes, 176, 1u); // total fields
        Store<uint32_t>(bytes, 188, 24u); // column metadata bytes
        Store<uint32_t>(bytes, 200, 1u); // sections

        Store<uint32_t>(bytes, 212, static_cast<uint32_t>(recordOffset));
        Store<uint32_t>(bytes, 216, 1u);
        Store<uint32_t>(bytes, 224, static_cast<uint32_t>(recordOffset + sizeof(expected)));

        Store<int16_t>(bytes, 244, -32);
        Store<uint16_t>(bytes, 246, 0u);
        Store<uint16_t>(bytes, 248, 0u);
        Store<uint16_t>(bytes, 250, 64u);
        Store<uint64_t>(bytes, recordOffset, expected);

        std::string error;
        wdc5::Table decoded;
        const std::vector<wdc5::FieldShape> shapes{{1, 2, false}};
        if (!decoded.Load(bytes.data(), bytes.size(), shapes, layout, &error) ||
            decoded.Rows().size() != 1 || decoded.Value64(decoded.Rows().front(), 0) != expected)
        {
            std::cerr << "64-bit decoder self-test failed: " << error << '\n';
            return false;
        }

        std::vector<uint8_t> snapshot;
        wdc5::Table roundTrip;
        if (!decoded.SaveSnapshot(snapshot, &error) ||
            !roundTrip.LoadSnapshot(snapshot.data(), snapshot.size(), layout, &error) ||
            roundTrip.Rows().size() != 1 ||
            roundTrip.Value64(roundTrip.Rows().front(), 0) != expected)
        {
            std::cerr << "64-bit snapshot self-test failed: " << error << '\n';
            return false;
        }
        return true;
    }

    bool ValidateFile(const catalog::Schema& schema, const char* path, const char* rowArgument)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            std::cerr << "cannot open " << path << '\n';
            return false;
        }
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        std::vector<wdc5::FieldShape> shapes;
        shapes.reserve(schema.fields.size());
        for (const wxl::runtime::db2::Field& field : schema.fields)
            shapes.push_back({field.elements, field.words, field.string});

        std::string error;
        wdc5::Table decoded;
        if (!decoded.Load(bytes.data(), bytes.size(), shapes, schema.layoutHash, &error))
        {
            std::cerr << error << '\n';
            return false;
        }

        std::vector<uint8_t> snapshot;
        wdc5::Table roundTrip;
        if (!decoded.SaveSnapshot(snapshot, &error) ||
            !roundTrip.LoadSnapshot(snapshot.data(), snapshot.size(), schema.layoutHash, &error) ||
            roundTrip.Rows().size() != decoded.Rows().size())
        {
            std::cerr << "snapshot round-trip failed: " << error << '\n';
            return false;
        }

        const wdc5::Row* row = nullptr;
        if (rowArgument)
        {
            try
            {
                const uint32_t id = static_cast<uint32_t>(std::stoul(rowArgument));
                row = roundTrip.Find(id);
            }
            catch (const std::exception&)
            {
                std::cerr << "invalid row id: " << rowArgument << '\n';
                return false;
            }
            if (!row)
            {
                std::cerr << schema.name << " has no row " << rowArgument << '\n';
                return false;
            }
        }
        else if (!roundTrip.Rows().empty())
        {
            row = &roundTrip.Rows().front();
        }

        std::cout << "table=" << schema.name
                  << " layout=0x" << std::hex << std::uppercase << schema.layoutHash << std::dec
                  << " rows=" << roundTrip.Rows().size()
                  << " fields=" << schema.fields.size()
                  << " snapshot=" << snapshot.size() << '\n';
        if (!row) return true;

        for (const catalog::Column& column : schema.columns)
        {
            std::cout << column.name << '=';
            if (column.source == catalog::ColumnSource::RowId)
                std::cout << row->id;
            else if (column.source == catalog::ColumnSource::ParentId)
                std::cout << row->parentId;
            else
            {
                const wxl::runtime::db2::Field& field = schema.fields[column.physicalField];
                if (field.string)
                    std::cout << std::quoted(roundTrip.String(*row, column.physicalField, column.element));
                else if (field.words == 2)
                    std::cout << roundTrip.Value64(*row, column.physicalField, column.element);
                else
                    std::cout << roundTrip.Value(*row, column.physicalField, column.element);
            }
            std::cout << '\n';
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    std::string error;
    if (!catalog::Validate(&error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (!Validate64BitRoundTrip()) return 1;

    size_t available = 0;
    size_t columns = 0;
    size_t links = 0;
    for (const catalog::Schema& schema : catalog::All())
    {
        available += schema.available ? 1u : 0u;
        columns += schema.columns.size();
        links += schema.links.size();
    }
    std::cout << "tables=" << catalog::All().size()
              << " available=" << available
              << " columns=" << columns
              << " links=" << links
              << " selftest64=ok\n";

    if (argc == 1) return 0;
    if (argc != 3 && argc != 4)
    {
        std::cerr << "usage: wxl-db2-schema-check [table file.db2 [row-id]]\n";
        return 2;
    }
    const catalog::Schema* schema = catalog::Find(argv[1]);
    if (!schema)
    {
        std::cerr << "unknown catalog table: " << argv[1] << '\n';
        return 2;
    }
    if (!schema->available)
    {
        std::cerr << schema->name << " has no loadable 12.1.x definition\n";
        return 2;
    }
    if (!ValidateFile(*schema, argv[2], argc == 4 ? argv[3] : nullptr)) return 1;
    return 0;
}
