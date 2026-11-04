#!/usr/bin/env python3
"""Implémentation de référence du protocole local Tuya 3.3 / 3.4 (trames 55AA).

Suit le code de tinytuya (jasonacox/tinytuya, dossier core) : format des trames, AES-128-ECB + PKCS#7,
en-tête de version, CRC32 (3.3) ou HMAC-SHA256 (3.4), négociation de la clé de session (3.4).

Usages :
  python tools/tuya_ref.py probe --ip 192.168.1.x --id <device_id> --key <local_key> --version 3.3
      interroge l'appareil et affiche ses DP (pour choisir dp_switch / dp_power) ;
  python tools/tuya_ref.py vectors > test/host/tuya_vectors.h
      génère les vecteurs de test du code C (test/host/test_tuya.c).
"""

import argparse
import binascii
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PREFIX = 0x000055AA
SUFFIX = 0x0000AA55
PORT = 6668

CONTROL = 0x07
STATUS = 0x08
DP_QUERY = 0x0A
CONTROL_NEW = 0x0D
DP_QUERY_NEW = 0x10
SESS_KEY_NEG_START = 0x03
SESS_KEY_NEG_RESP = 0x04
SESS_KEY_NEG_FINISH = 0x05

NO_PROTOCOL_HEADER_CMDS = {DP_QUERY, DP_QUERY_NEW, 0x12, 0x09, 0x03, 0x04, 0x05, 0x40}

# Journal des appels cryptographiques (pour les vecteurs de test du C).
CALLS = []


def aes_ecb(key, data, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.ECB())
    op = c.decryptor() if decrypt else c.encryptor()
    out = op.update(data) + op.finalize()
    CALLS.append(("aes_dec" if decrypt else "aes_enc", key, data, out))
    return out


def hmac_sha256(key, data):
    out = hmac.new(key, data, hashlib.sha256).digest()
    CALLS.append(("hmac", key, data, out))
    return out


def pad(b):
    n = 16 - len(b) % 16
    return b + bytes([n]) * n


def unpad(b):
    n = b[-1]
    if not 1 <= n <= 16:
        raise ValueError("remplissage PKCS#7 invalide")
    return b[:-n]


def version_header(ver):
    return ver.encode() + b"\x00" * 12


def encode_payload(ver, cmd, payload, key):
    """3.3 : chiffrer puis préfixer l'en-tête ; 3.4 : préfixer puis chiffrer (tinytuya _encode_message)."""
    if ver == "3.4":
        if cmd not in NO_PROTOCOL_HEADER_CMDS:
            payload = version_header(ver) + payload
        return aes_ecb(key, pad(payload))
    enc = aes_ecb(key, pad(payload))
    if cmd not in NO_PROTOCOL_HEADER_CMDS:
        enc = version_header(ver) + enc
    return enc


def build_frame(seq, cmd, payload, hmac_key=None):
    end_len = (32 if hmac_key else 4) + 4
    header = struct.pack(">4I", PREFIX, seq, cmd, len(payload) + end_len)
    data = header + payload
    if hmac_key:
        return data + hmac_sha256(hmac_key, data) + struct.pack(">I", SUFFIX)
    return data + struct.pack(">2I", binascii.crc32(data) & 0xFFFFFFFF, SUFFIX)


def build_device_frame(seq, cmd, retcode, payload, hmac_key=None):
    """Trame appareil → client (retcode avant la charge utile), pour les tests."""
    body = struct.pack(">I", retcode) + payload
    return build_frame(seq, cmd, body, hmac_key)


def parse_frame(buf, hmac_key=None):
    prefix, seq, cmd, length = struct.unpack(">4I", buf[:16])
    if prefix != PREFIX:
        raise ValueError("préfixe")
    end_len = (32 if hmac_key else 4) + 4
    total = 16 + length
    body = buf[16:total - end_len]
    retcode = struct.unpack(">I", body[:4])[0]
    return seq, cmd, retcode, body[4:], buf[total:]


def decode_payload(ver, payload, key):
    if ver == "3.4":
        payload = unpad(aes_ecb(key, payload, decrypt=True))
        if payload.startswith(b"3.4"):
            payload = payload[15:]
        return payload
    if payload.startswith(ver.encode()):
        payload = payload[15:]
    return unpad(aes_ecb(key, payload, decrypt=True))


def query_json(ver, dev_id, t):
    if ver == "3.4":
        return b"{}"
    d = {"gwId": dev_id, "devId": dev_id, "uid": dev_id, "t": str(t)}
    return json.dumps(d).replace(" ", "").encode()


def control_json(ver, dev_id, t, dps):
    if ver == "3.4":
        d = {"protocol": 5, "t": t, "data": {"dps": dps}}
    else:
        d = {"devId": dev_id, "uid": dev_id, "t": str(t), "dps": dps}
    return json.dumps(d).replace(" ", "").encode()


def session_key(real_key, local_nonce, remote_nonce):
    x = bytes(a ^ b for a, b in zip(local_nonce, remote_nonce))
    return aes_ecb(real_key, x)


# ---------- Sonde réseau ----------

def recv_frame(sock):
    buf = b""
    while len(buf) < 16:
        chunk = sock.recv(1024)
        if not chunk:
            raise ConnectionError("connexion fermée")
        buf += chunk
    length = struct.unpack(">I", buf[12:16])[0]
    while len(buf) < 16 + length:
        buf += sock.recv(1024)
    return buf


