import { Stack, ThemeProvider, DarkTheme } from "expo-router";
import { GestureHandlerRootView } from "react-native-gesture-handler";

export default function RootLayout() {
	return (
		<GestureHandlerRootView style={{ flex: 1 }}>
			<ThemeProvider value={DarkTheme}>
				<Stack
					screenOptions={{
						headerShown: false,
						animation: "none",
						contentStyle: {
							backgroundColor: "#101114",
						},
					}}
				/>
			</ThemeProvider>
		</GestureHandlerRootView>
	);
}
