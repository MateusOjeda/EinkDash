import * as ImagePicker from "expo-image-picker";
import { manipulateAsync, SaveFormat } from "expo-image-manipulator";
import { fromByteArray, toByteArray } from "base64-js";
import * as UPNG from "upng-js";
import type { HabitIcon } from "./habits";

export const HABIT_ICON_SIZE = 48;
export type DitherMode = "none" | "floyd-steinberg";

// Mesma codificação utilizada no conversor HTML: 4 pixels por byte,
// 0 = preto, 1 = cinza-escuro, 2 = cinza-claro, 3 = branco.
// Pixel 0 nos bits 7-6, pixel 1 nos bits 5-4 e assim por diante.
export async function convertHabitIcon(
  imageUri: string,
  mode: DitherMode = "none"
): Promise<HabitIcon> {
  // O picker já entrega o recorte quadrado, então reduzimos a imagem para 48x48.
  const resized = await manipulateAsync(
    imageUri,
    [{ resize: { width: HABIT_ICON_SIZE, height: HABIT_ICON_SIZE } }],
    { format: SaveFormat.PNG, base64: true }
  );
  if (!resized.base64) throw new Error("Não foi possível obter os pixels da imagem.");

  const image = UPNG.decode(toByteArray(resized.base64).buffer as ArrayBuffer);
  if (image.width !== HABIT_ICON_SIZE || image.height !== HABIT_ICON_SIZE) {
    throw new Error("A imagem gerada não tem 48×48 pixels.");
  }

  const rgba = new Uint8Array(UPNG.toRGBA8(image)[0]);
  const count = HABIT_ICON_SIZE * HABIT_ICON_SIZE;
  const lum = new Float32Array(count);
  const packed = new Uint8Array(count / 4);

  for (let i = 0; i < count; i++) {
    const p = i * 4;
    const alpha = rgba[p + 3] / 255;
    // Mescla transparência sobre fundo branco.
    const r = rgba[p] * alpha + 255 * (1 - alpha);
    const g = rgba[p + 1] * alpha + 255 * (1 - alpha);
    const b = rgba[p + 2] * alpha + 255 * (1 - alpha);
    lum[i] = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  }

  for (let y = 0; y < HABIT_ICON_SIZE; y++) {
    for (let x = 0; x < HABIT_ICON_SIZE; x++) {
      const i = y * HABIT_ICON_SIZE + x;
      const gray = Math.max(0, Math.min(255, lum[i]));
      const code = Math.round(gray / 85); // 0..3
      packed[i >> 2] |= code << (6 - 2 * (i & 3));

      if (mode === "floyd-steinberg") {
        const error = gray - code * 85;
        if (x + 1 < HABIT_ICON_SIZE) lum[i + 1] += error * 7 / 16;
        if (y + 1 < HABIT_ICON_SIZE) {
          if (x > 0) lum[i + HABIT_ICON_SIZE - 1] += error * 3 / 16;
          lum[i + HABIT_ICON_SIZE] += error * 5 / 16;
          if (x + 1 < HABIT_ICON_SIZE) lum[i + HABIT_ICON_SIZE + 1] += error / 16;
        }
      }
    }
  }

  return {
    width: HABIT_ICON_SIZE,
    height: HABIT_ICON_SIZE,
    format: "gray2bpp",
    data: fromByteArray(packed),
  };
}

export async function pickHabitIconSource(): Promise<string | null> {
  const result = await ImagePicker.launchImageLibraryAsync({
    mediaTypes: ["images"],
    allowsEditing: true,
    aspect: [1, 1],
    quality: 1,
  });
  if (result.canceled || !result.assets.length) return null;
  return result.assets[0].uri;
}

// Gera uma prévia PNG fiel aos 4 tons, somente em memória.
// O Firestore armazena apenas o binário 2bpp em Base64; NÃO o PNG.
export function habitIconPreviewUri(icon: HabitIcon): string {
  if (icon.format !== "gray2bpp") throw new Error("Formato de ícone inválido.");
  const { width, height, data } = icon;
  const packed = toByteArray(data);
  const count = width * height;
  if (count % 4 !== 0 || packed.length !== count / 4) {
    throw new Error("Tamanho do ícone inválido.");
  }
  const rgba = new Uint8Array(count * 4);
  for (let i = 0; i < count; i++) {
    const tone = (packed[i >> 2] >> (6 - 2 * (i & 3))) & 3;
    const val = tone * 85;
    const p = i * 4;
    rgba[p] = val;
    rgba[p + 1] = val;
    rgba[p + 2] = val;
    rgba[p + 3] = 255;
  }
  const png = new Uint8Array(UPNG.encode([rgba.buffer as ArrayBuffer], width, height, 0));
  return `data:image/png;base64,${fromByteArray(png)}`;
}
