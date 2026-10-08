#include "wp/ui_text.h"

#include <atomic>
#include <cstring>

namespace wp::ui {

namespace {

constexpr Label kEnglishLabels[] = {
    {"video.scale", "Internal resolution"},
    {"video.fullscreen", "Full screen"},
    {"video.copy_filter", "Anti-flicker filter"},
    {"video.custom_textures", "Custom textures"},
    {"video.dump_textures", "Dump textures"},
    {"input.gamepads", "Gamepads"},
    {"input.auto_grip", "Automatic grip"},
    {"input.joycon_pairs", "Join Joy-Con pairs"},
    {"input.wake_on_mouse", "Reconnect with the mouse"},
    {"input.hide_cursor", "Hide the mouse cursor"},
    {"system.interface_language", "Interface language"},
    {"system.language", "Console language"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.skip_notices", "Skip the strap notice"},
    {"system.options_menu", "Options menu (F10)"},
    {"audio.mute", "Mute"},
    {"saves.backups", "Save backups kept"},
};

constexpr Label kSpanishLabels[] = {
    {"video.scale", "Resolución interna"},
    {"video.fullscreen", "Pantalla completa"},
    {"video.copy_filter", "Filtro anti-parpadeo"},
    {"video.custom_textures", "Texturas personalizadas"},
    {"video.dump_textures", "Volcar texturas"},
    {"input.gamepads", "Mandos"},
    {"input.auto_grip", "Agarre automático"},
    {"input.joycon_pairs", "Unir parejas de Joy-Con"},
    {"input.wake_on_mouse", "Reconectar con el ratón"},
    {"input.hide_cursor", "Ocultar el cursor del ratón"},
    {"system.interface_language", "Idioma de la interfaz"},
    {"system.language", "Idioma de la consola"},
    {"system.pal60", "60 Hz (PAL60)"},
    {"system.skip_notices", "Saltar el aviso de la correa"},
    {"system.options_menu", "Menú de opciones (F10)"},
    {"audio.mute", "Silenciar"},
    {"saves.backups", "Copias de la partida"},
};

constexpr Text kEnglish{"FPS",
                        "DSP",
                        "NATIVE",
                        "INTERP",
                        "OFF",
                        "Options",
                        "Up/Down: choose     Left/Right/Enter: change     Esc: close",
                        "applies on restart",
                        "On",
                        "Off",
                        "Native",
                        "System",
                        kEnglishLabels,
                        sizeof(kEnglishLabels) / sizeof(kEnglishLabels[0])};

constexpr Text kSpanish{"FPS",
                        "DSP",
                        "NATIVO",
                        "INTERP",
                        "APAGADO",
                        "Opciones",
                        "Arriba/Abajo: elegir     Izquierda/Derecha/Intro: cambiar     Esc: cerrar",
                        "se aplica al reiniciar",
                        "Sí",
                        "No",
                        "Nativa",
                        "Sistema",
                        kSpanishLabels,
                        sizeof(kSpanishLabels) / sizeof(kSpanishLabels[0])};

std::atomic<const Text*> g_text{&kEnglish};

}

void set_language(Language language) {
    switch (language) {
    case Language::English:
        g_text = &kEnglish;
        break;
    case Language::Spanish:
        g_text = &kSpanish;
        break;
    }
}

const Text& text() {
    return *g_text.load();
}

const char* label(const char* key) {
    const Text& current = text();
    for (size_t i = 0; i < current.label_count; i++) {
        if (std::strcmp(current.labels[i].key, key) == 0) {
            return current.labels[i].text;
        }
    }
    return key;
}

}
