import { useRouter, Stack } from "expo-router";
import Ionicons from "@expo/vector-icons/Ionicons";
import { Pressable, StyleSheet, Text, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

export default function SettingsScreen() {
	const router = useRouter();

	return (
		<SafeAreaView style={styles.container}>
			<Stack.Screen options={{ headerShown: false }} />

			<View style={styles.header}>
				<Pressable onPress={() => router.back()} hitSlop={12}>
					<Ionicons name="arrow-back" size={24} color="#FFFFFF" />
				</Pressable>

				<Text style={styles.title}>Configurações</Text>
			</View>

			<Pressable
				style={({ pressed }) => [
					styles.item,
					pressed && { opacity: 0.7 },
				]}
				onPress={() => router.push("/wifi-config")}
			>
				<View style={styles.iconContainer}>
					<Ionicons name="wifi-outline" size={24} color="#92B8ED" />
				</View>

				<View style={styles.itemInfo}>
					<Text style={styles.itemTitle}>Configurar Wi-Fi</Text>

					<Text style={styles.itemDescription}>
						Conectar o EinkDash à rede
					</Text>
				</View>

				<Ionicons name="chevron-forward" size={19} color="#666" />
			</Pressable>
		</SafeAreaView>
	);
}

const styles = StyleSheet.create({
	container: {
		flex: 1,
		backgroundColor: "#101114",
		paddingHorizontal: 20,
	},
	header: {
		flexDirection: "row",
		alignItems: "center",
		gap: 20,
		marginTop: 30,
		marginBottom: 32,
	},
	title: {
		color: "#FFFFFF",
		fontSize: 26,
		fontWeight: "700",
	},
	item: {
		flexDirection: "row",
		alignItems: "center",
		padding: 16,
		backgroundColor: "#1D1F24",
		borderRadius: 16,
		borderWidth: 1,
		borderColor: "#2A2C31",
	},
	iconContainer: {
		width: 46,
		height: 46,
		borderRadius: 13,
		backgroundColor: "#92B8ED18",
		justifyContent: "center",
		alignItems: "center",
		marginRight: 14,
	},
	itemInfo: {
		flex: 1,
	},
	itemTitle: {
		color: "#F5F5F5",
		fontSize: 16,
		fontWeight: "600",
	},
	itemDescription: {
		color: "#8B8E96",
		fontSize: 12,
		marginTop: 5,
	},
});
