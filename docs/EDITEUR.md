# Éditeur de niveaux

`crusader-editor` crée et modifie les cartes de *Crusader: No Remorse* : le décor, les objets, les PNJ et tout ce qui pilote les scripts du jeu (œufs, déclencheurs, téléportations, liens entre objets). Il travaille sur les mêmes fichiers que le jeu d'origine et dessine la carte avec le moteur de rendu du portage : ce qu'on voit dans l'éditeur est ce qu'on verra en jeu.

Les données d'origine ne sont jamais modifiées. L'éditeur enregistre dans un **dossier de mod**, que le jeu charge par-dessus les données avec `--mod`.

## Lancer

```sh
crusader-editor data                                # dossier des données du jeu, mod par défaut : mods/mon-niveau
crusader-editor data --mod mods/base-secrete --map 3
```

Pour jouer le niveau, **F5** dans l'éditeur (ou *Fichier > Tester dans le jeu*) enregistre puis lance :

```sh
crusader data --mod mods/base-secrete --map 3 --egg 0x1E
```

`--map` commence une nouvelle partie sur cette carte, sur l'œuf d'arrivée `--egg` (0x1E par défaut, l'œuf où commence chaque mission). Si un œuf d'arrivée est sélectionné, F5 part de celui-ci. Les parties sauvegardées d'un mod vont dans `<mod>/SAVEGAME`.

## Ce que contient un niveau

Le format est celui des niveaux d'origine ; l'éditeur relit et réécrit les 63 cartes et les 3 072 globs du jeu à l'octet près (test `editor_data_tests`).

| Fichier | Contenu | Dans l'éditeur |
|---|---|---|
| `STATIC/FIXED.DAT` | une entrée par carte (1 à 255) : enregistrements de 16 octets (x, y, z, forme, image, drapeaux, qualité, n° PNJ, n° carte) | la carte ouverte |
| `STATIC/GLOB.FLX` | les globs : groupes de pièces (sols, murs) posés par un œuf de glob | onglet *Globs*, éditeur de globs |
| `STATIC/SHAPES.FLX`, `TYPEFLAG.DAT` | les formes (sprites) et leurs propriétés | onglet *Formes* (lecture seule) |
| `USECODE/EUSECODE.FLX` | les scripts (classes usecode) | onglet *Scripts* (lecture seule) |
| `STATIC/DTABLE.FLX` | les types de PNJ | onglet *Œufs* (lecture seule) |

Le jeu lit `FIXED.DAT`, `GLOB.FLX`, `SHAPES.FLX`, `TYPEFLAG.DAT`, `DAMAGE.FLX`, `ANIM.DAT`, `DTABLE.FLX`, `EUSECODE.FLX` et `UNKDS.DAT` dans le mod quand ils y sont, sinon dans les données.

### Les globs

Les sols et les murs d'une carte viennent presque tous de **globs** : un œuf de glob (forme 0x010, famille 3) de qualité *n* pose les pièces du glob *n* dans la case de 512 × 512 unités où il se trouve. Seule la case compte, pas la position exacte de l'œuf : l'éditeur les déplace donc de case en case.

- Onglet *Globs* : clic = pinceau, double clic = ouvrir dans l'éditeur de globs.
- *Édition > Créer un glob avec la sélection* : du décor simple pris dans une même case devient un nouveau glob, remplacé par son œuf.
- *Dégrouper le glob* : l'inverse, l'œuf devient ses pièces.
- Un glob est partagé par tous ses œufs, sur toutes les cartes : pour ne changer qu'un endroit, *Enregistrer comme nouveau glob*.

### Les œufs : comment les niveaux sont scriptés

Les niveaux d'origine n'ont pas de script propre à la carte. Les concepteurs posaient des **œufs** et réglaient les octets *qualité*, *n° PNJ* et *n° carte* des objets, que lisent le moteur et les classes usecode. Le panneau *Propriétés > Rôle dans le niveau* les décode :

| Œuf | Forme | Paramètres |
|---|---|---|
| Arrivée de téléportation | 0x194 image 1 | id = qualité basse ; la mission commence sur l'id 0x1E |
| Départ de téléportation | 0x194 image 0 | vers l'œuf (qualité basse) de la carte (n° carte, 0 = celle-ci), attente si qualité haute ≠ 0, zone de déclenchement |
| Déclencheur | 0x011 | classe 0x900 + qualité basse (`TRIGEGG`, `ONCEEGG`, `DOOREGG`, `CAMERA`, `MUSIC`, `MISS1EGG`…), paramètre = qualité haute, id = n° carte, zone de déclenchement |
| Monstre (PNJ) | 0x24F | type de PNJ (`DTABLE.FLX`), direction, difficulté minimale, autre arme, trois activités (au départ, en voyant le joueur, quand on lui tire dessus) ; ces trois dernières sont rangées dans le modèle que l'œuf contient |
| Marqueur | 0x476 | objet que donne le coffre de même qualité basse, ou objet coupé par un `TRIGGER` |

La **zone de déclenchement** (demi-largeurs × 32 unités, rangées dans le n° PNJ ; le joueur doit aussi être à moins de 0x30 en z) est dessinée en rose ; les départs de téléportation sont reliés à leur arrivée par un trait vert.

Beaucoup d'objets se répondent par leur **qualité basse** : interrupteur → murs d'énergie ou portes (`TRIGGER`), vanne → vapeur, coffre → marqueurs, caméra → alarme. Quand un objet est sélectionné, ceux qui partagent sa qualité basse et ont un script sont encadrés en cyan, et *Sélectionner les objets liés* les prend tous.

La section *Script (usecode)* donne la classe de l'objet et les événements qu'elle traite (`use`, `gotHit` pour les dalles et plaques, `hatch` pour les œufs, `equip` pour la mise en marche…).

### Nouveau niveau

- *Carte > Nouvelle carte* : un emplacement libre de `FIXED.DAT`, 3 × 3 cases du sol le plus courant de la mission 1, et l'œuf de départ 0x1E.
- *Carte > Copier cette carte vers un emplacement libre* : part d'une carte d'origine.

Attention, une carte copiée garde ses œufs de mission (`MISS1EGG`…) et ses téléportations, qui visent souvent d'autres cartes.

## Commandes

| Action | Commande |
|---|---|
| Sélectionner, ajouter à la sélection | clic, Ctrl+clic ou Maj+clic |
| Objet suivant sous le pointeur | Alt+clic répété |
| Sélection par rectangle | glisser dans le vide |
| Déplacer | glisser la sélection (aimanté à la grille ; Alt : sans aimant), flèches, PgPréc / PgSuiv pour z |
| Poser un objet | choisir un pinceau (Formes, Globs, Œufs), puis clic ; clic droit ou Échap pour le lâcher |
| Faire défiler, zoomer | clic droit ou molette enfoncée et glisser, flèches sans sélection ; molette |
| Centrer sur la sélection | F |
| Dupliquer, supprimer | Ctrl+D, Suppr |
| Annuler, rétablir | Ctrl+Z, Ctrl+Y |
| Enregistrer, tester | Ctrl+S, F5 |

*Coupe en z* cache ce qui est plus haut : pour travailler un étage sans les plafonds. *z de travail* est la hauteur où se posent les nouveaux objets. Le menu *Affichage* montre ou cache les marqueurs d'éditeur (les œufs ne sont pas dessinés en jeu), les objets invisibles, les globs, les zones et les liens.

## Limites

- Le jeu garde au plus 9 984 objets en mémoire, pièces des globs proches comprises. La carte la plus chargée du jeu a 7 855 enregistrements ; la barre d'état prévient au-delà de 8 000.
- Le joueur commence toujours avec l'équipement d'une nouvelle partie (datalink).
- Formes, scripts et types de PNJ sont en lecture seule pour l'instant.

## Suite prévue

1. **Scripts** : désassembleur usecode dans l'onglet *Scripts*, puis assembleur pour écrire de nouvelles classes d'œufs (0x91E et suivantes) et de nouvelles fonctions.
2. **Sprites** : import d'images PNG dans `SHAPES.FLX` (nouvelles formes et images, palette du jeu) et édition de `TYPEFLAG.DAT` (emprise, famille, solidité).
3. **PNJ** : édition des types de `DTABLE.FLX` (points de vie, armes, forme).
4. **Contenu des missions** : textes des ordinateurs, codes des claviers, musique, enchaînement des cartes.
