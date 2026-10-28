import Ionicons from "@expo/vector-icons/Ionicons";
import { useMemo } from "react";
import { Image, View } from "react-native";
import { habitIconPreviewUri } from "../lib/habit-icons";
import type { HabitIcon } from "../lib/habits";

type Props = { icon?: HabitIcon | null; size?: number };

export default function HabitIconImage({ icon, size = 36 }: Props) {
  const preview = useMemo(() => {
    if (!icon) return null;
    try { return habitIconPreviewUri(icon); }
    catch (error) { console.warn("Ícone inválido:", error); return null; }
  }, [icon?.data, icon?.width, icon?.height]);

  if (preview) {
    return (
      <Image
        source={{ uri: preview }}
        resizeMode="contain"
        style={{ width: size, height: size, borderRadius: 8, backgroundColor: "#FFFFFF" }}
      />
    );
  }

  return (
    <View style={{
      width: size, height: size, borderRadius: 8,
      backgroundColor: "#30333A", alignItems: "center", justifyContent: "center",
    }}>
      <Ionicons name="image-outline" size={Math.round(size * 0.56)} color="#858C96" />
    </View>
  );
}
