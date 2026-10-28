
#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <GxEPD2_4G_4G.h>

#define IMAGE_WIDTH 800
#define IMAGE_HEIGHT 480
#define IMAGE_SIZE 96000

GxEPD2_4G_4G<GxEPD2_750_GDEY075T7, 64> display(
    GxEPD2_750_GDEY075T7(27, 14, 12, 13));

void setup()
{
    Serial.begin(115200);

    if (!LittleFS.begin(false))
    {
        Serial.println("Erro ao montar LittleFS");
        return;
    }

    File file = LittleFS.open("/image.bin", "r");

    if (!file)
    {
        Serial.println("Foto nao encontrada!");
        return;
    }

    if (file.size() != IMAGE_SIZE)
    {
        Serial.printf("Tamanho incorreto: %u\n",
                      (unsigned)file.size());
        file.close();
        return;
    }

    uint8_t *image = (uint8_t *)malloc(IMAGE_SIZE);

    if (!image)
    {
        Serial.println("Memoria insuficiente!");
        file.close();
        return;
    }

    size_t bytesRead = file.read(image, IMAGE_SIZE);
    file.close();

    if (bytesRead != IMAGE_SIZE)
    {
        Serial.println("Erro ao ler imagem!");
        free(image);
        return;
    }

    Serial.println("Imagem carregada!");

    SPI.begin(18, 19, 23, 27);
    display.init(115200);
    display.setRotation(0);

    Serial.println("Exibindo imagem...");

    display.epd2.drawImage_4G(
        image,
        2,
        0, 0,
        IMAGE_WIDTH,
        IMAGE_HEIGHT);

    display.epd2.hibernate();
    free(image);

    Serial.println("Concluido!");
}

void loop()
{
}
