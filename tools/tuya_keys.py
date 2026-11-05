#!/usr/bin/env python3
"""Récupère l'identifiant et la clé locale des appareils Tuya par QR code, comme Home Assistant.

À lancer une seule fois sur le PC (l'ESP32 n'accède jamais au cloud Tuya) :
  python -m venv .venv-tuya
  .venv-tuya\\Scripts\\pip install tuya-device-sharing-sdk qrcode tinytuya
  .venv-tuya\\Scripts\\python tools/tuya_keys.py

Séquence reprise de l'intégration Tuya de Home Assistant (config_flow.py, coordinator.py ; relue le
2026-11-05) avec sa bibliothèque officielle tuya-device-sharing-sdk. L'outil s'identifie auprès de
Tuya avec l'identifiant d'application de Home Assistant (TUYA_CLIENT_ID / TUYA_SCHEMA de const.py).

Le « User Code » se trouve dans Smart Life ou Tuya Smart : Paramètres > Compte et sécurité > User Code
(sensible à la casse). La clé locale change si l'appareil est de nouveau appairé dans l'application.
"""

import sys

# Constantes de homeassistant/components/tuya/const.py.
HA_CLIENT_ID = "HA_3y9q4ak7g4ephrvke"
HA_SCHEMA = "haauthorize"


def show_qr(content):
    try:
        import qrcode
    except ImportError:
        print("(module qrcode absent : copiez ce texte dans un générateur de QR code)")
        print(content)
        return
    qr = qrcode.QRCode(border=2)
    qr.add_data(content)
    qr.make(fit=True)
    qr.print_ascii(invert=True)


def lan_scan():
    """Adresse locale et version du protocole, par les annonces UDP des appareils (tinytuya)."""
    try:
        import tinytuya
    except ImportError:
        print("tinytuya absent : pas de recherche sur le réseau local.")
        return {}
    print("Recherche des appareils Tuya sur le réseau local (quelques secondes)…")
    try:
        found = tinytuya.deviceScan(False)
        return {d.get("gwId") or d.get("id"): (ip, d.get("version")) for ip, d in found.items()}
    except Exception as e:  # noqa: BLE001 — étape facultative : l'identifiant et la clé suffisent
        print(f"Recherche locale impossible ({e}) : relevez l'IP sur la box.")
        return {}


def main():
    try:
        from tuya_sharing import LoginControl, Manager
    except ImportError:
        sys.exit("Installez d'abord : pip install tuya-device-sharing-sdk qrcode tinytuya")

    user_code = input("User Code (Smart Life > Paramètres > Compte et sécurité) : ").strip()
    login = LoginControl()
    r = login.qr_code(HA_CLIENT_ID, HA_SCHEMA, user_code)
    if not r.get("success"):
        sys.exit(f"Refusé par Tuya : {r.get('code')} {r.get('msg')}")
    token = r["result"]["qrcode"]

    print("\nScannez ce QR code avec l'application Smart Life (icône de scan en haut à droite),")
    print("puis confirmez la connexion dans l'application.\n")
    show_qr(f"tuyaSmart--qrLogin?token={token}")
    input("\nAppuyez sur Entrée une fois la connexion confirmée dans l'application…")

    ok, info = login.login_result(token, HA_CLIENT_ID, user_code)
    if not ok:
        sys.exit(f"Connexion refusée : {info.get('code')} {info.get('msg')} (le QR code expire en quelques minutes)")

    tok = {k: info[k] for k in ("t", "uid", "expire_time", "access_token", "refresh_token")}
    mgr = Manager(HA_CLIENT_ID, user_code, info["terminal_id"], info["endpoint"], tok)
    mgr.update_device_cache()
    lan = lan_scan()

    print(f"\n{len(mgr.device_map)} appareil(s) :\n")
    for d in mgr.device_map.values():
        ip, ver = lan.get(d.id, (None, None))
        print(f"  {d.name}  ({d.product_name}, catégorie {d.category})")
        print(f"    identifiant Tuya : {d.id}")
        print(f"    clé locale       : {d.local_key}")
        print(f"    IP locale        : {ip or 'non trouvée (voir la box)'}")
        print(f"    version          : {ver or 'inconnue (essayer 3.3 puis 3.4)'}")
        print()
    print("Recopiez ces valeurs dans Paramètres > Appareils (Firmware du Tongou : Tuya d'origine),")
    print("puis vérifiez les DP avec : python tools/tuya_ref.py probe --ip … --id … --key … --version …")

    # Ferme la session ouverte par cet outil (celle de Home Assistant n'est pas concernée).
    try:
        mgr.unload()
    except Exception:  # noqa: BLE001
        pass


if __name__ == "__main__":
    main()
