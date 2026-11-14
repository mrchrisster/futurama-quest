#!/usr/bin/env python3
"""Compile GLSL 450 fixed-function shaders to SPIR-V blobs for SdlGpuDevice (Vulkan)."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT_HEADER = ROOT / "src/render/xd3d/SdlGpuSpirvBlobs.h"

COMMON_DRAW_CB = """
struct RowMat {
    vec4 r0;
    vec4 r1;
    vec4 r2;
    vec4 r3;
};

vec4 mulRow(vec4 v, RowMat m) {
    return v.x * m.r0 + v.y * m.r1 + v.z * m.r2 + v.w * m.r3;
}

struct Light {
    vec4 diffuse;
    vec4 specular;
    vec4 ambient;
    vec4 pos;
    vec4 dir;
    vec4 att;
    vec4 spot;
};
"""

DRAW_CB_FIELDS = """
    RowMat world;
    RowMat view;
    RowMat proj;
    RowMat texm[4];
    vec4 vp;
    vec4 ssOffset;
    vec4 matDiffuse;
    vec4 matAmbient;
    vec4 matSpecular;
    vec4 matEmissive;
    vec4 ambient;
    vec4 misc0;
    vec4 misc1;
    vec4 misc2;
    vec4 misc3;
    vec4 fogParams;
    vec4 fogColor;
    vec4 tfactor;
    vec4 alphaTest;
    ivec4 tci[4];
    ivec4 colorOp[4];
    ivec4 alphaOp[4];
    ivec4 stageMisc[4];
    ivec4 lightCount;
    Light lights[8];
    vec4 texScale[4];
"""

VS_MAIN = f"""#version 450
{COMMON_DRAW_CB}

layout(std140, set = 1, binding = 0) uniform DrawCB {{
{DRAW_CB_FIELDS}
}} cb;

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec4 in_d0;
layout(location = 3) in vec4 in_d1;
layout(location = 4) in vec4 in_uv0;
layout(location = 5) in vec4 in_uv1;
layout(location = 6) in vec4 in_uv2;
layout(location = 7) in vec4 in_uv3;

layout(location = 0) out vec4 out_d0;
layout(location = 1) out vec4 out_d1;
layout(location = 2) flat out vec4 out_fd0;
layout(location = 3) flat out vec4 out_fd1;
layout(location = 4) out vec4 out_fog;
layout(location = 5) centroid out vec4 out_t0;
layout(location = 6) centroid out vec4 out_t1;
layout(location = 7) centroid out vec4 out_t2;
layout(location = 8) centroid out vec4 out_t3;

float saturate(float x) {{ return clamp(x, 0.0, 1.0); }}
vec3 saturate(vec3 x) {{ return clamp(x, 0.0, 1.0); }}

vec4 SourceColor(int s, vec4 vertexColor, vec4 material, vec4 misc2) {{
    return (s == 1 && misc2.y > 0.5) ? vertexColor
         : (s == 2 && misc2.z > 0.5) ? vertexColor
         : material;
}}

vec4 TexCoord(int stage, vec4 uv[4], vec3 pv, vec3 nv) {{
    ivec4 c = cb.tci[stage];
    vec4 t;
    if (c.y == 1) {{
        t = vec4(nv, 1.0);
    }} else if (c.y == 2) {{
        t = vec4(pv, 1.0);
    }} else if (c.y == 3) {{
        vec3 e = normalize(pv);
        t = vec4(e - 2.0 * dot(nv, e) * nv, 1.0);
    }} else {{
        t = uv[min(c.x, 3)];
        if (c.z > 0) {{
            int dim = cb.stageMisc[stage].z;
            if (dim == 1) t = vec4(t.x, 1.0, 0.0, 0.0);
            else if (dim == 2) t = vec4(t.xy, 1.0, 0.0);
            else if (dim == 3) t = vec4(t.xyz, 1.0);
        }}
    }}
    if (c.z > 0) {{
        vec4 r = mulRow(t, cb.texm[stage]);
        vec4 o = vec4(0.0, 0.0, 0.0, 1.0);
        o.x = r.x;
        if (c.z >= 2) o.y = r.y;
        if (c.z >= 3) o.z = r.z;
        if (c.z >= 4) o.w = r.w;
        if (c.w != 0) {{
            float q = c.z == 2 ? r.y : (c.z == 3 ? r.z : r.w);
            o = vec4(o.xy, 0.0, q);
        }} else {{
            o.w = 1.0;
        }}
        return o;
    }}
    return vec4(t.xy, t.z, 1.0);
}}

