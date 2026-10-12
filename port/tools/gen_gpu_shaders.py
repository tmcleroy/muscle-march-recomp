#!/usr/bin/env python3
"""Generate the SDL_GPU backend's shaders from the OpenGL backend's GLSL.

The GLSL in engine/src/gpu/gx_render_gl.cpp stays the single source of truth.
This script extracts each shader, rewrites its declarations for SDL_GPU's
resource layout (Vulkan GLSL 450: vertex uniforms in set 1, fragment samplers
in set 2, fragment uniforms in set 3), compiles it to SPIR-V with
glslangValidator and to Metal Shading Language with spirv-cross, and writes
engine/src/gpu/gpu_shaders.inc (committed, so builds need neither tool).

The TEV fragment shader interprets the stage setup per pixel. For every setup listed in
games/*/tev_keys.txt (written by the game with WP_LOG_TEV_KEYS=file; the words are those of
tev_key() in gx_render_common.cpp) a specialised shader is generated too: the general shader's
text rewritten for that setup by engine/src/gpu/tev_specialise.cpp (built here as a small
program, so this needs a C++ compiler), then reduced to straight-line code by spirv-opt.

  python3 tools/gen_gpu_shaders.py
"""
import glob
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "engine" / "src" / "gpu" / "gx_render_gl.cpp"
OUTPUT = ROOT / "engine" / "src" / "gpu" / "gpu_shaders.inc"
SPECIALISER = ROOT / "engine" / "src" / "gpu" / "tev_specialise.cpp"
KEY_HEADER = 8
KEY_STAGE = 5


def extract(text, name):
    m = re.search(r"const char\* " + name + r' = R"GLSL\((.*?)\)GLSL";', text, re.S)
    if not m:
        sys.exit(f"shader {name} not found")
    return m.group(1)


def fragment_common(src):
    """Samplers -> set 2, loose uniforms -> one block in set 3, std140 blocks -> set 3."""
    src = src.replace("#version 410 core", "")
    binding = 0
    def sampler(m):
        nonlocal binding
        out = f"layout(set = 2, binding = {binding}) uniform sampler2D {m.group(1)};"
        binding += 1
        return out
    src = re.sub(r"uniform sampler2D (\w+);", sampler, src)
    loose = re.findall(r"^uniform (\w+) (\w+);\s*$", src, re.M)
    src = re.sub(r"^uniform \w+ \w+;\s*$\n?", "", src, flags=re.M)
    block = 0
    def ubo(m):
        nonlocal block
        out = f"layout(set = 3, binding = {block}, std140) uniform {m.group(1)} {{"
        block += 1
        return out
    src = re.sub(r"layout\(std140\) uniform (\w+) \{", ubo, src)
    if loose:
        fields = "".join(f"    {t} {n};\n" for t, n in loose)
        src = f"layout(set = 3, binding = {block}, std140) uniform FragmentUniforms {{\n{fields}}};\n" + src
    src = src.replace("in vec2 uv;", "layout(location = 0) in vec2 uv;")
    src = re.sub(r"^out vec4 (\w+);", r"layout(location = 0) out vec4 \1;", src, flags=re.M)
    return "#version 450\n" + src


def fullscreen_vertex(src):
    src = src.replace("#version 410 core", "#version 450")
    src = src.replace("uniform float flip;", "layout(set = 1, binding = 0, std140) uniform VertexUniforms {\n    float flip;\n};")
    src = src.replace("out vec2 uv;", "layout(location = 0) out vec2 uv;")
    return src.replace("gl_VertexID", "gl_VertexIndex")


