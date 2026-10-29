#!/usr/bin/env python3
"""Empacota PNGs 4 tons (0,85,170,255) em data/icons.bin.
Formato WIC2 v1: header 16 bytes, indice 20 bytes/icone e pixels 2bpp MSB.
Nao requer dependencias alem de Pillow. Rode na raiz deste kit.
"""
from pathlib import Path
import json
import struct
import zlib
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'icons.json'
OUT = ROOT / 'data' / 'icons.bin'
PREVIEW = ROOT / 'icons_preview.png'
MAGIC = b'WIC2'
VERSION = 1
HEADER = struct.Struct('<4sHHII')   # magic, versao, total, indice_offset, dados_offset
ENTRY = struct.Struct('<IHHIII')     # id, largura, altura, offset, tamanho, crc32
PALETTE = (0, 85, 170, 255)       # 00 preto; 01 cinza escuro; 10 claro; 11 branco


def convert_png(path, w, h):
    with Image.open(path) as original:
        rgba = original.convert('RGBA')
        if rgba.size != (w, h):
            raise ValueError(f'{path.name}: esperado {w}x{h}, recebido {rgba.width}x{rgba.height}. '
                             f'Exporte os PNGs no tamanho correto; nao redimensionamos silenciosamente.')
        background = Image.new('RGBA', rgba.size, (255, 255, 255, 255))
        background.alpha_composite(rgba)
        rgb = background.convert('RGB')
        shades = bytearray(w * h)
        for i, (r, g, b) in enumerate(rgb.getdata()):
            # luminosidade perceptual aproximada, quantizada no tom mais proximo
            gray = (299 * r + 587 * g + 114 * b + 500) // 1000
            shades[i] = min(3, (gray + 42) // 85)
        stride = (w + 3) // 4
        packed = bytearray(stride * h)
        for y in range(h):
            for x in range(w):
                packed[y * stride + (x >> 2)] |= shades[y * w + x] << (6 - 2 * (x & 3))
        preview = Image.frombytes('L', (w, h), bytes(PALETTE[v] for v in shades))
        return bytes(packed), preview


def main():
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    entries = manifest['icons']
    if not entries:
        raise ValueError('Nenhum icone em icons.json')
    ids = set()
    prepared = []
    for e in entries:
        icon_id = int(e['id'])
        w, h = int(e['width']), int(e['height'])
        name = e['file']
        if icon_id in ids:
            raise ValueError(f'ID duplicado: {icon_id}')
        if not (1 <= icon_id <= 0xFFFFFFFF and 1 <= w <= 128 and 1 <= h <= 112):
            raise ValueError(f'Entrada invalida: {e}')
        ids.add(icon_id)
        png = ROOT / 'icons' / name
        if not png.is_file():
            raise FileNotFoundError(f'PNG nao encontrado: {png}')
        packed, preview = convert_png(png, w, h)
        prepared.append((icon_id, w, h, packed, preview, name))

    data_offset = HEADER.size + len(prepared) * ENTRY.size
    payload = bytearray()
    index = bytearray()
    for icon_id, w, h, packed, preview, name in prepared:
        offset = data_offset + len(payload)
        crc = zlib.crc32(packed) & 0xFFFFFFFF
        index.extend(ENTRY.pack(icon_id, w, h, offset, len(packed), crc))
        payload.extend(packed)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(HEADER.pack(MAGIC, VERSION, len(prepared), HEADER.size, data_offset) + index + payload)

    # Autovalidacao: verifica cada registro do BIN, CRC e a decodificacao dos tons
    blob = OUT.read_bytes()
    magic, version, n, index_offset, pixel_offset = HEADER.unpack_from(blob)
    assert (magic, version, n, index_offset, pixel_offset) == (MAGIC, VERSION, len(prepared), 16, data_offset)
    for k, (icon_id, w, h, packed, preview, name) in enumerate(prepared):
        rid, rw, rh, off, sz, crc = ENTRY.unpack_from(blob, HEADER.size + k * ENTRY.size)
        assert (rid, rw, rh) == (icon_id, w, h)
        assert sz == ((w + 3) // 4) * h
        assert blob[off:off+sz] == packed
        assert (zlib.crc32(blob[off:off+sz]) & 0xFFFFFFFF) == crc

    # Prancha apenas para conferir visualmente, nao faz parte do BIN
    from PIL import ImageDraw, ImageFont
    cols = 6
    cell_w, cell_h = 174, 164
    rows = (len(prepared) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * cell_w, rows * cell_h + 38), (240, 240, 240))
    draw = ImageDraw.Draw(sheet)
    draw.text((12, 8), 'EinkDash - Weather Icons | 4 tons | WIC2', fill=(0, 0, 0))
    for i, (icon_id, w, h, _, preview, name) in enumerate(prepared):
        x0, y0 = (i % cols) * cell_w, 38 + (i // cols) * cell_h
        draw.rounded_rectangle((x0+4, y0+4, x0+cell_w-5, y0+cell_h-5), radius=10, fill='white')
        # usa escala inteira para manter os quatro tons
        fitted = preview.copy()
        if w == 48:
            fitted = fitted.resize((96, 96), Image.Resampling.NEAREST)
        sheet.paste(fitted.convert('RGB'), (x0 + (cell_w-fitted.width)//2, y0 + 12))
        draw.text((x0+10, y0+128), f'ID {icon_id}: {name[:-4][:22]}', fill=(0, 0, 0))
    sheet.save(PREVIEW)
    print(f'OK: {OUT} ({len(blob)} bytes, {len(prepared)} icones)')
    print(f'Previa: {PREVIEW}')


if __name__ == '__main__':
    main()
