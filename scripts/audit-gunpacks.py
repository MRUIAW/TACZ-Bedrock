"""Read-only pack audit using the PRODUCTION C++ loader, no third-party assets copied.

python scripts/audit-gunpacks.py <gunpacks> [--output docs/GUNS-0.3.19.md]
The intermediate metadata lives in OpenCode's approved temp directory.
Static flags indicate source features/risk, not successful native/in-game acceptance.
"""
import argparse
import collections
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMP = Path.home() / "AppData/Local/Temp/opencode"


def strip_lua_comments(text):
    return re.sub(r"--\[\[.*?\]\]|--[^\n]*", "", text, flags=re.S)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gunpacks", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/GUNS-0.3.19.md")
    parser.add_argument("--reuse-metadata", action="store_true", help="Only for formatting a previously completed audit")
    args = parser.parse_args()
    if not args.gunpacks.is_dir():
        parser.error("gunpacks must be an existing directory (audit never creates it)")
    metadata = TEMP / "guns-0.3.19.json"
    if not args.reuse_metadata:
        subprocess.run(["xmake", "build", "tacz-pack-audit"], cwd=ROOT, check=True)
        subprocess.run(["xmake", "run", "tacz-pack-audit", str(args.gunpacks.resolve()), str(metadata)], cwd=ROOT, check=True)
    report = json.loads(metadata.read_text(encoding="utf-8"))
    rows = report["guns"]
    # Index Lua by module name. Later folder order matches the registry; walk only
    # plaintext scripts. No encrypted taczpack.dat or removed Vanguard pack access.
    modules = {}
    for folder in sorted(args.gunpacks.iterdir()):
        if not folder.is_dir() or not (folder / "gunpack.meta.json").exists():
            continue
        for kind in ("assets", "data"):
            base = folder / kind
            for path in sorted(base.glob("*/scripts/**/*.lua")):
                ns = path.relative_to(base).parts[0]
                rel = path.relative_to(base / ns / "scripts").with_suffix("").as_posix()
                modules[(kind, ns + "_" + rel)] = (ns + ":" + rel, strip_lua_comments(path.read_text(encoding="utf-8-sig")))
    builtin = (ROOT / "resources/tacz/default_state_machine.lua").read_text(encoding="utf-8")
    modules.setdefault(("assets", "tacz_default_state_machine"), ("tacz:default_state_machine", strip_lua_comments(builtin)))
    anim_impl = (ROOT / "src/lua/AnimationStateMachine.cpp").read_text(encoding="utf-8")
    logic_impl = (ROOT / "src/lua/GunLogic.cpp").read_text(encoding="utf-8")
    known_calls = {
        "assets": set(re.findall(r'"([A-Za-z]\w+)"', anim_impl)),
        "data": set(re.findall(r'"([A-Za-z]\w+)"', logic_impl)),
    }
    # Compare serialized field names against upstream TACZ POJOs, not a guessed
    # Arcana schema. Custom script_param and transform/maps are intentionally opaque.
    java = ROOT / "_reference/TACZ/src/main/java/com/tacz/guns"
    def fields(name):
        matches = list(java.rglob(name + ".java"))
        return set(re.findall(r'@SerializedName\("([^"]+)"\)', matches[0].read_text(encoding="utf-8"))) if matches else set()
    schemas = {"data": fields("GunData"), "display": fields("GunDisplay"),
               "data.bullet": fields("BulletData"), "data.reload": fields("GunReloadData"),
               "data.heat": fields("GunHeatData"), "data.charging.*": fields("ChargeData"),
               "data.bullet.explosion": fields("ExplosionData")}
    unknown_fields = collections.defaultdict(set)
    unknown_calls = collections.defaultdict(set)
    def lua_for(kind, resource):
        if not isinstance(resource, str):
            return "", []
        module = resource.replace(":", "_", 1)
        seen, texts, missing = set(), [], []
        def visit(name):
            if name in seen:
                return
            seen.add(name)
            entry = modules.get((kind, name))
            if not entry:
                missing.append(name)
                return
            texts.append(entry[1])
            for dep in re.findall(r'require\s*\(?\s*[\'"]([^\'"]+)', entry[1]):
                visit(dep)
        visit(module)
        return "\n".join(texts), missing
    for row in rows:
        data, display, clips, geo = (row.get(k, {}) for k in ("data", "display", "clips", "geometry"))
        row["namespace"] = row["id"].split(":")[0]
        row["reloadAdditional"] = any("additional_magazine" in c["bones"] for n, c in clips.items() if n.startswith("reload"))
        row["reloadHide"] = sorted({b for n, c in clips.items() if n.startswith("reload") for b in c["zeroScale"] if "mag" in b})
        row["reloadMove"] = sorted({b for n, c in clips.items() if n.startswith("reload") for b in c["bones"] if "mag" in b})
        row["inspectRot"] = sorted({b for n, c in clips.items() if n.startswith("inspect") for b in c["rotations"]})
        mechanics = []
        if data.get("bolt") == "manual_action": mechanics.append("bolt")
        if data.get("reload", {}).get("type") == "inventory": mechanics.append("inventory")
        for key in ("heat", "charging", "melee", "builtin_attachments"):
            if data.get(key): mechanics.append(key)
        if data.get("bullet", {}).get("explosion", {}).get("explode"): mechanics.append("explosion")
        # Pellets are already spawned/consumed per shot by BulletSystem; audit, not
        # blanket-label as missing mechanics.
        row["pellets"] = data.get("bullet", {}).get("bullet_amount", 1)
        row["mechanics"] = mechanics
        found = []
        for path, known in schemas.items():
            targets = [("", row)]
            for part in path.split("."):
                next_targets = []
                for prefix, obj in targets:
                    if not isinstance(obj, dict): continue
                    for name in (obj if part == "*" else [part]):
                        if name in obj: next_targets.append(((prefix + "." if prefix else "") + name, obj[name]))
                targets = next_targets
            for prefix, obj in targets:
                if not known or not isinstance(obj, dict): continue
                for key in sorted(obj.keys() - known):
                    name = prefix + "." + key
                    found.append(name); unknown_fields[row["pack"]].add(name)
                    if isinstance(obj[key], dict):
                        for child in obj[key]: unknown_fields[row["pack"]].add(name + "." + child)
        row["unknownFields"] = found
        calls, missing = set(), []
        for kind, resource in (("data", data.get("script")), ("assets", display.get("state_machine", "tacz:default_state_machine"))):
            text, absent = lua_for(kind, resource)
            missing.extend(absent)
            # Static receiver heuristic: names used by TACZ scripts; dynamic/reflection
            # calls and external engines are not claimed fully analyzed.
            for receiver, method in re.findall(r'\b(api|context|ctx)\s*:\s*(\w+)\s*\(', text):
                if method not in known_calls[kind]: calls.add(kind + ":" + method)
        row["unknownCalls"] = sorted(calls)
        row["missingModules"] = sorted(set(missing))
        unknown_calls[row["pack"]].update(calls)
        text, _ = lua_for("assets", display.get("state_machine", "tacz:default_state_machine"))
        row["nilTrack"] = bool(re.search(r'getTrack\([^\n]*SWITCH_MODE_TRACK', text)) and not bool(re.search(r'local\s+SWITCH_MODE_TRACK\s*=', text))
        row["putMethod"] = "tail-seek/HOLD" if '"put_away"' in text and "adjustAnimationProgress" in text else "Lua/custom"
    def cell(value):
        if isinstance(value, list): value = ", ".join(map(str, value)) or "—"
        return str(value).replace("|", "\\|").replace("\n", " ")
    lines = ["# 0.3.19 枪包离线审计（可重跑）", "",
             "生成：`python scripts/audit-gunpacks.py <gunpacks目录>`；使用生产 C++ parser/registry/模型编译器。",
             "只读本机第三方包；不复制素材、不执行包 Lua、不解密、不安装 Arcana。表内标记是静态特征/风险，不是实机成功或必现 bug。", "",
             f"本次目录：`{args.gunpacks.as_posix()}`。共 **{len(rows)} 个枪械 index / {report['loadedCount']} 个加载成功**，"
             f"{report['ammoCount']} ammo / {report['scriptCount']} data scripts。",
             "此前 0.3.18 的 322 是成功加载数，不是目录总数：另外 3 个 data JSON 失败；9 个 display JSON 失败（其中有重叠），不是 11 把枪全部未注册。", "",
             "动画解析警告：" + str(sum(bool(g.get("animationWarnings")) for g in rows)) + " 把枪（明细在文末）；可加载不保证所有动画 channel/脚本都支持。", "",
             "## 汇总（按物理包；各项可重叠）", "",
             "| 包 / namespace | 加载 / index | display可读 | 缺资产枪 | reload额外匣 | reload零缩放匣链 | nil轨道收枪 | open bolt | inspect旋转 | illuminated / 薄面 / 重面候选枪 | ammo entity | 待实现机制 | 未识别字段 / API枪 |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|"]
    grouped = collections.defaultdict(list)
    for row in rows: grouped[row["pack"]].append(row)
    for pack, guns in sorted(grouped.items()):
        counts = collections.Counter(m for g in guns for m in g["mechanics"])
        line = [pack + " / " + ",".join(sorted({g["namespace"] for g in guns})),
                f"{sum(g['loaded'] for g in guns)}/{len(guns)}", sum(g.get("displayLoaded", False) for g in guns),
                sum(bool(g.get("missing")) for g in guns), sum(g["reloadAdditional"] for g in guns),
                sum(bool(g["reloadHide"]) for g in guns), sum(g["nilTrack"] for g in guns),
                sum(g.get("data", {}).get("bolt") == "open_bolt" for g in guns), sum(bool(g["inspectRot"]) for g in guns),
                f"{sum(bool(g.get('geometry', {}).get('illuminated')) for g in guns)}/{sum(bool(g.get('geometry', {}).get('thin')) for g in guns)}/{sum(bool(g.get('geometry', {}).get('coincidentQuads')) for g in guns)}",
                sum(bool(g.get("ammoDisplay", {}).get("entity")) for g in guns),
                ", ".join(f"{k}:{v}" for k, v in sorted(counts.items())) or "—",
                f"{sum(bool(g['unknownFields']) for g in guns)}/{sum(bool(g['unknownCalls']) for g in guns)}"]
        lines.append("| " + " | ".join(map(cell, line)) + " |")
    lines += ["", "## 读表与限制", "",
              "- chamber 列是 `初始弹匣 / 原始HasBulletInBarrel / 有效膛弹`：TACZ 创造栏给所有枪写 true，但 open_bolt 必须忽略它。",
              "- reload `A`=片段 key 额外匣；`Z`=弹匣相关骨零缩放；`M`=相关运动骨。A/Z 是需要功能实例的候选，不等于每把都曾在游戏消失。",
              "- put-away 列给出片段长度 / data时长 / Lua方式；片段已合并 pistol/rifle fallback。nil轨道兼容不代表补造不存在的片段。",
              "- inspect 列列出带非零旋转的骨；这是作者动作，不能据此断言旋转顺序错误。",
              "- illuminated 仅全亮标志；薄面指源 cube 最薄边 ≤0.0011 pixel（含零厚度），不是已确认共面相交测试。未将所有薄面擅改偏移。",
              "- 重面是静态默认可见几何中四角完全重合的 quad 对（坐标取整到 1e-7 方块，忽略绕序、排除零面积）；不覆盖部分重叠多边形，不等同实机 z-fighting。",
              "- 机制是 data 声明且未实现的功能；pellets 单独标注，BulletSystem 已支持每发多弹丸，不列作未实现。melee data 也可能是普通枪默认枪托攻击。",
              "- 未识别字段相对于当前参考 TACZ POJO 的 SerializedName（检查 data/display、bullet/reload/heat/charging/explosion）；script_param 是作者自由参数，不算未知。",
              "- API 是 `api/context/ctx:method` 加递归 require 的静态扫描，对照当前 Lua bridge。未知不自动等于 Arcana；动态调用、其它 receiver/反射及参数语义仍需后续审计。",
              "- 缺资产含 display/model/texture/animation/state_machine 与显式/默认/片段音效；可选静音/作者未提供音效也会计入，非所有缺资产枪都无法渲染。", "",
              "## 第三方扩展 / 未识别项（不承诺兼容）", ""]
    for pack in sorted(grouped):
        lines += ["### " + pack, "", "字段：" + cell(sorted(unknown_fields[pack])), "",
                  "Lua API：" + cell(sorted(unknown_calls[pack])), ""]
    lines += ["## Retest first（≤15 把）", "",
              "1. `tacz:m4a1`、`tacz:glock_17`、`tacz:deagle`：两种换弹，旧匣/新匣、收枪及快速切槽。",
              "2. `tacz:b93r`：换弹（原文件不 key 额外匣，匣链会移出画面，需单独观察）及连发时无贴脸黄块。",
              "3. `tacz:m249`、`tacz:aa12`：打空后显示 0 而非 0+1，不能额外开一枪；闭膛枪+1仍保留。",
              "4. `tacz:hk416d`、`tacz:p320`：静态白字深度冲突、检视倾斜仍待对照，未承诺已修。",
              "5. `ccrp:timeless_50`、`ccrp:sr_3m`、`classicr:ak12`：尾逗号/正号、ClassicRCCRP 各 namespace。",
              "6. `sfms:mgl416`、`cyber_armorer:g58_dian`：JSON修复与模型/贴图/动画；尚不代表榴弹/充能功能已实现。",
              "7. `twx:twx_b3`：测试载入/换弹/收枪与未知 API 回退日志。", "",
              "## 每枪表", "",
              "| GunId | 加载/data/display/geometry | bolt；初始 mag/raw/effective | reload A/Z/M | put-away片段/data/方式/脚本 | inspect非零旋转骨 | illuminated / 薄面 / 重面对数 | ammo entity | 机制；pellets | 未识别字段；API / require |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for g in rows:
        data, clips, geo = (g.get(k, {}) for k in ("data", "clips", "geometry"))
        initial = g.get("initial", {})
        reload = ("A; " if g["reloadAdditional"] else "—; ") + "Z:" + cell(g["reloadHide"]) + "; M:" + cell(g["reloadMove"])
        duration = clips.get('put_away', {}).get('duration')
        put = f"{round(duration,4) if duration is not None else 'missing'} / {data.get('put_away_time', .4)} / {g['putMethod']} / {g.get('display',{}).get('state_machine','tacz:default_state_machine')}" + ("; nil-track" if g["nilTrack"] else "")
        entity = g.get("ammoDisplay", {}).get("entity")
        entity_text = json.dumps(entity, ensure_ascii=False) if entity else "无（普通弹不可见）"
        status = f"{'OK' if g['loaded'] else 'FAIL'}/{'OK' if 'data' in g else 'FAIL'}/{'OK' if g.get('displayLoaded') else 'FAIL'}/{'OK' if geo else 'FAIL'}"
        if g.get("missing"): status += f"; 缺{len(g['missing'])}项"
        if g.get("animationWarnings"): status += f"; 动画警告{len(g['animationWarnings'])}项"
        line = [g["id"], status,
                f"{data.get('bolt','?')}; {initial.get('magazine','?')}/{int(initial.get('rawChamber',False))}/{int(initial.get('effectiveChamber',False))}",
                reload, put, g["inspectRot"], f"{cell(geo.get('illuminated', []))} / {len(geo.get('thin', []))} / {geo.get('coincidentQuads',0)}", entity_text,
                cell(g["mechanics"]) + f"; pellets={g['pellets']}",
                cell(g["unknownFields"]) + "; " + cell(g["unknownCalls"]) + ("; require缺:" + cell(g["missingModules"]) if g["missingModules"] else "")]
        lines.append("| " + " | ".join(map(cell, line)) + " |")
    lines += ["", "## 缺资源明细（每枪一行；可选音效同样列出）", ""]
    for g in rows:
        if g.get("missing"): lines.append("- `" + g["id"] + "`: " + "; ".join(g["missing"]))
        for key in ("error", "geometryError", "animationError", "ammoError"):
            if key in g: lines.append(f"- `{g['id']}` {key}: {g[key]}")
        for warning in g.get("animationWarnings", []): lines.append(f"- `{g['id']}` animation warning: {warning}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {args.output}: {len(rows)} indexes / {report['loadedCount']} loaded")


if __name__ == "__main__":
    main()
