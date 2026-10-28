import { useRouter } from "expo-router";
import Ionicons from "@expo/vector-icons/Ionicons";
import { Pressable, ScrollView, StyleSheet, Text, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

type IconName = React.ComponentProps<typeof Ionicons>["name"];

type Section = {
	id: string;
	title: string;
	description: string;
	icon: IconName;
	color: string;
};

const sections: Section[] = [
	{
		id: "habits",
		title: "Hábitos",
		description: "Registros e organização",
		icon: "checkbox-outline",
		color: "#7FCBA5",
	},
	{
		id: "display",
		title: "Display",
		description: "Configurações do painel",
		icon: "desktop-outline",
		color: "#92B8ED",
	},
	{
		id: "weather",
		title: "Clima",
		description: "Previsão do tempo",
		icon: "partly-sunny-outline",
		color: "#E6BB76",
	},
	{
		id: "calendar",
		title: "Agenda",
		description: "Google Calendar",
		icon: "calendar-outline",
		color: "#C5A7EB",
	},
	{
		id: "screen-time",
		title: "Tempo de tela",
		description: "Celular e computador",
		icon: "phone-portrait-outline",
		color: "#E59E9E",
	},
	{
		id: "sensors",
		title: "Sensores",
		description: "Monitoramento ambiental",
		icon: "thermometer-outline",
		color: "#85C7CC",
	},
];

export default function HomeScreen() {
	const router = useRouter();

	return (
		<SafeAreaView style={styles.container}>
			<ScrollView
				contentContainerStyle={styles.content}
				showsVerticalScrollIndicator={false}
			>
				<View style={styles.header}>
					<Text style={styles.title}>EinkDash</Text>
					<Text style={styles.subtitle}>
						Gerenciamento do painel e-ink
					</Text>
				</View>

				<Text style={styles.sectionTitle}>Funcionalidades</Text>

				<View style={styles.grid}>
					{sections.map((section) => (
						<Pressable
							key={section.id}
							onPress={() => {
								if (section.id === "habits") {
									router.push("/habits");
								}
							}}
							style={({ pressed }) => [
								styles.card,
								pressed && styles.cardPressed,
							]}
						>
							<View
								style={[
									styles.iconContainer,
									{ backgroundColor: section.color + "18" },
								]}
							>
								<Ionicons
									name={section.icon}
									size={29}
									color={section.color}
								/>
							</View>

							<View style={styles.cardInfo}>
								<Text style={styles.cardTitle}>
									{section.title}
								</Text>

								<Text style={styles.cardDescription}>
									{section.description}
								</Text>
							</View>

							<Ionicons
								name="chevron-forward"
								size={17}
								color="#666"
								style={styles.arrow}
							/>
						</Pressable>
					))}
				</View>
			</ScrollView>
		</SafeAreaView>
	);
}

const styles = StyleSheet.create({
	container: {
		flex: 1,
		backgroundColor: "#101114",
	},
	content: {
		paddingHorizontal: 20,
		paddingTop: 30,
		paddingBottom: 40,
	},
	header: {
		marginBottom: 42,
	},
	title: {
		color: "#FFFFFF",
		fontSize: 34,
		fontWeight: "700",
		letterSpacing: -0.8,
	},
	subtitle: {
		color: "#91939A",
		fontSize: 14,
		marginTop: 7,
	},
	sectionTitle: {
		color: "#E0E0E0",
		fontSize: 17,
		fontWeight: "600",
		marginBottom: 18,
	},
	grid: {
		flexDirection: "row",
		flexWrap: "wrap",
		justifyContent: "space-between",
		rowGap: 14,
	},
	card: {
		width: "48%",
		minHeight: 172,
		backgroundColor: "#1D1F24",
		borderRadius: 20,
		padding: 16,
		borderWidth: 1,
		borderColor: "#2A2C31",
	},
	cardPressed: {
		opacity: 0.7,
	},
	iconContainer: {
		width: 52,
		height: 52,
		borderRadius: 15,
		justifyContent: "center",
		alignItems: "center",
	},
	cardInfo: {
		marginTop: 22,
		paddingRight: 8,
	},
	cardTitle: {
		color: "#F5F5F5",
		fontSize: 17,
		fontWeight: "600",
	},
	cardDescription: {
		color: "#8B8E96",
		fontSize: 12,
		marginTop: 6,
		lineHeight: 17,
	},
	arrow: {
		position: "absolute",
		right: 12,
		bottom: 15,
	},
});