def tev_vertex(src):
    src = src.replace("#version 410 core", "#version 450")
    src = src.replace("uniform int depth_zero_to_one;", "layout(set = 1, binding = 0, std140) uniform VertexUniforms {\n    int depth_zero_to_one;\n};")
    src = src.replace("out vec4 v_c0;", "layout(location = 0) out vec4 v_c0;")
    src = src.replace("out vec4 v_c1;", "layout(location = 1) out vec4 v_c1;")
    src = src.replace("out vec3 v_uv[8];", "layout(location = 2) out vec3 v_uv[8];")
    # SDL_GPU targets have a top-left origin (Metal/D3D style); GL's EFB is stored bottom-up and
    # flips y here. Without the flip, storage row 0 is the top of the EFB on both backends.
    if "-a_position.y" not in src:
        sys.exit("tev vertex: expected the GL y flip")
    return src.replace("-a_position.y", "a_position.y")


def tev_fragment(src, early):
    src = fragment_common(src)
    src = src.replace("in vec4 v_c0;", "layout(location = 0) in vec4 v_c0;")
    src = src.replace("in vec4 v_c1;", "layout(location = 1) in vec4 v_c1;")
    src = src.replace("in vec3 v_uv[8];", "layout(location = 2) in vec3 v_uv[8];")
    # No dual-source blending in SDL_GPU: out_blend becomes a private value, and pass 1 of a
    # "TEV alpha" blend writes the TEV result's alpha so the blender can use it (see backend).
    src = src.replace("layout(location = 0, index = 0) out vec4 out_color;", "layout(location = 0) out vec4 out_color;")
    src = src.replace("layout(location = 0, index = 1) out vec4 out_blend;", "vec4 out_blend;")
    src = src.replace("void main() {", "void tev_main() {", 1)
    src += """
layout(set = 3, binding = 1, std140) uniform PassUniforms {
    uvec4 pass_mode;
};

void main() {
    tev_main();
    if (pass_mode.x == 1u) {
        out_color = vec4(out_color.rgb, out_blend.a);
    } else if (pass_mode.x == 2u) {
        out_color = out_blend;
    }
}
"""
    if early:
        src = src.replace("#version 450\n", "#version 450\nlayout(early_fragment_tests) in;\n", 1)
    return src


def read_keys():
    """Every listed TEV setup, as a tuple of words, in a stable order."""
    keys = set()
    for path in sorted(glob.glob(str(ROOT / "games" / "*" / "tev_keys.txt"))):
        for line in Path(path).read_text().splitlines():
            line = line.split("#")[0].split()
            if not line:
                continue
            words = tuple(int(w, 16) for w in line)
            stages = words[0] & 0xFF
            if len(words) != KEY_HEADER + KEY_STAGE * stages or not 1 <= stages <= 16:
                sys.exit(f"{path}: malformed key: {' '.join(line)}")
            keys.add(words)
    return sorted(keys)


