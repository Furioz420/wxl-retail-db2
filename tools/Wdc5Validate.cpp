// Small developer validator for the WDC5 reader. It decodes a supported retail table and prints rows.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "../src/Schemas.hpp"
#include "wxl-host-extension/shared/db2/Wdc5.hpp"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace runtimeDb2 = wxl::runtime::db2;

namespace
{
    struct Schema
    {
        uint32_t hash;
        std::vector<runtimeDb2::wdc5::FieldShape> shapes;
    };

    Schema SchemaFor(const std::string& name)
    {
        for (const runtimeDb2::Definition& definition : wxl::scripts::retaildb2::schemas::All)
        {
            std::string lower(definition.name);
            for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower != name) continue;
            Schema schema{definition.layoutHash, {}};
            schema.shapes.reserve(definition.fields.size());
            for (const runtimeDb2::Field& field : definition.fields)
                schema.shapes.push_back({field.elements, field.words, field.string});
            return schema;
        }
        return {};
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: wxl-db2-validate <table-name> <file.db2> [row-id|parent:<id>]\n";
        return 2;
    }
    const Schema schema = SchemaFor(argv[1]);
    if (!schema.hash)
    {
        std::cerr << "unsupported table name\n";
        return 2;
    }

    std::ifstream stream(argv[2], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        std::cerr << "unable to read DB2\n";
        return 1;
    }

    runtimeDb2::wdc5::Table table;
    std::string error;
    if (!table.Load(bytes.data(), bytes.size(), schema.shapes, schema.hash, &error))
    {
        std::cerr << error << '\n';
        return 1;
    }

    std::vector<uint8_t> snapshot;
    runtimeDb2::wdc5::Table roundTrip;
    if (!table.SaveSnapshot(snapshot, &error) ||
        !roundTrip.LoadSnapshot(snapshot.data(), snapshot.size(), schema.hash, &error) ||
        roundTrip.Rows().size() != table.Rows().size() ||
        roundTrip.LayoutHash() != table.LayoutHash())
    {
        std::cerr << "snapshot round-trip failed: " << error << '\n';
        return 1;
    }

    const runtimeDb2::wdc5::Row* selected = nullptr;
    std::vector<const runtimeDb2::wdc5::Row*> selectedParents;
    bool parentQuery = false;
    if (argc >= 4)
    {
        const std::string selector = argv[3];
        constexpr char prefix[] = "parent:";
        if (selector.rfind(prefix, 0) == 0)
        {
            parentQuery = true;
            const uint32_t parentId = static_cast<uint32_t>(std::stoul(selector.substr(sizeof(prefix) - 1)));
            for (const auto& row : table.Rows())
                if (row.parentId == parentId) selectedParents.push_back(&row);
        }
        else selected = table.Find(static_cast<uint32_t>(std::stoul(selector)));
    }
    const size_t count = parentQuery ? selectedParents.size()
                       : selected ? 1 : std::min<size_t>(5, table.Rows().size());
    std::cout << "schema=" << table.Schema() << " layout=" << std::hex << std::uppercase
              << std::setw(8) << std::setfill('0') << table.LayoutHash() << std::dec
              << " rows=" << table.Rows().size() << '\n';
    for (size_t i = 0; i < count; ++i)
    {
        const auto& row = parentQuery ? *selectedParents[i] : selected ? *selected : table.Rows()[i];
        std::cout << "id=" << row.id << " parent=" << row.parentId;
        for (size_t f = 0; f < schema.shapes.size(); ++f)
        {
            std::cout << " f" << f << '=';
            for (size_t e = 0; e < table.ElementCount(f); ++e)
            {
                if (e) std::cout << ',';
                std::cout << table.Value64(row, f, e);
            }
        }
        std::cout << '\n';
    }
    if (argc >= 4 && !parentQuery && !selected)
    {
        std::cerr << "row not found\n";
        return 3;
    }
    if (parentQuery && selectedParents.empty())
    {
        std::cerr << "parent rows not found\n";
        return 3;
    }
    return 0;
}
