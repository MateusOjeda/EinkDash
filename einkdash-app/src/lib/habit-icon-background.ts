import { File, Paths } from "expo-file-system";
import * as UPNG from "upng-js";

const SIZE = 48;
const BACKGROUND = 192; // #C0C0C0: cinza claro dos cards do E-Ink

function isPng(bytes: Uint8Array): boolean {
  return (
    bytes.length >= 8 &&
    bytes[0] === 137 && bytes[1] === 80 &&
    bytes[2] === 78 && bytes[3] === 71 &&
    bytes[4] === 13 && bytes[5] === 10 &&
    bytes[6] === 26 && bytes[7] === 10
  );
}

function asArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  // Preserva byteOffset, mesmo se o Uint8Array for uma view.
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

/**
 * Preserva o canal alpha original e gera um PNG opaco de 48x48.
 *
 * - Primeiro le o PNG ORIGINAL (sem passar pelo resize/recorte do sistema).
 * - Depois aplica o fundo cinza apenas em pixels com alpha < 255.
 * - Por fim reduz para 48x48 com recorte central, para uso em gray2bpp.
 *
 * O retorno pode ser enviado ao convertHabitIcon(preparedUri, ditherMode)
 * existente; o protocolo do Firebase e do ESP32 nao muda.
 */
export async function prepareHabitIconSource(
  inputBytes: Uint8Array,
  sourceName?: string
): Promise<string> {
  // Recebe os bytes diretos do File.pickFileAsync, sem copiar ou reencodar.
  if (!isPng(inputBytes)) {
    throw new Error(
      `O arquivo recebido nao e PNG: ${sourceName ?? "sem nome"}. ` +
      "Selecione o PNG original em Arquivos/Downloads, nao uma copia JPEG da galeria."
    );
  }

  const pngBytes = inputBytes;
  const png = UPNG.decode(asArrayBuffer(pngBytes));
  const frames = UPNG.toRGBA8(png);
  if (frames.length === 0) {
    throw new Error("A imagem nao contem pixels decodificaveis.");
  }

  const source = new Uint8Array(frames[0]);
  const w = png.width;
  const h = png.height;
  if (w < 1 || h < 1 || source.byteLength !== w * h * 4) {
    throw new Error("Dimensoes RGBA invalidas.");
  }

  // Aplica o fundo sobre pixels *transparentes*, sem modificar branco opaco.
  // Fazer antes do redimensionamento evita halos brancos nas bordas.
  const rgb = new Uint8Array(w * h * 3);
  let alphaPixels = 0;
  for (let p = 0, j = 0; p < source.length; p += 4, j += 3) {
    const alpha = source[p + 3];
    if (alpha !== 255) alphaPixels++;
    const inv = 255 - alpha;
    rgb[j]     = Math.round((source[p]     * alpha + BACKGROUND * inv) / 255);
    rgb[j + 1] = Math.round((source[p + 1] * alpha + BACKGROUND * inv) / 255);
    rgb[j + 2] = Math.round((source[p + 2] * alpha + BACKGROUND * inv) / 255);
  }

  console.log(
    `HABIT ICON: origem ${w}x${h}; pixels com alpha < 255: ${alphaPixels}; PNG original: true; arquivo: ${sourceName ?? "sem nome"}`
  );
  if (isPng(inputBytes) && alphaPixels === 0) {
    console.warn("HABIT ICON: PNG selecionado nao possui pixels transparentes. Confira se e o arquivo original.");
  }

  const out = new Uint8Array(SIZE * SIZE * 4);
  const square = Math.min(w, h);
  const xOffset = (w - square) / 2;
  const yOffset = (h - square) / 2;

  // Resize bilinear depois da composicao. Recorte central quadrado.
  // Os icones fornecidos (512x512) nao sofrem corte.
  for (let y = 0; y < SIZE; y++) {
    const fy = Math.max(0, Math.min(h - 1, yOffset + (y + 0.5) * square / SIZE - 0.5));
    const y0 = Math.floor(fy);
    const y1 = Math.min(y0 + 1, h - 1);
    const wy = fy - y0;

    for (let x = 0; x < SIZE; x++) {
      const fx = Math.max(0, Math.min(w - 1, xOffset + (x + 0.5) * square / SIZE - 0.5));
      const x0 = Math.floor(fx);
      const x1 = Math.min(x0 + 1, w - 1);
      const wx = fx - x0;
      const outIndex = (y * SIZE + x) * 4;

      for (let c = 0; c < 3; c++) {
        const v00 = rgb[(y0 * w + x0) * 3 + c];
        const v10 = rgb[(y0 * w + x1) * 3 + c];
        const v01 = rgb[(y1 * w + x0) * 3 + c];
        const v11 = rgb[(y1 * w + x1) * 3 + c];
        const top = v00 * (1 - wx) + v10 * wx;
        const bottom = v01 * (1 - wx) + v11 * wx;
        out[outIndex + c] = Math.round(top * (1 - wy) + bottom * wy);
      }
      out[outIndex + 3] = 255;
    }
  }

  const encoded = UPNG.encode([out.buffer as ArrayBuffer], SIZE, SIZE, 0);
  const filename = `habit-opaque-${Date.now()}-${Math.random().toString(36).slice(2)}.png`;
  const file = new File(Paths.cache, filename);
  await file.write(new Uint8Array(encoded));
  return file.uri;
}