def specialise_tev(general, keys, tmp):
    """The general TEV fragment shader with each key's setup built in, by engine/src/gpu/tev_specialise.cpp."""
    if not keys:
        return []
    tool = Path(tmp) / "tev_specialise"
    r = subprocess.run(["c++", "-std=c++17", "-O1", "-DWP_TEV_SPECIALISE_TOOL", str(SPECIALISER), "-o", str(tool)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"tev_specialise.cpp does not compile:\n{r.stderr}")
    source = Path(tmp) / "general.glsl"
    source.write_text(general)
    lines = "".join(" ".join(f"{w:x}" for w in key) + "\n" for key in keys)
    r = subprocess.run([str(tool), str(source), tmp], input=lines, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"tev_specialise failed:\n{r.stderr}")
    return [(Path(tmp) / f"{i}.glsl").read_text() for i in range(len(keys))]


def compile_stage(name, stage, glsl, tmp, optimise=False):
    path = Path(tmp) / f"{name}.{stage}"
    path.write_text(glsl)
    spv = Path(tmp) / f"{name}.spv"
    r = subprocess.run(["glslangValidator", "-V", "-S", stage, "-o", str(spv), str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(glsl, file=sys.stderr)
        sys.exit(f"{name}: glslang failed:\n{r.stdout}{r.stderr}")
    if optimise:
        r = subprocess.run(["spirv-opt", "-O", str(spv), "-o", str(spv)], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{name}: spirv-opt failed:\n{r.stdout}{r.stderr}")
        r = subprocess.run(["spirv-val", str(spv)], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{name}: spirv-val failed:\n{r.stdout}{r.stderr}")
    msl = subprocess.run(["spirv-cross", "--msl", "--msl-version", "20100", "--msl-decoration-binding", str(spv)],
                         capture_output=True, text=True)
    if msl.returncode != 0:
        sys.exit(f"{name}: spirv-cross failed:\n{msl.stderr}")
    return spv.read_bytes(), msl.stdout


def main():
    text = SOURCE.read_text()
    shaders = [
        ("fullscreen_vertex", "vert", fullscreen_vertex(extract(text, "kFullscreenVertex"))),
        ("present_fragment", "frag", fragment_common(extract(text, "kPresentFragment"))),
        ("overlay_fragment", "frag", fragment_common(extract(text, "kOverlayFragment"))),
        ("clear_fragment", "frag", fragment_common(extract(text, "kClearFragment"))),
        ("copy_fragment", "frag", fragment_common(extract(text, "kCopyFragment"))),
        ("tev_vertex", "vert", tev_vertex(extract(text, "kTevVertex"))),
        ("tev_fragment", "frag", tev_fragment(extract(text, "kTevFragment"), False)),
        ("tev_early_fragment", "frag", tev_fragment(extract(text, "kTevFragment"), True)),
    ]
    out = ["// Generated by tools/gen_gpu_shaders.py from gx_render_gl.cpp. Do not edit.", ""]

    def emit(name, spv, msl):
        words = ", ".join(f"0x{w:x}" for (w,) in struct.iter_unpack("<I", spv))
        out.append(f"constexpr uint32_t k_{name}_spirv[] = {{{words}}};")
        escaped = msl.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n\"\n\"")
        out.append(f"constexpr const char k_{name}_msl[] =\n\"{escaped}\";")
        out.append("")

    with tempfile.TemporaryDirectory() as tmp:
        for name, stage, glsl in shaders:
            spv, msl = compile_stage(name, stage, glsl, tmp)
            emit(name, spv, msl)
            print(f"{name}: {len(spv)} bytes SPIR-V, {len(msl)} bytes MSL")
        # Specialised TEV shaders: keys whose shaders come out the same share one.
        general = extract(text, "kTevFragment")
        compiled = {}
        table = []
        size = 0
        keys = read_keys()
        for key, specialised in zip(keys, specialise_tev(general, keys, tmp)):
            glsl = tev_fragment(specialised, (key[0] & 0x100) != 0)
            spv, msl = compile_stage("tev_specialised", "frag", glsl, tmp, optimise=True)
            if (spv, msl) not in compiled:
                compiled[(spv, msl)] = len(compiled)
                emit(f"tev_s{len(compiled) - 1}", spv, msl)
                size += len(spv) + len(msl)
            table.append((key, compiled[(spv, msl)]))
        print(f"specialised TEV: {len(table)} keys, {len(compiled)} shaders, {size} bytes")
    out.append("struct TevSpecialised {\n    const uint32_t* spirv;\n    size_t spirv_size;\n    const char* msl;\n};")
    entries = "".join(f"\n    {{k_tev_s{i}_spirv, sizeof k_tev_s{i}_spirv, k_tev_s{i}_msl}}," for i in range(len(compiled)))
    out.append(f"constexpr TevSpecialised k_tev_specialised[] = {{{entries or ' {nullptr, 0, nullptr} '}\n}};")
    out.append(f"constexpr uint32_t k_tev_specialised_count = {len(compiled)};")
    out.append("// Per key: number of words, index in k_tev_specialised, the words. A zero ends the list.")
    rows = "".join(f"\n    {len(key)}, {index}, " + ", ".join(f"0x{w:x}" for w in key) + "," for key, index in table)
    out.append(f"constexpr uint32_t k_tev_key_words[] = {{{rows}\n    0\n}};")
    OUTPUT.write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
