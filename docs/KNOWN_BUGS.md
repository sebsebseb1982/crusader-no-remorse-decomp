# Known bugs
## gun shots replaced by lasers
## [solved] blinking sprites changing between variations (walls, bodies, ...)
Au bout d'un certain temps de jeu (niveau exploré ?), des sprites commencent à avoir un comportement erratique :
 - Des murs changent en permanence d'apparence
 - Des cadavres bougent

Je pense avoir un peu plus compris ce qui se passe : Je suppose que les sprites sont rangés dans des "collections". Par exemple :
 - toutes les déclinaisons d'un mur (par exemple, dans un niveau qui a des murs gris et d'autres rouillés, une collection de toutes les déclinaisons des murs gris et une autre des rouillés)
 - toutes les orientations d'un cadavre (pour chaque type d'ennemis, une collection des orientations du cadavre)

Et le problème en fait, c'est qu'au d'un moment de jeu, certains sprites (pas tous) prennent une valeur aléatoire et "clignotent".

Le comportement est visible dans C:\Users\sblon\Videos\Captures\Crusader_ No Remorse — DECOMP 2026-11-07 18-46-46.mp4

Ce comportement peut se voir un peu partout mais je l'ai constaté dernièrement et j'ai fait une sauvegarde sur le niveau où je l'ai constaté pour la dernière fois : DECORSCLIGNOTE.

**Corrigé.** Cause : quand un objet était détruit (`World::destroy`), ses processus (scripts usecode, déclencheurs d'œufs…) continuaient de tourner. Son numéro retournait en tête de la liste des emplacements libres, et le prochain objet créé le reprenait aussitôt. Les processus de l'ancien objet agissaient alors sur le nouveau, par exemple en changeant son image. Cela arrive surtout avec les objets « glob » du décor, détruits quand leur zone sort de la zone active. Plus on explore, plus il y a de processus orphelins : sur la sauvegarde, une caméra qui balaie la carte en fait 189 au lieu de 41. L'original termine les processus de l'objet à sa destruction (`Item_forget`, US `10B0:1B7F` : `Kernel_fail_processes(id, tout type)`), et le portage fait désormais de même. Tests : `test_processes_end_with_their_item` (sauvegarde `decors-clignotent.sav`) et `test_fast_area`. Les « sprites qui se déplacent » ci-dessous ont peut-être la même cause, à revérifier en jeu.

## ambiant sounds keep playing even without source visible
## [solved] jump stance when landing
J'ai l'imperssion que lorsque le silencer saute, lorsqu'il atterit, il reste sur son animation de chute tant qu'il ne bouge pas.

**Corrigé.** Le saut (action 0x3A) a 13 images : décollage, vol, réception (2108), redressement (2109 à 2111), puis debout (image 0). L'image de réception vérifie que le joueur a un appui. Quand il retombe plus bas que son point de départ, ce contrôle le fait chuter. Le portage terminait alors l'animation, et le joueur restait figé sur l'image 2108 jusqu'au prochain mouvement. Dans l'original (ANIMPRIM, US `1158:1906` / `1158:3090`), l'animation continue : elle attend la fin de la chute, puis joue ses dernières images et le joueur finit debout. Seule l'animation de chute 0x3B, quand elle remplace l'animation en cours (hors saut et roulades), y met fin. Sur un sol plat, le saut se terminait déjà normalement. Test : `test_jump_down_ends_standing` (sauvegarde 13, saut vers un sol 16 unités plus bas).

## Sprites qui se déplacent

## Monde qui apparait progressivement
A l'origine, ce jeu était prévu pour des écrans 4/3. Su un écran moderne type 21/9, on voit une des limites du jeu, les sprites apparaissent progressivement y compris dans la partie visible à l'écran. Il en va de même pour les ennemis qui peuvent apparaitre subitement en se déplacant.

J'aimerais donc que l'on réfléchisse ensemble sur une solution à ce problème :
 - que le niveau soit tout le temps complet dans la vue courante
 - que les ennemis n'apparaissent pas magiquement en plein milieux. Les afficher de suite me semble aussi problématique : Je ne veux ni pouvoir les snipers à l'autre bout du niveau sans qu'ils m'aient détectés, ni que eux me snipe de l'autre bout du niveau ... Je n'ai pas bcp d'idée pour résoudre ce problème de gameplay

**Cause.** L'original mélange trois zones :
 - la vue affichée (640×480) ;
 - la zone active (`FastArea`, ±5 cellules en `s`, ±3 en `d` autour de la caméra), qui déborde de l'écran : le décor « glob » n'y est déplié qu'à l'entrée, les œufs de monstres y créent leurs PNJ, l'IA et les animations n'y tournent que là ;
 - la zone de combat, qui est l'écran : un PNJ ne repère le joueur, ne le garde pour cible et ne tire que s'il est à l'écran (`is_partly_on_screen` / `is_mostly_on_screen`), et le tir du joueur ne choisit sa cible que parmi les objets à l'écran.

La résolution native élargissait la vue et la zone de combat, mais pas la zone active. On voyait donc ses bords.

**Étape 1, corrigée : le monde est complet.** La zone active grandit avec la vue en gardant la marge de l'original (voir AMELIORATIONS.md §6). Test : `test_fast_area`.

**Étape 2, en cours : le comportement des ennemis.**
 - *Fait* : la zone de combat reste le rectangle 640×480 de l'original, centré comme lui (voir AMELIORATIONS.md §6). Repérage, garde de la cible, tir, réveil par un tir et choix de cible du tir du joueur s'y limitent ; caméras de surveillance et tourelles aussi. La vue entière reste aux tests visuels. Tests : `test_on_screen`, `test_combat_zone`.
 - *À faire* : vérifier ce que devient un projectile du joueur tiré droit vers un ennemi des bandes (le choix de cible l'ignore, mais le projectile peut-il le toucher ?).
 - *À faire* : signaler les menaces hors de la zone de combat en les dessinant en niveaux de gris assombris (environ 55 à 60 % de la luminosité, à régler en jeu), avec une courte transition quand elles y entrent : PNJ vivants autres que le joueur, tourelles, caméras. Pas les cadavres, mines, projectiles ni le décor.
 - *À faire* : option « ZONE DE COMBAT : ORIGINALE / ÉCRAN ENTIER » dans Améliorations (`World::set_original_combat_zone`).
