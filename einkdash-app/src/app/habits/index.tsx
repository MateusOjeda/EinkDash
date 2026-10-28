import Ionicons from "@expo/vector-icons/Ionicons";
import HabitIconImage from "@/components/HabitIconImage";
import {
	createHabit,
	type Habit,
	lastSevenDays,
	listenCompletedDays,
	listenHabits,
	localDateKey,
	reorderHabits,
	toggleCompletedDay,
} from "@/firebase/habits";
import { useRouter } from "expo-router";
import {
	useCallback,
	useEffect,
	useLayoutEffect,
	useMemo,
	useRef,
	useState,
} from "react";
import {
	ActivityIndicator,
	Alert,
	Animated,
	Easing,
	PanResponder,
	Pressable,
	ScrollView,
	StyleSheet,
	Text,
	TextInput,
	View,
} from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

const DAYS = ["Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"];
const GREEN = "#378454";

type HabitCardProps = {
	habit: Habit;
	dates: Date[];
	isDragging: boolean;
	dragInProgress: boolean;
	saveInProgress: boolean;
	shiftY: number;
	onOpen: () => void;
	onMeasure: (id: string, y: number, height: number) => void;
	onDragStart: () => void;
	onDragMove: (dy: number) => void;
	onDragFinish: (
		dy: number,
		settle: (destinationY: number, commit: () => void) => void,
	) => void;
	onDragCancel: () => void;
};

function HabitCard({
	habit,
	dates,
	isDragging,
	dragInProgress,
	saveInProgress,
	shiftY,
	onOpen,
	onMeasure,
	onDragStart,
	onDragMove,
	onDragFinish,
	onDragCancel,
}: HabitCardProps) {
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
			(error) => Alert.alert("Erro ao carregar registros", error.message),
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

	// O card arrastado acompanha o dedo; os outros abrem espaço suavemente.
	const translateY = useRef(new Animated.Value(0)).current;
	const neighborShift = useRef(new Animated.Value(0)).current;
	const combinedTranslateY = useMemo(
		() => Animated.add(translateY, neighborShift),
		[translateY, neighborShift],
	);
	const callbacks = useRef({
		onDragStart,
		onDragMove,
		onDragFinish,
		onDragCancel,
	});
	callbacks.current = { onDragStart, onDragMove, onDragFinish, onDragCancel };

	// Resetar transforms no mesmo commit em que a ordem visual muda,
	// evitando um frame em que as posições sejam aplicadas duas vezes.
	useLayoutEffect(() => {
		if (!isDragging) translateY.setValue(0);
	}, [isDragging, translateY]);

	useLayoutEffect(() => {
		neighborShift.stopAnimation();
		if (!dragInProgress) {
			neighborShift.setValue(0);
			return;
		}
		Animated.timing(neighborShift, {
			toValue: shiftY,
			duration: 170,
			easing: Easing.out(Easing.cubic),
			useNativeDriver: true,
		}).start();
	}, [shiftY, dragInProgress, neighborShift]);

	const panResponder = useMemo(
		() =>
			PanResponder.create({
				onStartShouldSetPanResponder: () => true,
				onMoveShouldSetPanResponder: () => true,
				onPanResponderGrant: () => {
					translateY.stopAnimation();
					translateY.setValue(0);
					callbacks.current.onDragStart();
				},
				onPanResponderMove: (_, gesture) => {
					translateY.setValue(gesture.dy);
					callbacks.current.onDragMove(gesture.dy);
				},
				onPanResponderRelease: (_, gesture) => {
					callbacks.current.onDragFinish(
						gesture.dy,
						(destinationY, commit) => {
							Animated.timing(translateY, {
								toValue: destinationY,
								duration: 160,
								easing: Easing.out(Easing.cubic),
								useNativeDriver: true,
							}).start(() => {
								// O novo layout ocupa a mesma posição visual dos cards animados.
								commit();
							});
						},
					);
				},
				onPanResponderTerminate: () => {
					Animated.timing(translateY, {
						toValue: 0,
						duration: 160,
						useNativeDriver: true,
					}).start(() => callbacks.current.onDragCancel());
				},
				onPanResponderTerminationRequest: () => false,
			}),
		[translateY],
	);

	return (
		<Animated.View
			onLayout={({ nativeEvent }) => {
				const { y, height } = nativeEvent.layout;
				onMeasure(habit.id, y, height);
			}}
			style={[
				styles.rowWrapper,
				{ transform: [{ translateY: combinedTranslateY }] },
				isDragging && styles.draggingWrapper,
			]}
		>
			<View style={[styles.card, isDragging && styles.cardDragging]}>
				<View style={styles.cardHeader}>
					<Pressable style={styles.habitTitleArea} onPress={onOpen}>
						<HabitIconImage icon={habit.icon} size={36} />
						<Text style={styles.habitName} numberOfLines={2}>
							{habit.name}
						</Text>
						<Ionicons
							name="chevron-forward"
							size={17}
							color="#858C96"
						/>
					</Pressable>

					<View
						{...panResponder.panHandlers}
						pointerEvents={saveInProgress ? "none" : "auto"}
						collapsable={false}
						accessible
						accessibilityLabel={`Arrastar ${habit.name} para reordenar`}
						style={styles.dragHandle}
					>
						<Ionicons
							name="reorder-three-outline"
							size={28}
							color={
								saveInProgress
									? "#65686F"
									: isDragging
										? "#FFFFFF"
										: "#A5A9B1"
							}
						/>
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
								disabled={pendingDay !== null || isDragging}
								accessibilityLabel={`${habit.name}: ${key}, ${done ? "realizado" : "não realizado"}`}
								style={styles.dayColumn}
							>
								<Text style={styles.dayLabel}>
									{DAYS[date.getDay()]}
								</Text>
								<View
									style={[
										styles.dayCell,
										done && styles.dayDone,
									]}
								>
									<Text
										style={[
											styles.dayNumber,
											done && { color: "#FFFFFF" },
										]}
									>
										{date.getDate()}
									</Text>
								</View>
							</Pressable>
						);
					})}
				</View>
			</View>
		</Animated.View>
	);
}

