#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "QFontRenderer.h"

// Exemplo independente do driver do e-paper.
// Usa os pinos do microSD do seu protótipo ESP32-S3.
static constexpr int SD_SCK = 21;
static constexpr int SD_MISO = 13;
static constexpr int SD_MOSI = 47;
static constexpr int SD_CS = 48;

static constexpr uint16_t WIDTH = 800;
static constexpr uint16_t HEIGHT = 480;
static constexpr size_t BYTES = WIDTH * HEIGHT / 4; // 96.000 bytes

uint8_t* frame = nullptr;

void setup() {
  Serial.begin(115200);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("Erro: microSD");
    return;
  }
  frame = static_cast<uint8_t*>(ps_malloc(BYTES));
  if (!frame) {
    Serial.println("Erro: PSRAM insuficiente");
    return;
  }
  QFontRenderer::clear(frame, WIDTH, HEIGHT, 3); // Branco

  QFontRenderer font;
  if (font.begin(SD, "/fonts/MinhaFonte_48.bin")) {
    font.drawText(frame, WIDTH, HEIGHT, 24, 100, "Quadro E-ink", 0);
    font.end();
  } else {
    Serial.println("Fonte 48 não encontrada");
  }
  if (font.begin(SD, "/fonts/MinhaFonte_24.bin")) {
    font.drawText(frame, WIDTH, HEIGHT, 24, 160, "Meditação • há 3 dias", 0);
    font.drawText(frame, WIDTH, HEIGHT, 24, 210, "Próxima atualização: 20:00", 1);
    font.end();
  } else {
    Serial.println("Fonte 24 não encontrada");
  }

  // TODO: converter framebuffer lógico de 4 cinzas para o FORMATO
  // EXATO do controlador do seu e-paper e depois executar o refresh.
  // Não é necessariamente o mesmo formato de bytes aceito pela tela.
  // 'frame' contém 800x480 pixels (2 bpp; 96.000 bytes).
  Serial.printf("Framebuffer pronto: %u bytes\n", unsigned(BYTES));
}

void loop() {}
