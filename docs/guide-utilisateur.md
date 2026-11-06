# Gestionnaire d'énergie solaire — guide d'utilisation

Le boîtier (un ESP32 sur le réseau local) lit votre production solaire, votre consommation, la
batterie et le chauffe-eau. Il décide en continu où envoyer le surplus solaire, pour payer
l'électricité le moins cher possible. Tout reste chez vous : le seul accès à internet sert à
récupérer les prévisions météo (Open-Meteo).

Appareils pris en charge :

| Rôle | Appareil |
|---|---|
| Compteur réseau (indispensable) | Shelly Pro 3EM |
| Production solaire | Enphase Envoy / IQ Gateway |
| Batterie | Zendure SolarFlow 1600 AC+ |
| Chauffe-eau | interrupteur Tongou (firmware Tuya d'origine ou OpenBeken) |

---

## 1. Première mise en route

1. Branchez le boîtier sur une alimentation USB 5 V de bonne qualité (1 A minimum).
2. Au premier démarrage, il crée un réseau WiFi **« Solaire-Setup »** (sans mot de passe).
   Connectez-y un téléphone, puis ouvrez **http://192.168.4.1**.
3. Saisissez le nom et le mot de passe de votre WiFi, un **mot de passe administrateur**
   (8 caractères minimum, il protège toutes les modifications) et le nom réseau du boîtier
   (par défaut `solaire`).
4. Le boîtier redémarre et rejoint votre WiFi. L'interface est alors accessible sur
   **http://solaire.local** (ou par son adresse IP, visible dans votre box).

Si le WiFi devient injoignable (5 échecs de suite), le réseau « Solaire-Setup » se rouvre
pour vous permettre de corriger les identifiants.

## 2. Préparer les appareils

**Shelly Pro 3EM** : laissez l'authentification désactivée sur le réseau local. Notez son
adresse IP. Pour une installation monophasée, choisissez le profil « Monophasé » et le canal
qui mesure l'arrivée du réseau.

**Enphase Envoy** (firmware 7 ou plus) : il faut un jeton propriétaire, valable environ un an.
Récupérez-le sur entrez.enphaseenergy.com (connexion avec votre compte Enlighten), puis collez-le
dans Paramètres → Appareils. L'échéance du jeton est affichée dans la page Système, avec une
alerte un mois avant.

**Zendure SolarFlow** : dans l'application Zendure, activez l'**API locale (HEMS)** et
**désactivez tout mode intelligent** (lecture de compteur, CT). Le boîtier doit être le seul à
piloter la batterie. Notez l'adresse IP et le numéro de série.

**Tongou (Tuya d'origine)** : il faut l'identifiant de l'appareil et sa **clé locale**. Sur un
PC, lancez `python tools/tuya_keys.py`, scannez le QR code avec l'application Smart Life / Tuya,
et l'outil affiche les clés de vos appareils. La version du protocole (3.3, 3.4 ou 3.5) se
vérifie avec `python tools/tuya_ref.py probe`.

Fixez si possible les adresses IP des appareils dans votre box (bail DHCP permanent).

## 3. Paramètres

Toute modification demande le mot de passe administrateur. Une valeur hors bornes est refusée
et le champ fautif est signalé.

| Section | À renseigner |
|---|---|
| **Tarif** | **Obligatoire.** Type : *Base*, *HP/HC*, ou *HP/HC + week-end + saisons*. Plages d'heures creuses, prix TTC au kWh (abonnement exclu), prix de revente. |
| **Installation PV** | Puissance crête, latitude et longitude, inclinaison, azimut (0 = sud, −90 = est, +90 = ouest). |
| **Batterie** | Capacité utile, puissances max, rendement, SOC min et max, prix et cycles garantis (coût d'usure), date de mise en service. |
| **Chauffe-eau** | Puissance, durée d'une chauffe complète (apprise ensuite), **heure des douches**, marges, rattrapage en heures pleines. |
| **Rentabilité** | Productible annuel et répartition mensuelle (servent à estimer les mois pas encore observés). |
| **Appareils** | Adresses IP, jeton Envoy, numéro de série SolarFlow, identifiants Tuya. |
| **Système** | Période de lecture du compteur, **mode observateur**, nom réseau, WiFi, mot de passe. |

**Tarif « HP/HC + week-end + saisons »** : saisissez les 8 prix TTC (saison basse et haute ×
semaine et week-end × HP et HC), cochez les mois de saison haute, et indiquez si les jours fériés
comptent comme le week-end. Les plages d'heures creuses s'appliquent tous les jours ; le week-end
a simplement ses propres prix.

**Mode observateur** : le boîtier calcule et journalise ses décisions sans rien commander.
Il est activé par défaut ; désactivez-le seulement quand les mesures vous paraissent justes.

## 4. Les pages

**Accueil** : le diagramme des flux en direct. L'épaisseur des traits suit la puissance ;
les points avancent dans le sens du flux. L'anneau autour de la maison montre d'où vient
l'énergie consommée (solaire, batterie, réseau). Touchez un élément pour voir ses 2 dernières
heures. La bascule « W / kWh » affiche les énergies cumulées du jour.

Sous le diagramme :
- **Autoconsommation** : part de votre production utilisée sur place (batterie comprise).
  Dessous, l'**autoproduction** : part de votre consommation couverte par le solaire et la
  batterie.
- **Gains et rentabilité** : économies par rapport à une maison sans installation, dont la part
  due à la batterie (nette de son usure) et au chauffe-eau ; cumul vers le prix de la batterie
  et année de rentabilité estimée (« estimation » tant qu'il y a moins de 12 mois d'historique).
- **Tarif en cours** et prochain changement de prix.
- **Eau chaude** : ballon chauffé aujourd'hui, ou prochaine chauffe prévue (soleil, heures
  creuses ou rattrapage). Boutons de forçage ON / OFF / AUTO pour une durée choisie.

**Prévisions** : production et consommation prévues comparées au réel, et qualité des modèles
(erreur moyenne sur 7 jours). Les prévisions s'affinent seules : comptez quelques jours
d'apprentissage.

**Historique** : courbes des dernières 24 h et 7 jours, SOC de la batterie, tableau des données,
et **export CSV** (créneaux de 30 min sur 7 jours, ou bilans journaliers sur un an) lisible
directement par Excel ou LibreOffice.

**Système** : version, mémoire, WiFi, état de chaque appareil, journal des décisions du
régulateur, et **mise à jour**.

## 5. Comment le boîtier décide

- **Batterie** : en heures pleines, elle compense la maison pour ne rien acheter ni vendre au
  réseau. En heures creuses, elle ne se décharge pas (l'électricité y est moins chère que ce que
  la batterie a coûté à stocker).
- **Chauffe-eau** : chaque soir avant l'heure des douches, le ballon doit avoir atteint la coupure
  de son thermostat. Le boîtier vise le soleil de l'après-midi ; s'il n'y en aura pas assez, il
  chauffe la nuit le **plus tard possible** en heures creuses ; en dernier recours, il lance un
  rattrapage avant les douches, même en heures pleines.
- **Surplus** : il va à l'usage qui rapporte le plus (en général batterie, puis chauffe-eau, puis
  revente au réseau).
- Réglez le thermostat du ballon à **55 °C ou plus** : la chauffe complète quotidienne couvre
  alors le risque de légionelle.

## 6. Sécurités

| Situation | Ce que fait le boîtier |
|---|---|
| Démarrage | Batterie au repos, chauffe-eau en heures creuses uniquement, attente de l'heure et du compteur |
| Compteur Shelly muet plus de 30 s | Batterie au repos, chauffe-eau coupé hors heures creuses |
| Envoy muet | La régulation continue (le Shelly suffit) ; prévisions figées |
| SolarFlow muet | Chauffe-eau seulement sur un surplus réellement mesuré |
| Mémoire basse | Mise au repos des appareils puis redémarrage contrôlé |
| Mise à jour | Mise au repos des appareils avant le redémarrage |

## 7. Mise à jour

Les mises à jour se font par le réseau, sans câble, et **conservent l'historique**.

- Depuis la page **Système → Mise à jour** : choisissez les fichiers de l'interface
  (`littlefs_image/www/*.gz`) et/ou le firmware (`build/solar_center.bin`), saisissez le mot de
  passe administrateur, puis « Envoyer ».
- Ou depuis un PC : `python tools/ota.py <adresse>` (interface puis firmware), `--www` ou `--fw`
  pour n'envoyer que l'un des deux.

Si le nouveau firmware ne fonctionne pas 2 minutes d'affilée, le boîtier revient seul à l'ancien.

**Attention** : la commande `idf.py storage-flash` (par câble) réécrit toute la partition de
l'interface et **efface l'historique**.

## 8. Dépannage

| Symptôme | Piste |
|---|---|
| Interface injoignable | Le boîtier a-t-il rejoint le WiFi ? Sinon, cherchez le réseau « Solaire-Setup ». Essayez l'adresse IP au lieu de `solaire.local`. |
| Un appareil apparaît « en erreur » | Page Système : dernière lecture et nombre d'erreurs. Vérifiez son adresse IP, et pour l'Envoy l'échéance du jeton. |
| Redémarrages fréquents (cause « mise sous tension ») | Alimentation trop faible ou câble USB défaillant : changez-les. |
| Signal WiFi faible (RSSI sous −70 dBm) | Rapprochez le boîtier de la box ou d'un répéteur. |
| « Eau peut-être tiède ce soir » | Le rattrapage en heures pleines est désactivé et le ballon n'a pas atteint la coupure. |
| Prévisions absentes | Latitude et longitude non renseignées, ou internet coupé. |

L'API gratuite d'Open-Meteo est réservée à un usage non commercial.
