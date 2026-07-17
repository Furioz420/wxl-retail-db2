#!/usr/bin/env node
// Converts wowdbd/data/schema.json into the compact C++ catalog consumed by WXL.

import fs from "node:fs";
import path from "node:path";

function parseArgs(argv) {
  const args = {
    input: "C:\\wamp64\\www\\wowdbd\\data\\schema.json",
    output: path.resolve(import.meta.dirname, "../src/GeneratedSchemaCatalog.inc"),
    build: "12.1.0.68209",
    db2Root: null,
    check: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === "--input") args.input = argv[++i];
    else if (argv[i] === "--output") args.output = argv[++i];
    else if (argv[i] === "--build") args.build = argv[++i];
    else if (argv[i] === "--db2-root") args.db2Root = argv[++i];
    else if (argv[i] === "--check") args.check = true;
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  return args;
}

function quote(value) {
  return `"${String(value ?? "").replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

function enumName(type) {
  if (type === "int") return "ValueKind::Integer";
  if (type === "float") return "ValueKind::Float";
  if (type === "string") return "ValueKind::String";
  if (type === "locstring") return "ValueKind::LocalizedString";
  return "ValueKind::Unknown";
}

function wordCount(storage) {
  return String(storage).toLowerCase().includes("64") ? 2 : 1;
}

function splitArray(name) {
  const match = String(name).match(/^(.*)\[(\d+)]$/);
  return match ? { field: match[1], element: Number(match[2]) } : { field: String(name), element: 0 };
}

function relationName(column, used) {
  let name = column;
  let suffix = 2;
  while (used.has(name.toLowerCase())) name = `${column}#${suffix++}`;
  used.add(name.toLowerCase());
  return name;
}

const args = parseArgs(process.argv.slice(2));
const root = JSON.parse(fs.readFileSync(args.input, "utf8"));
if (root.source?.build !== args.build)
  throw new Error(`schema build ${root.source?.build ?? "unknown"} does not match ${args.build}`);

const fields = [];
const relations = [];
const columns = [];
const links = [];
const tables = [];
const tableNames = new Map(root.tables.map((table) => [table.name.toLowerCase(), table]));

for (const table of root.tables) {
  const columnNames = new Set(table.columns.map((column) => column.name.toLowerCase()));
  for (const parent of table.parents || []) {
    if (!columnNames.has(String(parent.sourceColumn).toLowerCase()))
      throw new Error(`${table.name}: relationship source column is absent: ${parent.sourceColumn}`);
    if (!parent.targetExists) continue;
    const target = tableNames.get(String(parent.targetTable).toLowerCase());
    if (!target) throw new Error(`${table.name}: relationship target table is absent: ${parent.targetTable}`);
    // Some DBD references point at a field that is not present in this exact build of the target.
    // Preserve those links as unresolved metadata instead of silently deleting historical graph edges.
  }
}

