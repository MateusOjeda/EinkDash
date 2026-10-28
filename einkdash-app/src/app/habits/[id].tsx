import Ionicons from "@expo/vector-icons/Ionicons";
import { File } from "expo-file-system";
import { useLocalSearchParams, useRouter } from "expo-router";
import { useEffect, useMemo, useState } from "react";
import {
	Alert,
	ActivityIndicator,
	Pressable,
	ScrollView,
	StyleSheet,
	Text,
	TextInput,
	View,
} from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";
import HabitIconImage from "@/components/HabitIconImage";
import { prepareHabitIconSource } from "../../lib/habit-icon-background";
import { convertHabitIcon, DitherMode } from "../../lib/habit-icons";
import {
	deleteHabitWithHistory,
	Habit,
	HabitIcon,
	listenCompletedDays,
	listenHabit,
	localDateKey,
	renameHabit,
	toggleCompletedDay,
	updateHabitIcon,
} from "@/firebase/habits";

const WEEKDAYS = ["Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"];
const MONTHS = [
	"Janeiro",
	"Fevereiro",
	"Março",
	"Abril",
	"Maio",
	"Junho",
	"Julho",
	"Agosto",
	"Setembro",
	"Outubro",
	"Novembro",
	"Dezembro",
];

export default function HabitCalendarScreen() {
	const router = useRouter();
	const { id } = useLocalSearchParams<{ id: string }>();
	const [habit, setHabit] = useState<Habit | null>(null);
	const [name, setName] = useState("");
	const [month, setMonth] = useState(() => {
		const today = new Date();
		return new Date(today.getFullYear(), today.getMonth(), 1);
	});
	const [doneDays, setDoneDays] = useState<Set<string>>(new Set());
	const [pendingDay, setPendingDay] = useState<string | null>(null);
	const [saving, setSaving] = useState(false);
	const [iconSource, setIconSource] = useState<string | null>(null);
	const [iconDraft, setIconDraft] = useState<HabitIcon | null>(null);
	const [ditherMode, setDitherMode] = useState<DitherMode>("none");
	const [iconBusy, setIconBusy] = useState(false);
	const [showSettings, setShowSettings] = useState(false);

	const firstKey = localDateKey(
		new Date(month.getFullYear(), month.getMonth(), 1),
	);
	const lastKey = localDateKey(
		new Date(month.getFullYear(), month.getMonth() + 1, 0),
	);
	const todayKey = localDateKey(new Date());

	const cells = useMemo(() => {
		const firstWeekday = (month.getDay() + 6) % 7; // Segunda = primeira coluna
		const total = new Date(
			month.getFullYear(),
			month.getMonth() + 1,
			0,
		).getDate();
		const result: (number | null)[] = Array(firstWeekday).fill(null);
		for (let day = 1; day <= total; day++) result.push(day);
		while (result.length % 7 !== 0) result.push(null);
		return result;
	}, [month]);

	useEffect(() => {
		if (!id) return;
		return listenHabit(
			id,
			(item) => {
				setHabit(item);
				if (item) setName(item.name);
			},
			(error) => Alert.alert("Erro ao carregar hábito", error.message),
		);
	}, [id]);

	useEffect(() => {
		if (!id) return;
		setDoneDays(new Set());
		return listenCompletedDays(
			id,
			firstKey,
			lastKey,
			setDoneDays,
			(error) =>
				Alert.alert("Erro ao carregar calendário", error.message),
		);
	}, [id, firstKey, lastKey]);

	async function toggle(date: string) {
		if (!id || pendingDay || date > todayKey) return;
		setPendingDay(date);
		try {
			await toggleCompletedDay(id, date, doneDays.has(date));
		} catch (error) {
			Alert.alert("Erro ao salvar", String(error));
		} finally {
			setPendingDay(null);
		}
	}

	async function chooseIcon() {
		if (iconBusy) return;
		try {
			// Seleciona diretamente um PNG no gerenciador de arquivos.
			// Nao usa a galeria, crop nativo ou DocumentPicker.
			const selection = await File.pickFileAsync({
				mimeTypes: ["image/png"],
				multipleFiles: false,
			});
			if (selection.canceled) return;

			setIconBusy(true);
			const selected = selection.result;
			console.log(
				"HABIT ICON: arquivo:",
				selected.name,
				"uri:",
				selected.uri,
			);

			// O objeto File devolvido pelo seletor conserva o acesso
			// ao documento, inclusive quando a URI usa content://.
			// Nao tentar copiar novamente o arquivo selecionado.
			const bytes = await selected.bytes();
			console.log("HABIT ICON: bytes lidos:", bytes.length);

			const preparedUri = await prepareHabitIconSource(
				bytes,
				selected.name,
			);
			const icon = await convertHabitIcon(preparedUri, ditherMode);
			setIconSource(preparedUri);
			setIconDraft(icon);
		} catch (error) {
			console.error("HABIT ICON: falha na selecao/leitura:", error);
			Alert.alert(
				"Erro ao escolher PNG",
				String(error) +
					"\n\nAbra o arquivo PNG original pela pasta Downloads/Arquivos, nao pela galeria de fotos.",
			);
		} finally {
			setIconBusy(false);
		}
	}

	async function changeDither(mode: DitherMode) {
		if (iconBusy) return;
		setDitherMode(mode);
		if (!iconSource) return;
		setIconBusy(true);
		try {
			const icon = await convertHabitIcon(iconSource, mode);
			setIconDraft(icon);
		} catch (error) {
			Alert.alert("Erro ao processar ícone", String(error));
		} finally {
			setIconBusy(false);
		}
	}

	async function saveIcon() {
		if (!id || !iconDraft || iconBusy) return;
		setIconBusy(true);
		try {
			await updateHabitIcon(id, iconDraft);
			setIconDraft(null);
			setIconSource(null);
			Alert.alert(
				"Ícone salvo",
				"O ícone de quatro tons foi enviado ao Firestore.",
			);
		} catch (error) {
			Alert.alert("Erro ao salvar ícone", String(error));
		} finally {
			setIconBusy(false);
		}
	}

	function removeIcon() {
		if (!id || iconBusy) return;
		Alert.alert("Remover ícone?", "O hábito continuará cadastrado.", [
			{ text: "Cancelar", style: "cancel" },
			{
				text: "Remover",
				style: "destructive",
				onPress: async () => {
					setIconBusy(true);
					try {
						await updateHabitIcon(id, null);
						setIconDraft(null);
						setIconSource(null);
					} catch (error) {
						Alert.alert("Erro ao remover", String(error));
					} finally {
						setIconBusy(false);
					}
				},
			},
		]);
	}

	async function saveName() {
		if (!id || !name.trim() || saving) return;
		setSaving(true);
		try {
			await renameHabit(id, name);
		} catch (error) {
			Alert.alert("Erro ao renomear", String(error));
		} finally {
			setSaving(false);
		}
	}

	function confirmDelete() {
		if (!id) return;
		Alert.alert(
			"Excluir hábito?",
			"O hábito e todo o histórico de dias realizados serão apagados.",
			[
				{ text: "Cancelar", style: "cancel" },
				{
					text: "Excluir",
					style: "destructive",
					onPress: async () => {
						try {
							await deleteHabitWithHistory(id);
							router.back();
						} catch (error) {
							Alert.alert("Erro ao excluir", String(error));
						}
					},
				},
			],
		);
	}

	function changeMonth(amount: number) {
		setMonth(
			(current) =>
				new Date(current.getFullYear(), current.getMonth() + amount, 1),
		);
	}

	return (
		<SafeAreaView style={styles.screen}>
			<View style={styles.topBar}>
				<Pressable onPress={() => router.back()} style={{ padding: 4 }}>
					<Ionicons name="arrow-back" color="#FFFFFF" size={24} />
				</Pressable>
				<Text style={styles.headerTitle} numberOfLines={1}>
					{habit?.name ?? "Hábito"}
				</Text>
				<Pressable
					onPress={() => setShowSettings((prev) => !prev)}
					style={{ padding: 8 }}
					accessibilityLabel={
						showSettings ? "Voltar ao calendário" : "Editar hábito"
					}
				>
					<Ionicons
						name={
							showSettings
								? "calendar-outline"
								: "settings-outline"
						}
						size={24}
						color="#C7C9CF"
					/>
				</Pressable>
			</View>
			<ScrollView
				key={showSettings ? "settings" : "calendar"}
				contentContainerStyle={styles.content}
			>
				{!showSettings && (
					<>
						<View style={styles.monthHeader}>
							<Pressable
								onPress={() => changeMonth(-1)}
								style={styles.monthArrow}
							>
								<Ionicons
									name="chevron-back"
									size={25}
									color="#FFFFFF"
								/>
							</Pressable>
							<Text style={styles.monthTitle}>
								{MONTHS[month.getMonth()]} de{" "}
								{month.getFullYear()}
							</Text>
							<Pressable
								onPress={() => changeMonth(1)}
								style={styles.monthArrow}
							>
								<Ionicons
									name="chevron-forward"
									size={25}
									color="#FFFFFF"
								/>
							</Pressable>
						</View>
						<View style={styles.calendar}>
							{WEEKDAYS.map((day) => (
								<View key={day} style={styles.cellWrapper}>
									<Text style={styles.weekday}>{day}</Text>
								</View>
							))}
							{cells.map((day, index) => {
								if (day === null)
									return (
										<View
											key={`empty-${index}`}
											style={styles.cellWrapper}
										/>
									);
								const key = localDateKey(
									new Date(
										month.getFullYear(),
										month.getMonth(),
										day,
									),
								);
								const done = doneDays.has(key);
								const future = key > todayKey;
								return (
									<Pressable
										key={key}
										disabled={future || pendingDay !== null}
										onPress={() => toggle(key)}
										accessibilityLabel={`${key}: ${done ? "realizado" : "não realizado"}`}
										style={styles.cellWrapper}
									>
										<View
											style={[
												styles.dayCell,
												done && styles.completed,
												key === todayKey &&
													styles.today,
											]}
										>
											<Text
												style={[
													styles.dayNumber,
													future &&
														!done &&
														styles.future,
													done && {
														color: "#FFFFFF",
													},
												]}
											>
												{day}
											</Text>
										</View>
									</Pressable>
								);
							})}
						</View>

						<View style={styles.divider} />
					</>
				)}

				{showSettings && (
					<>
						<Text style={styles.editHeading}>Nome do hábito</Text>
						<View style={styles.editRow}>
							<TextInput
								style={styles.input}
								value={name}
								onChangeText={setName}
								maxLength={80}
								placeholderTextColor="#888"
							/>
							<Pressable
								onPress={saveName}
								disabled={
									saving ||
									!name.trim() ||
									name === habit?.name
								}
								style={[
									styles.saveButton,
									(saving ||
										!name.trim() ||
										name === habit?.name) && {
										opacity: 0.5,
									},
								]}
							>
								<Text style={styles.saveText}>Salvar</Text>
							</Pressable>
						</View>
						<View style={styles.divider} />
						<Text style={styles.editHeading}>Ícone do hábito</Text>
						<Text style={styles.iconHelp}>
							Escolha uma imagem. Ela será recortada ao centro em
							formato quadrado e veja a prévia em 4 tons de cinza.
							O fundo transparente vira cinza-claro; o branco
							opaco é preservado.
						</Text>
						<View style={styles.iconEditorRow}>
							<HabitIconImage
								icon={iconDraft ?? habit?.icon}
								size={112}
							/>
							<View style={styles.iconControls}>
								<Pressable
									onPress={chooseIcon}
									disabled={iconBusy}
									style={styles.chooseIconButton}
								>
									<Ionicons
										name="images-outline"
										size={19}
										color="#FFFFFF"
									/>
									<Text style={styles.chooseIconLabel}>
										Escolher imagem
									</Text>
								</Pressable>
								<Text style={styles.iconHelp}>
									48 × 48 px · 4 tons · 2 bpp
								</Text>
								{iconBusy && (
									<ActivityIndicator color="#8DD3AD" />
								)}
							</View>
						</View>
						{iconSource && (
							<>
								<Text style={styles.ditherLabel}>
									Conversão
								</Text>
								<View style={styles.ditherRow}>
									{[
										{
											value: "none" as DitherMode,
											title: "Sem dithering",
										},
										{
											value: "floyd-steinberg" as DitherMode,
											title: "Floyd–Steinberg",
										},
									].map((option) => (
										<Pressable
											key={option.value}
											onPress={() =>
												changeDither(option.value)
											}
											disabled={iconBusy}
											style={[
												styles.ditherOption,
												ditherMode === option.value &&
													styles.ditherSelected,
											]}
										>
											<Text
												style={styles.ditherOptionText}
											>
												{option.title}
											</Text>
										</Pressable>
									))}
								</View>
								<Pressable
									onPress={saveIcon}
									disabled={iconBusy || !iconDraft}
									style={[
										styles.saveIconButton,
										iconBusy && { opacity: 0.5 },
									]}
								>
									<Text style={styles.saveText}>
										Salvar ícone no Firebase
									</Text>
								</Pressable>
							</>
						)}
						{habit?.icon && (
							<Pressable
								onPress={removeIcon}
								disabled={iconBusy}
								style={styles.removeIconButton}
							>
								<Text style={styles.removeIconText}>
									Remover ícone
								</Text>
							</Pressable>
						)}
						<Pressable
							style={styles.deleteButton}
							onPress={confirmDelete}
						>
							<Ionicons
								name="trash-outline"
								size={18}
								color="#E78E8E"
							/>
							<Text style={styles.deleteText}>
								Excluir hábito
							</Text>
						</Pressable>
					</>
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
		gap: 12,
		paddingHorizontal: 16,
		paddingTop: 10,
		paddingBottom: 14,
	},
	headerTitle: { color: "#FFFFFF", fontSize: 23, fontWeight: "700", flex: 1 },
	content: { paddingHorizontal: 16, paddingBottom: 48 },
	monthHeader: {
		flexDirection: "row",
		alignItems: "center",
		justifyContent: "space-between",
		marginVertical: 18,
	},
	monthArrow: { padding: 8 },
	monthTitle: { color: "#F2F2F2", fontSize: 18, fontWeight: "700" },
	calendar: {
		flexDirection: "row",
		flexWrap: "wrap",
		backgroundColor: "#1D1F24",
		borderRadius: 18,
		paddingVertical: 12,
		paddingHorizontal: 6,
	},
	cellWrapper: {
		width: "14.2%",
		alignItems: "center",
		justifyContent: "center",
		marginVertical: 4,
	},
	weekday: { color: "#9DA0A9", fontSize: 12, paddingBottom: 9 },
	dayCell: {
		width: 39,
		height: 39,
		borderRadius: 12,
		alignItems: "center",
		justifyContent: "center",
	},
	completed: {
		backgroundColor: "#378454",
		borderRadius: 12,
	},
	today: {
		borderWidth: 1,
		borderColor: "#96D5B0",
		borderRadius: 12,
	},
	dayNumber: { color: "#E7E7E9", fontSize: 15 },
	future: { color: "#61636A" },
	hint: {
		color: "#9DA0A9",
		marginTop: 16,
		textAlign: "center",
		fontSize: 13,
	},
	divider: { height: 1, backgroundColor: "#303239", marginVertical: 28 },
	editHeading: {
		color: "#F3F3F3",
		fontSize: 16,
		fontWeight: "600",
		marginBottom: 10,
	},
	editRow: { flexDirection: "row", gap: 10 },
	input: {
		backgroundColor: "#202228",
		borderRadius: 12,
		color: "#FFFFFF",
		fontSize: 15,
		paddingHorizontal: 14,
		minHeight: 50,
		flex: 1,
	},
	saveButton: {
		backgroundColor: "#378454",
		borderRadius: 12,
		paddingHorizontal: 17,
		alignItems: "center",
		justifyContent: "center",
	},
	saveText: { color: "#FFFFFF", fontWeight: "600" },
	deleteButton: {
		flexDirection: "row",
		alignItems: "center",
		gap: 8,
		marginTop: 36,
		alignSelf: "flex-start",
		paddingVertical: 10,
	},
	deleteText: { color: "#E78E8E", fontSize: 14, fontWeight: "600" },
	iconHelp: {
		color: "#9DA0A9",
		fontSize: 12,
		lineHeight: 18,
		marginBottom: 12,
	},
	iconEditorRow: {
		flexDirection: "row",
		alignItems: "center",
		gap: 16,
		marginBottom: 16,
	},
	iconControls: { flex: 1, gap: 10 },
	chooseIconButton: {
		flexDirection: "row",
		alignItems: "center",
		justifyContent: "center",
		gap: 8,
		borderRadius: 12,
		backgroundColor: "#31353D",
		paddingHorizontal: 12,
		minHeight: 48,
	},
	chooseIconLabel: { fontSize: 13, fontWeight: "600", color: "#FFFFFF" },
	ditherLabel: {
		color: "#E6E6E8",
		fontSize: 14,
		fontWeight: "600",
		marginBottom: 10,
	},
	ditherRow: { flexDirection: "row", gap: 8, marginBottom: 14 },
	ditherOption: {
		flex: 1,
		borderWidth: 1,
		borderColor: "#44474E",
		padding: 11,
		borderRadius: 10,
		alignItems: "center",
	},
	ditherSelected: { borderColor: "#8DD3AD", backgroundColor: "#274335" },
	ditherOptionText: { color: "#FFFFFF", fontSize: 12, fontWeight: "500" },
	saveIconButton: {
		backgroundColor: "#378454",
		borderRadius: 12,
		padding: 14,
		alignItems: "center",
	},
	removeIconButton: {
		alignSelf: "flex-start",
		paddingVertical: 12,
		marginTop: 4,
	},
	removeIconText: { color: "#E78E8E", fontSize: 13, fontWeight: "600" },
});
