"""Read-only, version-pinned Minecraft 1.26.51 native mesh packing evidence.

Requires pefile and capstone (pip install to an external dependency directory).
No game is launched, binary changed or executable code invoked. RVA assertions
fail closed on another build. Output is metadata/disassembly, never game assets.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--dependency-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.binary.resolve() == args.output.resolve():
        parser.error("output must not overwrite binary")
    if args.dependency_dir:
        sys.path.insert(0, str(args.dependency_dir))
    import pefile
    import capstone
    pe = pefile.PE(str(args.binary), fast_load=True)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    base = pe.OPTIONAL_HEADER.ImageBase
    def instructions(rva, size):
        return list(md.disasm(pe.get_data(rva,size),base+rva))
    # Default-layout field4 (UV0) -> jump-table entry -> constant type4.
    table = 0xE8EBC60
    destination = table+struct.unpack("<i",pe.get_data(table+4*4,4))[0]
    assert destination == 0x488DF19
    assert pe.get_data(destination,4) == bytes.fromhex("66 b8 04 00")
    assert struct.unpack("<f",pe.get_data(0xE61E17C,4))[0] == 65535.
    # Verify the actual imported rounding operation, not a guessed ceil/floor.
    thunk = instructions(0xE5A68E0,6)[0]
    assert thunk.mnemonic == "jmp"
    slot = thunk.address+thunk.size+struct.unpack("<i",thunk.bytes[-4:])[0]
    pe.parse_data_directories(directories=[1])
    imports = [(d.dll.decode(),i.name.decode()) for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports if i.address==slot]
    assert imports == [("api-ms-win-crt-math-l1-1-0.dll", "ceilf")]
    pack = instructions(0x4B017B0,0x2a)
    assert [i.mnemonic for i in pack] == ["movss","mulss","call","cvttss2si","mov","movss","mulss","call","cvttss2si","mov"]
    assert all("0x14e5a68e0" == i.op_str for i in pack if i.mnemonic == "call")
    assert [i.op_str for i in pack if i.mnemonic == "mov"] == ["word ptr [rsi], ax", "word ptr [rsi + 2], ax"]
    ranges = {"mesh_generate_default_layout_and_encode":(0x4AFCE78,0xed),
              "default_layout_UV0_from_MeshData_plus98":(0x4AFFF32,0x41),
              "field_type_dispatch":(0x488DEF2,0x65),
              "encoder_UV0_type4_and_type3":(0x4B0173F,0x137),
              "optimized_UV0_packer":(0x4B05D10,0x40),
              "position_half_converter_not_UV":(0x4B05A80,0x90)}
    disasm = {name:[f"{i.address-base:09x}: {i.mnemonic} {i.op_str}" for i in instructions(*span)] for name,span in ranges.items()}
    values=[]
    for uv in [.7890625,1.005859375,-.25,1.,0.]:
        scaled=struct.unpack("<f",struct.pack("<f",uv*65535))[0]
        integer=math.ceil(scaled);word=integer&65535
        values.append({"UV":uv,"float32_product":scaled,"ceil_int":integer,"word":word,"UNORM_decode":word/65535})
    with args.binary.open("rb") as binary:
        digest=hashlib.file_digest(binary,"sha256").hexdigest()
    result={"binary":str(args.binary),"sha256":digest,"UV0_type":4,"UV0_components":2,"UV0_bits_each":16,
            "encode":"low16(ceil(float32(UV*65535))); no clamp", "decoded_range":[0,1],
            "default_type_proof":"field4 jump table resolves mov ax,4, independent of position backend capability",
            "ceilf_IAT":hex(slot),"imports":imports,"examples":values,"disassembly":disasm,
            "limits":"CPU native encoder proven; UNORM interpretation follows65535 scale. Not a GPU capture; position half selection depends on backend capability."}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps({k:v for k,v in result.items() if k!="disassembly"},indent=2))


if __name__ == "__main__":
    main()
