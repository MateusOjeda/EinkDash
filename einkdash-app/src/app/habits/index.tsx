import Ionicons from "@expo/vector-icons/Ionicons";
import { useRouter } from "expo-router";
import { useEffect, useMemo, useState } from "react";
import {
  ActivityIndicator,
  Alert,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";
import HabitIconImage from "../../components/HabitIconImage";
import {
  createHabit,
  Habit,
  lastSevenDays,
  listenCompletedDays,
  listenHabits,
  localDateKey,
  swapHabits,
  toggleCompletedDay,
} from "../../lib/habits";

const DAYS = ["Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"];
const GREEN = "#378454";

function HabitCard({
  habit,
  dates,
  onOpen,
  onMoveUp,
  onMoveDown,
  first,
  last,
}: {
  habit: Habit;
  dates: Date[];
  onOpen: () => void;
  onMoveUp: () => void;
  onMoveDown: () => void;
  first: boolean;
  last: boolean;
}) {
  const [completed, setCompleted] = useState<Set<string>>(new Set());
  const [pendingDay, setPendingDay] = useState<string | null>(null);

  const start = localDateKey(dates[0]);
  const end = localDateKey(dates[6]);

  useEffect(() => {
    return listenCompletedDays(
      habit.id,
      start,
      end,
      setCompleted,
      (error) => Alert.alert("Erro ao carregar registros", error.message)
    );
  }, [habit.id, start, end]);

  async function toggle(day: string) {
    if (pendingDay) return;
    setPendingDay(day);
    try {
      await toggleCompletedDay(habit.id, day, completed.has(day));
    } catch (error) {
      Alert.alert("Erro ao salvar", String(error));
    } finally {
      setPendingDay(null);
    }
  }

  return (
    <View style={styles.card}>
      <View style={styles.cardHeader}>
        <Pressable style={styles.habitTitleArea} onPress={onOpen}>
          <HabitIconImage icon={habit.icon} size={36} />
          <Text style={styles.habitName} numberOfLines={2}>{habit.name}</Text>
          <Ionicons name="chevron-forward" size={17} color="#858C96" />
        </Pressable>
        <View style={styles.reorderActions}>
          <Pressable
            accessibilityLabel={`Mover ${habit.name} para cima`}
            disabled={first}
            onPress={onMoveUp}
            style={styles.orderButton}
          >
            <Ionicons name="chevron-up" color={first ? "#44474C" : "#D6D8DC"} size={21} />
          </Pressable>
          <Pressable
            accessibilityLabel={`Mover ${habit.name} para baixo`}
            disabled={last}
            onPress={onMoveDown}
            style={styles.orderButton}
          >
            <Ionicons name="chevron-down" color={last ? "#44474C" : "#D6D8DC"} size={21} />
          </Pressable>
        </View>
      </View>
      <View style={styles.weekRow}>
        {dates.map((date) => {
          const key = localDateKey(date);
          const done = completed.has(key);
          return (
            <Pressable
              key={key}
              onPress={() => toggle(key)}
              disabled={pendingDay !== null}
              accessibilityLabel={`${habit.name}: ${key}, ${done ? "realizado" : "não realizado"}`}
              style={styles.dayColumn}
            >
              <Text style={styles.dayLabel}>{DAYS[date.getDay()]}</Text>
              <View style={[styles.dayCell, done && styles.dayDone]}>
                {done ? (
                  <Ionicons name="checkmark" color="#FFFFFF" size={19} />
                ) : (
                  <Text style={styles.dayNumber}>{date.getDate()}</Text>
                )}
              </View>
            </Pressable>
          );
        })}
      </View>
    </View>
  );
}

export default function HabitsScreen() {
  const router = useRouter();
  const [habits, setHabits] = useState<Habit[]>([]);
  const [name, setName] = useState("");
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [ordering, setOrdering] = useState(false);
  const days = useMemo(() => lastSevenDays(), []);

  useEffect(() => {
    return listenHabits(
      (items) => { setHabits(items); setLoading(false); },
      (error) => { setLoading(false); Alert.alert("Erro ao carregar hábitos", error.message); }
    );
  }, []);

  async function add() {
    if (!name.trim() || saving) return;
    setSaving(true);
    try {
      await createHabit(name, habits);
      setName("");
    } catch (error) {
      Alert.alert("Erro ao criar hábito", String(error));
    } finally {
      setSaving(false);
    }
  }

  async function move(index: number, offset: number) {
    const other = habits[index + offset];
    if (!other || ordering) return;
    setOrdering(true);
    try {
      await swapHabits(habits[index], other);
    } catch (error) {
      Alert.alert("Erro ao ordenar", String(error));
    } finally {
      setOrdering(false);
    }
  }

  return (
    <SafeAreaView style={styles.screen}>
      <View style={styles.topBar}>
        <Pressable onPress={() => router.back()} style={styles.backButton}>
          <Ionicons name="arrow-back" size={24} color="#FFFFFF" />
        </Pressable>
        <Text style={styles.title}>Hábitos</Text>
      </View>
      <ScrollView contentContainerStyle={styles.content} keyboardShouldPersistTaps="handled">
        <Text style={styles.subheading}>Novo hábito</Text>
        <View style={styles.addRow}>
          <TextInput
            style={styles.input}
            placeholder="Ex.: Trocar toalhas"
            placeholderTextColor="#81858D"
            value={name}
            onChangeText={setName}
            onSubmitEditing={add}
            returnKeyType="done"
            maxLength={80}
          />
          <Pressable
            onPress={add}
            disabled={!name.trim() || saving}
            style={[styles.addButton, (!name.trim() || saving) && { opacity: 0.45 }]}
          >
            <Ionicons name="add" color="#FFFFFF" size={25} />
          </Pressable>
        </View>
        <Text style={styles.helper}>Use as setas para definir a ordem dos hábitos no display.</Text>
        {loading ? (
          <ActivityIndicator color="#8DD3AD" style={{ marginTop: 30 }} />
        ) : habits.length === 0 ? (
          <Text style={styles.empty}>Nenhum hábito cadastrado ainda.</Text>
        ) : (
          habits.map((habit, index) => (
            <HabitCard
              key={habit.id}
              habit={habit}
              dates={days}
              first={index === 0}
              last={index === habits.length - 1}
              onOpen={() => router.push({ pathname: "/habits/[id]", params: { id: habit.id } })}
              onMoveUp={() => move(index, -1)}
              onMoveDown={() => move(index, 1)}
            />
          ))
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: "#101114" },
  topBar: { flexDirection: "row", alignItems: "center", paddingHorizontal: 16, paddingTop: 10, paddingBottom: 14, gap: 12 },
  backButton: { padding: 4 },
  title: { color: "#FFFFFF", fontWeight: "700", fontSize: 24 },
  content: { paddingHorizontal: 16, paddingBottom: 36 },
  subheading: { color: "#F1F2F4", fontWeight: "600", fontSize: 16, marginBottom: 12 },
  addRow: { flexDirection: "row", gap: 10, marginBottom: 12 },
  input: { flex: 1, backgroundColor: "#202228", borderRadius: 12, color: "#FFFFFF", fontSize: 15, paddingHorizontal: 14, minHeight: 50 },
  addButton: { width: 50, height: 50, borderRadius: 12, backgroundColor: GREEN, alignItems: "center", justifyContent: "center" },
  helper: { color: "#8D9098", fontSize: 12, marginBottom: 22 },
  empty: { color: "#B8BAC1", marginTop: 24, textAlign: "center" },
  card: { backgroundColor: "#1D1F24", borderWidth: 1, borderColor: "#2B2D33", borderRadius: 16, padding: 13, marginBottom: 12 },
  cardHeader: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", marginBottom: 14 },
  habitTitleArea: { flex: 1, flexDirection: "row", alignItems: "center", gap: 9, paddingRight: 4 },
  iconPlaceholder: { width: 36, height: 36, borderRadius: 8, alignItems: "center", justifyContent: "center", backgroundColor: "#30333A" },
  habitName: { color: "#F2F2F2", fontSize: 15, fontWeight: "600", flexShrink: 1, flexGrow: 1 },
  reorderActions: { flexDirection: "row", paddingLeft: 4 },
  orderButton: { paddingHorizontal: 4, paddingVertical: 7 },
  weekRow: { flexDirection: "row", justifyContent: "space-between", gap: 4 },
  dayColumn: { flex: 1, alignItems: "center", gap: 6 },
  dayLabel: { fontSize: 11, color: "#9DA0A9" },
  dayCell: { width: "100%", maxWidth: 41, aspectRatio: 1, borderRadius: 10, backgroundColor: "#30333A", alignItems: "center", justifyContent: "center" },
  dayDone: { backgroundColor: GREEN },
  dayNumber: { color: "#C7C9CF", fontSize: 13 },
});
