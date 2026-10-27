# ESPER in SQL

`esper-sqlite` exports a Skyrim load order into a SQLite database: every record, every override,
and every FormID reference between records, as related tables you can query with SQL. This page
explains the table design and walks through example queries, each with its real output.

## Building and running

The tool is the only part of ESPER with a third-party dependency (SQLite), so it's off by default.
With vcpkg, one option turns on the target and vcpkg.json's `sqlite` feature:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DESPER_BUILD_SQLITE=ON
```

```bash
cmake --build build --config Release --target esper-sqlite
```

```bash
build/Release/esper-sqlite "<Skyrim Special Edition>/Data" build/skyrim.db
```

Options: `--plugins-txt <file>` reads a mod manager profile's load order instead of the game's
own, and `--strings <dir>` points at loose string tables, so localized names resolve.

The feature also builds the `sqlite3` shell
(`build/vcpkg_installed/x64-windows/tools/sqlite3.exe`). Open the database with
`sqlite3 build/skyrim.db`, type `.mode box` for readable tables, and end each query with `;`.

**The data on this page** comes from a current Skyrim SE install with no mods: the five masters,
plus the four free Creation Club plugins and `_ResourcePack.esl` that every install now ships.
That's 10 plugins, 1,177,987 records, 10,813 overrides and 1,933,517 references. The export takes
about 18 seconds and writes 260 MB.

Vanilla string tables live inside BSA archives, which ESPER doesn't read, so `name` columns are
NULL for vanilla records. The examples use EditorIDs, the internal names modders see in the
Creation Kit (Lydia is `HousecarlWhiterun`).

## The design

A Skyrim plugin is already a database. Each record (an NPC, a weapon, a placed object) has a
unique key, its FormID. Records point at other records by FormID: an NPC names its race, a
leveled list names its items. Plugins loaded later can override a record, and the last one
loaded wins. The export maps those ideas onto tables:

```
plugins ─┬─< plugin_masters >─ plugins          (a plugin's master list)
         │
         ├─< records >── NPC_ / WEAP / ARMO / … (one typed row per record, 1:1)
         │      │
         └─< overrides                          (one row per overriding plugin)
                │
records ──< refs >── fields                     (record → record, through a field)
```

`─<` reads "one to many": one plugin defines many records.

### `plugins`

| column | meaning |
|---|---|
| `load_order` | primary key: 0 for the first plugin loaded, 1 for the next, … |
| `name` | the file name; `UNIQUE COLLATE NOCASE`, because Windows file names ignore case |
| `runtime_prefix` | the FormID prefix the game assigns: `00`–`FD`, or `FE000`… for light (ESL) plugins |
| `is_master`, `is_light`, `is_localized` | header flags, 0 or 1 |

**Why the load order is the key:** it's unique, it's a small integer that's cheap to store
millions of times in other tables, and sorting by it gives the order that decides overrides.

### `plugin_masters`

A plugin's master list (the plugins it depends on). It's a *junction table*: each row links two
rows of `plugins`, so the relationship is many to many. Dragonborn.esm has two masters, and
Skyrim.esm is a master of every other plugin.

The primary key `(plugin, position)` is a *composite key*: neither column is unique alone, but the
pair is. `master` is NULL when the master isn't loaded; `master_name` still records what the
plugin asked for.

### `records`

One row per record in the load order.

| column | meaning |
|---|---|
| `form_id` | primary key: the runtime FormID, as the in-game console shows it (`000A2C8E` is Lydia) |
| `type` | the record signature: `NPC_`, `WEAP`, `REFR`, … |
| `editor_id` | the EditorID, `COLLATE NOCASE` because the game matches them without case |
| `name` | the display name (FULL), when it resolves |
| `defining_plugin` | foreign key → `plugins`: the plugin that created the record |
| `winning_plugin` | foreign key → `plugins`: the last plugin to override it, whose version the game uses |
| `is_deleted` | 1 when the winning version deletes the record |

**Why the runtime FormID is the key.** Inside a plugin file, a FormID's top byte is an index into
that file's *own* master list, so the same record has different on-disk IDs in different
plugins. The runtime FormID (load-order slot plus local ID) is the same everywhere, so every
table can use it to join. It's a *natural key* (it means something outside the database), and
in SQLite an `INTEGER PRIMARY KEY` is the table's internal row ID, the fastest possible lookup.

**`winning_plugin` is redundant on purpose.** It can be computed from `overrides` (example 4
proves the two agree). Storing a derivable value is *denormalization*: it trades a little
storage, and the risk of the copies disagreeing, for simpler and faster queries. "Who wins" is
the most common question, so it gets a column.

### `overrides`

One row per plugin that overrides a record. The defining plugin isn't repeated here; it's in
`records`. The primary key `(form_id, plugin)` stops one plugin from being listed twice for the
same record. `WITHOUT ROWID` stores the table sorted by that key, so "every override of this
record" is one contiguous read.

### `fields` and `refs`

`refs` has one row per FormID reference: `from_form_id` (the record holding the reference),
`to_form_id` (what it points at) and `field_id` (where in the record it sits).

The table isn't called `references` because `REFERENCES` is an SQL keyword (it's how you declare
a foreign key). You can use a keyword as a name if you quote it everywhere; it's simpler not to.

Field paths like `conditions/data/reference/ref` would repeat hundreds of thousands of times, so
they're stored once in `fields`, and `refs` keeps a small integer pointing at them. That's
*normalization*: each fact is stored once, and the database stays smaller.

`refs.to_form_id` has **no foreign key**, unlike every other relationship here. Real data has
references to records that don't exist in any plugin, and example 8 shows the biggest case. A
foreign key would declare those rows invalid; with no foreign key, they're kept and you can query
them.

The field paths come from ESPER's schema. The engine's own FormID visitor is the authority on
which FormIDs a record holds. The exporter labels each one by walking the same schema, and keeps
a label only when its walk finds exactly the FormIDs the engine does (on this load order, it does
for every record).

### One table per record type

There's a table named after each of the schema's 120 record types: `NPC_`, `WEAP`, `ARMO`,
`CELL`, …. They aren't written by hand. The exporter reads ESPER's compiled schema when it runs:
every field that appears once at the top of a record becomes a column, and structs are flattened
into one column per field. WEAP's `DATA` subrecord, for example, is the schema struct `basic`, and
becomes `basic_value`, `basic_weight` and `basic_damage`. A change to `schema/*.yaml` changes the
tables with no exporter change.

Each typed table's primary key `form_id` is also a foreign key to `records`. That's a one-to-one
relationship, the usual way to model subtypes: what every record has (type, EditorID, plugins)
lives in `records`, and what only weapons have lives in `WEAP`. Repeated fields (an NPC's
inventory, a leveled list's entries) aren't one value per record, so they aren't columns; the
FormIDs inside them are in `refs`.

Column types follow the schema: integers become `INTEGER`, floats `REAL`, strings `TEXT`.
FormID columns hold runtime FormIDs, so they join to `records.form_id`.

### Foreign keys

A foreign key says that a column's value must exist as a key in another table.
`records.defining_plugin REFERENCES plugins(load_order)` means every record's defining plugin is
a real plugin. Foreign keys document the relationships, and the database can enforce them. SQLite
only enforces them on connections that run `PRAGMA foreign_keys = ON`. The exporter instead runs
`PRAGMA foreign_key_check` once the data is in, and fails if any row breaks one. The run above
reported 0 violations.

### Indexes

An index is a sorted copy of one or more columns that points back at the rows, like the index at
the back of a book. Without one, finding the rows where `to_form_id = X` means reading all 1.9
million `refs` rows. With one, it's a binary search.

| index | serves |
|---|---|
| `records(type)` | `WHERE type = 'NPC_'`, and `GROUP BY type` |
| `records(editor_id)` | looking a record up by its EditorID |
| `records(defining_plugin)`, `records(winning_plugin)` | joins from `plugins` ("what does this plugin define") |
| `overrides(plugin)` | "what does this plugin override"; the primary key already serves "who overrides this record" |
| `refs(from_form_id)` | "what does this record point at" |
| `refs(to_form_id)` | "what points at this record", the reverse lookup |

Primary keys and `UNIQUE` columns get an index automatically. Indexes aren't free: each one takes
space and slows inserts. That's why the exporter creates them after loading the data (one sort at
the end is much faster than updating an index on every insert), and why there's no index on
columns nobody filters by.

`EXPLAIN QUERY PLAN` shows whether a query uses an index:

```sql
EXPLAIN QUERY PLAN SELECT COUNT(*) FROM refs WHERE to_form_id = 79686;
```
```
`--SEARCH refs USING COVERING INDEX refs_to (to_form_id=?)
```

```sql
EXPLAIN QUERY PLAN SELECT COUNT(*) FROM refs WHERE field_id = 1;
```
```
`--SCAN refs
```

`SEARCH` uses an index. `SCAN` reads every row. "Covering" means the index alone holds everything
the query needs, so the table itself is never read.

## Example queries

### 1. Find one record: `SELECT … WHERE`

```sql
SELECT printf('%08X', form_id) AS form_id, type, editor_id
FROM records
WHERE editor_id = 'HousecarlWhiterun';
```

| form_id  | type |     editor_id     |
|----------|------|-------------------|
| 000A2C8E | NPC_ | HousecarlWhiterun |

`SELECT` lists the columns to return, `FROM` names the table, and `WHERE` keeps only matching
rows. FormIDs are stored as integers; `printf('%08X', …)` shows them in the 8-digit hex the game
uses. `AS` names an output column.

### 2. Count by category: `GROUP BY`

```sql
SELECT type, COUNT(*) AS how_many
FROM records
GROUP BY type
ORDER BY how_many DESC
LIMIT 10;
```

| type | how_many |
|------|----------|
| REFR |   865004 |
| CELL |    73725 |
| LAND |    52181 |
| INFO |    40928 |
| DIAL |    19757 |
| NAVM |    19267 |
| ACHR |    12955 |
| STAT |    12432 |
| PACK |     7632 |
| NPC_ |     6462 |

`GROUP BY type` collapses the rows into one row per type, and `COUNT(*)` counts the rows in each
group. `ORDER BY … DESC` sorts largest first, and `LIMIT` keeps the first 10. Placed objects
(`REFR`) are almost three quarters of the game.

### 3. Combine tables: `JOIN`

How many records each plugin defines:

```sql
SELECT p.load_order, p.name, COUNT(r.form_id) AS defines
FROM plugins AS p
LEFT JOIN records AS r ON r.defining_plugin = p.load_order
GROUP BY p.load_order
ORDER BY p.load_order;
```

| load_order |             name             | defines |
|------------|------------------------------|---------|
|          0 | Skyrim.esm                   |  869687 |
|          1 | Update.esm                   |   10438 |
|          2 | Dawnguard.esm                |   93440 |
|          3 | HearthFires.esm              |   16764 |
|          4 | Dragonborn.esm               |  178327 |
|          5 | ccBGSSSE001-Fish.esm         |    5336 |
|          6 | ccQDRSSE001-SurvivalMode.esl |     508 |
|          7 | ccBGSSSE037-Curios.esl       |     151 |
|          8 | ccBGSSSE025-AdvDSGS.esm      |    2963 |
|          9 | _ResourcePack.esl            |     373 |

A `JOIN` pairs each row of one table with the rows of another that match the `ON` condition: here,
each plugin with the records whose `defining_plugin` is that plugin. `AS p` and `AS r` are short
aliases. A plain (inner) `JOIN` drops plugins with no matches. `LEFT JOIN` keeps every row of
the left table, so a plugin that defines nothing would still appear, with a count of 0.

### 4. Which plugin wins this record?

The whole override chain of the `Tamriel` worldspace, in load order:

```sql
SELECT p.load_order, p.name,
       CASE WHEN p.load_order = r.defining_plugin THEN 'defines'
            WHEN p.load_order = r.winning_plugin  THEN 'overrides (wins)'
            ELSE 'overrides' END AS role
FROM records AS r
JOIN plugins AS p
  ON p.load_order = r.defining_plugin
  OR p.load_order IN (SELECT o.plugin FROM overrides AS o WHERE o.form_id = r.form_id)
WHERE r.editor_id = 'Tamriel'
ORDER BY p.load_order;
```

| load_order |          name           |       role       |
|------------|-------------------------|------------------|
|          0 | Skyrim.esm              | defines          |
|          1 | Update.esm              | overrides        |
|          2 | Dawnguard.esm           | overrides        |
|          3 | HearthFires.esm         | overrides        |
|          4 | Dragonborn.esm          | overrides        |
|          5 | ccBGSSSE001-Fish.esm    | overrides        |
|          8 | ccBGSSSE025-AdvDSGS.esm | overrides (wins) |

Six plugins override Tamriel, and the last one loaded wins. `CASE` is SQL's if/else. The
`(SELECT …)` inside `IN` is a *subquery*: a query whose result another query uses.

The short answer is one join away:

```sql
SELECT r.editor_id, w.name AS winner
FROM records AS r
JOIN plugins AS w ON w.load_order = r.winning_plugin
WHERE r.editor_id = 'Tamriel';
```

| editor_id |         winner          |
|-----------|-------------------------|
| Tamriel   | ccBGSSSE025-AdvDSGS.esm |

`winning_plugin` is a stored copy of something `overrides` already implies: the winner is the
highest-load-order override, or the defining plugin if there are none. This query checks every
record:

```sql
SELECT COUNT(*) AS disagreements
FROM records AS r
WHERE r.winning_plugin <> COALESCE(
    (SELECT MAX(o.plugin) FROM overrides AS o WHERE o.form_id = r.form_id),
    r.defining_plugin);
```

| disagreements |
|---------------|
|             0 |

`COALESCE(a, b)` returns `a` unless it's NULL, and then `b`. `MAX` over no rows is NULL, so a
record nobody overrides falls back to its defining plugin. A query like this is the usual way to
check a denormalized column.

### 5. Typed columns: the most valuable weapons

```sql
SELECT r.editor_id, w.basic_damage AS damage, w.basic_weight AS weight, w.basic_value AS value
FROM WEAP AS w
JOIN records AS r ON r.form_id = w.form_id
WHERE w.templateWeapon IS NULL
ORDER BY w.basic_value DESC
LIMIT 8;
```

|           editor_id           | damage | weight | value |
|-------------------------------|--------|--------|-------|
| ccBGSSSE025_MadnessWarhammer  |     29 |   34.0 |  4600 |
| ccBGSSSE025_AmberWarhammer    |     28 |   29.0 |  4475 |
| DLC1WrathmanWarhammer         |     23 |   33.0 |  4275 |
| DLC1DragonboneWarhammer       |     28 |   33.0 |  4275 |
| DaedricWarhammer              |     27 |   31.0 |  4000 |
| ccBGSSSE025_MadnessBattleaxe  |     27 |   31.0 |  3300 |
| ccBGSSSE025_Jyggalag          |     25 |   25.0 |  3200 |
| ccBGSSSE025_MadnessGreatsword |     26 |   28.0 |  3100 |

`WEAP` holds the weapon-only columns; the join to `records` adds the EditorID. Enchanted
variants (`EnchDaedricWarhammerAbsorbH04`, …) copy a base weapon's stats through
`templateWeapon`, so `IS NULL` keeps only base weapons. Use `IS NULL`, never `= NULL`: NULL means
"unknown", so `x = NULL` is never true, even when `x` is NULL.

### 6. Follow references out: what Lydia points at

```sql
SELECT f.path AS field, t.type, t.editor_id
FROM records AS lydia
JOIN refs        ON refs.from_form_id = lydia.form_id
JOIN fields AS f ON f.field_id = refs.field_id
LEFT JOIN records AS t ON t.form_id = refs.to_form_id
WHERE lydia.editor_id = 'HousecarlWhiterun';
```

|      field       | type |              editor_id               |
|------------------|------|--------------------------------------|
| factions/faction | FACT | CurrentFollowerFaction               |
| factions/faction | FACT | PlayerHousecarlFaction               |
| factions/faction | FACT | PotentialFollowerFaction             |
| factions/faction | FACT | PotentialMarriageFaction             |
| factions/faction | FACT | TownWhiterunFaction                  |
| factions/faction | FACT | HousecarlWhiterunCrimeFaction        |
| factions/faction | FACT | PlayerFaction                        |
| voice            | VTYP | FemaleEvenToned                      |
| race             | RACE | NordRace                             |
| defaultOutfit    | OTFT | PlayerHousecarlOutfit                |
| crimeFaction     | FACT | HousecarlWhiterunCrimeFaction        |
| headParts        | HDPT | MarksFemaleHumanoid00NoGash          |
| headParts        | HDPT | HairFemaleNord02                     |
| headParts        | HDPT | FemaleEyesHumanGreenHazel            |
| hairColor        | CLFM | HairColor11Black                     |
| combatStyle      | CSTY | csHumanBoss1H                        |
| items/item       | WEAP | SteelSword                           |
| packages         | PACK | HousecarlWhiterunGreetPlayer         |
| packages         | PACK | HousecarlWhiterunSandboxDragonsreach |
| packages         | PACK | DefaultSandboxEditorLocation512      |
| npcClass         | CLAS | CombatWarrior1H                      |

`records` appears twice under two aliases: `lydia` is the record doing the pointing and `t` is
the target. Joining a table to itself (directly or through another table) is common whenever
rows relate to rows of the same kind.

### 7. Follow references in: what uses NordRace

```sql
SELECT src.type, COUNT(*) AS refs
FROM records AS nord
JOIN refs           ON refs.to_form_id = nord.form_id
JOIN records AS src ON src.form_id = refs.from_form_id
WHERE nord.editor_id = 'NordRace'
GROUP BY src.type
ORDER BY refs DESC;
```

| type | refs |
|------|------|
| NPC_ | 1423 |
| ARMA |  614 |
| INFO |   94 |
| ARMO |   19 |
| FLST |    9 |
| QUST |    7 |
| RACE |    2 |
| PACK |    2 |
| SPEL |    1 |
| MGEF |    1 |

The same two tables as example 6, joined in the other direction. This is the query the
`refs(to_form_id)` index exists for. Without it, SQLite would read all 1.9 million references.

### 8. Find what's missing: `LEFT JOIN … IS NULL`

References whose target isn't a record in any loaded plugin:

```sql
SELECT printf('%08X', refs.to_form_id) AS missing_target, COUNT(*) AS refs
FROM refs
LEFT JOIN records AS t ON t.form_id = refs.to_form_id
WHERE t.form_id IS NULL
GROUP BY refs.to_form_id
ORDER BY refs DESC
LIMIT 5;
```

| missing_target | refs |
|----------------|------|
| 00000014       | 7246 |
| 00000844       |  101 |
| 01017004       |   76 |
| 00106633       |   30 |
| 00000011       |   13 |

A `LEFT JOIN` fills the right side with NULLs when nothing matches, so `WHERE t.form_id IS NULL`
keeps exactly the references with no target. This pattern is called an *anti-join*.

7,246 of the 7,530 dangling references point at `00000014`, the player's reference
(`PlayerRef`). The game engine creates it at startup, so no plugin contains it. That's why
`refs.to_form_id` has no foreign key: these references are valid in the game, even though their
target exists in no plugin.

### 9. Walk a tree: a recursive CTE over leveled lists

A leveled list (`LVLI`) names items or other leveled lists, which can name more lists, to any
depth. Expanding one fully takes recursion:

```sql
WITH RECURSIVE tree(form_id, depth, path) AS (
  -- start: the list itself
  SELECT form_id, 0, editor_id FROM records WHERE editor_id = 'LItemWeaponAny1HTown'
  UNION ALL
  -- step: every entry of every leveled list found so far
  SELECT refs.to_form_id, tree.depth + 1, tree.path || ' > ' || child.editor_id
  FROM tree
  JOIN records AS parent ON parent.form_id = tree.form_id AND parent.type = 'LVLI'
  JOIN refs              ON refs.from_form_id = tree.form_id
  JOIN fields AS f       ON f.field_id = refs.field_id AND f.path = 'entries/reference'
  JOIN records AS child  ON child.form_id = refs.to_form_id
  WHERE tree.depth < 10
)
SELECT tree.depth, r.type, tree.path, COUNT(*) AS entries
FROM tree
JOIN records AS r USING (form_id)
GROUP BY tree.path
ORDER BY tree.path;
```

| depth | type |                             path                             | entries |
|-------|------|--------------------------------------------------------------|---------|
|     0 | LVLI | LItemWeaponAny1HTown                                         |       1 |
|     1 | LVLI | LItemWeaponAny1HTown > LItemWeaponMaceTown                   |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponMaceTown > DwarvenMace     |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponMaceTown > ElvenMace       |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponMaceTown > IronMace        |       4 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponMaceTown > OrcishMace      |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponMaceTown > SteelMace       |       4 |
|     1 | LVLI | LItemWeaponAny1HTown > LItemWeaponSwordTown                  |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponSwordTown > DwarvenSword   |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponSwordTown > ElvenSword     |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponSwordTown > IronSword      |       4 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponSwordTown > OrcishSword    |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponSwordTown > SteelSword     |       4 |
|     1 | LVLI | LItemWeaponAny1HTown > LItemWeaponWarAxeTown                 |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponWarAxeTown > DwarvenWarAxe |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponWarAxeTown > ElvenWarAxe   |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponWarAxeTown > IronWarAxe    |       4 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponWarAxeTown > OrcishWarAxe  |       1 |
|     2 | WEAP | LItemWeaponAny1HTown > LItemWeaponWarAxeTown > SteelWarAxe   |       4 |

A CTE (*common table expression*) is a named, temporary result set defined with `WITH`, which the
main query then reads like a table. A *recursive* CTE has two parts joined by `UNION ALL`:

1. **The anchor** runs once and produces the starting rows: the list itself, at depth 0.
2. **The recursive step** joins the rows found so far (`FROM tree`) to their entries, producing
   the next level. SQLite repeats it with each new batch of rows until a step produces none.

Only leveled lists have entries, so the `parent.type = 'LVLI'` condition stops the step at
weapons. `depth < 10` is a safety limit in case two lists contain each other. `||` joins strings,
and `USING (form_id)` is shorthand for `ON tree.form_id = r.form_id`.

The `entries` column shows a real game mechanic: iron and steel weapons are listed four times,
which makes them four times as likely to drop as dwarven or elven ones. (Each entry also has a
minimum level, which is in the record but not in `refs`.)

### 10. A junction table: a plugin's masters

```sql
SELECT p.name AS plugin, pm.position, m.name AS master
FROM plugin_masters AS pm
JOIN plugins AS p ON p.load_order = pm.plugin
JOIN plugins AS m ON m.load_order = pm.master
WHERE p.name = 'Dragonborn.esm'
ORDER BY pm.position;
```

|     plugin     | position |   master   |
|----------------|----------|------------|
| Dragonborn.esm |        0 | Skyrim.esm |
| Dragonborn.esm |        1 | Update.esm |

`plugins` is joined twice: once as the plugin and once as its master. That's the usual way to
read a many-to-many relationship through its junction table.
