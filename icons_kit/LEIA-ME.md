# EinkDash — ícones meteorológicos V2

Esta é a **substituição artística dos PNGs anteriores**. Foram produzidos desenhos mais trabalhados, com nuvens sobrepostas, lua e estrelas, chuva com gotas, neve, chuva congelante, raios e granizo. Continuamos usando **PNG** na pasta `icons/` e o mesmo formato **WIC2** do BIN anterior.

## O que vem no ZIP

- `icons/`: 37 PNGs, com nomes e tamanhos corretos para seu gerador. São 17 condições × 2 tamanhos + 3 indicadores.
- `icons.json`: lista dos 37 ícones, **mantendo os IDs originais 100–111, 200–211 e 301–303**; IDs novos apenas para as variações.
- `weather_icons_preview.png`: prancha que mostra o resultado **real quantizado em quatro tons**.
- `data/weather_icons.bin`: binário já empacotado e validado com CRC por ícone.
- `tools/build_weather_icons.py`: script original para regerar o mesmo BIN com Pillow.
- `WeatherIconIds.h`: função C++ autônoma para mapear TODOS os códigos WMO documentados pela Open-Meteo (data do levantamento 28/10/2026).

## Instalação no seu projeto

**Mais simples:** copie só `data/weather_icons.bin` deste pacote para `data/weather_icons.bin` do projeto PlatformIO e rode `pio run -t uploadfs` (mantenha as suas fontes QFNT na pasta `data/`; o upload substitui a imagem inteira do LittleFS).

**Se quiser editar as imagens:** extraia o ZIP, modifique os PNGs em `icons/` sem alterar resolução nem nome, execute `py -m pip install pillow` (uma vez) e `py tools/build_weather_icons.py`. O gerador recria BIN e prévia.

Não execute o antigo script `create_example_icons.py` sobre esta pasta, pois ele pode substituir os PNGs por desenhos de exemplo; ele **não** faz parte deste pacote.

As imagens PNG são RGBA com transparência; o script compõe cada uma sobre **branco** na hora de gerar o BIN. A codificação do BIN é `00=#000`, `01=#555`, `10=#AAA`, `11=#FFF`, 4 pixels por byte, bit mais significativo primeiro. A estrutura original WIC2 de cabeçalho + índice é preservada.

### Tamanhos

- `*_main.png`: 128×112 px, 3.584 bytes por desenho no BIN.
- `*_small.png`: 48×48 px, 576 bytes por desenho no BIN.
- `indicator_*.png`: 48×48 px, 576 bytes.
- Total: 37 desenhos; BIN: 73.204 bytes.

## Open-Meteo: todos os códigos cobertos

O `WeatherIconIds.h` tem a conversão completa. Alguns códigos meteorológicos distintos compartilham o mesmo símbolo, mas todos os códigos da tabela WMO publicada pela Open-Meteo estão incluídos, inclusive chuva congelante, granizo e o código 97. Para previsão **diária** o ícone é normalmente diurno; para o tempo **atual**, use `current.is_day`.

| WMO | Ícone dia | ID dia grande/pequeno | Ícone noite | ID noite grande/pequeno |
|---|---|---|---|---|
| 0 | clear_day | 100/200 | clear_night | 109/209 |
| 1 | mostly_clear_day | 116/216 | partly_cloudy_night | 110/210 |
| 2 | partly_cloudy_day | 101/201 | partly_cloudy_night | 110/210 |
| 3 | overcast | 103/203 | cloudy_night | 112/212 |
| 45 | fog | 104/204 | fog | 104/204 |
| 48 | fog | 104/204 | fog | 104/204 |
| 51 | drizzle | 105/205 | drizzle | 105/205 |
| 53 | drizzle | 105/205 | drizzle | 105/205 |
| 55 | drizzle | 105/205 | drizzle | 105/205 |
| 56 | freezing_rain | 115/215 | freezing_rain | 115/215 |
| 57 | freezing_rain | 115/215 | freezing_rain | 115/215 |
| 61 | rain | 106/206 | rain | 106/206 |
| 63 | rain | 106/206 | rain | 106/206 |
| 65 | heavy_rain | 113/213 | heavy_rain | 113/213 |
| 66 | freezing_rain | 115/215 | freezing_rain | 115/215 |
| 67 | freezing_rain | 115/215 | freezing_rain | 115/215 |
| 71 | snow | 111/211 | snow | 111/211 |
| 73 | snow | 111/211 | snow | 111/211 |
| 75 | snow | 111/211 | snow | 111/211 |
| 77 | snow | 111/211 | snow | 111/211 |
| 80 | showers | 107/207 | showers | 107/207 |
| 81 | showers | 107/207 | showers | 107/207 |
| 82 | heavy_rain | 113/213 | heavy_rain | 113/213 |
| 85 | snow | 111/211 | snow | 111/211 |
| 86 | snow | 111/211 | snow | 111/211 |
| 95 | thunder | 108/208 | thunder | 108/208 |
| 96 | thunder_hail | 114/214 | thunder_hail | 114/214 |
| 97 | thunder | 108/208 | thunder | 108/208 |
| 99 | thunder_hail | 114/214 | thunder_hail | 114/214 |

### Lua depois de escurecer

Sua consulta atual já pede `current=is_day`, portanto a Open-Meteo entrega 1 para dia e 0 para noite. Para trocar **no horário exato do pôr do sol sem esperar a próxima consulta** (seus dados são atualizados aproximadamente a cada hora), é melhor acrescentar `sunrise,sunset` às variáveis `daily` e comparar o horário local atual com a janela de luz do dia, usando o fuso `America/Sao_Paulo` já selecionado. Para isso, o `WeatherManager` precisará também parsear/guardar os horários de nascer/pôr do sol; **não fiz alterações no firmware nesta entrega**.

### Limitações

- Cobrimos todos os códigos WMO que a API documenta, não cada valor meteorológico (temperaturas, vento, chuva em mm etc. continuam números na tela).
- O `weather_icons.bin` **ainda precisa do leitor integrado ao display** antes que os ícones apareçam no painel.
- O traçado vem das novas imagens PNG e o preview corresponde ao BIN de 4 tons; a aparência final do e-ink depende do display.

Fonte: https://open-meteo.com/en/docs#weather_variable_documentation
