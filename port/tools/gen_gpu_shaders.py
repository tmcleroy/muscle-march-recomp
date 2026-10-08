#!/usr/bin/env python3
"""Generate the SDL_GPU backend's shaders from the OpenGL backend's GLSL.

The GLSL in engine/src/gpu/gx_render_gl.cpp stays the single source of truth.
This script extracts each shader, rewrites its declarations for SDL_GPU's
resource layout (Vulkan GLSL 450: vertex uniforms in set 1, fragment samplers
in set 2, fragment uniforms in set 3), compiles it to SPIR-V with
glslangValidator and to Metal Shading Language with spirv-cross, and writes
engine/src/gpu/gpu_shaders.inc (committed, so builds need neither tool).

  python3 tools/gen_gpu_shaders.py
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "engine" / "src" / "gpu" / "gx_render_gl.cpp"
OUTPUT = ROOT / "engine" / "src" / "gpu" / "gpu_shaders.inc"


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


def compile_stage(name, stage, glsl, tmp):
    path = Path(tmp) / f"{name}.{stage}"
    path.write_text(glsl)
    spv = Path(tmp) / f"{name}.spv"
    r = subprocess.run(["glslangValidator", "-V", "-S", stage, "-o", str(spv), str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(glsl, file=sys.stderr)
        sys.exit(f"{name}: glslang failed:\n{r.stdout}{r.stderr}")
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
    with tempfile.TemporaryDirectory() as tmp:
        for name, stage, glsl in shaders:
            spv, msl = compile_stage(name, stage, glsl, tmp)
            words = ", ".join(f"0x{b:02x}" for b in spv)
            out.append(f"constexpr unsigned char k_{name}_spirv[] = {{{words}}};")
            escaped = msl.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n\"\n\"")
            out.append(f"constexpr const char k_{name}_msl[] =\n\"{escaped}\";")
            out.append("")
            print(f"{name}: {len(spv)} bytes SPIR-V, {len(msl)} bytes MSL")
    OUTPUT.write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
