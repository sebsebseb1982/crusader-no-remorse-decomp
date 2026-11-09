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