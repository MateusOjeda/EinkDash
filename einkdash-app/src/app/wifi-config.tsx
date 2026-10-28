import React, { useEffect, useRef, useState } from "react";
import {
	PermissionsAndroid,
	Platform,
	ActivityIndicator,
	Alert,
	KeyboardAvoidingView,
	ScrollView,
	StyleSheet,
	Pressable,
	Text,
	TextInput,
	View,
} from "react-native";
import { useRouter, Stack } from "expo-router";

import {
	ESPDevice,
	ESPProvisionManager,
	ESPSecurity,
	ESPTransport,
} from "@orbital-systems/react-native-esp-idf-provisioning";

import { BleManager } from "react-native-ble-plx";

const SERVICE_NAME = "EinkDash";
const POP = "K7mR9xT2pL4q"; // Deve coincidir com WIFI_PROVISIONING_POP do firmware

const DEVICE_SERVICE_UUID = "7b7d0001-4e69-4d91-9a31-7d5e8f000001";

const DEVICE_MAC_CHARACTERISTIC_UUID = "7b7d0002-4e69-4d91-9a31-7d5e8f000001";

const STATUS_SERVICE_UUID = "7b7d1001-4e69-4d91-9a31-7d5e8f000001";

const STATUS_CHARACTERISTIC_UUID = "7b7d1002-4e69-4d91-9a31-7d5e8f000001";

const STATUS_DEVICE_NAME = "EinkDashStatus";

export type DisplayType = "einkdash-7.5";

const textToBase64 = (text: string): string => {
	const chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	let result = "";

	for (let i = 0; i < text.length; i += 3) {
		const a = text.charCodeAt(i);
		const b = i + 1 < text.length ? text.charCodeAt(i + 1) : 0;
		const c = i + 2 < text.length ? text.charCodeAt(i + 2) : 0;

		const triple = (a << 16) | (b << 8) | c;

		result += chars[(triple >> 18) & 63];
		result += chars[(triple >> 12) & 63];
		result += i + 1 < text.length ? chars[(triple >> 6) & 63] : "=";
		result += i + 2 < text.length ? chars[triple & 63] : "=";
	}

	return result;
};

function base64ToText(value: string): string {
	const chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	let buffer = 0;
	let bits = 0;
	let result = "";

	for (const char of value.replace(/=+$/, "")) {
		const index = chars.indexOf(char);

		if (index === -1) {
			continue;
		}

		buffer = (buffer << 6) | index;
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			result += String.fromCharCode((buffer >> bits) & 0xff);
		}
	}

	return result;
}

type DeviceInfo = {
	mac: string;
	displayType: DisplayType;
};

