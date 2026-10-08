#include "texts.h"

#include <cstring>

namespace {

constexpr Texts kEnglish = {
    "Game",
    "Settings",
    "Saves",
    "Controls",
    "Licenses",
    "Ready to play.",
    "Choose your copy of Wii Party to install it.",
    "Play",
    "Options",
    "Full screen",
    "Screenshot",
    "Frame capture",
    "An update is ready",
    "This launcher brings a newer version of the game. Install it to rebuild the game; your saves and settings stay as they are.",
    "Update",
    "Installing translates the game from your own disc into a native program. Missing tools are downloaded from their official sites; nothing from the game is downloaded and nothing leaves your computer. It takes about 10 minutes on a 6-core processor.",
    "Install folder",
    "Change",
    "Disc",
    "No disc chosen",
    "Already extracted",
    "Choose",
    "Wii disc images",
    "Install",
    "Installing",
    "Cancel",
    "Try again",
    "Back",
    "Show details",
    "Hide details",
    "About %1 minutes left",
    "About a minute left",
    "Choose your copy of Wii Party first.",
    "Needs about %1 GB of free space. That drive has %2 GB free.",
    "Not enough free space: the installation needs about %1 GB and that drive has %2 GB free.",
    "The install folder path is too long for the compiler. Choose a folder with a shorter path, for example C:\\Games.",
    "%1: %2, ready",
    "%1: missing or not compatible, it will be downloaded",
    "Compiling with %1 parallel jobs.",
    "Prepare the build files",
    "Find %1",
    "Download %1",
    "Check %1",
    "Unpack %1",
    "Finish %1",
    "Extract the disc",
    "Download SDL3 for gamepads",
    "Unpack the code modules",
    "Translate the main executable",
    "Translate the code modules",
    "Link the modules to the main executable",
    "Translate the audio microcode",
    "Configure the build",
    "Compile the game",
    "The game is installed.",
    "The installation stopped at a failed step. The details show the error.",
    "The installation was cancelled.",
    "Missing tool: %1",
    "%1 is missing. Install the build tools with your system's package manager and try again. Debian/Ubuntu: sudo apt install g++ cmake ninja-build python3 curl. Fedora: sudo dnf install gcc-c++ cmake ninja-build python3 curl. Arch: sudo pacman -S --needed gcc cmake ninja python curl.",
    "%1 does not match its published checksum and was deleted. Try again.",
    "Could not write the build files to %1.",
    "Build files written to %1.",
    "The game could not be started.",
    "PC improvements, kept apart from the translated game. They apply the next time the game starts; F10 opens them while playing, where most apply at once.",
    "Video",
    "Input",
    "System",
    "Audio",
    "Saves",
    "Copies of your progress, made automatically. Restoring one replaces the current save, which is first kept as a new copy.",
    "No backups yet. The first one is made the next time the game starts with a save.",
    "Automatic backups are off. Turn them on in Settings.",
    "Open folder",
    "Restore",
    "%1 KB",
    "Restore this backup?",
    "The backup from %1 replaces the current save. Close the game first; the current save is kept as a new backup.",
    "The backup from %1 is now the save.",
    "The save already matches the backup from %1.",
    "The backup could not be restored: %1",
    "Keyboard and mouse for Wii Remote 1. Click a binding, then press a key, click or scroll to add it. F10, F11 and F12 are reserved.",
    "Reset to defaults",
    "Clear",
    "Not set",
    "Press a key, click or scroll",
    "F10, F11 and F12 are reserved",
    "Wii Party Recomp is free software under the GPL-3.0-or-later. The launcher includes the libraries below, each under its own license.",
    "Wii Party Recomp",
    "Third-party notices",
    "GCC runtime",    "Launcher",
    "Check for launcher updates",
    "When it opens, asks GitHub whether a newer launcher exists. Nothing else is sent.",
    "Launcher %1 is available",
    "It is downloaded from GitHub, checked and restarted. Then the game is updated from it.",
    "Update launcher",
    "Downloading…",
    "The launcher could not be updated: %1",
    "About %1 minutes. You can keep playing the current version while it builds.",
    "About %1 minutes, because this update rebuilds the whole game. You can keep playing the current version while it builds.",
    "Play current version",
    "The new version will be used the next time you press Play.",
    "Update the calls between the modules and the executable",
    "Europe (PAL)",
    "Americas (NTSC)",
    "Japan (NTSC)",
    "Korea (NTSC)",
    "Release notes",
    "Repair installation",
    "Translates and compiles the game again from scratch. Saves and settings are kept.",
    "Repair",
    "Repair the installation?",
    "The game is translated and compiled again from the extracted disc. It takes about %1 minutes; you can keep playing the current version meanwhile.",
    "Desktop shortcut",
    "Adds a desktop shortcut that opens this launcher, or the game directly.",
    "Create",
    "Shortcut created on the desktop.",
    "The shortcut could not be created: %1",
    "Textures",
    "Disc versions",
    "Each version keeps its own saves. Switching to a version that is already built is instant.",
    "In use",
    "Use",
    "Will be built when used",
    "Add another version",
    "Choose the disc of another region, such as the NTSC one. Only the parts that differ are built again.",
    "The version could not be changed. Close the game and try again.",
    "Check the disc",
    "Disc checked: %1",
    "The disc is not in the Redump list; the installation continues.",
    "The disc image is damaged or modified. Dump it again from your console.",
    "Switch to the new disc version",
    "This disc version is already installed.",
    "Connected gamepads",
    "No gamepad is connected. Joy-Con, Xbox, PlayStation and other gamepads appear here.",
    "Wii Remote %1",
    "Report a problem",
    "Saves a report with the logs, versions and settings, and opens the issues page on GitHub to attach it.",
    "Create report",
    "Report saved in %1.",
    "What's new in v%1",
    "Close",
    "Screenshots",
    "Opens the folder with the screenshots taken with F9.",
    "Replacement texture packs, with Dolphin's file names. Each pack is a folder in games/wiiparty/textures/load; a pack that is off is kept without being used.",
    "No texture packs yet. Put each pack in its own folder inside the load folder.",
    "%1 files, %2",
    "Custom textures are off in Settings, so no pack is used.",
    "Launcher",
    "Game",
    "Offline mode",
    "The launcher never connects to the internet: no update check and no tool downloads. Missing tools must come from the offline pack next to the launcher or be installed by you.",
    "%1 is missing and offline mode is on. Put the offline tools pack next to the launcher, install %1 yourself, or turn offline mode off in Settings.",
    "%1 %2 (offline pack)",
};

constexpr Texts kSpanish = {
    "Juego",
    "Opciones",
    "Partidas",
    "Controles",
    "Licencias",
    "Listo para jugar.",
    "Elige tu copia de Wii Party para instalarlo.",
    "Jugar",
    "Opciones",
    "Pantalla completa",
    "Captura",
    "Captura de fotograma",
    "Hay una actualización",
    "Este lanzador trae una versión más nueva del juego. Instálala para recompilarlo; tus partidas y opciones se conservan.",
    "Actualizar",
    "La instalación traduce el juego desde tu propio disco a un programa nativo. Las herramientas que falten se descargan de sus webs oficiales; no se descarga nada del juego y nada sale de tu ordenador. Tarda unos 10 minutos con un procesador de 6 núcleos.",
    "Carpeta de instalación",
    "Cambiar",
    "Disco",
    "Ningún disco elegido",
    "Ya extraído",
    "Elegir",
    "Imágenes de disco de Wii",
    "Instalar",
    "Instalando",
    "Cancelar",
    "Reintentar",
    "Volver",
    "Mostrar detalles",
    "Ocultar detalles",
    "Quedan unos %1 minutos",
    "Queda un minuto",
    "Elige primero tu copia de Wii Party.",
    "Necesita unos %1 GB libres. Esa unidad tiene %2 GB libres.",
    "No hay espacio suficiente: la instalación necesita unos %1 GB y esa unidad tiene %2 GB libres.",
    "La ruta de la carpeta de instalación es demasiado larga para el compilador. Elige una carpeta con una ruta más corta, por ejemplo C:\\Juegos.",
    "%1: %2, lista",
    "%1: falta o no es compatible, se descargará",
    "Compilando con %1 procesos en paralelo.",
    "Preparar los archivos de compilación",
    "Buscar %1",
    "Descargar %1",
    "Comprobar %1",
    "Descomprimir %1",
    "Terminar %1",
    "Extraer el disco",
    "Descargar SDL3 para los mandos",
    "Descomprimir los módulos de código",
    "Traducir el ejecutable principal",
    "Traducir los módulos de código",
    "Enlazar los módulos con el ejecutable",
    "Traducir el microcódigo de audio",
    "Preparar la compilación",
    "Compilar el juego",
    "El juego está instalado.",
    "La instalación se ha detenido en un paso con error. Los detalles muestran el fallo.",
    "La instalación se ha cancelado.",
    "Falta la herramienta: %1",
    "Falta %1. Instala las herramientas de compilación con el gestor de paquetes del sistema y vuelve a intentarlo. Debian/Ubuntu: sudo apt install g++ cmake ninja-build python3 curl. Fedora: sudo dnf install gcc-c++ cmake ninja-build python3 curl. Arch: sudo pacman -S --needed gcc cmake ninja python curl.",
    "%1 no coincide con su suma de comprobación publicada y se ha borrado. Vuelve a intentarlo.",
    "No se han podido escribir los archivos de compilación en %1.",
    "Archivos de compilación escritos en %1.",
    "No se ha podido abrir el juego.",
    "Mejoras de PC, separadas del juego traducido. Se aplican la próxima vez que se abra el juego; F10 las abre durante la partida, donde casi todas se aplican al momento.",
    "Vídeo",
    "Entrada",
    "Sistema",
    "Sonido",
    "Partidas",
    "Copias de tu progreso, hechas automáticamente. Restaurar una sustituye la partida actual, que antes se guarda como una copia nueva.",
    "Aún no hay copias. La primera se hará la próxima vez que el juego se abra con una partida guardada.",
    "Las copias automáticas están desactivadas. Actívalas en Opciones.",
    "Abrir carpeta",
    "Restaurar",
    "%1 KB",
    "¿Restaurar esta copia?",
    "La copia del %1 sustituye a la partida actual. Cierra antes el juego; la partida actual se guarda como una copia nueva.",
    "La copia del %1 es ahora la partida.",
    "La partida ya coincide con la copia del %1.",
    "No se ha podido restaurar la copia: %1",
    "Teclado y ratón para el Wii Remote 1. Pulsa una asignación y después una tecla, un clic o la rueda para añadirla. F10, F11 y F12 están reservadas.",
    "Restablecer",
    "Quitar",
    "Sin asignar",
    "Pulsa una tecla, haz clic o gira la rueda",
    "F10, F11 y F12 están reservadas",
    "Wii Party Recomp es software libre bajo la GPL-3.0-or-later. El lanzador incluye las bibliotecas de abajo, cada una con su propia licencia.",
    "Wii Party Recomp",
    "Avisos de terceros",
    "Runtime de GCC",    "Lanzador",
    "Buscar actualizaciones del lanzador",
    "Al abrirse, pregunta a GitHub si hay un lanzador más nuevo. No se envía nada más.",
    "El lanzador %1 está disponible",
    "Se descarga de GitHub, se comprueba y se reinicia. Después actualiza el juego.",
    "Actualizar lanzador",
    "Descargando…",
    "No se pudo actualizar el lanzador: %1",
    "Unos %1 minutos. Puedes seguir jugando a la versión actual mientras se compila.",
    "Unos %1 minutos, porque esta actualización recompila todo el juego. Puedes seguir jugando a la versión actual mientras se compila.",
    "Jugar a la versión actual",
    "La nueva versión se usará la próxima vez que pulses Jugar.",
    "Actualizar las llamadas entre los módulos y el ejecutable",
    "Europa (PAL)",
    "América (NTSC)",
    "Japón (NTSC)",
    "Corea (NTSC)",
    "Notas de la versión",
    "Reparar instalación",
    "Vuelve a traducir y compilar el juego desde cero. Se conservan las partidas y las opciones.",
    "Reparar",
    "¿Reparar la instalación?",
    "El juego se vuelve a traducir y compilar desde el disco extraído. Tarda unos %1 minutos; mientras tanto puedes seguir jugando a la versión actual.",
    "Acceso directo en el escritorio",
    "Añade en el escritorio un acceso directo que abre este lanzador o directamente el juego.",
    "Crear",
    "Acceso directo creado en el escritorio.",
    "No se pudo crear el acceso directo: %1",
    "Texturas",
    "Versiones del disco",
    "Cada versión tiene sus propias partidas. Cambiar a una versión ya compilada es instantáneo.",
    "En uso",
    "Usar",
    "Se compilará al usarla",
    "Añadir otra versión",
    "Elige el disco de otra región, como el NTSC. Solo se vuelve a compilar lo que cambia.",
    "No se pudo cambiar de versión. Cierra el juego y vuelve a intentarlo.",
    "Comprobar el disco",
    "Disco comprobado: %1",
    "El disco no está en la lista de Redump; la instalación continúa.",
    "La imagen del disco está dañada o modificada. Vuelve a volcarla desde tu consola.",
    "Cambiar a la nueva versión del disco",
    "Esta versión del disco ya está instalada.",
    "Mandos conectados",
    "No hay ningún mando conectado. Aquí aparecen los Joy-Con y los mandos de Xbox, PlayStation y otros.",
    "Wii Remote %1",
    "Informar de un problema",
    "Guarda un informe con los registros, las versiones y las opciones, y abre la página de problemas de GitHub para adjuntarlo.",
    "Crear informe",
    "Informe guardado en %1.",
    "Novedades de la v%1",
    "Cerrar",
    "Capturas de pantalla",
    "Abre la carpeta con las capturas hechas con F9.",
    "Packs de texturas de reemplazo, con los nombres de archivo de Dolphin. Cada pack es una carpeta en games/wiiparty/textures/load; un pack desactivado se conserva sin usarse.",
    "Aún no hay packs de texturas. Pon cada pack en su propia carpeta dentro de la carpeta load.",
    "%1 archivos, %2",
    "Las texturas personalizadas están desactivadas en Opciones, así que no se usa ningún pack.",
    "Lanzador",
    "Juego",
    "Modo sin conexión",
    "El lanzador no se conecta nunca a Internet: no busca actualizaciones ni descarga herramientas. Las que falten deben venir del paquete sin conexión junto al lanzador o instalarlas tú.",
    "Falta %1 y el modo sin conexión está activado. Pon el paquete de herramientas sin conexión junto al lanzador, instala %1 tú mismo o desactiva el modo sin conexión en Ajustes.",
    "%1 %2 (paquete sin conexión)",
};

struct Detail {
    const char* key;
    const char* english;
    const char* spanish;
};

constexpr Detail kDetails[] = {
    {"video.scale", "Renders the game at a multiple of the console's resolution.", "Dibuja el juego a un múltiplo de la resolución de la consola."},
    {"video.fullscreen", "Starts in borderless full screen. F11 also switches it while playing.", "Empieza en pantalla completa sin bordes. F11 también la cambia durante la partida."},
    {"video.copy_filter", "The soft vertical filter the game uses on the console. Turn it off for a sharper picture.",
     "El suave filtro vertical que usa el juego en la consola. Desactívalo para una imagen más nítida."},
    {"video.custom_textures", "Uses the replacement textures in games/wiiparty/textures/load, with Dolphin's file names.",
     "Usa las texturas de reemplazo de games/wiiparty/textures/load, con los nombres de archivo de Dolphin."},
    {"video.dump_textures", "Saves each texture the game uses to games/wiiparty/textures/dump, for making texture packs.",
     "Guarda cada textura que usa el juego en games/wiiparty/textures/dump, para crear packs de texturas."},
    {"input.gamepads", "Up to four gamepads act as Wii Remotes 1 to 4.", "Hasta cuatro mandos funcionan como los Wii Remote 1 a 4."},
    {"input.auto_grip", "Holds the emulated Wii Remote sideways in the minigames that ask for it.",
     "Sujeta el Wii Remote emulado en horizontal en los minijuegos que lo piden."},
    {"input.joycon_pairs", "A left and a right Joy-Con act as one Wii Remote. Off, each Joy-Con is its own Wii Remote, held sideways. Applies the next time the game opens.",
     "Un Joy-Con izquierdo y uno derecho funcionan como un solo Wii Remote. Desactivado, cada Joy-Con es un Wii Remote, en horizontal. Se aplica al abrir el juego."},
    {"input.wake_on_mouse", "Moving the mouse reconnects a Wii Remote the game disconnected for inactivity.",
     "Mover el ratón reconecta un Wii Remote que el juego desconectó por inactividad."},
    {"input.hide_cursor", "Hides the system cursor over the game; the game draws its own pointer.",
     "Oculta el cursor del sistema sobre el juego; el juego dibuja su propio puntero."},
    {"system.interface_language", "The language of this launcher and of the F10 menu.", "El idioma de este lanzador y del menú F10."},
    {"system.language", "The language of the virtual console, which the game follows.", "El idioma de la consola virtual, que el juego sigue."},
    {"system.pal60", "The console's own PAL60 setting. Off runs the game at 50 Hz.", "El ajuste PAL60 de la propia consola. Desactivado, el juego va a 50 Hz."},
    {"system.skip_notices", "Presses A on the Wii Remote strap notice at start, as a player would.",
     "Pulsa A en el aviso de la correa del Wii Remote al empezar, como haría un jugador."},
    {"system.options_menu", "F10, or Start with Guide or Back on a gamepad, opens these settings over the game.",
     "F10, o Start con Guide o Back en un mando, abre estas opciones sobre el juego."},
    {"audio.mute", "Silences the game's sound.", "Quita el sonido del juego."},
    {"saves.backups", "Copies of the save to keep. One is made at every start when the save changed, and at most once a minute after the game saves.",
     "Copias de la partida que se guardan. Se hace una al abrir el juego si la partida ha cambiado, y como mucho una por minuto después de que el juego guarde."},
};

struct Action {
    const char* key;
    const char* english;
    const char* spanish;
    const char* english_detail;
    const char* spanish_detail;
};

constexpr Action kActions[] = {
    {"a", "A button", "Botón A", nullptr, nullptr},
    {"b", "B button", "Botón B", nullptr, nullptr},
    {"one", "1 button", "Botón 1", nullptr, nullptr},
    {"two", "2 button", "Botón 2", nullptr, nullptr},
    {"plus", "+ button", "Botón +", nullptr, nullptr},
    {"minus", "- button", "Botón -", nullptr, nullptr},
    {"home", "HOME button", "Botón HOME", nullptr, nullptr},
    {"up", "D-pad up", "Cruceta arriba", "Left on screen when the remote is held sideways.", "Izquierda en pantalla con el mando en horizontal."},
    {"down", "D-pad down", "Cruceta abajo", "Right on screen when the remote is held sideways.", "Derecha en pantalla con el mando en horizontal."},
    {"left", "D-pad left", "Cruceta izquierda", "Down on screen when the remote is held sideways.", "Abajo en pantalla con el mando en horizontal."},
    {"right", "D-pad right", "Cruceta derecha", "Up on screen when the remote is held sideways.", "Arriba en pantalla con el mando en horizontal."},
    {"shake", "Shake", "Agitar", nullptr, nullptr},
    {"swing_up", "Swing up", "Mover hacia arriba", "The mouse wheel gives a short swing.", "La rueda del ratón da un movimiento corto."},
    {"swing_down", "Swing down", "Mover hacia abajo", "The mouse wheel gives a short swing.", "La rueda del ratón da un movimiento corto."},
    {"tilt_left", "Tilt left", "Inclinar a la izquierda", nullptr, nullptr},
    {"tilt_right", "Tilt right", "Inclinar a la derecha", nullptr, nullptr},
    {"tilt_up", "Tilt up", "Inclinar hacia arriba", nullptr, nullptr},
    {"tilt_down", "Tilt down", "Inclinar hacia abajo", nullptr, nullptr},
    {"grip", "Swap upright and sideways grip", "Cambiar entre agarre vertical y horizontal", nullptr, nullptr},
    {"screenshot", "Screenshot", "Captura de pantalla", "Saves the picture of the game as PNG in the screenshots folder.",
     "Guarda la imagen del juego como PNG en la carpeta screenshots."},
};

bool g_spanish = false;

const Action* find_action(const char* key) {
    for (const Action& action : kActions) {
        if (std::strcmp(action.key, key) == 0) {
            return &action;
        }
    }
    return nullptr;
}

}

