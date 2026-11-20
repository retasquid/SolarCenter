# SolarCenter — gestionnaire d'énergie solaire sur ESP32

Un ESP32 sur le réseau local pilote une installation solaire domestique : il lit le compteur, la
production, la batterie et le chauffe-eau, **prévoit** la production et la consommation, puis
**décide** où envoyer chaque kWh pour payer l'électricité le moins cher possible avec un tarif
heures pleines / heures creuses. Une interface web légère, servie par l'ESP32, affiche les flux en
direct, l'historique, les prévisions et les gains.

Tout reste **en local** : le seul accès à internet sert aux prévisions météo
([Open-Meteo](https://open-meteo.com)).

> Projet personnel, conçu pour une maison précise (voir [Matériel](#matériel)). Il pilote une
> batterie et un chauffe-eau sur le 230 V : lisez [Avertissement](#avertissement) avant de l'utiliser.

## Fonctionnalités

- **Mesures locales** des quatre appareils, toutes les 2 à 10 s, avec contrôles de plausibilité,
  détection des mesures périmées et modes de repli.
- **Batterie** : asservissement « zéro injection / zéro soutirage » au compteur en heures pleines,
  pas de décharge en heures creuses, **arbitrage réseau** optionnel (charge de nuit seulement si le kWh
  restitué rapporte plus que son prix, les pertes et l'usure), **rendement réel appris** sur les cycles.
- **Chauffe-eau** : garantie d'une chauffe complète avant l'heure des douches, chauffe solaire sur le
  surplus, chauffe de nuit placée **le plus tard possible** en heures creuses, rattrapage si besoin.
  Énergie d'une chauffe complète apprise.
- **Prévisions** par moindres carrés récursifs (RLS) : production réelle à partir de l'irradiance
  prévue (apprend ombrages et orientation), consommation à partir d'un profil appris semaine / week-end.
- **Tarifs** : Base, HP/HC, ou grille HP/HC × semaine / week-end × saison haute / basse, jours fériés
  français compris.
- **Statistiques** : autoconsommation, autoproduction, coûts réels et contrefactuels (sans batterie,
  sans installation), gains, projection de rentabilité de la batterie. Coupures de l'Envoy reconstituées
  grâce à son compteur cumulé.
- **Interface web** (HTML/CSS/JS sans framework, ~27 Ko compressés) : diagramme animé des flux,
  jauge d'autoconsommation, prévisions comparées au réel (hier, aujourd'hui, demain), historique,
  paramètres, journal des décisions, export CSV.
- **Mise à jour par le réseau** (OTA) du firmware et de l'interface, sans effacer l'historique, avec
  retour automatique à l'ancien firmware s'il ne démarre pas correctement.
- **Mode observateur** : toutes les décisions sont calculées et journalisées sans rien commander.

## Matériel

| Rôle | Appareil | Accès |
|---|---|---|
| Contrôleur | ESP32-WROOM-32D (flash 4 Mo, sans PSRAM) | — |
| Compteur réseau (indispensable) | Shelly Pro 3EM (profil triphasé ou monophasé) | HTTP local (RPC) |
| Production solaire | Enphase Envoy / IQ Gateway, firmware 7+ | HTTPS local, jeton propriétaire |
| Batterie | Zendure SolarFlow 1600 AC+ | API HTTP locale (zenSDK) |
| Chauffe-eau | Interrupteur Tongou, firmware Tuya d'origine ou OpenBeken | protocole local Tuya 3.3 / 3.4 / 3.5 chiffré, ou HTTP |

Les pilotes sont propres à ces modèles. Adapter le projet à d'autres appareils demande d'écrire un
pilote dans `components/drv_*`.

## Démarrage rapide

### 1. Prérequis

- [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/) (installé par EIM sous Windows).
- Python 3 (fourni avec ESP-IDF) pour les outils.

### 2. Compiler et flasher

```bash
# Dans un terminal où l'environnement ESP-IDF est activé
python tools/build_web.py          # minifie et compresse web/ vers littlefs_image/
idf.py set-target esp32            # première fois seulement
idf.py build
idf.py -p <PORT> flash             # firmware ; historique conservé
idf.py -p <PORT> storage-flash     # interface web — première installation seulement (efface l'historique)
```

### 3. Première mise en route

1. Au premier démarrage, l'ESP32 ouvre le réseau WiFi **« Solaire-Setup »** : s'y connecter avec un
   téléphone et ouvrir **http://192.168.4.1**.
2. Saisir le WiFi de la maison, un mot de passe administrateur et le nom réseau (par défaut `solaire`).
3. Ouvrir ensuite **http://solaire.local**, puis renseigner **Paramètres** : tarif (obligatoire),
   installation PV, batterie, chauffe-eau, adresses des appareils.

Le [guide d'utilisation](docs/guide-utilisateur.md) détaille la préparation de chaque appareil
(jeton Envoy, API locale Zendure, clés Tuya), les paramètres et les pages.

### 4. Mettre à jour par le réseau

```bash
python tools/build_web.py && idf.py build
python tools/ota.py solaire.local          # interface puis firmware ; --www ou --fw pour l'un des deux
```

## Outils PC (`tools/`)

| Script | Rôle |
|---|---|
| `build_web.py` | Minifie et compresse l'interface (budget 100 Ko) |
| `ota.py` | Mise à jour par le réseau (mot de passe administrateur) |
| `tuya_keys.py` | Récupère l'identifiant et la clé locale des appareils Tuya par QR code, comme Home Assistant |
| `tuya_ref.py` | Implémentation de référence du protocole Tuya : interroge un appareil, génère les vecteurs de test |

Les deux outils Tuya demandent quelques paquets : `pip install -r tools/requirements.txt`
(de préférence dans un environnement virtuel).

## Tests

Toute la logique de décision (`components/algo/`) est en **C pur**, sans dépendance à ESP-IDF, et
testée sur PC avec Unity (fourni par ESP-IDF) :

```bash
cmake -S test/host -B build-host -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-host && ctest --test-dir build-host
```

Les tests couvrent les tarifs (plages qui passent minuit, jours fériés), la répartition des flux,
les statistiques et la reconstitution des coupures, les RLS, le planificateur et l'arbitrage, le
régulateur (dont la garantie de chauffe sur 30 jours simulés), l'apprentissage du rendement, le
protocole Tuya (octet par octet contre `tools/tuya_ref.py`) et la validation de la configuration.

## Organisation du code

```
main/              initialisation et création des tâches
components/
  algo/            décisions et calculs, C pur testé sur PC (tarifs, flux, stats, RLS, plan, régulateur…)
  app_state/       état partagé protégé par mutex, file de commandes
  config/          paramètres en NVS (blob versionné, CRC, migration, validation)
  control/         tâches de régulation et d'action (commandes aux appareils)
  drv_*/           pilotes Shelly, Envoy, Zendure, Tongou (OpenBeken) et Tuya
  forecast/        client Open-Meteo
  history/         agrégats 30 min / jour / mois sur LittleFS, apprentissage des modèles
  http_util/       client HTTP/HTTPS commun (une seule session TLS à la fois)
  net/             WiFi, portail de configuration, NTP, mDNS
  poll/            lectures périodiques des appareils
  sysmon/          surveillance mémoire, redémarrage contrôlé, validation OTA
  web/             serveur HTTP, API REST, WebSocket, OTA
web/               interface (HTML/CSS/JS vanilla)
test/host/         tests unitaires PC
docs/              guide d'utilisation
```

Le cahier des charges complet (algorithmes, sécurités, API, conventions) est dans
[CLAUDE.md](CLAUDE.md).

## État du projet

Version courante : voir [`version.txt`](version.txt). Le firmware affiche `version+commit` dans la
page Système.

Points encore à confirmer sur le matériel (marqués `TODO(verify)` dans le code) :

- signe de la puissance du Shelly pendant une injection (profil monophasé) ;
- formule de puissance AC du SolarFlow et son temps de réponse à une consigne ;
- DP du relais et de la puissance du Tongou sous Tuya ;
- **sécurité côté appareil du chauffe-eau** : sous Tuya, rien ne coupe encore le relais si l'ESP32
  s'arrête pendant une chauffe.

Non fait : mode simulation (rejeu d'un CSV sans matériel).

## Avertissement

Ce logiciel commande une batterie et un chauffe-eau raccordés au 230 V. Il est fourni **sans aucune
garantie**. Commencez en **mode observateur**, vérifiez les mesures et les décisions pendant plusieurs
jours, et gardez les protections électriques et le thermostat du ballon en place. Réglez le thermostat
à 55 °C ou plus (risque de légionelle).

L'API gratuite d'Open-Meteo est réservée à un usage non commercial.