void main() {{
    vec3 pv = vec3(0.0);
    vec3 nv = vec3(0.0, 0.0, 1.0);

    if (cb.misc2.w > 0.5) {{
        float w = in_pos.w != 0.0 ? 1.0 / in_pos.w : 1.0;
        vec2 s = in_pos.xy + cb.ssOffset.xy + 0.53125 * cb.ssOffset.zw - cb.vp.xy;
        gl_Position = vec4((s.x / cb.vp.z * 2.0 - 1.0) * w, (1.0 - s.y / cb.vp.w * 2.0) * w, in_pos.z * w, w);
        pv = vec3(in_pos.xy, in_pos.z);
    }} else {{
        vec4 pw = mulRow(vec4(in_pos.xyz, 1.0), cb.world);
        vec4 v = mulRow(pw, cb.view);
        vec4 c = mulRow(v, cb.proj);
        c.x += c.w / cb.vp.z * 2.0 * (0.53125 * cb.ssOffset.z + cb.ssOffset.x);
        c.y -= c.w / cb.vp.w * 2.0 * (0.53125 * cb.ssOffset.w + cb.ssOffset.y);
        gl_Position = c;
        pv = v.xyz;
        if (cb.misc2.x > 0.5) {{
            nv = mulRow(mulRow(vec4(in_nrm.xyz, 0.0), cb.world), cb.view).xyz;
            if (cb.misc0.w > 0.5) {{
                float nl2 = dot(nv, nv);
                if (nl2 > 0.0) nv = nv * inversesqrt(nl2);
            }}
        }}
    }}

    vec4 cd = cb.misc2.y > 0.5 ? in_d0 : vec4(1.0, 1.0, 1.0, 1.0);
    vec4 cs = cb.misc2.z > 0.5 ? in_d1 : vec4(0.0, 0.0, 0.0, 0.0);
    if (cb.misc0.y > 0.5 && cb.misc2.w < 0.5) {{
        bool cv = cb.misc0.z > 0.5;
        vec4 md = cv ? SourceColor(int(cb.misc1.x), int(cb.misc1.x) == 2 ? cs : cd, cb.matDiffuse, cb.misc2) : cb.matDiffuse;
        vec4 ma = cv ? SourceColor(int(cb.misc1.y), int(cb.misc1.y) == 2 ? cs : cd, cb.matAmbient, cb.misc2) : cb.matAmbient;
        vec4 ms = cv ? SourceColor(int(cb.misc1.z), int(cb.misc1.z) == 2 ? cs : cd, cb.matSpecular, cb.misc2) : cb.matSpecular;
        vec4 me = cv ? SourceColor(int(cb.misc1.w), int(cb.misc1.w) == 2 ? cs : cd, cb.matEmissive, cb.misc2) : cb.matEmissive;
        vec3 a = ma.rgb * cb.ambient.rgb;
        vec3 dif = vec3(0.0);
        vec3 spec = vec3(0.0);
        for (int k = 0; k < cb.lightCount.x; ++k) {{
            Light L = cb.lights[k];
            vec3 l;
            float att = 1.0;
            float sp = 1.0;
            if (L.pos.w > 2.5) {{
                l = -normalize(L.dir.xyz);
            }} else {{
                vec3 d = L.pos.xyz - pv;
                float dist = length(d);
                l = d / max(dist, 1e-20);
                att = dist <= L.dir.w ? 1.0 / (L.att.x + L.att.y * dist + L.att.z * dist * dist) : 0.0;
                if (L.pos.w > 1.5) {{
                    float rho = dot(-l, normalize(L.dir.xyz));
                    if (rho > L.spot.x) {{
                        sp = 1.0;
                    }} else if (rho <= L.spot.y) {{
                        sp = 0.0;
                    }} else {{
                        float sb = saturate((rho - L.spot.y) / max(L.spot.x - L.spot.y, 1e-20));
                        sp = sb > 0.0 ? pow(sb, L.att.w) : (L.att.w == 0.0 ? 1.0 : 0.0);
                    }}
                }}
            }}
            float k2 = att * sp;
            a += k2 * ma.rgb * L.ambient.rgb;
            float nl = dot(nv, l);
            dif += k2 * max(0.0, nl) * L.diffuse.rgb * md.rgb;
            if (cb.misc3.x > 0.5 && nl > 0.0) {{
                vec3 e = cb.misc3.y > 0.5 ? normalize(-pv) : vec3(0.0, 0.0, 1.0);
                vec3 h = normalize(e + l);
                float ndh = max(0.0, dot(nv, h));
                float p = ndh > 0.0 ? pow(ndh, cb.misc0.x) : (cb.misc0.x == 0.0 ? 1.0 : 0.0);
                spec += k2 * p * L.specular.rgb * ms.rgb;
            }}
        }}
        cd = vec4(saturate(me.rgb + a + dif), saturate(md.a));
        cs = vec4(cb.misc3.x > 0.5 ? saturate(spec) : vec3(0.0), 0.0);
    }}
    out_d0 = cd;
    out_d1 = cs;
    out_fd0 = cd;
    out_fd1 = cs;

    float f = 1.0;
    if (cb.misc3.z > 0.5) {{
        int gen = int(cb.fogParams.w);
        float d = gen == 1 ? length(pv) : pv.z;
        int m = int(cb.misc3.w);
        if (gen == 0) f = cs.a;
        else if (m == 0) f = cb.fogParams.x + cb.fogParams.y * d - 1.0;
        else if (m == 1) f = cb.fogParams.x + exp2(16.0 * cb.fogParams.y * d) - 1.5;
        else f = cb.fogParams.x + exp2(-32.0 * (cb.fogParams.y * d) * (cb.fogParams.y * d)) - 1.5;
    }}
    out_fog = vec4(f, 0.0, 0.0, 0.0);
    vec4 uv[4] = vec4[4](in_uv0, in_uv1, in_uv2, in_uv3);
    out_t0 = TexCoord(0, uv, pv, nv);
    out_t1 = TexCoord(1, uv, pv, nv);
    out_t2 = TexCoord(2, uv, pv, nv);
    out_t3 = TexCoord(3, uv, pv, nv);
}}
"""

PS_MAIN = f"""#version 450
{COMMON_DRAW_CB}