export default function HabitsScreen() {
	const router = useRouter();
	const [habits, setHabits] = useState<Habit[]>([]);
	const [name, setName] = useState("");
	const [loading, setLoading] = useState(true);
	const [saving, setSaving] = useState(false);
	const [showAddInput, setShowAddInput] = useState(false);
	const [draggingId, setDraggingId] = useState<string | null>(null);
	const [dragTargetIndex, setDragTargetIndex] = useState<number | null>(null);
	const targetIndexRef = useRef<number | null>(null);
	const [localOrderIds, setLocalOrderIds] = useState<string[] | null>(null);
	const [ordering, setOrdering] = useState(false);
	const orderingRef = useRef(false);
	const days = useMemo(() => lastSevenDays(), []);

	// Medidas reais dos cards, para não depender de uma altura fixa.
	const positions = useRef(new Map<string, { y: number; height: number }>());

	const visibleHabits = useMemo(() => {
		if (!localOrderIds) return habits;
		const order = new Map(localOrderIds.map((id, index) => [id, index]));
		return [...habits].sort(
			(a, b) =>
				(order.get(a.id) ?? Number.MAX_SAFE_INTEGER) -
				(order.get(b.id) ?? Number.MAX_SAFE_INTEGER),
		);
	}, [habits, localOrderIds]);

	useEffect(() => {
		return listenHabits(
			(items) => {
				setHabits(items);
				setLoading(false);
			},
			(error) => {
				setLoading(false);
				Alert.alert("Erro ao carregar hábitos", error.message);
			},
		);
	}, []);

	const measureCard = useCallback((id: string, y: number, height: number) => {
		positions.current.set(id, { y, height });
	}, []);

	// Posição-alvo calculada com os centros dos cards ainda nas posições originais.
	const findTarget = useCallback(
		(habitId: string, dy: number) => {
			const from = visibleHabits.findIndex(
				(habit) => habit.id === habitId,
			);
			const dragged = positions.current.get(habitId);
			if (from < 0 || !dragged) return from;

			const movedCenter = dragged.y + dragged.height / 2 + dy;
			let to = from;
			let smallestDistance = Infinity;
			visibleHabits.forEach((habit, index) => {
				const rect = positions.current.get(habit.id);
				if (!rect) return;
				const distance = Math.abs(
					rect.y + rect.height / 2 - movedCenter,
				);
				if (distance < smallestDistance) {
					smallestDistance = distance;
					to = index;
				}
			});
			return to;
		},
		[visibleHabits],
	);

	const moveDrag = useCallback(
		(habitId: string, dy: number) => {
			const index = findTarget(habitId, dy);
			if (index !== targetIndexRef.current) {
				targetIndexRef.current = index;
				setDragTargetIndex(index);
			}
		},
		[findTarget],
	);

	// Mantém a animação local independente da gravação no Firestore.
	// Enquanto salva, bloqueia um novo arraste para não criar conflitos de ordem.
	const saveOrder = useCallback(async (reordered: Habit[]) => {
		if (orderingRef.current) return;
		orderingRef.current = true;
		setOrdering(true);
		try {
			await reorderHabits(reordered);
			// Mantemos a ordem local: o listener pode enviar snapshots intermediários.
			// Ao reabrir a tela, a ordenação vem exclusivamente do Firestore.
		} catch (error) {
			setLocalOrderIds(null);
			Alert.alert("Erro ao salvar a ordem", String(error));
		} finally {
			orderingRef.current = false;
			setOrdering(false);
		}
	}, []);

	const finishDrag = useCallback(
		(
			habitId: string,
			dy: number,
			settle: (destinationY: number, commit: () => void) => void,
		) => {
			const from = visibleHabits.findIndex(
				(habit) => habit.id === habitId,
			);
			const to = findTarget(habitId, dy);
			const draggedRect = positions.current.get(habitId);
			const targetRect = visibleHabits[to]
				? positions.current.get(visibleHabits[to].id)
				: undefined;

			if (from < 0 || to < 0 || !draggedRect || !targetRect) {
				settle(0, () => {
					setDraggingId(null);
					setDragTargetIndex(null);
					targetIndexRef.current = null;
				});
				return;
			}

			// Para cards de alturas diferentes, a posição de destino também muda.
			const destinationY =
				from === to
					? 0
					: from < to
						? targetRect.y +
							targetRect.height -
							draggedRect.height -
							draggedRect.y
						: targetRect.y - draggedRect.y;

			settle(destinationY, () => {
				if (from !== to) {
					const reordered = [...visibleHabits];
					const [moved] = reordered.splice(from, 1);
					reordered.splice(to, 0, moved);
					setLocalOrderIds(reordered.map((habit) => habit.id));
					void saveOrder(reordered);
				}
				targetIndexRef.current = null;
				setDragTargetIndex(null);
				setDraggingId(null);
			});
		},
		[visibleHabits, findTarget, saveOrder],
	);

	// Os cards entre a origem e o destino deslizam para abrir espaço.
	// A margem de 12px faz parte da distância percorrida.
	const draggedIndex = draggingId
		? visibleHabits.findIndex((habit) => habit.id === draggingId)
		: -1;
	const draggedHeight = draggingId
		? (positions.current.get(draggingId)?.height ?? 0) + 12
		: 0;

	const getShift = (index: number) => {
		if (draggedIndex < 0 || dragTargetIndex === null || !draggedHeight)
			return 0;
		if (
			draggedIndex < dragTargetIndex &&
			index > draggedIndex &&
			index <= dragTargetIndex
		)
			return -draggedHeight;
		if (
			draggedIndex > dragTargetIndex &&
			index >= dragTargetIndex &&
			index < draggedIndex
		)
			return draggedHeight;
		return 0;
	};

	async function add() {
		if (!name.trim() || saving) return;
		setSaving(true);
		try {
			await createHabit(name, habits);
			setName("");
			setShowAddInput(false);
		} catch (error) {
			Alert.alert("Erro ao criar hábito", String(error));
		} finally {
			setSaving(false);
		}
	}

	return (
		<SafeAreaView style={styles.screen}>
			<View style={styles.topBar}>
				<Pressable
					onPress={() => router.back()}
					style={styles.backButton}
				>
					<Ionicons name="arrow-back" size={24} color="#FFFFFF" />
				</Pressable>
				<Text style={styles.title}>Hábitos</Text>
				<Pressable
					style={styles.addTrigger}
					onPress={() => {
						setShowAddInput((previous) => !previous);
						setName("");
					}}
					disabled={saving}
				>
					<Ionicons
						name={showAddInput ? "close-outline" : "add-outline"}
						size={24}
						color="#989BA3"
					/>
				</Pressable>
			</View>

			<ScrollView
				style={styles.scroll}
				contentContainerStyle={styles.content}
				scrollEnabled={draggingId === null}
				keyboardShouldPersistTaps="handled"
			>
				{showAddInput && (
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
							style={[
								styles.addButton,
								(!name.trim() || saving) && { opacity: 0.45 },
							]}
						>
							<Ionicons name="add" color="#FFFFFF" size={25} />
						</Pressable>
					</View>
				)}
				<Text style={styles.helper}>
					{ordering
						? "Salvando a nova ordem..."
						: "Arraste pela alça à direita para mudar a ordem dos hábitos."}
				</Text>

				{loading ? (
					<ActivityIndicator
						color="#8DD3AD"
						style={{ marginTop: 30 }}
					/>
				) : habits.length === 0 ? (
					<Text style={styles.empty}>
						Nenhum hábito cadastrado ainda.
					</Text>
				) : (
					visibleHabits.map((habit, index) => (
						<HabitCard
							key={habit.id}
							habit={habit}
							dates={days}
							isDragging={draggingId === habit.id}
							dragInProgress={draggingId !== null}
							saveInProgress={ordering}
							shiftY={getShift(index)}
							onOpen={() =>
								router.push({
									pathname: "/habits/[id]",
									params: { id: habit.id },
								})
							}
							onMeasure={measureCard}
							onDragStart={() => {
								targetIndexRef.current = null;
								setDragTargetIndex(null);
								setDraggingId(habit.id);
							}}
							onDragMove={(dy) => moveDrag(habit.id, dy)}
							onDragFinish={(dy, settle) =>
								finishDrag(habit.id, dy, settle)
							}
							onDragCancel={() => {
								targetIndexRef.current = null;
								setDragTargetIndex(null);
								setDraggingId(null);
							}}
						/>
					))
				)}
			</ScrollView>
		</SafeAreaView>
	);
}

