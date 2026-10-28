import { Stack, ThemeProvider, DarkTheme } from "expo-router";

export default function RootLayout() {
	return (
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
	);
}
