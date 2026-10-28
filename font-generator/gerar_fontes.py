#!/usr/bin/env python3
"""Converte uma fonte TTF/OTF local em fontes QFNT 2-bpp com cobertura de 4 níveis.

Dependência: pip install Pillow
Uso: python gerar_fontes.py caminho/MinhaFonte.ttf --sizes 24 32 48 60 --out ./fonts --preview
"""
from __future__ import annotations
import argparse
import math
from pathlib import Path
import re
import struct
from PIL import Image, ImageDraw, ImageFont

# Layout do formato QFNT v1. Todos os valores little-endian, sem padding.
HEADER = struct.Struct('<4s6H4I')  # 32 bytes
GLYPH = struct.Struct('<IhhHHhHII') # 24 bytes
FLAG_COVERAGE_2BPP = 1

# ASCII imprimível + Latin-1 + sinais de pontuação/símbolos comuns.
CODEPOINTS = sorted(set(range(0x20, 0x7F)) |
                    set(range(0xA0, 0x100)) |
                    {0x20AC, 0x2013, 0x2014, 0x2018, 0x2019,
                     0x201C, 0x201D, 0x2022, 0x2026, 0x00BA, 0x00AA})


def pack_coverage_2bpp(image: Image.Image) -> bytes:
    """Cada pixel recebe cobertura 0 (transparente) a 3 (opaco).
    Quatro pixels/byte, do bit mais significativo ao menos significativo.
    """
    out = bytearray((image.width * image.height + 3) // 4)
    pixels = image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata()
    for i, intensity in enumerate(pixels):
        cov = (intensity * 3 + 127) // 255
        out[i // 4] |= cov << (6 - 2 * (i % 4))
    return bytes(out)


def glyph_for(font: ImageFont.FreeTypeFont, ch: str):
    # Offsets relativos à origem horizontal e à LINHA DE BASE (baseline).
    x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
    width = max(1, x1 - x0)
    height = max(1, y1 - y0)
    image = Image.new('L', (width, height), 0)
    ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
    ink_bounds = image.getbbox()
    advance = round(font.getlength(ch))
    if ink_bounds is None:
        return 0, 0, 0, 0, advance, b''
    crop = image.crop(ink_bounds)
    return (x0 + ink_bounds[0], y0 + ink_bounds[1], crop.width,
            crop.height, advance, pack_coverage_2bpp(crop))


def write_qfont(source: Path, size: int, target: Path, chars: list[int]):
    font = ImageFont.truetype(str(source), size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for cp in chars:
        xoff, yoff, w, h, advance, packed = glyph_for(font, chr(cp))
        if not (-32768 <= xoff <= 32767 and -32768 <= yoff <= 32767
                and -32768 <= advance <= 32767):
            raise ValueError('Métricas fora dos limites do formato.')
        glyphs.append((cp, xoff, yoff, w, h, advance, packed))
    line_height = ascent + descent
    index_offset = HEADER.size
    data_offset = index_offset + GLYPH.size * len(glyphs)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open('wb') as out:
        out.write(HEADER.pack(b'QFNT', 1, HEADER.size, size, ascent,
                              descent, line_height, len(glyphs),
                              index_offset, data_offset, FLAG_COVERAGE_2BPP))
        cursor = data_offset
        for cp, xo, yo, w, h, advance, packed in glyphs:
            out.write(GLYPH.pack(cp, xo, yo, w, h, advance, 0, cursor,
                                 len(packed)))
            cursor += len(packed)
        for _, _, _, _, _, _, packed in glyphs:
            out.write(packed)
    validate_font(target)
    return target.stat().st_size


def validate_font(path: Path):
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError('Arquivo muito pequeno')
    magic, version, hsize, px, asc, desc, lh, count, idx, data, flags = HEADER.unpack_from(raw)
    if (magic, version, hsize, flags) != (b'QFNT', 1, 32, FLAG_COVERAGE_2BPP):
        raise ValueError('Cabeçalho inválido')
    assert 0 < px and lh == asc + desc
    assert idx == HEADER.size and data == idx + GLYPH.size * count
    last_cp = -1
    for i in range(count):
        cp, xo, yo, w, h, adv, reserved, offset, nbytes = GLYPH.unpack_from(raw, idx + i * GLYPH.size)
        assert cp > last_cp and nbytes == (w*h + 3)//4 and offset >= data
        assert offset + nbytes <= len(raw) and reserved == 0
        last_cp = cp


def load_qfont(path: Path):
    raw = path.read_bytes()
    validate_font(path)
    h = HEADER.unpack_from(raw)
    records = {}
    for i in range(h[7]):
        rec = GLYPH.unpack_from(raw, h[8] + i * GLYPH.size)
        records[rec[0]] = rec
    return {'data': raw, 'line_height': h[6], 'records': records}


def draw_qfont(image: Image.Image, font, xy: tuple[int, int], text: str, foreground=0):
    """Exemplo lógico de renderização QFNT em 4 cinzas (0 preto, 3 branco).
    xy informa (x, BASELINE). Usa apenas os quatro valores de cinza.
    """
    x_start, baseline = xy
    x = x_start
    pixels = image.load()
    raw = font['data']
    for ch in text:
        if ch == '\n':
            x = x_start
            baseline += font['line_height']
            continue
        cp = ord(ch)
        rec = font['records'].get(cp) or font['records'].get(ord('?'))
        if not rec:
            continue
        _, xo, yo, w, h, advance, _, offset, nbytes = rec
        for p in range(w*h):
            coverage = (raw[offset + p//4] >> (6 - 2 * (p%4))) & 3
            if coverage == 0:
                continue
            xx = x + xo + (p % w)
            yy = baseline + yo + (p // w)
            if 0 <= xx < image.width and 0 <= yy < image.height:
                bg = pixels[xx, yy]
                pixels[xx, yy] = (bg*(3-coverage) + foreground*coverage + 1)//3
        x += advance
    return x


def make_preview(paths: dict[int, Path], filename: Path):
    image = Image.new('L', (800, 480), 3)
    samples = [
        (60, 92, 'Quadro E-ink'),
        (48, 152, 'SEXTA-FEIRA, 16 OUTUBRO'),
        (32, 220, 'Meditação  •  há 3 dias'),
        (32, 270, 'Trocar a toalha  —  hoje'),
        (24, 330, 'Próxima atualização: 20:00'),
        (24, 365, 'ÁÉÍÓÚ  ãõç  €  “aspas”'),
    ]
    for size, y, line in samples:
        nearest_size = min(paths, key=lambda n: abs(n-size))
        draw_qfont(image, load_qfont(paths[nearest_size]), (24, y), line)
    palette = [0, 85, 170, 255]
    image.point(palette + [255] * 252).save(filename)


def main():
    parser = argparse.ArgumentParser(description='Gerador QFNT 4 tons para Quadro E-ink')
    parser.add_argument('ttf', type=Path, help='caminho de arquivo TTF ou OTF que você possui')
    parser.add_argument('--sizes', nargs='+', type=int, default=[24, 32, 48, 60], help='tamanhos em pixels')
    parser.add_argument('--out', type=Path, default=Path('fonts'))
    parser.add_argument('--name', type=str, default=None, help='nome base (padrão: nome do TTF)')
    parser.add_argument('--extra', type=str, default='', help='caracteres adicionais')
    parser.add_argument('--preview', action='store_true', help='gera preview_800x480.png')
    args = parser.parse_args()
    if not args.ttf.is_file():
        parser.error(f'Fonte não encontrada: {args.ttf}')
    basename = re.sub(r'[^A-Za-z0-9_-]', '_', args.name or args.ttf.stem)
    chars = sorted(set(CODEPOINTS) | {ord(c) for c in args.extra})
    outpaths = {}
    for size in args.sizes:
        if not 4 <= size <= 255:
            parser.error('Use tamanhos entre 4 e 255 pixels')
        path = args.out / f'{basename}_{size}.bin'
        num_bytes = write_qfont(args.ttf, size, path, chars)
        outpaths[size] = path
        print(f'OK {path}  ({num_bytes:,} bytes; {len(chars)} caracteres)')
    if args.preview:
        preview_path = args.out / 'preview_800x480.png'
        make_preview(outpaths, preview_path)
        print(f'Preview: {preview_path}')


if __name__ == '__main__':
    main()