const styles = StyleSheet.create({
	screen: { flex: 1, backgroundColor: "#101114" },
	topBar: {
		flexDirection: "row",
		alignItems: "center",
		paddingHorizontal: 16,
		paddingTop: 10,
		paddingBottom: 14,
		gap: 12,
	},
	backButton: { padding: 4 },
	title: { color: "#FFFFFF", fontWeight: "700", fontSize: 24 },
	addTrigger: { marginLeft: "auto", padding: 8 },
	scroll: { flex: 1 },
	content: { paddingHorizontal: 16, paddingBottom: 36 },
	addRow: { flexDirection: "row", gap: 10, marginBottom: 12 },
	input: {
		flex: 1,
		backgroundColor: "#202228",
		borderRadius: 12,
		color: "#FFFFFF",
		fontSize: 15,
		paddingHorizontal: 14,
		minHeight: 50,
	},
	addButton: {
		width: 50,
		height: 50,
		borderRadius: 12,
		backgroundColor: GREEN,
		alignItems: "center",
		justifyContent: "center",
	},
	helper: { color: "#8D9098", fontSize: 12, marginBottom: 22 },
	empty: { color: "#B8BAC1", marginTop: 24, textAlign: "center" },
	rowWrapper: { marginBottom: 12 },
	draggingWrapper: { zIndex: 10, elevation: 10 },
	card: {
		backgroundColor: "#1D1F24",
		borderWidth: 1,
		borderColor: "#2B2D33",
		borderRadius: 16,
		padding: 13,
	},
	cardDragging: { backgroundColor: "#2B3037", borderColor: "#6F9B84" },
	cardHeader: {
		flexDirection: "row",
		alignItems: "center",
		justifyContent: "space-between",
		marginBottom: 14,
	},
	habitTitleArea: {
		flex: 1,
		flexDirection: "row",
		alignItems: "center",
		gap: 9,
		paddingRight: 4,
	},
	habitName: {
		color: "#F2F2F2",
		fontSize: 15,
		fontWeight: "600",
		flexShrink: 1,
		flexGrow: 1,
	},
	dragHandle: {
		paddingHorizontal: 8,
		paddingVertical: 10,
		marginLeft: 4,
		alignItems: "center",
		justifyContent: "center",
	},
	weekRow: { flexDirection: "row", justifyContent: "space-between", gap: 4 },
	dayColumn: { flex: 1, alignItems: "center", gap: 6 },
	dayLabel: { fontSize: 11, color: "#9DA0A9" },
	dayCell: {
		width: "100%",
		maxWidth: 41,
		aspectRatio: 1,
		borderRadius: 10,
		backgroundColor: "#30333A",
		alignItems: "center",
		justifyContent: "center",
	},
	dayDone: { backgroundColor: GREEN },
	dayNumber: { color: "#C7C9CF", fontSize: 13 },
});
