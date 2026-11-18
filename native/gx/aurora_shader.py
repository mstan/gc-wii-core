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
    texture_input = '    CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");'
    if text.count(texture_input) != 2:
        raise ValueError('Pinned Aurora texture dependency source differs')
    # A disabled TEV texture contributes a constant without a sampler or
    # texcoord dependency, even when the combiner selects that texture input.
    text = text.replace(texture_input,
        '    if (stage.texMapId == GX_TEXMAP_NULL) break;\n' + texture_input)
    return text


def shader(text):
    text = once(text,
        '    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));',
        '''    // XF can read a texture source absent from the CP vertex layout.
    // Its default ST is zero; the texgen supplies the homogeneous components.
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));''')
    texture_check = '''    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", stageIdx);
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);'''
    if text.count(texture_check) != 3:
        raise ValueError('Pinned Aurora texture argument checks differ')
    text = text.replace(texture_check,
        '''    CHECK(stage.texMapId == GX_TEXMAP_NULL ||
              (stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7),
          "invalid texture {} for stage {}", underlying(stage.texMapId), stageIdx);''')
    text = once(text,
        '    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", i);',
        '''    // Disabled texturing supplies a constant to the TEV combiner. Keep this
    // after indirect-coordinate processing, which may feed later stages.
    if (stage.texMapId == GX_TEXMAP_NULL) {
      // Match the functional no-texgen case in Dolphin's PixelShaderGen.
      fragmentFnPre += fmt::format("\\n    var sampled{} = vec4f({});", i, config.pad2 ? "1.0" : "0.0");
      continue;
    }''')
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