void use_spanish(bool spanish) {
    g_spanish = spanish;
}

const Texts& texts() {
    return g_spanish ? kSpanish : kEnglish;
}

std::string format(const char* pattern, const std::string& first, const std::string& second) {
    std::string result;
    for (const char* c = pattern; *c; c++) {
        if (c[0] == '%' && (c[1] == '1' || c[1] == '2')) {
            result += c[1] == '1' ? first : second;
            c++;
        } else {
            result += *c;
        }
    }
    return result;
}

const char* option_detail(const std::string& key) {
    for (const Detail& detail : kDetails) {
        if (key == detail.key) {
            return g_spanish ? detail.spanish : detail.english;
        }
    }
    return "";
}

const char* group_name(const std::string& key) {
    const Texts& t = texts();
    std::string group = key.substr(0, key.find('.'));
    if (group == "video") {
        return t.group_video;
    }
    if (group == "input") {
        return t.group_input;
    }
    if (group == "audio") {
        return t.group_audio;
    }
    if (group == "saves") {
        return t.group_saves;
    }
    return t.group_system;
}

const char* action_name(const char* key) {
    const Action* action = find_action(key);
    return action ? (g_spanish ? action->spanish : action->english) : key;
}

const char* action_detail(const char* key) {
    const Action* action = find_action(key);
    if (!action || !action->english_detail) {
        return nullptr;
    }
    return g_spanish ? action->spanish_detail : action->english_detail;
}
