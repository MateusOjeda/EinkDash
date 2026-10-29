# EinkDash icons_kit — ampliado

Este é o **seu icons_kit original**, com os 37 ícones meteorológicos e os IDs preservados.
Adicionamos 13 ícones individuais em 48x48 PNG, com os SVGs vetoriais correspondentes em `svg/`.
O gerador, as fontes de dados meteorológicos, o `WeatherIconIds.h` e o formato WIC2 original continuam iguais.

## Novos IDs

| ID | Arquivo PNG | Uso |
|---:|---|---|
| 304 | `sensor_temperature.png` | Temperatura |
| 305 | `sensor_humidity.png` | Umidade |
| 306 | `sensor_co2.png` | CO2 |
| 307 | `sensor_lux.png` | Luminosidade |
| 308 | `sensor_pm25.png` | PM2.5 |
| 309 | `sensor_noise.png` | Ruído |
| 310 | `wifi_off.png` | Wi-Fi desconectado |
| 311 | `water_bottle.png` | Garrafa / consumo de água |
| 312 | `bicycle.png` | Bicicleta |
| 313 | `steps.png` | Passos |
| 314 | `mobile_screen_time.png` | Tempo de tela — celular |
| 315 | `desktop_screen_time.png` | Tempo de tela — computador |
| 316 | `computer.png` | Computador (genérico) |

## Regenerar

Na pasta `icons_kit/`:

```powershell
py -m pip install pillow
py tools/build_weather_icons.py
```

O script usa `icons.json` para empacotar todos os PNGs no mesmo `data/weather_icons.bin` (2 bits/pixel, 4 tons, ID e CRC).
**Não** é preciso modificar o gerador.

Copie `data/weather_icons.bin` para a pasta `data/` do projeto PlatformIO (onde já há `/weather_icons.bin`), mantendo também as fontes e `/image.bin`, e execute:

```bash
pio run -t uploadfs
```

⚠️ O upload do LittleFS substitui a imagem inteira do filesystem, então mantenha todos os recursos dentro da pasta `data/` do firmware.

As novas entradas ainda não aparecem automaticamente na interface: o `DisplayManager` deverá chamar `WeatherIcons::draw()` com o ID correspondente.
O ícone de `wifi_off` (ID 310) pode substituir o desenho manual atual, em futura alteração do display.

As duas imagens de **tempo de tela** usam símbolos distintos de telefone/computador com um pequeno relógio. O ícone **computador** (sem relógio) foi mantido separado para usos que não representam tempo de tela.

## Compatibilidade

- IDs meteorológicos 100–116 / 200–216 / 301–303 mantidos.
- Arquivos PNG antigos mantidos.
- Arquivo `WeatherIconIds.h` original mantido.
- Estrutura WIC2 original preservada.

O pacote não modifica o firmware; apenas os recursos gráficos e o índice do BIN.