layout(std140, set = 3, binding = 0) uniform DrawCB {{
{DRAW_CB_FIELDS}
}} cb;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;

layout(location = 0) in vec4 in_d0;
layout(location = 1) in vec4 in_d1;
layout(location = 2) flat in vec4 in_fd0;
layout(location = 3) flat in vec4 in_fd1;
layout(location = 4) in vec4 in_fog;
layout(location = 5) centroid in vec4 in_t0;
layout(location = 6) centroid in vec4 in_t1;
layout(location = 7) centroid in vec4 in_t2;
layout(location = 8) centroid in vec4 in_t3;

layout(location = 0) out vec4 fragColor;

float saturate(float x) {{ return clamp(x, 0.0, 1.0); }}
vec3 saturate(vec3 x) {{ return clamp(x, 0.0, 1.0); }}
vec4 saturate(vec4 x) {{ return clamp(x, 0.0, 1.0); }}

vec4 ArgFull(int a, vec4 diffuse, vec4 current, vec4 tex, vec4 tfac, vec4 spec, vec4 temp) {{
    int s = a & 15;
    vec4 v = s == 0 ? diffuse : s == 1 ? current : s == 2 ? tex : s == 3 ? tfac : s == 4 ? spec : temp;
    if ((a & 0x20) != 0) v = v.aaaa;
    if ((a & 0x10) != 0) v = 1.0 - v;
    return v;
}}

