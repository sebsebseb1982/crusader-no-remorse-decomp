# Modifications et améliorations par rapport au jeu original

Ce document liste tout ce que le portage C++20/SDL2 ajoute ou change par rapport à *Crusader: No Remorse* (Origin, 1995, exécutable DOS). Pour chaque point, il donne les règles métier telles qu'elles sont codées.

Le reste du moteur reproduit l'original : chaque fonction porte une étiquette `@orig <segment:offset>` qui renvoie à la routine de l'exécutable. Les ajouts propres au portage portent `@orig none` ou le commentaire « Port addition ». Ce document est construit à partir de ces marqueurs, de la configuration (`src/core/game_config.hpp`) et de l'historique git.

---

## Sommaire

1. [Écran « Améliorations »](#1-écran--améliorations-)
2. [Support manette](#2-support-manette)
3. [Déplacement directionnel (stick gauche)](#3-déplacement-directionnel-stick-gauche)
4. [Visée directionnelle (stick droit)](#4-visée-directionnelle-stick-droit)
5. [Caméra qui suit le joueur](#5-caméra-qui-suit-le-joueur)
6. [Résolution native](#6-résolution-native)
7. [Zoom](#7-zoom)
8. [Sauvegarde automatique](#8-sauvegarde-automatique)
9. [Course permanente (toggle run)](#9-course-permanente-toggle-run)
10. [Navigation au clavier ou à la manette sur le digicode](#10-navigation-au-clavier-ou-à-la-manette-sur-le-digicode)
11. [Plein écran, fenêtre, icône](#11-plein-écran-fenêtre-icône)
12. [Lignes de version dans le menu principal](#12-lignes-de-version-dans-le-menu-principal)
13. [Support de la version US 1.23](#13-support-de-la-version-us-123)
14. [Installation depuis une image ISO](#14-installation-depuis-une-image-iso)
15. [Démarrage, ligne de commande et outils](#15-démarrage-ligne-de-commande-et-outils)
16. [Fichier de configuration `CRUSADER.CFG`](#16-fichier-de-configuration-crusadercfg)
17. [Différences de comportement assumées](#17-différences-de-comportement-assumées)
18. [Portabilité et distribution](#18-portabilité-et-distribution)
19. [Éditeur de niveaux](#19-éditeur-de-niveaux)

---

## 1. Écran « Améliorations »

**Fichiers :** `src/engine/ui/enhancements_screen.*`, `src/engine/ui/options_screen.*`

L'original n'a pas d'équivalent. Cet écran regroupe toutes les options ajoutées par le portage.

**Accès**
- Dans l'écran Options, un bouton texte **« AMELIORATIONS »** (EN : « ENHANCEMENTS ») avec une icône « + » se trouve sur la ligne 9, à gauche de « Quitter », dans les couleurs du bouton Quitter.
- Gauche depuis « Quitter » ou droite depuis le bouton passe de l'un à l'autre. Entrée l'ouvre.
- Si les polices ne sont pas chargées, le bouton n'apparaît pas.

**Aspect**
- L'écran est effacé. Une fenêtre à double cadre rouge (couleur `0x57`, coins coupés sur 3 pixels comme les boutons d'origine) occupe le centre (100,84)–(539,426).
- Le titre est en vert (`0x0C`) avec l'icône « + ».
- 10 lignes séparées de 27 px. La ligne qui a le focus est en vert vif (`0x0C`), les autres en vert sombre (`0x75`).
- Une option on/off affiche son nom à gauche et sa valeur à droite (« MARCHE » / « ARRET », EN « ON » / « OFF »).

| # | Ligne (FR / EN) | Effet | Clé CFG |
|---|---|---|---|
| 0 | MANETTE / CONTROLLER | Ouvre l'écran de configuration de la manette | — |
| 1 | SUIVI CAMERA / CAMERA FOLLOW | Bascule le suivi caméra | `followplayer` |
| 2 | MODE DIRECTIONNEL / DIRECTIONAL MOVE | Bascule le déplacement directionnel | `directionalmove` |
| 3 | VISEE DIRECTIONNELLE / DIRECTIONAL AIM | Bascule la visée directionnelle | `directionalaim` |
| 4 | DEGAINE EN VISANT / DRAW ON AIM | Dégaîne l'arme quand le stick de visée est poussé | `drawonaim` |
| 5 | RESOLUTION NATIVE / NATIVE RESOLUTION | Bascule la résolution native | `nativeresolution` |
| 6 | ZOOM | Fait défiler AUTO → x1 → x1,5 → x2 → … → xN → AUTO | `viewzoom` |
| 7 | SAUVEGARDE AUTO / AUTOSAVE | Bascule la sauvegarde automatique | `autosave` |
| 8 | COURSE PERMANENTE / TOGGLE RUN | Bascule le mode course permanente | `togglerun` |
| 9 | RETOUR / BACK | Ferme l'écran | — |

**Commandes**
- Haut ou `8`, bas ou `2` : change de ligne, en butée aux extrémités (pas de bouclage).
- Entrée : active la ligne.
- Échap : ferme l'écran.
- Souris : un clic sur une ligne lui donne le focus et l'active.
- Tant que l'écran Manette est ouvert, il reçoit tous les événements jusqu'à sa fermeture.
- Les changements modifient la configuration en mémoire immédiatement. Ils sont écrits dans `CRUSADER.CFG` à la fermeture de l'écran Options, comme les options d'origine.

---

## 2. Support manette

**Fichiers :** `src/core/pad_config.hpp`, `src/platform/sdl_gamepad.*`, `src/engine/ui/gamepad_screen.*`

L'original ne gère que le joystick PC, que le portage ne reproduit pas. Le portage prend en charge toute manette reconnue par `SDL_GameController` (Xbox et compatibles), avec branchement et débranchement à chaud.
Dans la fenêtre Améliorations, les vibrations des manettes peuvent être activées ou désactivées et leur intensité réglée par pas de 10 % avec les flèches gauche/droite (ou un clic sur la ligne). La manette vibre brièvement pour illustrer chaque niveau choisi. Les explosions sont modulées selon leur taille ; le tir du lance-roquettes provoque une vibration soutenue, légèrement moins forte qu'une grosse explosion. Une vibration accompagne aussi les dégâts réellement reçus par le personnage contrôlé, mais pas ceux absorbés par un bouclier.

### 2.1 Principe
Chaque entrée de la manette est **traduite en touche (ou combinaison de touches) des contrôles d'origine**. Le moteur reçoit donc des événements clavier ou souris ordinaires et n'a rien de spécifique à la manette.
- Pour une combinaison, le modificateur (Ctrl, Alt, Maj) est enfoncé **en premier** et relâché **en dernier**.
- La touche relâchée est **toujours celle envoyée à l'appui**, même si un menu s'est ouvert entre-temps. Cela évite les touches « collées ».

### 2.2 Actions disponibles

| Action (FR / EN) | Touche d'origine envoyée |
|---|---|
| AUCUNE / NONE | rien |
| TIR / FIRE | Espace |
| COURIR / RUN | Maj + ↑ |
| ROULADE GAUCHE / DROITE | Ctrl + ← / → |
| PAS A GAUCHE / DROITE | Alt + ← / → |
| SE BAISSER OU LEVER | Ctrl + ↓ |
| MAJ / SHIFT | Maj seule |
| ARME PRECEDENTE / SUIVANTE | `q` / `w` |
| OBJET PRECEDENT / SUIVANT | `i` / `o` |
| UTILISER OBJET | `u` |
| TROUSSE / MEDICAL KIT | `m` |
| SAUT / JUMP | Tab |
| MENU | Échap |
| VALIDER / ENTER | Entrée |
| SELECTIONNER / SELECT | `s` (mode sélection, puis objet suivant) |
| AVANCER / RECULER / TOURNER A GAUCHE / A DROITE | ↑ / ↓ / ← / → |

### 2.3 Entrées configurables (24)
- 12 boutons : A, B, X, Y, LB, RB, LT, RT, clic stick gauche (L3), clic stick droit (R3), Start, Select.
- Les 4 directions du stick gauche, du stick droit et de la croix directionnelle.

### 2.4 Affectation par défaut

| Entrée | Action |
|---|---|
| A | Sélectionner |
| B | Saut |
| X | Valider |
| Y | Arme suivante |
| LB / RB | Roulade gauche / droite |
| LT | Aucune |
| RT | Tir |
| L3 | Courir |
| R3 | Se baisser / se lever |
| Start | Menu |
| Select | Aucune |
| Stick gauche ↑ / ↓ | Avancer / reculer |
| Stick gauche ← / → | Pas à gauche / à droite |
| Stick droit ← / → | Tourner à gauche / à droite |
| Stick droit ↑ / ↓ | rôle par défaut : visée (mouvement de souris) |
| Croix ↑ / ↓ / ← / → | Utiliser objet / trousse / objet précédent / objet suivant |

> Le tableau de `tools/portmaster/README.md` décrit une affectation plus ancienne. Le code (`PadSettings::default_buttons`) fait foi.

### 2.5 Règles d'affectation
- **Une action ne peut être que sur une seule entrée.** L'affecter à une nouvelle entrée la retire de l'ancienne.
- L'ancienne action de l'entrée choisie est désaffectée.
- Une entrée désaffectée :
  - un **bouton** ne fait plus rien (`AUCUNE`) ;
  - une **direction** reprend son **rôle par défaut** : touche fléchée pour le stick gauche et la croix, mouvement de souris pour le stick droit.
- Une direction de stick droit affectée à une action **ne déplace plus la souris** dans ce sens.

### 2.6 Seuils et réponse des sticks
- **Directions numériques** (sticks et gâchettes) : activées au-delà de **16000** / 32767, désactivées sous **10000** (hystérésis contre le rebond).
- **Stick droit en souris** :
  - zone morte réglable sur 5 niveaux : 3000, 5000, 7000, 10000, 13000 (défaut : niveau 2 = 7000) ;
  - vitesse réglable sur 5 niveaux : 300, 450, 700, 950, 1250 px/s à fond (défaut : niveau 3 = 700) ;
  - réponse **quadratique** : précise près du centre, vitesse maximale au bord ;
  - les fractions de pixel sont conservées d'une image à l'autre, puis remises à zéro quand le stick revient au centre.

### 2.7 Dans les menus
Dans les menus, boîtes de message et écrans, certaines entrées sont **fixes**, quelle que soit la configuration, pour qu'une mauvaise configuration ne bloque jamais le joueur :
- **A** = Entrée, **B** = Échap, **Start** = Échap.
- Les directions gardent leur rôle (flèches, souris) et n'envoient pas l'action qui leur est affectée.

### 2.8 Quitter le jeu
**Select + Start** maintenus ensemble ferment le jeu. C'est la seule sortie possible sur console portable (PortMaster), où `gptokeyb` n'est pas utilisé.

### 2.9 Écran « MANETTE »
- Ouvert depuis la ligne 0 de l'écran Améliorations.
- Deux colonnes, lignes espacées de 18 px :
  - à gauche, les 22 actions, puis « vitesse de visée », « zone morte », « par défaut » et « retour » ;
  - à droite, l'entrée affectée (« A », « LB », … ou « AUCUN »), ou `< n / 5 >` pour les deux réglages du stick.
- **Affecter** : un clic ou Entrée sur une action met l'écran en attente (« APPUYEZ SUR UN BOUTON »).
  - La prochaine entrée pressée sur la manette est capturée et affectée. Elle n'envoie pas de touche au jeu.
  - Échap, un clic ou **10 secondes** sans appui annulent l'attente.
- **Suppr** désaffecte l'action qui a le focus.
- Gauche/droite (ou `4`/`6`), ou un clic sur la moitié gauche/droite de la valeur, règlent vitesse et zone morte.
- « Par défaut » restaure toute la configuration.
- Hors attente, la manette pilote l'écran avec les flèches, A (Entrée) et B (Échap).
- La ligne qui avait le focus est conservée entre deux ouvertures.

### 2.10 Diagnostic
La variable d'environnement `CRUSADER_PAD_DEBUG=1` affiche la manette détectée (nom, GUID, mapping SDL) et chaque entrée brute ou traduite. Elle sert à diagnostiquer un mapping incorrect sur console.

---

## 3. Déplacement directionnel (stick gauche)

**Option :** `MODE DIRECTIONNEL` (`directionalmove`, désactivé par défaut)

L'original utilise des contrôles « tank » : haut avance, gauche/droite tournent. Avec cette option, le Silencer **marche dans la direction indiquée par le stick gauche**.

**Règles**
- Uniquement **en jeu** : ni dans un menu, ni pendant une capture de l'écran Manette.
- L'angle du stick est découpé en **8 secteurs de 45°** (sens anti-horaire depuis l'est). Chaque secteur envoie la touche du **pavé numérique** correspondante :
  - 6 → est, 9 (PgUp) → nord-est, 8 → nord, 7 (Home) → nord-ouest ;
  - 4 → ouest, 1 (End) → sud-ouest, 2 → sud, 3 (PgDn) → sud-est.
- Ce sont les déplacements absolus « directions écran » qui existent déjà dans l'original via le pavé numérique.
- **Hystérésis angulaire** : le secteur en cours est conservé tant que le stick reste à moins de **30°** de son axe.
- **Changement de secteur** : la nouvelle touche est enfoncée **avant** de relâcher l'ancienne, pour que l'avatar pivote sans s'arrêter.
- L'action **COURIR** devient **Maj seule**, maintenue avec la touche de déplacement, au lieu de Maj + ↑.
- Au changement de mode, tout ce que l'ancien mode maintenait enfoncé est relâché.
- **Strafe en visant** : tant que le stick droit vise (visée directionnelle) et que le NPC contrôlé a son arme sortie (dégainée par les actions 7 / 0xC, rangée par 0xB / 0x10), le stick strafe au lieu de donner les directions absolues du pavé. Sa direction est prise par rapport à la visée, en 4 secteurs de 90° (conservés jusqu'à 60° de leur axe), et donne les déplacements Alt de l'original, qui gardent le mode visée :
  - vers la visée : `Alt + haut`, avance lente (action `0x24`, `0x2C` avec une arme qui ne garde pas les actions) ;
  - à l'opposé : `Alt + bas`, recul lent (action `2`, ou `0x2D`) ;
  - à sa gauche / droite : **pas de côté**, `Alt + gauche / droite` (actions `0x1C` / `0x1D`).
- Sans viser, le stick garde les directions absolues, arme sortie ou non.
- En passant des directions absolues aux strafes, l'ancienne touche et Alt sont relâchés **avant** d'enfoncer la nouvelle, pour qu'aucune touche ne parte avec le mauvais modificateur.

---

## 4. Visée directionnelle (stick droit)

**Option :** `VISEE DIRECTIONNELLE` (`directionalaim`, désactivée par défaut)
**Option :** `DEGAINE EN VISANT` (`drawonaim`, activée par défaut, utilisée avec la visée directionnelle)
**Fichiers :** `src/platform/sdl_gamepad.cpp`, `src/engine/world/aim_mode.cpp` (`aim_at_angle`)

Le stick droit place directement le réticule dans la direction pointée.

**Règles**
- Uniquement en jeu. Dans ce mode, le stick droit **ne tourne plus** l'avatar et **ne déplace plus la souris**.
- Activation au-delà de 16000, désactivation sous 10000 (hystérésis).
- Avec **DEGAINE EN VISANT** activée, pousser le stick alors que l'arme est rangée la **dégaine aussitôt** (action `7`, remappée par l'arme), comme la touche de tir, et le mode visée suit. Plus besoin d'appuyer d'abord sur la gâchette. Si l'avatar est occupé (marche, roulade...), le dégainage attend qu'il soit libre tant que le stick reste poussé. Désactivée, le stick ne fait rien tant que la gâchette n'a pas dégainé l'arme, comme avant.
- Conversion d'angle : angle monde = `315° − angle écran`, car l'est de l'écran correspond à 315° dans le monde.
- À chaque mise à jour, `aim_at_angle` s'applique si toutes ces conditions sont réunies :
  - le mode visée (combat) est actif, ou l'arme est rangée, **DEGAINE EN VISANT** est activée et l'avatar libre (elle est alors dégainée) ;
  - aucune interdiction de se lever, aucun demi-tour en cours ;
  - aucune fenêtre modale ni cinématique ;
  - le joueur n'est pas en stase et contrôle bien le Silencer (NPC 1).
- Le réticule est repositionné quand l'angle change.
- Si l'avatar ne fait pas face au réticule (direction sur 16 calculée depuis ses pieds) et n'est pas occupé, il pivote avec son animation de garde : `0x2E` à genoux, `4` debout.
- Si la désignation de cible est active, la recherche de cible est relancée après le pivot.
- **Quitter le mode visée** : relâcher le stick ne le quitte pas, comme dans l'original. Marcher avec le stick gauche (sans viser) le quitte aussitôt ; un **pas de côté** ou une **roulade** le quitte le temps de l'animation. Pour ranger l'arme, double-clic **bouton droit** de la souris ; à la manette, l'action **saut** (B par défaut) range d'abord une arme dégainée, puis saute.

---

## 5. Caméra qui suit le joueur

**Option :** `SUIVI CAMERA` (`followplayer`, désactivée par défaut)
**Fichiers :** `src/engine/world/world.cpp` (`track_camera_target`, `shift_camera`, `update_follow_camera`)

**Original** : la vue ne bouge que lorsque le personnage contrôlé approche d'un bord, à moins de 100 px à gauche/droite, 140 px en haut ou 70 px en bas. Elle **saute** alors pour replacer le personnage à x=520, x=120 ou au centre.

**Avec l'option**
- Le personnage contrôlé est maintenu au **centre de la vue** (centre horizontal, centre vertical + 40 px) à chaque pas.
- **Glissement progressif** : à chaque image, la caméra parcourt environ **1/10 de l'écart**, au minimum 1 px.
- Si l'écart dépasse **200 px horizontalement ou 150 px verticalement** (téléportation, changement de carte), la caméra saute directement.
- Le suivi ne s'applique que si la cible de la caméra est le personnage contrôlé. Il est suspendu pendant les fenêtres modales.
- **Anti-tremblement** : la conversion écran → monde tronque séparément moitiés et quarts, ce qui décale la position voulue d'un ou deux pixels. Le portage teste les positions voisines (±3 unités sur chaque axe) et retient celle dont la vue est la plus proche de la vue voulue. Si aucune ne fait mieux que la vue actuelle, la caméra ne bouge pas.
- L'option est relue à chaque image : la changer prend effet immédiatement.

---

## 6. Résolution native

**Option :** `RESOLUTION NATIVE` (`nativeresolution`, **activée par défaut**)
**Fichiers :** `src/game/main.cpp` (`fit_screen`), `src/game/game.cpp` (`render`, `draw_window`), `src/engine/world/world.cpp` (`set_view_size`, `combat_rect`)

**Original** : 640×480. **Avec l'option** : la vue de jeu occupe toute la fenêtre et montre davantage le décor autour du personnage.

**Règles**
- Facteur d'échelle = le plus grand entier tel que `fenêtre / facteur` fait encore au moins 640×480. Les pixels restent carrés et nets.
- Taille logique de la vue = `max(640, largeur / facteur)` × `max(480, hauteur / facteur)`.
- La caméra garde sa place par rapport au centre : origine à `w/2` depuis la gauche et `300 + (h − 480)/2` depuis le haut. Les pixels supplémentaires se répartissent donc autour de la scène.
- La zone active (les cellules de la carte où le décor « glob » est déplié, où les œufs de monstres créent leurs PNJ et où les objets s'animent) grandit avec la vue. L'original la fixe à ±5 cellules en `s = cx + cy` et ±3 en `d = cx − cy` autour de la cellule de la caméra, ce qui déborde de l'écran 640×480. Le portage ajoute une cellule en `d` par tranche de 256 pixels de plus de chaque côté, et une en `s` par tranche de 128 pixels de plus en haut et en bas. Le monde est donc toujours complet dans la vue, et tout continue d'apparaître hors champ. En 640×480, la zone est celle de l'original.
- **La zone de combat reste celle de l'original** : le rectangle 640×480 placé dans la vue comme l'écran d'origine autour de la caméra (`World::combat_rect`). Un PNJ ne repère le joueur, ne le garde pour cible, ne tire et n'est réveillé par un tir que s'il y est, et le tir du joueur n'y choisit sa cible que là. Les caméras de surveillance (CAMERACO, CAMERANS, CAMERAEW) et les tourelles (WALLGUN) testent aussi cette zone (`Item::I_isPartlyOnScreen`, 0x007). Les tests purement visuels gardent la vue entière : saut d'images des animations, volume des sons, scripts d'effets (bulles, flammes, vapeur) et scripts qui agissent hors de la vue. Un ennemi des bandes latérales est donc visible, patrouille et s'approche, mais se comporte comme un ennemi hors écran dans l'original.
- **Restent en 640×480, centrés** : menus, écrans (options, chargement, digicode, ordinateurs…), cinématiques, barre d'état, boîtes de message, curseur et icône de sauvegarde. Le reste de l'écran est noir pour les menus et les cinématiques.
- Les coordonnées de la souris sont rapportées à ce bloc 640×480 centré.
- Option désactivée, ou `--classic` en ligne de commande : vue 640×480 d'origine, étirée à la fenêtre.
- La fenêtre s'ouvre maximisée, sauf avec `--classic` ou `--fullscreen`.
- Windows : le processus déclare la prise en charge du DPI par moniteur (`permonitorv2`), pour éviter que le système ne ré-étire la fenêtre à 125 % ou 150 %.

---

## 7. Zoom

**Option :** `ZOOM` (`viewzoom`, défaut `0` = AUTO)

Le zoom fixe la **taille maximale d'un pixel du jeu** en résolution native.

**Règles**
- `AUTO` (0) : le plus grand facteur **entier** que la fenêtre permet (voir §6).
- `x1`, `x1,5`, `x2`, `x3`, `x4` : le facteur choisi si la fenêtre le permet (taille de la fenêtre ÷ 640×480, fractions comprises), sinon le plus grand facteur entier. `x1` donne l'image la plus fine et le champ de vision le plus large.
- `x1,5` sert aux fenêtres qui ne sont pas un multiple entier de 640×480, par exemple 1360×768 (limite 1,6 : seuls x1 et x1,5 sont proposés). La vue est dessinée à fenêtre ÷ 1,5 puis étirée : certains pixels du jeu font un pixel d'écran de plus que d'autres.
- Dans l'écran Améliorations, la valeur défile AUTO → x1 → x1,5 → x2 → x3 → x4 → AUTO, en s'arrêtant au plus grand que la fenêtre courante permet (recalculé quand la fenêtre change).
- Valeur écrite : `1`, `1.5` (`1,5` est lu aussi), `2`… ; bornée entre 1 et 4 (0 = AUTO).

---

## 8. Sauvegarde automatique

**Option :** `SAUVEGARDE AUTO` (`autosave`, **activée par défaut**)
**Fichiers :** `src/game/game.cpp` (`autosave`, `request_autosave`, `draw_autosave_icon`), `src/engine/world/inventory.cpp`, `src/engine/usecode/npc_intrinsics.cpp`, `src/engine/world/teleport.cpp`, `src/engine/ui/load_screen.*`, `src/engine/ui/floppy_icon.hpp`

### 8.1 Emplacement
- Fichier dédié **`SAVEGAME.12`**, distinct des 11 emplacements du joueur et de la sauvegarde rapide (`SAVEGAME.13`).
- Nom de la sauvegarde : « SAUVEGARDE AUTO » (EN : « AUTOSAVE »).

### 8.2 Déclencheurs

| Événement | Moment de la sauvegarde |
|---|---|
| Ramassage d'une **carte magnétique** (forme `0x111`) | Immédiat |
| Porte ouverte avec une carte : un script interroge `I_hasKeycard` et la carte est possédée | Immédiat |
| **Digicode** validé avec le bon code (porte ou coffre) | Immédiat, à la fermeture du digicode |
| **Début de mission** : téléportation vers l'œuf de départ `0x1E` | Différé |
| **Fin d'une cinématique de mission** | Différé |

Une fin de cinématique ne déclenche **pas** de sauvegarde dans ces cas : cinématique de la boutique de Weasel, cinématiques d'introduction sous le menu de démarrage, générique de fin.

### 8.3 Sauvegarde différée
- Une demande différée attend **1 seconde** (60 ticks du noyau) pour laisser les scripts se stabiliser.
- Ensuite, elle attend que le joueur **puisse agir** : pas en stase, vivant, contrôlant le Silencer, aucune fenêtre modale.

### 8.4 Conditions de blocage
Aucune sauvegarde (immédiate ou différée) n'est écrite si l'une de ces conditions est vraie :
- l'option est désactivée ;
- un menu, un écran modal ou une cinématique est ouvert ;
- aucun monde n'est chargé ;
- le joueur (NPC 1) n'existe pas ou est mort ;
- le joueur contrôle un autre NPC, par exemple un robot pris depuis un moniteur.

Une erreur d'écriture est consignée sur stderr et ne bloque pas le jeu.

### 8.5 Indicateur
- Une **disquette** s'affiche **2,5 s**, en taille double, à 8 px du coin.
- L'image source RGBA est convertie à la volée vers la couleur la plus proche de la palette courante.

### 8.6 Chargement
- **Écran Charger** : l'autosave forme une **12ᵉ ligne** (y = 406). L'écran Sauvegarder garde les 11 emplacements d'origine, donc on ne peut pas écraser l'autosave à la main.
- **F6** en jeu : propose de charger l'autosave, avec la même question et le même avis que F4 (chargement rapide). La touche est ignorée si le fichier n'existe pas, en stase, ou quand un autre NPC est contrôlé.

---

## 9. Course permanente (toggle run)

**Option :** `COURSE PERMANENTE` (`togglerun`, désactivée par défaut)

**Original** : on court tant que Maj est maintenue.

**Avec l'option**
- Chaque **appui** sur Maj (gauche ou droite) **inverse** l'état de course. Les répétitions automatiques de la touche sont ignorées.
- L'appui sur Maj n'est pas transmis au jeu.
- Le contrôleur de mouvement voit Maj enfoncée **exactement tant que la course est activée**, quel que soit l'état physique de la touche.
- Désactiver l'option remet l'état de course à « arrêté ».

---

## 10. Navigation au clavier ou à la manette sur le digicode

**Fichiers :** `src/engine/ui/keypad_gump.*`

L'original ne se contrôle qu'à la souris ou avec les chiffres du clavier. Le portage ajoute une sélection à la flèche, utile à la manette.

**Règles**
- Les flèches déplacent une sélection sur la grille de **3 colonnes × 4 lignes** : 1–9, effacement, 0, entrée. La sélection reste en butée aux bords (pas de bouclage).
- La sélection est **inactive tant qu'aucune flèche n'a été pressée**. Entrée garde alors son rôle d'origine : valider le code tapé, s'il contient au moins un chiffre.
- Sélection active : Entrée **appuie sur le bouton sélectionné**, avec le son `0x3A` pour l'effacement et `0x3B` pour les autres.
- Taper un chiffre, effacer ou cliquer désactive la sélection.
- Le reste reste identique à l'original : 8 chiffres maximum, code de triche `74697689`, comparaison au code modulo 10000, sons `0x32`/`0x31`.

---

## 11. Plein écran, fenêtre, icône

- **Alt+Entrée**, à tout moment : bascule fenêtre ↔ plein écran bureau. La touche n'est pas transmise au jeu.
- `--fullscreen` démarre en plein écran.
- La fenêtre est redimensionnable. Titre : « Crusader: No Remorse — DECOMP ». Le jeu a une icône dédiée (`src/game/res/crusader.ico`, ressource Windows).
- Le curseur système est caché, car le jeu dessine le sien.
- En jeu, la souris relative de SDL remplace le recentrage du pointeur à (320, 200) que fait l'original : même mouvement, sans déplacer le pointeur système. En revenant dans un menu, le pointeur système est replacé sur celui du jeu.
- `--gamma <x>` éclaircit (> 1) ou assombrit (< 1) l'image.

---

## 12. Lignes de version dans le menu principal

Deux petites lignes sont centrées en bas du menu principal, dans la couleur atténuée des boutons d'options :
1. `PORTAGE V<version>` (EN : `PORT V<version>`). Un éventuel `v` en tête de la version de build est retiré. La ligne est masquée si la version est vide.
2. `DONNEES DU JEU V<x.yy> <FR|US>`. La version est lue dans `CRUSADER.EXE` (chaîne « CRUSADER: No Remorse Vx.yy »). Si elle est introuvable, la ligne affiche « INCONNUES » ou « LANGUE INCONNUE ».

---

## 13. Support de la version US 1.23

**Fichiers :** `src/core/language.*`, `src/core/data_info.*`, `src/engine/usecode/intrinsic_numbering.cpp`, écrans UI

Un exécutable d'origine ne gère qu'une seule langue. Le portage accepte les données **FR 1.08** et **US 1.23** et choisit selon les données présentes.

**Règles**
- La langue est détectée au chargement des données. Elle vaut français par défaut.
- Les textes contenus dans `CRUSADER.EXE` (noms d'objets, messages, boutique de Weasel) sont gardés en double dans le portage, puis choisis selon la langue. Les écrans propres au portage suivent la même langue (fonction `tr(fr, en)`).
- Les scripts US appellent les routines natives (intrinsèques) sous **leur propre numérotation**. Le portage la traduit.
- Certains écrans US placent leurs boutons pour les images anglaises : positions propres à la version US.
- Saisie d'un nom de sauvegarde : la version US n'accepte pas `<`, `:` et `;`.
- **Absence de `CRUSADER.CFG`**, cas des données US copiées sans installateur : les choix par défaut de l'installateur s'appliquent, soit **vidéos doublées** (`bigvideo`) et **sous-titres** activés.
- Recherche des musiques (`M01.AMF`…) **insensible à la casse**, pour les systèmes de fichiers Linux.

---

## 14. Installation depuis une image ISO

**Fichiers :** `src/core/iso_image.*`

**Règles**
- On peut passer un fichier `.iso` en argument, ou le déposer dans `data/`. Le premier `*.iso` trouvé est utilisé.
- Si `data/` n'existe pas, il est créé avec une note `PUT_GAME_DATA_HERE.txt`.
- Si `data/STATIC/` existe déjà, l'ISO est ignorée : les données sont considérées comme installées.
- Sinon, l'ISO (ISO 9660, secteurs de 2048 octets ou images brutes de 2352) est extraite dans `data/`, environ 550 Mo, avec une progression affichée sur stderr.
  - Le dossier `CATALOG/` (navigateur de démos sans rapport avec le jeu) est ignoré.
  - Le suffixe de version `;1` des noms de fichiers est retiré.
- L'ISO peut ensuite être supprimée.
- Si aucune donnée n'est trouvée, un message indique de copier une installation (FR 1.08 ou US 1.23) ou l'ISO.

---

## 15. Démarrage, ligne de commande et outils

- **Lancement normal**, sans `--save` ni `--new-game` : cinématiques T01 et T02, puis menu de démarrage sans partie en cours, comme l'original (`Game_start`). Dans ce menu, « Retour » et « Sauvegarder » ne font rien. Les cycles de couleurs de la palette restent en pause sous ce menu, pour éviter un scintillement.
- `--new-game` : lance directement la première mission.
- `--save N` : reprend directement `SAVEGAME.N`.
- `--mod DIR` : charge un dossier de mod fait par l'éditeur de niveaux ; ses fichiers remplacent ceux des données, ses sauvegardes vont dans `DIR/SAVEGAME` (voir §19).
- `--map N [--egg E]` : nouvelle partie sur la carte N, sur son œuf d'arrivée E (0x1E par défaut).
- `--version` : affiche la version.
- `--classic` : vue d'origine 640×480 (voir §6).
- Options de test et d'outillage, sans fenêtre :
  - `--frames N --png out.png [--wav out.wav]` : exécute N images et écrit la dernière image (et le son) ;
  - `--size WxH`, `--scroll DX DY`, `--walk`, `--fire`, `--use ID` ;
  - `--menu*`, `--movie NAME`, `--movie-overlay NAME` ;
  - `--debug-title` : diagnostics dans le titre de la fenêtre (FPS, processus, intrinsèques manquantes…).
- Outils fournis : `crusader-editor` (éditeur de niveaux, §19), `shapeview`, `mapview`, `shapedump`, `sortcheck`, `ucsmoke`, `useprobe`, `diffcheck` (comparaison avec l'exécutable d'origine).

---

## 16. Fichier de configuration `CRUSADER.CFG`

**Fichiers :** `src/core/game_config.*`

**Original** : le fichier entier est réécrit depuis les variables globales, avec des fins de ligne `\n\r`.

**Portage**
- Seules les lignes des clés connues sont remplacées. Les autres lignes (carte son, lecteur CD, boutons souris…) sont **conservées, dans leur ordre**.
- Fins de ligne `\r\n`, comme l'installateur.
- Fichier absent ou illisible : valeurs par défaut du segment de données d'origine.

**Clés ajoutées** (écrites seulement si elles diffèrent du défaut, pour garder un fichier compatible avec l'original) :

| Clé | Défaut | Écrite quand |
|---|---|---|
| `followplayer` | false | `true` |
| `directionalmove` | false | `true` |
| `directionalaim` | false | `true` |
| `drawonaim` | true | `false` |
| `togglerun` | false | `true` |
| `nativeresolution` | true | `false` |
| `autosave` | true | `false` |
| `viewzoom` | 0 (auto), 1..4 avec décimales (`1.5`) | ≠ 0 |
| `padmap` | voir §2.4 | config. manette ≠ défaut. Liste de 24 numéros d'action séparés par des virgules ; `-1` = rôle par défaut, valeurs bornées |
| `padaim` | 3, borné à 1..5 | idem |
| `paddeadzone` | 2, borné à 0..4 | idem |

L'ancienne clé `padbuttons`, issue d'un format antérieur, est supprimée à l'écriture.

---

## 17. Différences de comportement assumées

Ces écarts sont volontaires et documentés dans le code.

- **Sauvegardes** : le portage n'écrit que les données qu'il gère :
  - entrées 9, 90, 91 ;
  - table d'objets et ses listes (0, 1) ;
  - NPC (10) ;
  - bloc joueur (20) ;
  - objets suivis (50–52) ;
  - globales usecode (60) ;
  - musique (70, 71).

  Le gestionnaire de sprites, la mémoire YAMM et les processus du noyau ne sont pas écrits. **Ses sauvegardes ne sont donc pas chargeables par l'exécutable d'origine.** Le portage lit en revanche les sauvegardes d'origine.
- **Nom de sauvegarde** : l'original l'affiche via `printf` en tant que chaîne de format (un `%` dans le nom pose problème). Le portage l'affiche tel quel.
- **Clavier AZERTY** : la version FR d'origine convertissait QWERTY → AZERTY, parce que son BIOS renvoyait des codes QWERTY. Les codes du portage suivent déjà la disposition du joueur, donc aucune conversion n'est faite.
- **Non reproduit** :
  - le curseur caché pendant la saisie d'un nom de sauvegarde ;
  - quelques détails internes du digicode (événement de type `0x10`, vidage clavier fait par le jeu à la fermeture) ;
  - le joystick PC.
- **Projectiles** : dessinés comme des objets au lieu de sprites, avec le même rendu.
- **Corrections de bugs du portage**, pour rapprocher son comportement de l'original. Exemple : la fin de la mission 2, où un NPC nouveau ou réutilisé est maintenant rattaché à la carte courante (`NPC_10f0_236b`).

---

## 18. Portabilité et distribution

- **Plateformes** : Windows x64 (SDL2 intégré à l'exécutable), Linux x64, et **Linux aarch64 pour consoles portables via PortMaster** (muOS, Knulli, ROCKNIX, ArkOS…). La compilation croisée passe par Zig (`tools/portmaster/build.py`).
- **PortMaster** :
  - lanceur `Crusader.sh` avec vérification des données et journalisation dans `log.txt` ;
  - métadonnées `port.json` et `gameinfo.xml` ;
  - manette lue directement par le jeu, **sans `gptokeyb`** : ses touches par défaut doublaient chaque entrée ;
  - Select + Start quitte le jeu ;
  - script de mise à jour par MTP pour la RG40XX-H.
- **Versions** : SemVer calculé à partir des Conventional Commits. Chaque release est construite et testée sous Linux et Windows (voir `docs/RELEASING.md`).
- **Tests** : tests unitaires sans données, et tests de bout en bout (`e2e/`) sur les données réelles et des sauvegardes de reproduction (`e2e/saves`). Plusieurs améliorations ont leurs propres tests : sauvegarde automatique, digicode, caméra scriptée, configuration.

---

## 19. Éditeur de niveaux

**Fichiers :** `src/editor/`, `src/game/game.*` (`game_file`, `set_start`), `docs/EDITEUR.md`

L'original n'a pas d'éditeur livré. `crusader-editor` (Dear ImGui sur SDL2, rendu par le moteur du portage) ouvre les cartes de `FIXED.DAT` et les globs de `GLOB.FLX`, décode les paramètres des œufs (déclencheurs, téléportations, monstres, globs, marqueurs) et les liens par qualité basse, et enregistre dans un dossier de mod sans toucher aux données. Le jeu lit ce dossier avec `--mod` et démarre une carte avec `--map`. Mode d'emploi complet : `docs/EDITEUR.md`.

**Règles**
- Les cartes et globs non modifiés sont réécrits à l'octet près (`editor_data_tests`) ; les contenus de conteneurs gardent leur profondeur brute (la carte 244 contient des enregistrements qui ne sont pas des objets).
- Un fichier du mod remplace celui des données : `FIXED.DAT`, `GLOB.FLX`, `SHAPES.FLX`, `TYPEFLAG.DAT`, `DAMAGE.FLX`, `ANIM.DAT`, `DTABLE.FLX`, `EUSECODE.FLX`, `UNKDS.DAT`.
- `--map N` : la nouvelle partie se déroule comme celle de l'original, mais `MainActor_I_teleportToEgg` reçoit la carte N et l'œuf `--egg` au lieu de 1 et 0x1E (`game_tests --editor-mod`).