for (const table of root.tables) {
  const fieldBegin = fields.length;
  const relationBegin = relations.length;
  const columnBegin = columns.length;
  const linkBegin = links.length;
  const fieldByName = new Map();
  const columnByName = new Map(table.columns.map((column) => [column.name.toLowerCase(), column]));

  for (let i = 0; i < table.columns.length;) {
    const first = table.columns[i];
    const base = first.baseName || splitArray(first.name).field;
    const grouped = [];
    while (i < table.columns.length && (table.columns[i].baseName || splitArray(table.columns[i].name).field) === base)
      grouped.push(table.columns[i++]);
    const annotations = new Set(grouped.flatMap((column) => column.annotations || []));
    if (annotations.has("noninline")) continue;
    const index = fields.length - fieldBegin;
    fieldByName.set(base.toLowerCase(), index);
    fields.push({
      name: base,
      elements: grouped.length,
      words: wordCount(first.storage),
      string: first.type === "string" || first.type === "locstring",
    });
  }

  for (const column of table.columns) {
    const { field, element } = splitArray(column.name);
    const annotations = new Set(column.annotations || []);
    let source = "ColumnSource::Field";
    let physicalField = fieldByName.get(field.toLowerCase()) ?? 0;
    if (annotations.has("noninline") && annotations.has("relation")) source = "ColumnSource::ParentId";
    else if (annotations.has("id") && annotations.has("noninline")) source = "ColumnSource::RowId";
    columns.push({
      name: column.name,
      field,
      kind: enumName(column.type),
      storage: column.storage || "",
      source,
      physicalField,
      element,
      optional: Boolean(column.optional),
    });
  }

  const usedNames = new Set();
  for (const parent of table.parents || []) {
    const sourceColumn = columnByName.get(String(parent.sourceColumn).toLowerCase());
    if (!sourceColumn) throw new Error(`${table.name}: missing source column ${parent.sourceColumn}`);
    const parsed = splitArray(parent.sourceColumn);
    const annotations = new Set(sourceColumn.annotations || []);
    let source = "db2::RelationSource::Field";
    if (annotations.has("noninline") && annotations.has("relation")) source = "db2::RelationSource::ParentId";
    else if (annotations.has("id")) source = "db2::RelationSource::RowId";
    const name = relationName(parent.sourceColumn, usedNames);
    const targetColumn = String(parent.targetColumn).toLowerCase() === "id" ? "@id" : parent.targetColumn;
    relations.push({
      name,
      source,
      sourceField: source === "db2::RelationSource::Field" ? parsed.field : "",
      sourceElement: parsed.element,
      targetTable: parent.targetTable,
      targetColumn,
    });
    const target = tableNames.get(String(parent.targetTable).toLowerCase());
    const targetColumnExists = Boolean(target && target.columns.some(
      (column) => column.name.toLowerCase() === String(parent.targetColumn).toLowerCase()));
    links.push({
      sourceTable: table.name,
      sourceColumn: parent.sourceColumn,
      relation: name,
      targetTable: parent.targetTable,
      targetColumn,
      targetExists: Boolean(parent.targetExists && targetColumnExists),
    });
  }

  const filename = table.db2Path ? path.basename(table.db2Path).toLowerCase() : "";
  const layoutHash = table.layout ? Number.parseInt(table.layout, 16) >>> 0 : 0;
  const available = Boolean(filename && layoutHash && fields.length > fieldBegin);
  if (available && args.db2Root) {
    const db2Path = path.resolve(args.db2Root, table.db2Path);
    const handle = fs.openSync(db2Path, "r");
    const header = Buffer.alloc(204);
    try {
      if (fs.readSync(handle, header, 0, header.length, 0) !== header.length ||
          header.toString("ascii", 0, 4) !== "WDC5")
        throw new Error(`${table.name}: missing/truncated WDC5 header at ${db2Path}`);
    } finally {
      fs.closeSync(handle);
    }
    const physicalFields = header.readUInt32LE(140);
    const fileLayout = header.readUInt32LE(156);
    if (fileLayout !== layoutHash)
      throw new Error(`${table.name}: file layout ${fileLayout.toString(16)} != schema ${layoutHash.toString(16)}`);
    if (physicalFields !== fields.length - fieldBegin)
      throw new Error(`${table.name}: file fields ${physicalFields} != generated ${fields.length - fieldBegin}`);
  }
  tables.push({
    name: table.name,
    filename,
    layoutHash,
    rowCount: Number(table.rowCount) || 0,
    fileSize: Number(table.sizeBytes?.db2) || 0,
    fieldBegin,
    fieldCount: fields.length - fieldBegin,
    relationBegin,
    relationCount: relations.length - relationBegin,
    columnBegin,
    columnCount: columns.length - columnBegin,
    linkBegin,
    linkCount: links.length - linkBegin,
    available,
  });
}

const out = [];
out.push("// Generated by tools/GenerateSchemaCatalog.mjs. Do not edit manually.");
out.push(`// Source build: ${args.build}; tables=${tables.length}; fields=${fields.length}; columns=${columns.length}; links=${links.length}`);
out.push("inline constexpr db2::Field kGeneratedFields[]{");
for (const field of fields) out.push(`    {${quote(field.name)}, ${field.elements}, ${field.words}, ${field.string}},`);
out.push("};");
out.push("inline constexpr db2::Relation kGeneratedRelations[]{");
for (const relation of relations)
  out.push(`    {${quote(relation.name)}, ${relation.source}, ${quote(relation.sourceField)}, ${relation.sourceElement}, ${quote(relation.targetTable)}, ${quote(relation.targetColumn)}},`);
out.push("};");
out.push("inline constexpr Column kGeneratedColumns[]{");
for (const column of columns)
  out.push(`    {${quote(column.name)}, ${quote(column.field)}, ${column.kind}, ${quote(column.storage)}, ${column.source}, ${column.physicalField}, ${column.element}, ${column.optional}},`);
out.push("};");
out.push("inline constexpr Link kGeneratedLinks[]{");
for (const link of links)
  out.push(`    {${quote(link.sourceTable)}, ${quote(link.sourceColumn)}, ${quote(link.relation)}, ${quote(link.targetTable)}, ${quote(link.targetColumn)}, ${link.targetExists}},`);
out.push("};");
out.push("inline constexpr GeneratedTable kGeneratedTables[]{");
for (const table of tables)
  out.push(`    {${quote(table.name)}, ${quote(table.filename)}, 0x${table.layoutHash.toString(16).toUpperCase().padStart(8, "0")}u, ${table.rowCount}u, ${table.fileSize}ull, ${table.fieldBegin}u, ${table.fieldCount}u, ${table.relationBegin}u, ${table.relationCount}u, ${table.columnBegin}u, ${table.columnCount}u, ${table.linkBegin}u, ${table.linkCount}u, ${table.available}},`);
out.push("};");
out.push("");

const generated = out.join("\n");
if (args.check) {
  if (!fs.existsSync(args.output) || fs.readFileSync(args.output, "utf8") !== generated)
    throw new Error(`generated catalog is stale: ${args.output}`);
} else {
  fs.mkdirSync(path.dirname(args.output), { recursive: true });
  fs.writeFileSync(args.output, generated);
}
console.log(`[ok] ${args.output}: tables=${tables.length} loadable=${tables.filter((table) => table.available).length} fields=${fields.length} columns=${columns.length} links=${links.length}${args.check ? " (current)" : ""}`);