vec4 StageOp(int op, vec4 a1, vec4 a2, vec4 a0, vec4 diffuse, vec4 current, vec4 tex, vec4 tfac) {{
    if (op == 2) return a1;
    if (op == 3) return a2;
    if (op == 4) return a1 * a2;
    if (op == 5) return a1 * a2 * 2.0;
    if (op == 6) return a1 * a2 * 4.0;
    if (op == 7) return a1 + a2;
    if (op == 8) return a1 + a2 - 0.5;
    if (op == 9) return (a1 + a2 - 0.5) * 2.0;
    if (op == 10) return a1 - a2;
    if (op == 11) return a1 + a2 - a1 * a2;
    if (op == 12) return mix(a2, a1, diffuse.a);
    if (op == 13) return mix(a2, a1, current.a);
    if (op == 14) return mix(a2, a1, tex.a);
    if (op == 15) return mix(a2, a1, tfac.a);
    if (op == 16) return a1 + a2 * (1.0 - tex.a);
    if (op == 18) return vec4(a1.rgb + a1.a * a2.rgb, a1.a);
    if (op == 19) return vec4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);
    if (op == 20) return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
    if (op == 21) return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    if (op == 22) {{
        float d = saturate(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4.0);
        return vec4(d, d, d, d);
    }}
    if (op == 23) return a0 + a1 * a2;
    if (op == 24) return mix(a2, a1, a0);
    return a1;
}}

vec4 YuvToRgb(vec4 c) {{
    float y = (c.r * 255.0 - 16.0) * 38139.0;
    float u = c.g * 255.0 - 128.0;
    float v = c.b * 255.0 - 128.0;
    vec3 rgb = vec3(y + 52298.0 * v, y - 12812.0 * u - 26640.0 * v, y + 66126.0 * u) / 32768.0;
    return vec4(saturate(rgb / 255.0), c.a);
}}

vec4 SampleStage(int s, vec4 t) {{
    vec2 uv = (t.w != 0.0 && t.w != 1.0) ? t.xy / t.w : t.xy;
    uv *= cb.texScale[s].xy;
    vec4 c;
    if (s == 0) c = texture(tex0, uv);
    else if (s == 1) c = texture(tex1, uv);
    else if (s == 2) c = texture(tex2, uv);
    else c = texture(tex3, uv);
    return cb.texScale[s].z > 0.5 ? YuvToRgb(c) : c;
}}

