# Minigame check

Manual play-through of every minigame in Free Play, by the user, with the Spanish version of the game (names as the game shows them). Build from 2026-09-26 (with the coroutine resume fix); pair games on 2026-09-28. "OK" means no fault was seen while playing it once.

## 4 players

| Minigame | Result | Notes |
| --- | --- | --- |
| Al galope | OK | |
| Tiro a la lata | OK | |
| Terror zombi | OK | |
| Montaña de regalos | OK | Unlocked "Montaña de regalos+" in Challenges |
| Obstáculos rodantes | OK | Was 40-50 fps and crashed at the race end; after the fixes of 2026-09-26 a steady 50 fps and no crash |
| ¡A por la bandera! | OK | |
| Pica y trocea | OK | Was 42-50 fps; a steady 50 fps after the vertex speed-up of 2026-09-26 |
| La pieza correcta | OK | |
| ¡Decid patata! | OK | |
| Salto de esquí | OK | |
| Cinturón de asteroides | OK | |
| Caída libre | OK | |
| Carrera rural | OK | |
| El swing perfecto | OK | |
| Guerra cósmica | OK | |
| Lluvia de plumas | OK | |
| Mii trilero | OK | |
| Cronómetro mental | OK | |
| Remates de volea | OK | |
| Laberinto mareante | OK | Was 39-41 fps; 50 fps after the vertex speed-up of 2026-09-26 |
| Mii puzle | OK | |
| ¡Ovni a la vista! | OK | |
| Helicóptero al rescate | OK | |
| Por los pelos | OK | |
| Bolas de choque | OK | |
| Tópate al topo | OK | |
| La estrella de la pista | OK | |
| ¡Puños fuera! | OK | Water fixed on 2026-09-28 (projective texture coordinates divided per pixel) |
| Carrera galáctica | OK | Confirmed by the user on 2026-09-28 after the texture fixes; was: Blue or dark blue texture errors on the space background for about one second. 50 fps |
| Fotos perrunas | OK | Black photos with interference fixed on 2026-09-28 (EFB copies written to RAM) |
| Paso a paso | OK | |
| Saltos selváticos | OK | Mii faces fixed on 2026-09-28 (texture cache invalidation) |
| Bebés llorones | OK | Confirmed by the user on 2026-09-28; was: After switching windows at the end, clicks were no longer detected |
| Como pez en el agua | OK | |
| Mii a la carta | OK | |
| Pirotecnia al azar | OK | |
| Ataque sobre raíles | OK | |
| Domina las dominadas | OK | |
| La distancia justa | OK | |
| Rodeo al volante | OK | |
| El vagón del destino | OK | Played on 2026-09-28 |

## 1 vs 3

| Minigame | Result | Notes |
| --- | --- | --- |
| Tensión en la piscina | OK | |
| Pequeño saltamontes | OK | |
| Frutas voladoras | OK | |
| Clase de aeróbic | OK | |
| El escondite | OK | |

## 1 vs 1

| Minigame | Result | Notes |
| --- | --- | --- |
| Viajeros al tren | OK | |
| Sintonízate | OK | |
| ¡A por la bandera! | OK | |
| ¡Árbol va! | OK | |
| Bote a bote | OK | Mii faces fixed on 2026-09-28 (texture cache invalidation) |
| Laberinto inclinable | OK | |
| Pizza a domicilio | OK | |
| Tiradores de élite | OK | |
| Memoria frutal | OK | |
| De vuelta al redil | OK | |
| Ases del aire | OK | |

## Pair games

Played by the user on 2026-09-28 with build 2609-002 (v0.1.1-alpha): 22 pair minigames, all playable to the end.

| Minigame | Result | Notes |
| --- | --- | --- |
| Lluvia de frutas | OK | |
| ¡Canastas! | OK | Mii icons fixed on 2026-09-28 (texture cache invalidation) |
| ¡Explotad los globos! | OK | |
| Ensamblaje robótico | OK | |
| El puente movedizo | OK | |
| La bici voladora | OK | |
| Río abajo | OK | |
| Ovejas descarriadas | OK | |
| Las puertas del pánico | OK | Mii face fixed on 2026-09-28 (texture cache invalidation) |
| La casa encantada | OK | |
| Pesca sincronizada | OK | |
| Huida en vagoneta | OK | |
| Objetivo: la Tierra | OK | |
| Trineo veloz | OK | |
| Los cinco tréboles | OK | |
| Atrapad al ratón | OK | |
| La comba | OK | |
| Almacén de plátanos | OK | |
| Tiro a la lata | OK | |
| El templo maldito | OK | |
| Las anillas | OK | |
| Laberinto en equipo | OK | |

## Summary

Every minigame in every Free Play list (4 players, 1 vs 3, 1 vs 1 and pairs) has been played by hand: 79 entries, all 79 without faults (confirmed by the user on 2026-09-28).

Every game mode has also been entered and tried by hand (not full games) without faults (2026-09-28).

## Other observations

- Moving the game window feels laggy.

- Menu sounds: the + and - scrolling sound in the Free Play minigame list matches Dolphin (checked by the user on 2026-09-28).
- Mii faces: the user's Mii shows lips it does not have, in the HUD icons of ¡Canastas! and in game in Las puertas del pánico; Saltos selváticos and Bote a bote also change faces. Probably one shared fault in how Mii face parts (mouth, eyes, eyebrows) are chosen or drawn. Some female guest Miis look masculine. Abby, described by Mii wikis as blonde with blue eyes and slim eyebrows, showed brown hair and marked eyebrows in the Mii selection. Dolphin shows every face correctly (checked by the user on 2026-09-28); the large 3D faces in the Mii selection are also right in our build. Cause found on 2026-09-28: the texture cache ignored the game's texture memory invalidation within a frame, so faces made in the same frame reused the parts (eyes, eyebrows, mouth) of another Mii. Fixed and confirmed by the user in these minigames.
- The cursor leaves a trail, also in menus. Whether the game draws it is not yet known.
