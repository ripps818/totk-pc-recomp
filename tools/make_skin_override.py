#!/usr/bin/env python3
"""Build a TOTK_SHADER_OVERRIDE file that adds soft form shading to the game's skin lighting shader.

The game's character-skin lighting shader (a fragment shader of the deferred lighting pass) lights skin with a hard toon
step on the sun's N.L and nothing inside the shaded side, so a face turned away from the sun is one flat tone: the head
runs into the neck and chest. This tool takes the translated SPIR-V of that shader from your own machine, round-trips it
through spirv-cross and glslangValidator with one change, and writes <out>/<hash>.spv. Start the game with
TOTK_SHADER_OVERRIDE=<out> (runtime/gfx_shader.c shader_override) to use it.

The change: the shader's output colour is multiplied by mix(1 - strength, 1, smoothstep(-width, width, -N.L_sun)), where
N is the shader's own decoded normal and L_sun its sun direction uniform. Lit areas keep their colour; the shaded side
darkens smoothly with the surface turning away from the sun.

    tools/make_skin_override.py --out <folder> (--spv <file> | --scan <folder>) [--strength 0.35] [--width 0.7]

--spv: one SPIR-V file (for example the sh_<hash>.spv of a TOTK_DUMP frame dump). --scan: a folder of .spv files; the one
whose hash matches is used. Needs spirv-cross and glslangValidator on PATH (Arch: pacman -S spirv-cross glslang). The
output derives from the game's shader: keep it out of the repository (AGENTS.md, game content boundary).
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKIN_HASH = "0cbaa19c2ed9eefb"   # FNV-1a 64 of the shader's SPIR-V bytes: the sh_<hash>.spv name in a frame dump


def fnv(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def find(args) -> bytes:
    if args.spv:
        data = Path(args.spv).read_bytes()
        if f"{fnv(data):016x}" != args.hash:
            sys.exit(f"{args.spv} hashes to {fnv(data):016x}, not {args.hash}")
        return data
    for p in sorted(Path(args.scan).glob("*.spv")):
        data = p.read_bytes()
        if len(data) % 4 == 0 and f"{fnv(data):016x}" == args.hash:
            print(f"found {p}")
            return data
    sys.exit(f"no .spv in {args.scan} has hash {args.hash} (a frame dump, TOTK_DUMP, writes sh_<hash>.spv files)")


def patch(glsl: str, strength: float, width: float) -> str:
    # glslang wants int offset and count where spirv-cross writes unsigned ones
    glsl = re.sub(r"bitfieldExtract\(([^,]+), (\d+)u, (\d+)u\)", r"bitfieldExtract(\1, \2, \3)", glsl)
    outs = [m for m in re.finditer(r"^    _63\.([xyz]) = (.*);$", glsl, re.M)]
    if len(outs) != 3 or "fp_c9_1._m0[4]" not in glsl:
        sys.exit("this is not the skin shader this tool was written for (output stores or sun uniform not found)")
    # the decoded normal is (_120, _122, _194) and the sun direction is fp_c9_1._m0[4].xyz in this shader (see its main())
    for n in ("_120", "_122", "_194"):
        if not re.search(rf"^    {n} = ", glsl, re.M):
            sys.exit(f"variable {n} (the decoded normal) not found: the shader differs from the one this tool expects")
    form = (f"    float _nl = -fma(_194, fp_c9_1._m0[4].z, fma(_122, fp_c9_1._m0[4].y, _120 * fp_c9_1._m0[4].x));\n"
            f"    float _shade = mix(1.0 - {strength}, 1.0, smoothstep(-{width}, {width}, _nl));\n")
    first = outs[0].start()
    glsl = glsl[:first] + form + glsl[first:]
    return re.sub(r"^    _63\.([xyz]) = (.*);$", r"    _63.\1 = _shade * (\2);", glsl, flags=re.M)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--spv")
    src.add_argument("--scan")
    ap.add_argument("--out", required=True)
    ap.add_argument("--strength", type=float, default=0.35, help="darkest fraction removed on the shaded side (0 = off)")
    ap.add_argument("--width", type=float, default=0.7, help="half-width of the N.L ramp around the terminator")
    ap.add_argument("--hash", default=SKIN_HASH)
    args = ap.parse_args()
    if not 0.0 <= args.strength < 1.0 or not 0.0 < args.width <= 1.0:
        sys.exit("strength must be in [0, 1) and width in (0, 1]")
    for tool in ("spirv-cross", "glslangValidator"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found on PATH")
    data = find(args)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        spv, frag, res = Path(t, "in.spv"), Path(t, "skin.frag"), Path(t, "out.spv")
        spv.write_bytes(data)
        glsl = subprocess.run(["spirv-cross", "--vulkan-semantics", "--version", "450", str(spv)], check=True,
                              capture_output=True, text=True).stdout
        frag.write_text(patch(glsl, args.strength, args.width))
        r = subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.3", "-S", "frag", str(frag), "-o", str(res)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        dest = out / f"{args.hash}.spv"
        shutil.copyfile(res, dest)
    print(f"wrote {dest}\nrun the game with TOTK_SHADER_OVERRIDE={out}")


if __name__ == "__main__":
    main()
