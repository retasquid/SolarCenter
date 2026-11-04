# CLAUDE.md — Gestionnaire d'énergie solaire (ESP32-WROOM-32D)

Ce fichier guide Claude Code pour implémenter le projet. Lis-le en entier avant toute modification.

---

## 1. Objectif du projet

Un ESP32-WROOM-32D sur le réseau local de la maison :

1. **lit** en local (sans cloud) :
   - le compteur **Shelly Pro 3EM** (puissance réseau, 3 phases),
   - la passerelle **Enphase Envoy / IQ Gateway** (production photovoltaïque),
   - la batterie **Zendure SolarFlow 1600 AC+** (SOC, puissance de charge et de décharge),
   - l'interrupteur **Tongou** du chauffe-eau, flashé sous **OpenBeken** (état du relais, puissance) ;
2. **prévoit** la production solaire réelle et la consommation de la maison, par **régression linéaire récursive (RLS)** à partir des prévisions météo **Open-Meteo** et de l'historique ;
3. **pilote** la batterie et le chauffe-eau pour minimiser le coût de l'électricité, avec un tarif **heures pleines / heures creuses** ;
4. **sert une interface web** : diagramme animé des flux d'énergie, historique, prévisions et paramètres.

Seul appel internet : Open-Meteo (prévisions). Tout le reste reste sur le réseau local.

---

## 2. Matériel et contraintes (À RESPECTER STRICTEMENT)

| Ressource | Valeur | Conséquence |
|---|---|---|
| CPU | 2 × Xtensa LX6 @ 240 MHz | Une FPU simple précision : utiliser `float`, **jamais `double`** dans le code de calcul |
| SRAM | 520 Ko, environ 280–300 Ko libres une fois le WiFi lancé | **Pas de PSRAM**. Allocation statique, buffers dimensionnés |
| Flash | 4 Mo (à vérifier avec `esptool.py flash_id`) | Deux partitions OTA d'environ 1,6 Mo : surveiller la taille du binaire |
| Révision de puce | Probablement < v3 (lire `chip revision` au boot) | Pas de Secure Boot v2 : OTA uniquement par téléversement local protégé par mot de passe |

Règles mémoire :
- **Une seule session TLS à la fois** (Envoy et Open-Meteo), protégée par `g_tls_mutex`. Ne jamais ouvrir deux connexions HTTPS en parallèle.
- Dans menuconfig : `MBEDTLS_DYNAMIC_BUFFER=y`, `MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`, buffer entrant 16384, buffer sortant 4096.
- Aucun `malloc` ni `cJSON_Parse` dans une boucle rapide sans `cJSON_Delete` immédiat. Pas de VLA ni de récursion.
- Journaliser `heap_caps_get_free_size()` et `heap_caps_get_minimum_free_size()` toutes les 60 s. **Seuil d'alerte : 40 Ko libres minimum.**
- Taille du binaire : alerter si `idf.py size` dépasse 1,45 Mo.

---

## 3. Chaîne d'outils

- **ESP-IDF v6.1** (installé par EIM dans `C:\esp\v6.1\esp-idf`), langage **C11**, composants CMake.
  - Activer l'environnement dans chaque shell PowerShell : `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.