def probe(args):
    key = args.key.encode("latin1")
    if len(key) != 16:
        sys.exit("la clé locale doit faire 16 caractères")
    ver = args.version
    s = socket.create_connection((args.ip, PORT), timeout=5)
    seq = 1
    hkey = None
    if ver == "3.4":
        local_nonce = os.urandom(16)
        s.sendall(build_frame(seq, SESS_KEY_NEG_START, encode_payload(ver, SESS_KEY_NEG_START, local_nonce, key), key))
        seq += 1
        _, cmd, _, payload, _ = parse_frame(recv_frame(s), key)
        resp = unpad(aes_ecb(key, payload, decrypt=True))
        remote_nonce = resp[:16]
        if resp[16:48] != hmac_sha256(key, local_nonce):
            print("attention : HMAC de la réponse de négociation incorrect (clé ?)")
        fin = hmac_sha256(key, remote_nonce)
        s.sendall(build_frame(seq, SESS_KEY_NEG_FINISH, encode_payload(ver, SESS_KEY_NEG_FINISH, fin, key), key))
        seq += 1
        key = session_key(key, local_nonce, remote_nonce)
        hkey = key
    cmd = DP_QUERY_NEW if ver == "3.4" else DP_QUERY
    s.sendall(build_frame(seq, cmd, encode_payload(ver, cmd, query_json(ver, args.id, int(time.time())), key), hkey))
    frame = recv_frame(s)
    _, rcmd, ret, payload, _ = parse_frame(frame, hkey)
    s.close()
    print(f"réponse : commande 0x{rcmd:02x}, code {ret}, {len(payload)} octets")
    data = json.loads(decode_payload(ver, payload, key))
    if "dps" not in data and "data" in data:
        data["dps"] = data["data"].get("dps", {})
    print(json.dumps(data, indent=2, ensure_ascii=False))


# ---------- Vecteurs de test ----------

def c_bytes(b):
    return "{" + ", ".join(f"0x{x:02x}" for x in b) + "}"


def vectors():
    key = b"0123456789abcdef"
    dev = "bf1234567890abcdefgh"
    t = 1793750000
    out = ["/* Généré par tools/tuya_ref.py vectors : ne pas modifier à la main. */", "#pragma once", "#include <stdint.h>", ""]
    CALLS.clear()

    def emit(name, b):
        out.append(f"static const uint8_t {name}[{len(b)}] = {c_bytes(b)};")

    # 3.3 : requête, commande, réponse appareil.
    q33 = build_frame(1, DP_QUERY, encode_payload("3.3", DP_QUERY, query_json("3.3", dev, t), key))
    c33 = build_frame(2, CONTROL, encode_payload("3.3", CONTROL, control_json("3.3", dev, t, {"1": True}), key))
    r33_json = b'{"devId":"bf1234567890abcdefgh","dps":{"1":true,"19":1234,"20":2301}}'
    r33 = build_device_frame(1, DP_QUERY, 0, encode_payload("3.3", STATUS, r33_json, key))

    # 3.4 : clé de session, requête, réponse appareil.
    ln = bytes(range(16))
    rn = bytes(range(16, 32))
    skey = session_key(key, ln, rn)
    q34 = build_frame(3, DP_QUERY_NEW, encode_payload("3.4", DP_QUERY_NEW, query_json("3.4", dev, t), skey), skey)
    c34 = build_frame(4, CONTROL_NEW, encode_payload("3.4", CONTROL_NEW, control_json("3.4", dev, t, {"1": False}), skey), skey)
    r34_json = b'{"protocol":4,"t":1793750001,"data":{"dps":{"1":false,"19":0}}}'
    r34 = build_device_frame(5, DP_QUERY_NEW, 0, encode_payload("3.4", STATUS, r34_json, skey), skey)
    neg_start = build_frame(1, SESS_KEY_NEG_START, encode_payload("3.4", SESS_KEY_NEG_START, ln, key), key)

    # Décodage aller-retour des réponses : vérifie la référence et enregistre les déchiffrements.
    assert decode_payload("3.3", parse_frame(r33)[3], key) == r33_json
    assert decode_payload("3.4", parse_frame(r34, skey)[3], skey) == r34_json

    out.append(f'#define TV_KEY "{key.decode()}"')
    out.append(f'#define TV_DEV "{dev}"')
    out.append(f"#define TV_T {t}")
    emit("tv_q33", q33)
    emit("tv_c33", c33)
    emit("tv_r33", r33)
    out.append(f"static const char tv_r33_json[] = {json.dumps(r33_json.decode())};")
    emit("tv_lnonce", ln)
    emit("tv_rnonce", rn)
    emit("tv_skey", skey)
    emit("tv_neg_start", neg_start)
    emit("tv_q34", q34)
    emit("tv_c34", c34)
    emit("tv_r34", r34)
    out.append(f"static const char tv_r34_json[] = {json.dumps(r34_json.decode())};")

    # Table des appels cryptographiques : le test C remplace AES/HMAC par une recherche dans cette table.
    out.append("")
    out.append("typedef struct { uint8_t op; const uint8_t *key; uint16_t in_len; const uint8_t *in; uint16_t out_len; const uint8_t *out; } tv_call_t;")
    for i, (op, k, din, dout) in enumerate(CALLS):
        emit(f"tv_k{i}", k)
        emit(f"tv_i{i}", din)
        emit(f"tv_o{i}", dout)
    ops = {"aes_enc": 0, "aes_dec": 1, "hmac": 2}
    out.append(f"static const tv_call_t tv_calls[{len(CALLS)}] = {{")
    for i, (op, k, din, dout) in enumerate(CALLS):
        out.append(f"    {{ {ops[op]}, tv_k{i}, {len(din)}, tv_i{i}, {len(dout)}, tv_o{i} }},")
    out.append("};")
    print("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe")
    p.add_argument("--ip", required=True)
    p.add_argument("--id", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--version", choices=["3.3", "3.4"], default="3.3")
    sub.add_parser("vectors")
    a = ap.parse_args()
    probe(a) if a.cmd == "probe" else vectors()


if __name__ == "__main__":
    main()
