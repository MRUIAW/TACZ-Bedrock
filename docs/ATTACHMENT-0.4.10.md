# 0.4.10 — read-only attachment queries and LuaJ boolean parameters

## Java contract and scope

`J` below is `_reference/TACZ/src/main/java/com/tacz/guns/`.

| Rule | Reference |
| --- | --- |
| Case-sensitive AttachmentType.valueOf; invalid name returns tacz:empty | `J/item/ModernKineticGunScriptAPI.java:705–712`; `J/client/animation/statemachine/GunAnimationStateContext.java:395–402` |
| Check allowAttachmentType before looking at NBT | `J/api/item/nbt/GunItemDataAccessor.java:226–239` |
| Unknown gun/index, null list or type absent from list disallows | `J/api/item/gun/AbstractGunItem.java:299–312` |
| Absent allow_attachment_types defaults to an empty list | `J/resource/pojo/data/gun/GunData.java:105–106` |
| Attachment<TYPE> compound → tag compound → AttachmentId string | `J/api/item/nbt/GunItemDataAccessor.java:29,226–239,291–297`; `J/api/item/nbt/AttachmentItemDataAccessor.java:15,21–35` |
| Built-in attachment lookup is separate and must not be substituted | `J/api/item/nbt/GunItemDataAccessor.java:274–287` |
| Enum constants and their JSON aliases | `J/api/item/attachment/AttachmentType.java` |

No attachment installation, built-in fallback, attachment-stat application, new
NBT writes, native hooks or protocol changes. Both sides query their own gun data.
Unknown client gun data disallows all. Existing reload/bolt and error policies
remain; shoot/shootOnce is 0.4.11 and inventory-direct shooting remains later work.

## Shared pure helper and adapters

`src/gun/AttachmentRules.h` accepts an allowed-types list (nullptr for unknown
gun data), a read-only typed NBT view and the API type string. API names must be
exactly SCOPE, MUZZLE, STOCK, GRIP, LASER, EXTENDED_MAG or NONE. Type validation
and allow-list mapping live in this one helper. NONE is valid, but must also be
allowed and have its own AttachmentNONE NBT; it is not automatically empty.

```cpp
if (!permitted || !read) return empty();
auto key = "Attachment" + std::string(type);
std::vector<std::string> path{key};
if (read({}, key, 0) != 10 || read(path, "tag", 0) != 10) return empty();
path.push_back("tag");
if (read(path, "AttachmentId", 0) != 8) return empty();
auto value = read(path, "AttachmentId", 1);
if (!value.is_string()) return empty();
return parseAttachmentId(value.get_ref<std::string const&>()).value_or(std::string(EmptyAttachment));
```

10/8 are the Java/Bedrock compound/string tag IDs. Missing, wrongly typed or
unparsable values return tacz:empty; no item-stack id/Count/registry-existence
check is invented. Builtin data is not an input to the helper.

`nbt::readNbt` in `src/nbt/GunItem.cpp` factors out the existing client's typed
primitive/type reader unchanged. Server ScriptApi receives a view of the selected
item's user data for the duration of the hook. Client AnimationContext receives
its entry's allowed list and reuses the existing owned item-NBT clone/readNbt
path, retaining the outgoing gun's data during put-away. The animation VM/header
have no mc/LL includes or native ItemStack ownership.

Both registrations call the same helper through their context method:

```cpp
// GunLogic.cpp
"getAttachment", &ScriptApi::getAttachment,
// AnimationStateMachine.cpp (replaces the unconditional tacz:empty stub)
a["getAttachment"] = &AnimationContext::getAttachment;
```

## Parser boundaries: Gson and ResourceLocation

Gson 2.10/2.10.1 EnumTypeAdapter first uses @SerializedName, then falls back to
enum.toString(). AttachmentType has no toString override. Thus the allow-list
accepts exact scope/SCOPE, muzzle/MUZZLE, stock/STOCK, grip/GRIP, laser/LASER,
extended_mag/EXTENDED_MAG and NONE. Scope, leading/trailing spaces and none do
not match. API arguments still use constant names only. Unknown raw strings and
null list entries grant nothing.

Before the parser edit, the 0.3.19 implementation/document was checked:
`docs/MULTIGUN-0.3.19.md:88–111`, `GunPackParser.cpp`'s string reader and array
readers. Ordinary string fields already convert numeric/boolean primitives to
text; generic lenient JSON intentionally does no global type coercion. There is
no shared enum-list conversion to widen. Only allow_attachment_types now maps
numeric elements to null: Gson nextString accepts NUMBER text, which cannot name
an AttachmentType. BOOLEAN/object/array elements and non-array list fields still
fail the gun file, as Gson does. Other enum lists and ordinary fields are unchanged.
Explicit-null and absent lists are represented as an empty vector here; both
produce exactly Java's disallow-all result for this query.

