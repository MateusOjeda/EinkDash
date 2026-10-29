#pragma once

// Codigo WMO da Open-Meteo -> ID do recurso em weather_icons.bin (WIC2).
// isDay: valor current.is_day, 1=dia, 0=noite; para previsao diaria passe true.
// small: false=128x112, true=48x48.
// Nao exige bibliotecas nem altera WeatherManager.
#include <stdint.h>

inline uint16_t weatherIconId(int weatherCode, bool isDay, bool small) {
    uint16_t id = 102; // fallback: nublado
    switch (weatherCode) {
        case 0:
            id = isDay ? 100 : 109; break;
        case 1:
            id = isDay ? 116 : 110; break;
        case 2:
            id = isDay ? 101 : 110; break;
        case 3:
            id = isDay ? 103 : 112; break;
        case 45:
        case 48:
            id = 104; break;
        case 51:
        case 53:
        case 55:
            id = 105; break;
        case 56:
        case 57:
        case 66:
        case 67:
            id = 115; break;
        case 61:
        case 63:
            id = 106; break;
        case 65:
        case 82:
            id = 113; break;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            id = 111; break;
        case 80:
        case 81:
            id = 107; break;
        case 95:
        case 97:
            id = 108; break;
        case 96:
        case 99:
            id = 114; break;
        default: break;
    }
    return id + (small ? 100 : 0);
}
