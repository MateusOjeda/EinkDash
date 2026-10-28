# Quadro E-ink — fontes bitmap QFNT de 4 níveis

Primeira implementação das fontes gravadas **no microSD**, separadas da imagem de fundo. A ideia é manter um template 800×480 e sobrepor textos proporcionais com letras acentuadas. O código **não inclui nem distribui fontes tipográficas**: você usa um arquivo `.ttf` ou `.otf` da sua escolha no computador para gerar os `.bin`.

## Gerar os arquivos

1. Instale Python 3 e a dependência: `python -m pip install Pillow`
2. Salve a fonte que deseja converter em seu computador (`MinhaFonte.ttf`).
3. Execute:

```bash
python gerar_fontes.py MinhaFonte.ttf --sizes 24 32 48 60 --name MinhaFonte --out fonts --preview
```

Saída:

```text
fonts/MinhaFonte_24.bin
fonts/MinhaFonte_32.bin
fonts/MinhaFonte_48.bin
fonts/MinhaFonte_60.bin
fonts/preview_800x480.png
```

Copie **somente os `.bin`** para a pasta `/fonts` do microSD. São tamanhos separados para preservar detalhes/antialiasing; redimensionar os pixels de uma fonte menor perde qualidade.

Por padrão, estão incluídos ASCII, Latin-1 (acentos do português), euro, travessão, aspas curvas, reticências e marcador `•`. Para incluir outros caracteres: `--extra "✓→☀☁"` (dependem de cobertura no TTF/OTF escolhido).

## Formato do arquivo (`QFNT v1`)

Todos os inteiros são **little-endian**, structs `packed`, sem preenchimento. Arquivo único para cada família/peso/tamanho; **fonte proporcional** (avanço individual por glifo):

**Cabeçalho: 32 bytes**

| Offset | Tipo | Significado |
|---:|---|---|
| 0 | char[4] | `QFNT` |
| 4 | u16 | versão = 1 |
| 6 | u16 | tamanho cabeçalho = 32 |
| 8 | u16 | tamanho em pixels |
| 10 | u16 | ascender (baseline) |
| 12 | u16 | descender |
| 14 | u16 | lineHeight |
| 16 | u32 | quantidade de glifos |
| 20 | u32 | offset índice = 32 |
| 24 | u32 | offset dos bitmaps |
| 28 | u32 | flags = 1 (2bpp cobertura) |

**Índice: 24 bytes por glifo**, ordenados por código Unicode:

| Offset relativo | Tipo | Campo |
|---:|---|---|
| 0 | u32 | código Unicode (codepoint) |
| 4 | i16 | xOffset (relativo à posição da caneta) |
| 6 | i16 | yOffset (relativo à baseline) |
| 8 | u16 | width |
| 10 | u16 | height |
| 12 | i16 | advance horizontal |
| 14 | u16 | reservado = 0 |
| 16 | u32 | offset absoluto do bitmap |
| 20 | u32 | tamanho do bitmap em bytes |

**Bitmap** por glifo: 4 pixels por byte, 2 bits cada, em ordem visual **esquerda para direita, cima para baixo**, bit 7 primeiro. Os níveis representam **cobertura da tinta**, não uma cor absoluta: `00` transparente, `01` 33%, `10` 67%, `11` 100% da cor do texto. Glifos sem pixels (como espaço) têm bitmap vazio, mas possuem `advance`.

## Renderizador no ESP32

Inclua `QFontRenderer.h` e `.cpp` no projeto Arduino. A demonstração `EXEMPLO_ESP32.ino` usa ESP32-S3, os pinos do microSD do projeto e PSRAM. O framebuffer de destino usa 2 bpp, ou seja, **96.000 bytes** em 800×480:

- `0`: preto
- `1`: cinza-escuro
- `2`: cinza-claro
- `3`: branco

O texto mistura a cobertura do bitmap com a cor de texto e o pixel existente, portanto funciona sobre templates já carregados na RAM. O parâmetro vertical de `drawText` é a **baseline**.

**Importante:** esse framebuffer é uma **representação lógica intermediária**. Ele precisa ser convertido para o protocolo/formato correto do seu driver GDEY/EPD antes do envio ao painel. Não confundir com os `.bin` de 4 bpp do Spectra 6 (192.000 bytes).

## Limitações da versão inicial

- Não há kerning entre pares nem shaping (desenho avançado de texto), mas fontes proporcionais, UTF-8, acentos precompostos e múltiplas linhas funcionam.
- Não há quebra automática de linha/alinhamento/ellipsis. Dá para adicionar depois no renderizador.
- Recomenda-se que a largura do framebuffer seja múltipla de 4.
- Fonte com caracteres faltantes pode exibir o glifo padrão do arquivo tipográfico; verifique o preview ou use outro arquivo.
- Os 4 tons finais dependem das capacidades/restrições de atualização do painel e de seu driver.