export default function WifiConfigScreen() {
	const router = useRouter();

	const bleManager = useRef<BleManager | null>(null);
	if (!bleManager.current) bleManager.current = new BleManager();
	const ble = bleManager.current;

	const [scanning, setScanning] = useState(false);
	const [device, setDevice] = useState<ESPDevice | null>(null);
	const [connecting, setConnecting] = useState(false);

	const [ssid, setSsid] = useState("");
	const [password, setPassword] = useState("");
	const [provisioning, setProvisioning] = useState(false);

	const [wifiMac, setWifiMac] = useState<string | null>(null);
	const [displayType, setDisplayType] = useState<DisplayType | null>(null);
	const [readingMac, setReadingMac] = useState(false);
	const [startingProvisioning, setStartingProvisioning] = useState(false);

	useEffect(() => {
		return () => {
			ble.stopDeviceScan();
			ble.destroy();
		};
	}, [ble]);

	useEffect(() => {
		return () => {
			if (device) {
				try {
					device.disconnect();
				} catch (error) {
					console.log("Erro ao desconectar:", error);
				}
			}
		};
	}, [device]);

	const requestBluetoothPermission = async () => {
		if (Platform.OS !== "android") {
			return true;
		}

		if (Platform.Version >= 31) {
			const result = await PermissionsAndroid.requestMultiple([
				PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
				PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
			]);

			return (
				result[PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN] ===
					PermissionsAndroid.RESULTS.GRANTED &&
				result[PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT] ===
					PermissionsAndroid.RESULTS.GRANTED
			);
		}

		const result = await PermissionsAndroid.request(
			PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
		);

		return result === PermissionsAndroid.RESULTS.GRANTED;
	};

	const identifyDeviceAndRequestProvisioning =
		async (): Promise<DeviceInfo | null> => {
			setReadingMac(true);

			return new Promise((resolve) => {
				let finished = false;

				const finish = (deviceInfo: DeviceInfo | null) => {
					if (finished) {
						return;
					}

					finished = true;
					ble.stopDeviceScan();
					setReadingMac(false);
					resolve(deviceInfo);
				};

				console.log("=== PROCURANDO BLE DO QUADRO ===");

				setTimeout(() => finish(null), 20000);

				ble.startDeviceScan(
					null,
					null,
					async (error, scannedDevice) => {
						if (finished) return;
						if (error) {
							console.log("Erro no scan BLE:", error);
							finish(null);
							return;
						}

						if (!scannedDevice) {
							return;
						}

						console.log(
							"BLE encontrado:",
							scannedDevice.name,
							scannedDevice.localName,
							scannedDevice.id,
						);

						if (
							scannedDevice.name !== SERVICE_NAME &&
							scannedDevice.localName !== SERVICE_NAME
						) {
							return;
						}

						ble.stopDeviceScan();

						try {
							console.log("Conectando ao BLE...");

							const connectedDevice =
								await scannedDevice.connect();

							console.log("BLE conectado.");

							await connectedDevice.discoverAllServicesAndCharacteristics();

							console.log("Serviços BLE descobertos.");

							const characteristic =
								await connectedDevice.readCharacteristicForService(
									DEVICE_SERVICE_UUID,
									DEVICE_MAC_CHARACTERISTIC_UUID,
								);

							console.log(
								"Informações recebidas em Base64:",
								characteristic.value,
							);

							if (!characteristic.value) {
								throw new Error(
									"Característica não retornou nenhum valor.",
								);
							}

							const deviceInfoString = base64ToText(
								characteristic.value,
							);

							console.log(
								"Informações do dispositivo:",
								deviceInfoString,
							);

							const [mac, type] = deviceInfoString.split("|");

							if (!mac || !type) {
								throw new Error(
									"Informações do dispositivo inválidas.",
								);
							}

							if (type !== "einkdash-7.5") {
								throw new Error(
									`Tipo de dispositivo desconhecido: ${type}`,
								);
							}

							const deviceInfo: DeviceInfo = {
								mac,
								displayType: type,
							};

							console.log("MAC decodificado:", mac);
							console.log("Tipo do dispositivo:", type);

							console.log(
								"Solicitando início do provisioning...",
							);

							await connectedDevice.writeCharacteristicWithResponseForService(
								DEVICE_SERVICE_UUID,
								DEVICE_MAC_CHARACTERISTIC_UUID,
								textToBase64("PROVISION"),
							);

							console.log("Comando PROVISION enviado.");

							try {
								await connectedDevice.cancelConnection();
								console.log("BLE desconectado.");
							} catch (error) {
								console.log(
									"BLE já estava desconectado pelo quadro.",
								);
							}

							finish(deviceInfo);
						} catch (connectionError) {
							console.log(
								"Erro ao identificar/solicitar provisioning:",
								connectionError,
							);

							try {
								await scannedDevice.cancelConnection();
							} catch {}

							finish(null);
						}
					},
				);
			});
		};

	const scan = async () => {
		const hasPermission = await requestBluetoothPermission();

		console.log("Permissão Bluetooth concedida:", hasPermission);

		if (!hasPermission) {
			Alert.alert(
				"Permissão necessária",
				"Permita o acesso ao Bluetooth para procurar o EinkDash.",
			);
			return;
		}

		try {
			setDevice(null);
			setWifiMac(null);
			setDisplayType(null);
			setScanning(true);

			console.log("=== IDENTIFICANDO QUADRO ===");

			const deviceInfo = await identifyDeviceAndRequestProvisioning();

			if (!deviceInfo) {
				Alert.alert(
					"Quadro não encontrado",
					"Não foi possível identificar o EinkDash. Inicie o modo de configuração pelo comando wifi no Serial Monitor e tente novamente.",
				);
				return;
			}

			console.log("MAC do quadro:", deviceInfo.mac);
			console.log("Tipo do quadro:", deviceInfo.displayType);

			setWifiMac(deviceInfo.mac);
			setDisplayType(deviceInfo.displayType);
			setStartingProvisioning(true);

			await new Promise((resolve) => setTimeout(resolve, 3000));

			console.log("=== PROCURANDO PROVISIONAMENTO WI-FI ===");

			const devices = await ESPProvisionManager.searchESPDevices(
				"PROV_EinkDash",
				ESPTransport.ble,
				ESPSecurity.secure,
			);

			console.log(
				"Dispositivos de provisionamento encontrados:",
				devices,
			);

			if (devices.length === 0) {
				Alert.alert(
					"Quadro encontrado",
					"Foi possível identificar o quadro, mas não foi possível iniciar a configuração Wi-Fi.",
				);
				return;
			}

			const foundDevice = devices[0];

			console.log("EinkDash encontrado para provisionamento.");

			setDevice(foundDevice);
		} catch (error) {
			console.log("Erro ao procurar EinkDash:", error);

			Alert.alert("Erro", "Não foi possível encontrar o EinkDash.");
		} finally {
			setScanning(false);
			setReadingMac(false);
			setStartingProvisioning(false);
		}
	};

	const waitForWifiStatus = async (): Promise<string | null> => {
		return new Promise((resolve) => {
			let finished = false;

			const finish = (status: string | null) => {
				if (finished) {
					return;
				}

				finished = true;
				ble.stopDeviceScan();
				resolve(status);
			};

			console.log("=== PROCURANDO STATUS DO WI-FI ===");

			ble.startDeviceScan(null, null, async (error, scannedDevice) => {
				if (error) {
					console.log("Erro no scan do status BLE:", error);
					finish(null);
					return;
				}

				if (!scannedDevice) {
					return;
				}

				if (
					scannedDevice.name !== STATUS_DEVICE_NAME &&
					scannedDevice.localName !== STATUS_DEVICE_NAME
				) {
					return;
				}

				console.log(
					"BLE de status encontrado:",
					scannedDevice.name,
					scannedDevice.localName,
					scannedDevice.id,
				);

				ble.stopDeviceScan();

				try {
					const connectedDevice = await scannedDevice.connect();

					console.log("BLE de status conectado.");

					await connectedDevice.discoverAllServicesAndCharacteristics();

					console.log("Serviços do BLE de status descobertos.");

					const characteristic =
						await connectedDevice.readCharacteristicForService(
							STATUS_SERVICE_UUID,
							STATUS_CHARACTERISTIC_UUID,
						);

					console.log(
						"Status recebido em Base64:",
						characteristic.value,
					);

					if (!characteristic.value) {
						throw new Error(
							"Característica de status não retornou nenhum valor.",
						);
					}

					const status = base64ToText(characteristic.value);

					console.log("Status do Wi-Fi:", status);

					await connectedDevice.cancelConnection();

					finish(status);
				} catch (error) {
					console.log("Erro ao ler status do Wi-Fi:", error);

					try {
						await scannedDevice.cancelConnection();
					} catch {}

					finish(null);
				}
			});

			setTimeout(() => {
				finish(null);
			}, 30000);
		});
	};

	const connectAndProvision = async () => {
		if (!device) {
			return;
		}

		if (!ssid.trim()) {
			Alert.alert("SSID necessário", "Digite o nome da rede Wi-Fi.");
			return;
		}

		try {
			setConnecting(true);
			setProvisioning(true);

			console.log("Conectando ao EinkDash...");
			console.log("Security: 1");
			console.log("MAC do quadro:", wifiMac);
			console.log("Tipo do quadro:", displayType);

			await device.connect(POP);

			console.log("Provisionamento BLE conectado.");

			console.log("Enviando configuração Wi-Fi...");

			console.log("SSID:", ssid);
			console.log("Senha:", password ? "********" : "(vazia)");

			await device.provision(ssid.trim(), password);

			console.log("Configuração Wi-Fi enviada com sucesso.");

			const wifiStatus = await waitForWifiStatus();

			console.log("Resultado do status Wi-Fi:", wifiStatus);

			const [status, firebaseUid] = wifiStatus?.split("|") ?? [];

			if (status !== "WIFI_CONNECTED" || !firebaseUid) {
				throw new Error(
					`Wi-Fi não foi confirmado. Status: ${wifiStatus ?? "nenhum"}`,
				);
			}

			console.log("Wi-Fi confirmado pelo quadro.");
			console.log("Firebase UID:", firebaseUid);

			// O EinkDash ainda nao tem um fluxo de cadastro de dispositivos.
			// O firmware ja salva o refresh token na NVS. Nesta etapa,
			// confirmamos o UID; vincular o dispositivo no Firestore sera
			// uma integracao separada quando a estrutura estiver definida.
			Alert.alert(
				"EinkDash conectado",
				`Wi-Fi configurado com sucesso!\n\nMAC: ${wifiMac ?? "-"}\nFirebase UID: ${firebaseUid}`,
				[{ text: "Concluir", onPress: () => router.back() }],
			);
		} catch (error) {
			console.log("Erro ao configurar Wi-Fi:", error);

			try {
				device.disconnect();
			} catch (disconnectError) {
				console.log("Erro ao desconectar após falha:", disconnectError);
			}

			setDevice(null);

			Alert.alert(
				"Não foi possível configurar o Wi-Fi",
				"Verifique o nome e a senha da rede e tente novamente.",
			);

			return;
		} finally {
			setConnecting(false);
			setProvisioning(false);
		}
	};

	return (
		<>
			<Stack.Screen options={{ title: "Configurar Wi-Fi" }} />
			<KeyboardAvoidingView
				style={styles.screen}
				behavior={Platform.OS === "ios" ? "padding" : undefined}
			>
				<ScrollView
					contentContainerStyle={styles.content}
					keyboardShouldPersistTaps="handled"
				>
					<Text style={styles.heading}>
						Conectar EinkDash ao Wi-Fi
					</Text>
					<Text style={styles.help}>
						Ative o modo de configuracao no ESP32. Durante os
						testes, digite
						{' "wifi" '}no Serial Monitor. Em seguida, toque em
						Procurar dispositivo.
					</Text>

					<View style={styles.card}>
						<Text style={styles.statusTitle}>
							{device
								? "ESP32 encontrado"
								: startingProvisioning
									? "Iniciando provisionamento..."
									: readingMac
										? "Identificando dispositivo..."
										: scanning
											? "Procurando ESP32..."
											: "Dispositivo nao conectado"}
						</Text>
						{wifiMac && (
							<Text style={styles.subtext}>MAC: {wifiMac}</Text>
						)}
						{displayType && (
							<Text style={styles.subtext}>
								Display: {displayType}
							</Text>
						)}
						{(scanning || connecting) && (
							<ActivityIndicator style={styles.spinner} />
						)}
					</View>

					{device && (
						<View style={styles.card}>
							<Text style={styles.label}>Nome da rede Wi-Fi</Text>
							<TextInput
								style={styles.input}
								value={ssid}
								onChangeText={setSsid}
								placeholder="SSID"
								autoCapitalize="none"
								autoCorrect={false}
								editable={!connecting}
							/>
							<Text style={styles.label}>
								Senha da rede Wi-Fi
							</Text>
							<TextInput
								style={styles.input}
								value={password}
								onChangeText={setPassword}
								placeholder="Senha"
								secureTextEntry
								autoCapitalize="none"
								autoCorrect={false}
								editable={!connecting}
							/>
						</View>
					)}

					<Pressable
						style={[
							styles.button,
							(scanning || connecting || provisioning) &&
								styles.buttonDisabled,
						]}
						onPress={device ? connectAndProvision : scan}
						disabled={
							scanning || connecting || provisioning || readingMac
						}
					>
						<Text style={styles.buttonText}>
							{device
								? connecting
									? "Configurando Wi-Fi..."
									: "Conectar e configurar"
								: scanning
									? "Procurando..."
									: "Procurar dispositivo"}
						</Text>
					</Pressable>
				</ScrollView>
			</KeyboardAvoidingView>
		</>
	);
}

const styles = StyleSheet.create({
	screen: { flex: 1, backgroundColor: "#F7F8FA" },
	content: { padding: 20, paddingBottom: 32, gap: 18 },
	heading: { fontSize: 24, fontWeight: "700", color: "#17202C" },
	help: { fontSize: 14, lineHeight: 21, color: "#52606D" },
	card: {
		padding: 18,
		backgroundColor: "#FFFFFF",
		borderRadius: 12,
		gap: 10,
	},
	statusTitle: { fontSize: 17, fontWeight: "600", color: "#17202C" },
	subtext: { fontSize: 14, color: "#52606D" },
	spinner: { marginTop: 8 },
	label: { fontSize: 14, fontWeight: "600", color: "#17202C" },
	input: {
		height: 48,
		borderWidth: 1,
		borderColor: "#D9DEE4",
		borderRadius: 8,
		paddingHorizontal: 12,
		backgroundColor: "#FFFFFF",
		color: "#17202C",
	},
	button: {
		backgroundColor: "#273B6A",
		paddingVertical: 16,
		borderRadius: 10,
		alignItems: "center",
	},
	buttonDisabled: { opacity: 0.55 },
	buttonText: { color: "#FFFFFF", fontSize: 16, fontWeight: "600" },
});