void main() {{
    vec4 diffuse = cb.alphaTest.w > 0.5 ? in_fd0 : in_d0;
    vec4 spec = cb.alphaTest.w > 0.5 ? in_fd1 : in_d1;
    vec4 current = diffuse;
    vec4 temp = vec4(0.0);
    vec4 t[4] = vec4[4](in_t0, in_t1, in_t2, in_t3);
    for (int s = 0; s < 4; ++s) {{
        ivec4 c = cb.colorOp[s];
        ivec4 a = cb.alphaOp[s];
        if (c.x == 1) break;
        vec4 tex = cb.stageMisc[s].y != 0 ? SampleStage(s, t[s]) : vec4(0.0, 0.0, 0.0, 1.0);
        vec4 cr = StageOp(c.x,
                          ArgFull(c.y, diffuse, current, tex, cb.tfactor, spec, temp),
                          ArgFull(c.z, diffuse, current, tex, cb.tfactor, spec, temp),
                          ArgFull(c.w, diffuse, current, tex, cb.tfactor, spec, temp),
                          diffuse, current, tex, cb.tfactor);
        vec4 ar = a.x == 1 ? current
                : StageOp(a.x,
                          ArgFull(a.y | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                          ArgFull(a.z | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                          ArgFull(a.w | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                          diffuse, current, tex, cb.tfactor);
        vec4 r = saturate(vec4(cr.rgb, ar.a));
        if (cb.stageMisc[s].x == 5) temp = r;
        else current = r;
    }}
    vec4 c = current;
    if (cb.misc3.x > 0.5) c.rgb = saturate(c.rgb + spec.rgb);
    if (cb.misc3.z > 0.5) c.rgb = mix(cb.fogColor.rgb, c.rgb, saturate(in_fog.x));
    if (cb.alphaTest.x > 0.5) {{
        int f = int(cb.alphaTest.y);
        float r = cb.alphaTest.z;
        float v = round(c.a * 255.0) / 255.0;
        bool ok = f == 7 || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r) ||
                  (f == 4 && v > r) || (f == 5 && v != r) || (f == 6 && v >= r);
        if (!ok) discard;
    }}
    fragColor = c;
}}
"""

CLEAR_VS = """#version 450
layout(std140, set = 1, binding = 0) uniform ClearCB {
    vec4 clearRect;
    vec4 clearColor;
    vec4 clearZ;
} cb;

void main() {
    uint id = uint(gl_VertexIndex);
    vec2 p = vec2((id & 1u) != 0u ? cb.clearRect.z : cb.clearRect.x,
                  (id & 2u) != 0u ? cb.clearRect.w : cb.clearRect.y);
    gl_Position = vec4(p, cb.clearZ.x, 1.0);
}
"""

CLEAR_PS = """#version 450
layout(std140, set = 3, binding = 0) uniform ClearCB {
    vec4 clearRect;
    vec4 clearColor;
    vec4 clearZ;
} cb;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = cb.clearColor;
}
"""

BLIT_VS = """#version 450
layout(location = 0) out vec2 out_uv;

void main() {
    uint id = uint(gl_VertexIndex);
    out_uv = vec2(float(id & 1u), float(id >> 1u));
    gl_Position = vec4(out_uv.x * 2.0 - 1.0, 1.0 - out_uv.y * 2.0, 0.0, 1.0);
}
"""

BLIT_PS = """#version 450
layout(set = 2, binding = 0) uniform sampler2D blitTex;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(texture(blitTex, in_uv).rgb, 1.0);
}
"""

SHADERS = [
    ("kVSMain", "vert", VS_MAIN),
    ("kPSMain", "frag", PS_MAIN),
    ("kClearVS", "vert", CLEAR_VS),
    ("kClearPS", "frag", CLEAR_PS),
    ("kBlitVS", "vert", BLIT_VS),
    ("kBlitPS", "frag", BLIT_PS),
]


def compile_glsl(stage: str, src: str, tmpdir: pathlib.Path) -> bytes:
    src_path = tmpdir / f"shader.{stage}"
    spv_path = tmpdir / f"shader.{stage}.spv"
    src_path.write_text(src)
    subprocess.run(
        ["glslangValidator", "-V", "-S", stage, "-o", str(spv_path), str(src_path)],
        check=True,
    )
    return spv_path.read_bytes()


def format_array(name: str, data: bytes) -> str:
    lines = [f"inline constexpr unsigned char {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    blocks = []
    with tempfile.TemporaryDirectory() as td:
        tmpdir = pathlib.Path(td)
        for name, stage, src in SHADERS:
            spv = compile_glsl(stage, src, tmpdir)
            blocks.append(format_array(name, spv))
            print(f"Compiled {name}: {len(spv)} bytes")

    OUT_HEADER.write_text(
        "// Precompiled SPIR-V shaders for SdlGpuDevice (Vulkan / Quest 3); generated by tools/compile_spirv_shaders.py.\n"
        "#pragma once\n\n"
        "namespace fut::render::xd3d::sdl_spirv_shaders {\n\n"
        + "\n\n".join(blocks)
        + "\n\n}  // namespace fut::render::xd3d::sdl_spirv_shaders\n"
    )
    print(f"Wrote {OUT_HEADER}")


if __name__ == "__main__":
    main()
