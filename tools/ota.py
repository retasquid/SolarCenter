#!/usr/bin/env python3
"""Mise à jour par le réseau, sans câble et sans effacer l'historique.

    python tools/ota.py solaire.local           # interface web puis firmware
    python tools/ota.py solaire.local --www     # interface seule (après tools/build_web.py)
    python tools/ota.py solaire.local --fw      # firmware seul (après idf.py build)

Le mot de passe administrateur est demandé, ou lu dans SOLAR_ADMIN_PASS.
Les fichiers web sont envoyés un par un (POST /api/www), le firmware en dernier
(POST /api/ota) : la carte met les appareils au repos puis redémarre.
"""

import argparse
import getpass
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "build" / "solar_center.bin"
WWW = ROOT / "littlefs_image" / "www"


def post(url: str, data: bytes, password: str, timeout: float) -> str:
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/octet-stream",
        "X-Admin-Password": password,
    })
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        raise SystemExit(f"refusé ({e.code}) : {body}")


def wait_back(base: str, timeout: float = 90.0) -> str:
    """Attend le redémarrage et renvoie la version annoncée par /api/system."""
    time.sleep(5)
    end = time.time() + timeout
    while time.time() < end:
        try:
            with urllib.request.urlopen(base + "/api/system", timeout=3) as r:
                txt = r.read().decode("utf-8", "replace")
                return json.loads(txt).get("version", "?")
        except (OSError, ValueError):
            time.sleep(2)
    raise SystemExit("la carte ne répond plus après le redémarrage (retour automatique à l'ancien firmware si le nouveau plante)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="adresse IP ou nom de la carte")
    ap.add_argument("--www", action="store_true", help="envoyer l'interface web (littlefs_image/www/*.gz)")
    ap.add_argument("--fw", action="store_true", help="envoyer le firmware (build/solar_center.bin)")
    args = ap.parse_args()
    if not args.www and not args.fw:
        args.www = args.fw = True

    base = "http://" + args.host
    password = os.environ.get("SOLAR_ADMIN_PASS") or getpass.getpass("Mot de passe administrateur : ")

    if args.www:
        files = sorted(WWW.glob("*.gz"))
        if not files:
            print(f"erreur : aucun fichier dans {WWW} (lancer python tools/build_web.py)", file=sys.stderr)
            return 1
        for f in files:
            post(f"{base}/api/www?file={f.name}", f.read_bytes(), password, 30)
            print(f"  interface : {f.name} ({f.stat().st_size} o)")

    if args.fw:
        if not FW.is_file():
            print(f"erreur : {FW} absent (lancer idf.py build)", file=sys.stderr)
            return 1
        data = FW.read_bytes()
        print(f"  firmware : {len(data)} o, écriture en cours…")
        t0 = time.time()
        post(f"{base}/api/ota", data, password, 180)
        print(f"  écrit en {time.time() - t0:.0f} s, redémarrage…")
        print(f"  carte revenue, version {wait_back(base)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
