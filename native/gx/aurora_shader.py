# SPDX-License-Identifier: MIT
"""Scoped correctness adaptations for the pinned Aurora shader backend.

Generated translation units retain their dependency's license. Keep the donor
checkout unchanged and reject a changed source anchor instead of guessing.
"""
import argparse
from pathlib import Path


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned Aurora shader source differs: ' + old[:80])
    return text.replace(old, new, 1)


def shader_info(text):
    return once(text, '  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);', '''  // Color-source texgens consume the lit XF color channels even when TEV
  // never reads raster color. This dependency must participate in both the
  // generated shader and its uniform layout.
  for (size_t i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    if (config.tcgs[i].src == GX_TG_COLOR0) info.sampledColorChannels.set(0);
    if (config.tcgs[i].src == GX_TG_COLOR1) info.sampledColorChannels.set(1);
  }

  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);''')


def shader(text):
    first = text.index('auto lighting_func(')
    last = text.index('\nstd::string build_shader_source(', first)
    lighting = text[first:last]
    lighting = once(lighting,
        'auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {',
        'auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha, bool vertexOnly = false) -> std::string {\n'
        '  const bool perPixel = UsePerPixelLighting && !vertexOnly;')
    # A color-source texgen always needs vertex lighting, including builds
    # where TEV's ordinary raster color is lit later in the fragment shader.
    lighting = lighting.replace('if (UsePerPixelLighting)', 'if (perPixel)')
    lighting = lighting.replace('UsePerPixelLighting ? "in.mv_nrm"sv', 'perPixel ? "in.mv_nrm"sv')
    lighting = once(lighting,
        '  if (perPixel) {\n    outVar = fmt::format("rast{}", i);',
        '  if (vertexOnly) {\n    outVar = fmt::format("texgen_color{}", i);\n    posVar = "mv_pos"sv;\n  } else if (perPixel) {\n    outVar = fmt::format("rast{}", i);')
    text = text[:first] + lighting + text[last:]
    text = once(text, '    // Output vertex color if necessary', '''    if (UsePerPixelLighting) {
      bool colorTexgen = false;
      for (size_t t = 0; t < info.sampledTexCoords.size(); ++t) {
        if (info.sampledTexCoords.test(t) && config.tcgs[t].src == GX_TG_COLOR0 + i) colorTexgen = true;
      }
      if (colorTexgen) {
        vtxXfrAttrs += fmt::format("\\n    var texgen_color{}: vec4f;", i);
        vtxXfrAttrs += lighting_func(config, cc, i, false, true);
        vtxXfrAttrs += lighting_func(config, cca, i, true, true);
      }
    }

    // Output vertex color if necessary''')
    text = once(text,
        'vtxXfrAttrs += fmt::format("\\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR0));',
        'vtxXfrAttrs += fmt::format("\\n    var tc{} = {};", i, UsePerPixelLighting ? "texgen_color0"sv : "out.cc0"sv);')
    text = once(text,
        'vtxXfrAttrs += fmt::format("\\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR1));',
        'vtxXfrAttrs += fmt::format("\\n    var tc{} = {};", i, UsePerPixelLighting ? "texgen_color1"sv : "out.cc1"sv);')
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--kind', choices=('shader', 'shader_info'), required=True)
    args = p.parse_args()
    result = globals()[args.kind](args.source.read_text(encoding='utf-8'))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if not args.out.exists() or args.out.read_text(encoding='utf-8') != result:
        args.out.write_text(result, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