Sources: Gson [EnumTypeAdapter](https://github.com/google/gson/blob/gson-parent-2.10/gson/src/main/java/com/google/gson/internal/bind/TypeAdapters.java)
and [JsonTreeReader.nextString](https://github.com/google/gson/blob/gson-parent-2.10/gson/src/main/java/com/google/gson/internal/bind/JsonTreeReader.java).

`ResourceId::parse` is stricter than ResourceLocation.tryParse: it rejects empty
namespaces and paths. This task leaves that project-wide parser untouched and
uses an attachment-specific parser matching Java's decompose/character checks:

- Unqualified lens → minecraft:lens; :lens also → minecraft:lens.
- Empty string / : → minecraft:; fixture: is a valid empty path.
- Namespace accepts [a-z0-9_.-], path additionally accepts /; no trimming,
  case folding or Unicode relaxation. Additional colons and invalid characters
  reject the value. A missing AttachmentId is not an empty-string AttachmentId.

These resolve the prompt/reference discrepancies in favor of the user-approved
Java behavior; they do not introduce an intentional gameplay divergence.

## Six boolean-parameter registrations

The exact shipped FiguraMC LuaJ `3.0.8-figura`,
`org/luaj/vm2/lib/jse/CoerceLuaToJava.java` BoolCoercion.coerce, uses
value.toboolean() for both Java boolean and Boolean. Its overload score is not a
type rejection. Only false/nil are false; 0, 1, strings and tables are true.

`src/lua/BooleanCoercion.h` applies this at the binding boundary:

```cpp
return value.valid() && (value.get_type() != sol::type::boolean || value.as<bool>());
// Gun logic registration:
[](ScriptApi& a, sol::object value) { a.setAmmoInBarrel(luaJBoolean(value)); }
```

Changed registrations (all currently registered Java boolean parameters):

1. Gun logic **setAmmoInBarrel(value)** (`ModernKineticGunScriptAPI.java:646`).
2. Animation **findIdleTrack(..., interruptHolding)** (`AnimationStateContext.java:82`).
3. Animation **runAnimation(..., blending, ...)** (`:154`).
4. Animation **setAnimationProgress(..., normalization)** (`:217`).
5. Animation **adjustAnimationProgress(..., normalization)** (`:247`).
6. Animation **setShouldHideCrossHair(value)** (`:342`).

The last five are inherited from
`J/api/client/animation/statemachine/AnimationStateContext.java`. No other
currently registered gun-logic method accepts a Java boolean parameter;
shootOnce(boolean) and setOverheatLocked(boolean) are not yet registered.
**Return-value checks are unchanged**: start_bolt/tick_bolt/start_reload still
require booleans; truthy numbers/strings/tables are not accepted as their results.

Faithful quirk: Kar98's empty clip-load script calls
`setAmmoInBarrel(consumeAmmoFromPlayer(1))`. A returned 0 is truthy, so Java sets
the chamber to true even with no stock. The previous strict sol2 binding crashed
the offline probe at that path; this adapter now preserves Java's behavior rather
than correcting the pack script. Synthetic tests explicitly cover stock 0, 1 and 6.

User's raw-pack audit found existing animation boolean calls already supply
booleans (3,295 runAnimation, 313 progress, 47 findIdleTrack, 69 literal hide-crosshair
calls plus the boolean-valued SPAS-12 forwarding call). These figures are user
evidence, not reinterpreted as this session's runtime measurements.

## Scratch caller scan — installed packs, 2026-10-30

Production registry/resolver exports, not a second precedence model. Scratch-only
instrumentation was removed from the repository source after use. Reports:

- `C:\Users\MRUIAW\AppData\Local\Temp\opencode\tacz-0.4.10-attachment-scan.json`
- `C:\Users\MRUIAW\AppData\Local\Temp\opencode\tacz-0.4.10-classicr-reload-probe.json`

Five packs. Effective installed script callers (literal type arguments):

| Side | Script | Calls / queried types | Unattached item result |
| --- | --- | --- | --- |
| data | classicr:kar98_gun_logic | 4 / SCOPE | tacz:empty |
| data | sfms:alternative_heat_logic | 2 / STOCK | tacz:empty |
| data | sfms:tactical_bow_logic | 2 / MUZZLE | tacz:empty |
| data | tacz:kar98_gun_logic | 1 / SCOPE | tacz:empty |
| assets | classicr:kar98_state_machine | 6 / SCOPE | tacz:empty |
| assets | sfms:tactical_bow_state_machine | 3 / MUZZLE | tacz:empty |
| assets | tacz:fn_evolys_state_machine | 1 / SCOPE | tacz:empty |
| assets | tacz:kar98_state_machine | 3 / SCOPE | tacz:empty |
| assets | tacz:m16a4_state_machine | 5 / SCOPE, GRIP | tacz:empty |
| assets | tacz:spas_12_state_machine | 1 / SCOPE | tacz:empty |

All no-attachment queries produce tacz:empty, whether the type is permitted or
not. Other APIs used by these scripts are not thereby claimed implemented (heat,
inventory-direct shooting and shoot hooks remain outside this version).

The current production scan differs from an earlier prompt example:
classicr:kar98 points to classicr:kar98_data, whose installed file **does declare
scope and muzzle**. tacz:kar98 allows scope/extended_mag. The helper uses actual
resolved data, not either gun's name. An absent list remains synthetically tested
as disallow-all. The scan also includes unknown lower-case module entries; these
grant no eligibility, just like an unknown Gson enum. No special case was added.

Both Kar98 reload probes now finish tactical, empty, insufficient-tactical and
insufficient-empty scenarios without warnings. Existing cancel probes also run
without warnings, but their diagnostic helper cancels then stops simulation;
they are not evidence of live interrupt/finishing acceptance. No game was launched.

## Accepted 0.4.9 and live checklist

User acceptance on local LeviLamina BDS 0.4.9 + client (partly headless-input-driven):
M870 bolt repeated over ten times, 0.32/0.57 s feed/finish; empty shell reload
0+1 → 1+1 → 2+1; firing ends reload after the current shell and the interrupting
press does not fire (Java behavior). CCRP M1887 long 0.59/0.69 s; ClassicR Kar98
total-time bolt 1.29 s without errors (reload untested); SPAS-12 SEMI 0.34/0.54 s
(BURST not tested, deferred to 0.4.11). AWP default 0.94 s and zero-action M107
passed. User confirmed feel and bolt animations look right.

0.4.10 acceptance is pending:

1. tacz:kar98 and classicr:kar98: empty reload with no scope takes the clip-load
   path; tactical reload takes the expected shell/round path. Verify ammo and
   animation agree and no getAttachment/boolean-argument script errors occur.
2. Interrupt both guns' empty and tactical reloads by firing before/after feed;
   verify script finishing rules and that the interrupting press follows Java,
   not an invented immediate shot/cancel acknowledgement.
3. Spot-check M16A4, SPAS-12 SEMI and FN Evolys state machines for missing-method
   errors/fallback, reload/inspect/shoot presentation and scope/grip-empty branches.
   BURST shoot behavior and heat/inventory-direct APIs are not accepted here.
4. Regress M870 bolt/shell reload, AWP/M107, switching, respawn, dimensions and
   rejoin. No attachment installer exists yet: normal item calls remain empty.

## Future stage-5 attachment installation

Write the gun's user data as a **compound**, not a list/map under a new key:

```text
AttachmentSCOPE: compound (the serialized attachment ItemStack)
  tag: compound
    AttachmentId: string "namespace:attachment"
```

Other types use their exact constant suffix, including NONE where allowed. The
gun's allow_attachment_types must permit that type, and the client must have the
corresponding gun data and item-NBT sync. Item-stack id/Count may be required by
the later installation system, but this Java ID query reads only the nested tag.
Do not write builtin_attachments into this query or infer installed attachments
from built-in gun data. Once those tags are written, both current registrations
read real IDs automatically.

## Offline validation

Synthetic NBT fixtures cover typed paths, allowed/disallowed/unknown types and gun
data, absent/null lists, exact Gson aliases, numeric/null/unknown entries, rejected
boolean/non-array inputs, NONE, parser edge cases and no built-in substitution.
Both Lua registrations use the helper and reflect item/side-data changes without
writing NBT. Boolean tests exercise all seven requested gun-logic values, the
empty clip-load sequence and number/nil/string animation arguments; strict return
checks are retained.

- Server then client x64 Release: **build ok**; tests build: **build ok**.
- Full suite: **305 cases / 925,258 assertions passed** (+12 synthetic cases,
  +178 assertions over 0.4.9). Existing hook/cast/u8path warnings remain.
- Client configuration remains active; whitespace and UTF-8-without-BOM checks
  pass. No push/deployment/game launch. Live 0.4.10 acceptance remains pending.