- JSON : `espressif/cjson` depuis le Component Registry (le composant `json` a été retiré d'IDF v6).
- mDNS : `espressif/mdns` depuis le Component Registry (il ne fait plus partie d'IDF).
- Système de fichiers : **LittleFS** (`joltwallet/littlefs` depuis le Component Registry) pour l'interface web et l'historique.
- Commandes :
  ```bash
  idf.py set-target esp32
  idf.py menuconfig
  python tools/build_web.py          # minifie et compresse web/ en gzip → littlefs_image/
  idf.py build flash monitor         # flashe aussi l'image LittleFS (FLASH_IN_PROJECT)
  idf.py storage-flash               # ne flashe que la partition LittleFS
  cmake -S test/host -B build-host -G Ninja -DCMAKE_C_COMPILER=gcc && cmake --build build-host && ctest --test-dir build-host
  ```

---

## 4. Arborescence

```
/
├── CLAUDE.md
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   └── main.c                 # init, création des tâches, rien d'autre
├── components/
│   ├── app_state/             # état partagé + mutex + file de commandes
│   ├── config/                # paramètres (NVS, blob versionné + CRC), valeurs par défaut, validation
│   ├── net/                   # WiFi STA, portail SoftAP au 1er démarrage, NTP, mDNS
│   ├── http_util/             # client HTTP/HTTPS commun (timeouts, mutex TLS, buffers statiques)
│   ├── drv_shelly/            # Shelly Pro 3EM
│   ├── drv_envoy/             # Enphase Envoy
│   ├── drv_zendure/           # SolarFlow 1600 AC+ (zenSDK)
│   ├── drv_tongou/            # OpenBeken (API compatible Tasmota)
│   ├── forecast/              # client Open-Meteo
│   ├── algo/                  # ⚠ C PUR, AUCUNE dépendance ESP-IDF (testable sur PC)
│   │   ├── rls.c/.h           # moindres carrés récursifs
│   │   ├── profile.c/.h       # profil de consommation EMA 48 créneaux × 2 types de jour
│   │   ├── tariff.c/.h        # créneaux HP/HC, prix à un instant donné
│   │   ├── planner.c/.h       # plan journalier
│   │   ├── waterheater.c/.h   # cycle douches, plan de nuit, rattrapage, E_full appris
│   │   ├── regulator.c/.h     # décision temps réel (fonction pure : état → commandes)
│   │   ├── flows.c/.h         # répartition des flux (diagramme + statistiques)
│   │   └── stats.c/.h         # autoconsommation, coûts, gains, rentabilité
│   ├── history/               # agrégation 30 min, persistance LittleFS
│   └── web/                   # serveur HTTP, API REST, WebSocket
├── web/                       # sources du front (HTML/CSS/JS vanilla)
│   ├── index.html
│   ├── app.js
│   └── style.css
├── tools/
│   ├── build_web.py
│   └── replay_csv.py          # rejoue un historique pour le mode simulation
└── test/host/                 # tests unitaires de algo/ (Unity ou équivalent)
```

**Règle d'or** : `components/algo/` ne doit inclure **que** `<stdint.h>`, `<stdbool.h>`, `<math.h>`, `<string.h>`. Le temps y est passé en paramètre (pas d'appel à `time()`).

---

## 5. Table de partitions (flash 4 Mo)

```csv
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
otadata,   data, ota,      0xF000,   0x2000
phy_init,  data, phy,      0x11000,  0x1000
ota_0,     app,  ota_0,    0x20000,  0x1A0000
ota_1,     app,  ota_1,    0x1C0000, 0x1A0000
storage,   data, littlefs, 0x360000, 0xA0000
```

`storage` (640 Ko) contient l'interface web compressée (budget ≤ 100 Ko) et l'historique.

---

## 6. Architecture d'exécution

### Tâches FreeRTOS

| Tâche | Cœur | Priorité | Période | Rôle |
|---|---|---|---|---|
| `task_regul` | 1 | 7 | 5 s | Copie l'état, appelle `regulator_step()`, pousse les commandes dans `q_cmd` |
| `task_act` | 0 | 6 | sur événement | Consomme `q_cmd` et envoie les ordres HTTP (Zendure, Tongou) |
| `task_poll` | 0 | 5 | 2 s | Lectures séquentielles : Shelly 2 s, Zendure 5 s, Tongou 5 s, Envoy 10 s |
| `task_forecast` | 0 | 3 | 1 h, plus 05:30 | Open-Meteo, puis re-planification |
| `task_hist` | 0 | 2 | toutes les 30 min | Agrégation, mise à jour des RLS et du profil, persistance |
| httpd (IDF) | 0 | 5 | — | API REST, WebSocket (envoi de l'état toutes les 2 s) |

- Chaque tâche est inscrite au **Task Watchdog** (`esp_task_wdt_add`).
- **État partagé** : `app_state_t` protégé par un mutex. Les lecteurs font une **copie locale** puis relâchent le verrou immédiatement : on ne garde jamais le mutex pendant un appel réseau.
- **Commandes** (web, écran, régulateur) : file `q_cmd` (`xQueueSend`). Seule la tâche concernée modifie l'état.
- **Horodatage** : NTP obligatoire avant toute régulation basée sur les tarifs. Fuseau `CET-1CEST,M3.5.0,M10.5.0/3`.

---

## 7. Conventions de signes et bilan énergétique

Toutes les puissances sont en **W**, en `float` :

| Variable | Source | Signe |
|---|---|---|
| `p_grid` | Shelly `total_act_power` | **> 0 import**, < 0 export |
| `p_pv` | Envoy | ≥ 0 |
| `p_batt` | Zendure, côté AC | **> 0 décharge** (vers la maison), < 0 charge |
| `p_ce` | Tongou (OpenBeken) | ≥ 0 |
| `p_home` | calculé | `p_grid + p_pv + p_batt` |
| `p_base` | calculé | `p_home − p_ce` (consommation non pilotée, **c'est elle que l'on apprend**) |

- `p_batt = outputHomePower − gridInputPower` (côté AC du SolarFlow). `TODO(verify)` : valider sur l'appareil réel en comparant avec le Shelly lors d'une charge et d'une décharge forcées.
- **Bornes de plausibilité** (une valeur hors bornes est rejetée et marquée invalide) : `|p_grid| < 15000`, `0 ≤ p_pv < 1,2 × kWc × 1000`, `|p_batt| < 3000`, `0 ≤ p_ce < 4000`, `0 ≤ soc ≤ 100`.
- Chaque mesure porte un horodatage `ts_ms` et un booléen `valid`. Une mesure plus vieille que 3 périodes de sondage devient **périmée** (`stale`).

---

## 8. Pilotes d'appareils

Interface commune (dans `app_state.h`) :
```c
typedef struct { float value; uint32_t ts_ms; bool valid; } meas_t;
// Chaque pilote : esp_err_t drv_xxx_poll(const cfg_t *cfg, xxx_reading_t *out);
//                 esp_err_t drv_xxx_set(...);   // si pilotable
```
Timeout HTTP **3 s**, au plus 1 nouvel essai, puis `valid = false`. Ne **jamais** bloquer la régulation sur un pilote.

> ⚠ **Ne jamais inventer un endpoint ni un champ JSON.** Tout ce qui est marqué `TODO(verify)` doit être confirmé sur l'appareil réel (ou dans la documentation officielle) avant d'écrire la logique qui en dépend.

### 8.1 Shelly Pro 3EM
- `GET http://<ip>/rpc/EM.GetStatus?id=0` → `total_act_power`, `a_act_power`, `b_act_power`, `c_act_power`.
- **Profil « monophase »** (constaté sur l'appareil de la maison le 2026-11-04, fw 2.1.0) : `EM.GetStatus` répond 404 ; les canaux sont `em1:0..2` et `GET /rpc/EM1.GetStatus?id=<canal>` → `act_power`. Le canal 0 mesure le réseau. Config `shelly_mode` (auto / triphase / monophase) et `shelly_channel`. `TODO(verify)` : signe de `act_power` pendant une injection.
- L'authentification doit être désactivée sur le réseau local en v1 (sinon `TODO` : authentification digest).
- C'est le **capteur maître** : si le Shelly est invalide plus de 30 s → **mode repli** (§10).

### 8.2 Enphase Envoy (firmware ≥ 7)
- HTTPS avec certificat auto-signé : pas de vérification du certificat (option documentée dans la config, avec un avertissement dans l'interface).
- En-tête `Authorization: Bearer <token>` : jeton propriétaire valable environ 1 an, collé par l'utilisateur dans les Paramètres.
- `GET /production.json?details=1` → dans `production[]`, prendre l'entrée `type == "eim"` et `measurementType == "production"` → `wNow`. **Repli** sur l'entrée `type == "inverters"` → `wNow`.
- Buffer de réponse statique de 8 Ko, période ≥ 10 s, connexion fermée après chaque requête, prise du `g_tls_mutex`.
- Décoder la date `exp` du JWT (base64url) et **afficher l'échéance** dans l'interface (alerte à J−30).
- Firmware < 7 : HTTP sans jeton (option de configuration).

### 8.3 Zendure SolarFlow 1600 AC+ (zenSDK, API HTTP locale)
- Prérequis utilisateur : activer l'API locale (HEMS) dans l'app Zendure, et **désactiver tout mode intelligent de Zendure** (lecture de compteur, CT). **Un seul régulateur : l'ESP32.**
- Lecture : `GET http://<ip>/properties/report` → `sn`, `properties.electricLevel` (SOC %), `outputHomePower`, `gridInputPower`, `packInputPower`, `outputPackPower`, `acMode`, `inputLimit`, `outputLimit`, `socSet` et `minSoc` (en ‰, `1000` = 100 %).
- Écriture : `POST http://<ip>/properties/write` avec le corps `{"sn":"<sn>","properties":{...}}`.
  - Charge : `{"acMode":1,"inputLimit":W}`. Décharge : `{"acMode":2,"outputLimit":W}`. Repos : limites à 0.
  - Ajouter `"smartMode":1` pour éviter l'usure de la flash de la batterie lors des écritures fréquentes. `TODO(verify)` : confirmer la sémantique de `smartMode` dans la documentation zenSDK officielle.
  - **Ne pas écrire** si l'écart avec la consigne actuelle est < 20 W. **Au plus 1 écriture toutes les 10 s.**
- Au démarrage, écrire `minSoc` et `socSet` depuis la config.
- Au démarrage de l'ESP32 ou si le Shelly est invalide : consigne **repos** (0 W).
- Lecture irrégulière possible (des timeouts sont signalés par d'autres utilisateurs) : tolérer jusqu'à 3 échecs consécutifs avant `stale`.

### 8.4 Tongou (OpenBeken, API compatible Tasmota)
- Commande : `GET http://<ip>/cm?cmnd=POWER%20ON` / `POWER%20OFF`.
- État et puissance : `GET http://<ip>/cm?cmnd=Status%2010` → `StatusSNS.ENERGY.Power`. `TODO(verify)` : la commande exacte et le chemin JSON dépendent de la version d'OpenBeken.
- **Sécurité côté appareil (obligatoire avant le M4)** : script `autoexec.bat` sur le Tongou qui coupe le relais si aucun battement de cœur n'est reçu de l'ESP32 depuis 10 min. L'ESP32 envoie ce battement toutes les 60 s quand le relais doit rester allumé. `TODO(verify)` : syntaxe de script OpenBeken (`addRepeatingEvent`, variables de canal) à vérifier dans la documentation avant écriture.
- Firmware Tuya d'origine : **hors périmètre v1**.

### 8.5 Open-Meteo
- `GET https://api.open-meteo.com/v1/forecast?latitude=<lat>&longitude=<lon>&hourly=global_tilted_irradiance,temperature_2m&tilt=<inclinaison>&azimuth=<azimut>&timezone=auto&forecast_days=2`
- Vérifié le 2026-11-04 (documentation officielle et essais) : azimut 0 = sud, −90 = est, +90 = ouest ; `tilt` et `azimuth` sont bien pris en compte ; `global_tilted_irradiance` est la **moyenne de l'heure précédente** (W/m²). On ajoute `timeformat=unixtime` (heures en epoch, sans ambiguïté au changement d'heure).
- Stocker 48 valeurs horaires d'irradiance (W/m²) et de température (°C), interpolées linéairement sur les créneaux de 30 min.
- ⚠ L'API gratuite d'Open-Meteo est réservée à un **usage non commercial**. Pour une commercialisation, prévoir un abonnement ou une autre source, en gardant le client isolé dans `forecast/`.

---

## 9. Algorithme

### 9.1 Principe : la valeur de chaque kWh
À tout instant, chaque usage possible du surplus a une **valeur** en €/kWh :

| Usage | Valeur |
|---|---|
| Batterie | `prix_HP_à_venir × η_batt − coût_usure` |
| Chauffe-eau | `prix_HC` (sinon il chaufferait en heures creuses) |
| Injection réseau | `prix_revente` |

On sert les usages **par valeur décroissante**. Avec un tarif HP/HC classique, on obtient donc **batterie > chauffe-eau > injection**, **sauf** si le plan prévoit que la batterie sera pleine de toute façon (§9.3). Le code doit calculer ces valeurs depuis la config, pas coder l'ordre en dur : c'est ce qui permettra d'ajouter d'autres charges plus tard.

`coût_usure = prix_batterie / (cycles_garantis × capacité_utile_kWh)`.

### 9.2 Prévisions par RLS

**Moindres carrés récursifs** avec facteur d'oubli λ (`algo/rls.c`, dimension `n ≤ 4`, tout en `float`) :
```
k     = P·x / (λ + xᵀ·P·x)
e     = y − θᵀ·x
θ     = θ + k·e
P     = (P − k·xᵀ·P) / λ
```
Garde-fous obligatoires :
- Ne pas mettre à jour si `y` ou `x` sont invalides, NaN ou hors bornes.
- Si `trace(P) > 1e6` ou qu'un élément de θ est NaN → **réinitialiser** (θ = θ₀, P = P₀) et journaliser l'événement.
- Prédiction bornée : `[0, max_physique]`. Si le modèle a moins de 96 échantillons (2 jours) → utiliser le **repli** (ci-dessous).
- Persister θ, P et le nombre d'échantillons en NVS chaque jour, et à chaque mise à jour en mode debug.

**a) Production solaire réelle** (un échantillon par créneau de 30 min avec `gti > 20 W/m²`) :
- `y` = énergie PV mesurée sur le créneau (Wh, intégrée depuis l'Envoy).
- `x = [gti_prévu, gti_prévu × (T_prévue − 25), 1]`.
- θ₀ = `[kWc × 0,85 × 0,5, 0, 0]` (0,5 h par créneau). λ = 0,998.
- Repli : `kWc × 0,85 × gti/1000 × 0,5` Wh.
- C'est ce modèle qui apprend les ombrages, l'orientation réelle et le biais de la météo.

**b) Consommation de base** (`p_base`, **hors chauffe-eau**, un échantillon par créneau) :
- D'abord un **profil EMA** de 48 créneaux × 2 types de jour (semaine / week-end) : `prof = 0,9·prof + 0,1·mesure`.
- `y` = énergie `p_base` du créneau (Wh).
- `x = [prof[créneau][type_jour], max(0, 15 − T_prévue), 1]` (le second terme capture le chauffage électrique éventuel).
- θ₀ = `[1, 0, 0]`. λ = 0,998.
- Repli : `prof[créneau][type_jour]`, puis 300 Wh par créneau si le profil est vide.

### 9.3 Plan journalier (`planner.c`)
Calculé à 05:30, puis recalculé toutes les heures à partir de l'état réel. Un **plan de nuit** est aussi calculé au début de chaque plage HC, avec la prévision du lendemain (§9.5).

Entrées : prévisions PV et consommation par créneau pour les 24 h à venir, SOC actuel, config.

1. `S` = Σ sur la journée de `max(0, PV_prévu − base_prévue)`, le surplus solaire prévu (Wh).
2. `B` = énergie nécessaire pour couvrir les créneaux **HP** sans soleil jusqu'au prochain début d'HC, plafonnée à `capa_utile − énergie_actuelle`.
3. `E_ce` = énergie encore nécessaire pour que le ballon soit plein avant `heure_douches` : `E_full` (§9.5) moins ce qui a déjà été chauffé dans le cycle en cours, y compris la préchauffe de fin de nuit.
4. Mode du chauffe-eau :
   - `S < B` → `CE_HC_ONLY` (tout le solaire va à la batterie).
   - `B ≤ S < B + E_ce` → `CE_SOLAR_SURPLUS` (batterie prioritaire, chauffe-eau sur le surplus restant, complément en HC).
   - `S ≥ B + E_ce` → `CE_SOLAR_EARLY` (chauffe-eau autour du pic de midi, la batterie absorbe les écarts).
5. Courbe de SOC cible par créneau : rejoindre `B` avant la fin de la production. Le chauffe-eau n'a le droit de puiser dans la batterie que si `SOC > SOC_cible(créneau)`.

Sortie : une structure `plan_t` exposée dans l'API (`/api/plan`) et affichée dans l'interface.

### 9.4 Boucle temps réel (`regulator_step`, toutes les 5 s)

Fonction **pure** : `regulator_step(const reg_input_t*, reg_state_t*, reg_output_t*)`.

**Batterie (asservissement zéro injection / zéro soutirage)** :
- En **HP** : consigne `p_batt_cible = p_batt_actuelle + p_grid` (vise 0 W au compteur), avec une zone morte de ±30 W, une rampe max de ±300 W par pas, et des bornes `[−p_charge_max, +p_décharge_max]`. Décharge interdite si `SOC ≤ SOC_min`.
- En **HC** : **pas de décharge**. Charge seulement depuis le surplus solaire (rare la nuit) ou en arbitrage (§9.6).
- Surplus solaire (`p_grid < −30`) : charger, sauf si le chauffe-eau est prioritaire selon le plan.

**Chauffe-eau** :
- Démarrage si `mode ≠ CE_HC_ONLY` **et** surplus disponible `(−p_grid + p_ce + charge_batt_actuelle) ≥ seuil_on` (par défaut 1500 W) pendant 2 min **et** `SOC ≥ SOC_cible(créneau)`.
- Arrêt si l'énergie disponible (surplus plus batterie autorisée) est < 2500 W pendant 3 min.
- Durée minimale ON 10 min, durée minimale OFF 5 min.
- **Thermostat coupé** : relais ON et `p_ce < 100 W` pendant 3 min → `tank_full = true` (remis à `false` à `heure_douches`, au début d'un nouveau cycle, §9.5). Dès que `tank_full` est vrai, plus aucune tentative de chauffe solaire ce jour-là, et le surplus va à la batterie.
- **Forçage utilisateur** (ON / OFF / AUTO pour N minutes) : prioritaire, sauf sur les sécurités.

### 9.5 Stratégie du chauffe-eau (douches le soir)

Les douches ont lieu le soir (`heure_douches`, par défaut 20:00) et l'eau refroidit dans le ballon (pertes statiques). **Plus la chauffe se termine près de l'heure des douches, mieux c'est.** Le soleil de l'après-midi est idéal ; la nuit, il faut chauffer **le plus tard possible**, jamais en début de plage HC.

**Cycle de chauffe** : de `heure_douches` le jour J à `heure_douches` le jour J+1. Au début de chaque cycle, `tank_full = false` et `E_chauffée_cycle = 0`.

**Garantie (NON NÉGOCIABLE)** : le ballon doit atteindre la coupure du thermostat (`tank_full`) **au moins une fois par cycle, avant `heure_douches`**. Cette garantie couvre aussi l'anti-légionelle, à condition que le thermostat soit réglé à 55 °C ou plus (le rappeler dans l'interface).

**Énergie d'une chauffe complète, apprise** : `E_full` = énergie consommée entre l'allumage et la coupure du thermostat, mesurée sur les chauffes démarrées juste après les douches (ballon froid). Mise à jour par moyenne glissante (α = 0,2). Valeur initiale : `puissance × durée_chauffe_config`. On en déduit `T(E) = E / puissance`.

**1. Plan de nuit** (calculé au début de chaque plage HC, avec la prévision du lendemain) :
- `S_ce` = surplus solaire prévu pour le chauffe-eau sur les créneaux qui se terminent avant `heure_douches` : `max(0, S − B)` (§9.3) × `marge_prévision` (par défaut 0,7, pour ne pas surestimer le soleil).
- Énergie à préchauffer en HC : `E_nuit = clamp(E_full − E_chauffée_cycle − S_ce, 0, E_full)`. Si `E_nuit < 300 Wh` → pas de chauffe de nuit, le soleil s'en charge.
- **Placement au plus tard** : début = `fin_HC − T(E_nuit) − marge_démarrage` (par défaut 15 min), jamais avant le début de la plage HC. Si la plage est trop courte, démarrer à son début.
- Si plusieurs plages HC existent dans le cycle (par exemple une plage en début d'après-midi), placer la chauffe dans celle qui **se termine le plus près avant `heure_douches`**.
- Une chauffe de nuit partielle s'arrête quand `E_nuit` est atteinte ; une chauffe complète s'arrête à la coupure du thermostat ou à la fin de la plage HC.

**2. Journée** : routage solaire normal (§9.4). Le soleil complète ce que la nuit n'a pas fait.

**3. Rattrapage** : à l'instant `heure_douches − T(E_full − E_chauffée_cycle) − marge_démarrage`, si `!tank_full` → chauffe forcée, **même en HP**, jusqu'à la coupure du thermostat. Option `rattrapage_HP` (activée par défaut). Si elle est désactivée : journaliser et afficher l'alerte « eau peut-être tiède ce soir ».

**4. Batterie en HC** : maintien, pas de décharge (inchangé).

Afficher dans l'interface la prochaine chauffe prévue (heure, durée, origine : soleil / HC / rattrapage).

### 9.6 Arbitrage réseau (désactivé par défaut)
Charger la batterie en HC pour la décharger en HP seulement si :
`prix_HP − prix_HC / η_batt > coût_usure` **et** que la prévision PV du lendemain ne suffit pas à remplir `B`.
Afficher le gain calculé par kWh dans les Paramètres, pour que l'utilisateur voie pourquoi l'option est rentable ou non.

### 9.7 Anti-légionelle
Couverte par la garantie de chauffe quotidienne (§9.5). Afficher la date de la dernière coupure du thermostat, et une alerte si aucun `tank_full` depuis plus de 24 h.

### 9.8 Injection
En dernier recours seulement (batterie pleine **et** `tank_full`). Aucune action particulière : c'est l'état résiduel.

### 9.9 Indicateurs : autoconsommation, gains et rentabilité (`algo/stats.c`, `algo/flows.c`)

**Répartition des flux** (`flows_split()`, partagée avec le diagramme §11.3) : à partir de `p_pv`, `p_grid`, `p_batt` et `p_home`, avec `charge = max(0, −p_batt)`, `dis = max(0, p_batt)` :
```
pv_home   = min(p_pv, p_home)
pv_batt   = min(p_pv − pv_home, charge)
pv_grid   = p_pv − pv_home − pv_batt
batt_home = min(dis, p_home − pv_home)
grid_home = p_home − pv_home − batt_home
grid_batt = charge − pv_batt
```
Toutes les valeurs sont bornées à ≥ 0. Part locale du chauffe-eau : `ce_local = p_ce × (pv_home + batt_home) / p_home`.

**Compteurs d'énergie**, intégrés **à chaque échantillon du Shelly** (méthode des trapèzes, Δt réel ; ignorer Δt > 10 s). Ce pas fin est indispensable, car le signe de `p_grid` change souvent à l'intérieur d'un créneau de 30 min :
`E_pv, E_import, E_export, E_home, E_batt_ch, E_batt_dis, E_ce, E_ce_local`, plus les trois coûts ci-dessous.
Optionnel : recaler `E_import` et `E_export` sur les compteurs du Shelly (`EMData.GetStatus`, `TODO(verify)`).

**Taux** (par jour, 7 jours, 30 jours, année, depuis la mise en service) :
- **Autoconsommation** = `(E_pv − E_export) / E_pv` : part de la production utilisée sur place, batterie comprise.
- **Autoproduction** (autarcie) = `1 − E_import / E_home` : part de la consommation couverte localement.

**Coûts**, calculés à chaque pas avec le prix en vigueur `prix(t)` :
- Réel : `c_réel = E_import·prix(t) − E_export·prix_revente`.
- Sans batterie : recalculer avec `p_grid_sb = p_grid + p_batt` (ce que la batterie a fourni aurait été importé, ce qu'elle a stocké aurait été exporté), en séparant l'import et l'export **après** ce recalcul.
- Sans installation (ni PV ni batterie) : `c_sans_install = E_home·prix(t)`.

**Gains** :
- `gain_batt = c_sans_batt − c_réel` ; `gain_batt_net = gain_batt − E_batt_dis × coût_usure`.
- `gain_total = c_sans_install − c_réel`.
- `gain_routage_ce = E_ce_local × prix_HC` (approximation : sans routage, le chauffe-eau aurait chauffé en HC).

**Rentabilité de la batterie** :
- Cumul de `gain_batt_net` depuis `date_mise_en_service`.
- Projection annuelle : les mois observés comptent pour leur valeur réelle. Pour un mois non observé :
  `jours × min(capa_utile, surplus_journalier_mois) × (η·prix_HP − prix_revente)`,
  où `surplus_journalier_mois` = surplus moyen observé ce mois-là les années précédentes, sinon `kWc × productible × fraction_mois × ratio_surplus / jours`.
  Par défaut : `productible` = 1200 kWh/kWc/an, `ratio_surplus` = surplus/PV observé (0,5 tant que l'historique fait moins de 30 jours), `fraction_mois` = `[3, 5, 8, 10.5, 12, 13, 13.5, 12, 9, 6.5, 4, 3.5] %` (approximation, modifiable dans les Paramètres, `TODO(verify)` selon le site).
- Temps de retour = `prix_batterie / gain_batt_net_annuel_projeté`. Date de rentabilité estimée = `date_mise_en_service + temps de retour`. Tant que moins de 12 mois sont observés, afficher le résultat comme **« estimation »**, avec une fourchette de ±20 %.
- Persistance dans LittleFS : agrégats journaliers (365 jours glissants, environ 64 octets par jour) et mensuels (sans limite).

---

## 10. Sécurités et repli (NON NÉGOCIABLE)

| Situation | Action |
|---|---|
| Démarrage de l'ESP32 | Batterie au repos (0 W), chauffe-eau en mode **HC uniquement**, pas de régulation avant NTP + Shelly valide |
| Shelly invalide plus de 30 s | Batterie au repos, chauffe-eau OFF hors HC |
| Envoy invalide | La régulation continue (le Shelly suffit). Prévision PV gelée, pas de mise à jour du RLS |
| Zendure invalide | Chauffe-eau seulement sur un surplus réel mesuré par le Shelly, batterie ignorée |
| Tongou invalide | Journaliser, réessayer, et compter sur la sécurité côté appareil (§8.4) |
| NTP perdu plus de 24 h | Heure dérivée de l'horloge interne, alerte dans l'interface |
| Heap libre < 40 Ko | Journaliser ; si < 25 Ko → redémarrage contrôlé après mise au repos des appareils |
| Mise à jour OTA | Mettre les appareils dans leur état de repli **avant** le redémarrage |

Autres règles :
- Détection de brownout activée. Task Watchdog sur toutes les tâches.
- Toute valeur de config est **validée** (bornes, cohérence) avant enregistrement. Configuration invalide → refus HTTP 400 avec le champ fautif.
- Chaque décision du régulateur est journalisée sur une ligne (raison et valeurs), consultable dans la page Système (tampon circulaire de 200 lignes en RAM).

---

## 11. Interface web

### 11.1 Contraintes techniques
- **HTML, CSS et JS vanilla**, sans framework ni CDN (fonctionnement 100 % hors ligne sur le réseau local).
- Fichiers minifiés et compressés en gzip par `tools/build_web.py`, servis avec `Content-Encoding: gzip`. **Budget total ≤ 100 Ko compressés.**
- Graphiques : SVG ou canvas écrits à la main (pas de Chart.js).
- Temps réel par **WebSocket** `/ws` (état toutes les 2 s), avec repli en interrogeant `/api/state` si le WebSocket tombe.
- Responsive : mobile d'abord (largeur 360 px), puis bureau.
- Langue : français.

### 11.2 Palette (noir, rouge, blanc), à définir en variables CSS uniquement
```css
:root {
  --bg:        #0a0a0a;
  --surface:   #141414;
  --surface-2: #1e1e1e;
  --line:      #2a2a2a;   /* flux inactif */
  --red:       #e10600;   /* accent principal, import réseau, alertes */
  --red-soft:  #ff4d4d;   /* batterie */
  --red-dim:   #4a0a0a;   /* fonds d'alerte */
  --white:     #ffffff;   /* solaire, maison, valeurs principales */
  --text-2:    #b3b3b3;
  --text-3:    #6b6b6b;   /* valeurs à 0 W */
}
```
Correspondance : **solaire = blanc**, **import réseau = rouge**, **export réseau = blanc en pointillés**, **batterie = rouge clair**, **chauffe-eau = rouge clair**, **maison = blanc**, flux à 0 W = `--line` / `--text-3`. Thème sombre uniquement.

### 11.3 Diagramme des flux d'énergie (page d'accueil)
Reproduire la disposition de la capture de référence (carte de flux d'énergie façon Home Assistant) :
- **Solaire** en haut au centre (icône panneau, valeur au-dessus), **Réseau** à gauche (icône pylône, valeur dessous avec une flèche → pour l'import et ← pour l'export), **Batterie** à droite (icône batterie, valeur et SOC), **Maison** en bas au centre (grande icône, valeur dessous), **Chauffe-eau** en bas à gauche, à côté de la maison.
- Les liaisons sont des **chemins orthogonaux aux coins arrondis**, parallèles. Les lignes passent en couleur quand le flux est > 10 W.
- Des **points animés** se déplacent le long des chemins actifs, dans le sens du flux, à une vitesse proportionnelle à la puissance (bornée). Les animations utilisent `requestAnimationFrame` et se mettent en pause si l'onglet est caché.
- Flux à afficher : PV→Maison, PV→Batterie, PV→Réseau, Réseau→Maison, Réseau→Batterie, Batterie→Maison, Maison→Chauffe-eau.
- Icônes en SVG intégré (pas de police d'icônes).
- Les valeurs de chaque flux viennent de `flows_split()` (§9.9), la même fonction que pour les statistiques : le diagramme et les chiffres sont toujours cohérents.

**Améliorations par rapport à la carte Home Assistant** (garder le même type de graphe) :
1. **Épaisseur des traits proportionnelle à la puissance** (échelle en racine carrée, de 2 à 6 px) : on voit d'un coup d'œil le flux dominant.
2. **Anneau de provenance autour de la maison** : un cercle segmenté en solaire (blanc), batterie (rouge clair) et réseau (rouge), proportionnellement à `pv_home`, `batt_home` et `grid_home`.
3. **Anneau de SOC autour de la batterie**, avec une marque de la cible du plan, et un indicateur ▲ charge / ▼ décharge.
4. **Nœud chauffe-eau enrichi** : niveau de remplissage stylisé (0 → `E_full`, plein quand `tank_full`), badge du mode (Soleil / HC / Rattrapage / Forcé), et heure de la prochaine chauffe prévue.
5. **Bascule « Instantané (W) / Aujourd'hui (kWh) »** : le même diagramme affiche les énergies cumulées de la journée.
6. **Toucher un nœud** ouvre une mini-courbe des 2 dernières heures de ce nœud.
7. **Accessibilité** : le sens d'un flux n'est jamais porté par la seule couleur (flèches et signes), les valeurs restent en texte.

**Jauge d'autoconsommation** (à côté du diagramme sur bureau, dessous sur mobile) :
- Demi-cercle SVG de 0 à 100 % : arc en `--white` sur un fond `--line`, valeur en grand au centre.
- Sous la jauge, en plus petit : le taux d'**autoproduction**.
- Sélecteur de période : Aujourd'hui / 7 j / 30 j / Année.
- Seuils de couleur de l'arc : < 50 % `--red`, 50–80 % `--red-soft`, > 80 % `--white`.

**Carte « Gains et rentabilité »** :
- Gain du jour et du mois en € (total, dont batterie, dont routage du chauffe-eau).
- Cumul du gain net de la batterie, avec une barre de progression vers le prix de la batterie (retour sur investissement).
- « Batterie rentabilisée en ~AAAA » (ou « dans ~N ans »), avec la mention « estimation » et la fourchette tant que l'historique fait moins de 12 mois.
- Lien vers le détail mensuel (graphique en barres : gain réel et gain projeté).

Autres cartes sous le diagramme : tarif en cours (HP/HC) avec le prochain changement, et état de la garantie d'eau chaude (✔ ballon chauffé aujourd'hui, ou heure de la chauffe prévue).

### 11.4 Autres pages
- **Prévisions** : PV prévu (modèle RLS) et PV réel, consommation de base prévue et réelle, sur 24 h. Indicateurs de qualité des modèles : erreur moyenne absolue sur 7 jours, nombre d'échantillons.
- **Historique** : 24 h et 7 jours (créneaux de 30 min). Courbes empilées production / consommation / batterie / réseau.
- **Paramètres** : formulaire par sections (§11.6), validation côté client **et** côté serveur.
- **Système** : version, temps de fonctionnement, heap (actuel et minimum), RSSI WiFi, état de chaque appareil (dernière lecture, erreurs), échéance du jeton Envoy, journal des décisions, téléversement OTA protégé par mot de passe, export CSV de l'historique.

### 11.5 API REST
| Méthode | Route | Rôle |
|---|---|---|
| GET | `/api/state` | état instantané (toutes les puissances, SOC, modes, validité) |
| GET | `/api/plan` | plan du jour, SOC cible, mode du chauffe-eau |
| GET | `/api/forecast` | prévisions PV et consommation par créneau (brutes et corrigées par les RLS) |
| GET | `/api/history?range=24h\|7d` | séries de 30 min |
| GET | `/api/stats?period=day\|7d\|30d\|year\|all` | autoconsommation, autoproduction, énergies, coûts, gains, projection de rentabilité |
| GET / POST | `/api/config` | lire / écrire la configuration (le POST exige le mot de passe administrateur) |
| POST | `/api/override` | `{"target":"ce","mode":"on\|off\|auto","minutes":N}` |
| POST | `/api/models/reset` | réinitialiser les RLS ou le profil (mot de passe) |
| GET | `/api/log` | journal des décisions |
| POST | `/api/ota` | téléversement du firmware (mot de passe) |
| WS | `/ws` | envoi de `state` toutes les 2 s |

Toutes les réponses sont en JSON, les erreurs au format `{"error":"...","field":"..."}`. Les secrets (jeton Envoy, mot de passe) ne sont **jamais** renvoyés en lecture : on renvoie `"***"` si la valeur est définie.

### 11.6 Paramètres configurables (NVS)
| Section | Champs (unité, bornes) |
|---|---|
| **Tarif** | type (`base` / `hphc`) ; jusqu'à 4 plages HC `HH:MM–HH:MM` ; prix HP, prix HC, prix base (€/kWh, 0–2) ; prix de revente (€/kWh, 0–1). **À saisir obligatoirement au premier démarrage** (pas de valeur par défaut fictive) |
| **Installation PV** | puissance crête (kWc, 0,1–36) ; latitude / longitude ; inclinaison (°, 0–90) ; azimut (°) |
| **Batterie** | capacité utile (kWh, par défaut 3,2) ; puissance max de charge et de décharge (W, à vérifier sur la fiche du SF 1600 AC+) ; rendement aller-retour (0,5–1, par défaut 0,88) ; SOC min / max (%) ; prix (€) et cycles garantis (pour le coût d'usure) ; date de mise en service ; arbitrage réseau (on/off) |
| **Chauffe-eau** | puissance nominale (W, par défaut 2500) ; durée de chauffe complète initiale (min, par défaut 105, ensuite apprise) ; heure des douches (HH:MM, par défaut 20:00) ; marge de prévision solaire (0,3–1, par défaut 0,7) ; marge de démarrage (min, par défaut 15) ; rattrapage en HP (on/off, par défaut on) ; seuil de démarrage solaire (W, par défaut 1500) ; durées min ON/OFF (min) |
| **Rentabilité** | productible (kWh/kWc/an, par défaut 1200) ; répartition mensuelle de la production (12 valeurs en %, total = 100) |
| **Appareils** | IP Shelly ; IP et jeton Envoy, version de firmware (<7 / ≥7) ; IP et numéro de série Zendure ; IP Tongou |
| **Système** | mot de passe administrateur ; période de sondage ; mode `observateur` (on/off, voir §13) |

La config est stockée comme **blob versionné** (`cfg_version`, CRC32). Si la version change → migration, ou valeurs par défaut accompagnées d'une alerte dans l'interface.

---

## 12. Tests

### 12.1 Tests sur PC (obligatoires pour `algo/`)
- `rls` : convergence vers des coefficients connus, comportement avec λ, réinitialisation sur NaN.
- `tariff` : plages HC qui passent minuit, changements d'heure été/hiver.
- `planner` : trois scénarios (S < B, B ≤ S < B+E_ce, S ≥ B+E_ce), batterie déjà pleine, prévision absente.
- `regulator` : journée ensoleillée, journée nuageuse, hiver, coupure du thermostat, Shelly invalide, forçage utilisateur, respect des durées min ON/OFF, pas de décharge en HC.
- Chauffe-eau (§9.5) : la chauffe de nuit **se termine à la fin de la plage HC** et ne commence jamais à son début ; préchauffe partielle quand le lendemain est ensoleillé ; aucune chauffe de nuit si `S_ce ≥ E_full` ; rattrapage en HP déclenché à la bonne heure ; deux plages HC (choix de la plus proche des douches) ; **garantie d'un `tank_full` par cycle sur 30 jours simulés** avec des météos variées.
- `flows` : conservation de l'énergie (sommes des flux = puissances des nœuds), aucune valeur négative, cas où la batterie charge depuis le réseau.
- `stats` : taux connus sur des scénarios simples, `gain_batt = 0` si la batterie reste au repos, signe du gain correct pour une journée HP typique, projection et temps de retour sur un historique fictif de 3 mois puis de 13 mois.

### 12.2 Mode simulation
`CONFIG_APP_SIMULATION=y` : les pilotes sont remplacés par des faux qui rejouent un CSV (`tools/replay_csv.py`) à vitesse ×1 à ×60. L'interface web fonctionne alors à l'identique, pour développer le front sans le matériel.

### 12.3 Critères d'acceptation
- 72 h sans redémarrage, heap minimum ≥ 40 Ko.
- Aucune écriture Zendure plus fréquente qu'une toutes les 10 s, et aucun basculement du relais qui ne respecte pas les durées min.
- Débrancher un appareil déclenche le bon repli en moins de 30 s.
- Sur une semaine réelle : le ballon atteint la coupure du thermostat à chaque cycle avant l'heure des douches.
- Les énergies intégrées (`E_import`, `E_export`) s'écartent de moins de 3 % des compteurs du Shelly sur 24 h.

---

## 13. Feuille de route (dans l'ordre, ne pas sauter d'étape)

1. **M0** : squelette, partitions, WiFi STA plus portail SoftAP « Solaire-Setup » au premier démarrage, NTP, config NVS, page Système.
2. **M1** : pilotes **en lecture seule** pour les 4 appareils, et diagramme des flux en direct.
3. **M2** : historique 30 min, Open-Meteo, profil EMA, RLS. Affichage des prévisions comparées au réel. Jauge d'autoconsommation et carte « Gains et rentabilité » (lecture seule, aucune action).
4. **M3** : planner et régulateur en **mode observateur** : les décisions sont calculées et journalisées, **aucune commande n'est envoyée**. Valider au moins une semaine.
5. **M4** : pilotage réel, avec toutes les sécurités du §10 et le script de sécurité OpenBeken en place.
6. **M5** : finitions de l'interface, export CSV, OTA local, documentation utilisateur.

---

## 14. Règles de travail pour Claude

- Après chaque modification : `idf.py build`, tests PC si `algo/` est touché, et vérification de `idf.py size`.
- Conserver la séparation **algo pur ↔ ESP-IDF**. Toute nouvelle logique de décision va dans `algo/` avec un test.
- Identifiants et code en **anglais**, commentaires et interface en **français**. Un tag `ESP_LOGx` par composant.
- Vérifier **tous** les codes de retour (`ESP_ERROR_CHECK` uniquement à l'initialisation ; ailleurs, gérer l'erreur).
- Ne jamais coder en dur une IP, un jeton, un prix ou un horaire HC : tout passe par `config`.
- Ne jamais coder un endpoint ou un champ marqué `TODO(verify)` comme s'il était confirmé : isoler la valeur dans une constante nommée, commentée `TODO(verify)`.
- Petits commits ciblés, un par fonctionnalité, avec un message en français.
