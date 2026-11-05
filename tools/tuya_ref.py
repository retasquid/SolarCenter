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


def aes_gcm_enc(key, iv, aad, data):
    """3.5 : AES-GCM, IV de 12 octets, étiquette de 16 octets (tinytuya crypto_helper.encrypt, iv=…)."""
    e = Cipher(algorithms.AES(key), modes.GCM(iv)).encryptor()
    if aad:
        e.authenticate_additional_data(aad)
    ct = e.update(data) + e.finalize()
    CALLS.append(("gcm_enc", key, iv + aad + data, ct + e.tag))
    return ct, e.tag


def aes_gcm_dec(key, iv, aad, ct, tag):
    d = Cipher(algorithms.AES(key), modes.GCM(iv, tag)).decryptor()
    if aad:
        d.authenticate_additional_data(aad)
    pt = d.update(ct) + d.finalize()
    CALLS.append(("gcm_dec", key, iv + aad + ct + tag, pt))
    return pt


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
    """3.3 : chiffrer puis préfixer l'en-tête ; 3.4 : préfixer puis chiffrer ; 3.5 : préfixer seulement,
    le chiffrement GCM se fait à la construction de la trame (tinytuya _encode_message)."""
    if ver == "3.5":
        return (version_header(ver) + payload) if cmd not in NO_PROTOCOL_HEADER_CMDS else payload
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


PREFIX_6699 = 0x00006699
SUFFIX_6699 = 0x00009966


def build_frame_6699(seq, cmd, raw, key, iv):
    """3.5 : en-tête 18 octets, IV, GCM(raw), étiquette, suffixe ; AAD = en-tête sans le préfixe
    (tinytuya message_helper.pack_message)."""
    header = struct.pack(">IHIII", PREFIX_6699, 0, seq, cmd, len(raw) + 12 + 16)
    ct, tag = aes_gcm_enc(key, iv, header[4:], raw)
    return header + iv + ct + tag + struct.pack(">I", SUFFIX_6699)


def parse_frame_6699(buf, key):
    """Trame appareil 3.5 : retourne (seq, cmd, retcode, charge utile, reste)."""
    prefix, _, seq, cmd, length = struct.unpack(">IHIII", buf[:18])
    if prefix != PREFIX_6699:
        raise ValueError("préfixe 6699")
    total = 18 + length + 4
    body = buf[18:total - 4]
    iv, ct, tag = body[:12], body[12:-16], body[-16:]
    pt = aes_gcm_dec(key, iv, buf[4:18], ct, tag)
    return seq, cmd, struct.unpack(">I", pt[:4])[0], pt[4:], buf[total:]


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
    if ver == "3.5":
        return payload[15:] if payload.startswith(b"3.5") else payload
    if ver == "3.4":
        payload = unpad(aes_ecb(key, payload, decrypt=True))
        if payload.startswith(b"3.4"):
            payload = payload[15:]
        return payload
    if payload.startswith(ver.encode()):
        payload = payload[15:]
    return unpad(aes_ecb(key, payload, decrypt=True))


def query_json(ver, dev_id, t):
    if ver in ("3.4", "3.5"):
        return b"{}"
    d = {"gwId": dev_id, "devId": dev_id, "uid": dev_id, "t": str(t)}
    return json.dumps(d).replace(" ", "").encode()


def control_json(ver, dev_id, t, dps):
    if ver in ("3.4", "3.5"):
        d = {"protocol": 5, "t": t, "data": {"dps": dps}}
    else:
        d = {"devId": dev_id, "uid": dev_id, "t": str(t), "dps": dps}
    return json.dumps(d).replace(" ", "").encode()


def session_key(real_key, local_nonce, remote_nonce, ver="3.4"):
    x = bytes(a ^ b for a, b in zip(local_nonce, remote_nonce))
    if ver == "3.5":
        return aes_gcm_enc(real_key, local_nonce[:12], b"", x)[0]     # octets [12:28] de iv+ct+tag
    return aes_ecb(real_key, x)


# ---------- Sonde réseau ----------

def recv_exact(sock, n, buf=b""):
    while len(buf) < n:
        chunk = sock.recv(2048)
        if not chunk:
            raise ConnectionError("connexion fermée par l'appareil (version ou clé ?)")
        buf += chunk
    return buf


def recv_frame(sock):
    buf = recv_exact(sock, 4)
    if struct.unpack(">I", buf[:4])[0] == PREFIX_6699:
        buf = recv_exact(sock, 18, buf)
        return recv_exact(sock, 18 + struct.unpack(">I", buf[14:18])[0] + 4, buf)
    buf = recv_exact(sock, 16, buf)
    return recv_exact(sock, 16 + struct.unpack(">I", buf[12:16])[0], buf)


class Link:
    """Envoi et réception de trames selon la version (55AA ou 6699)."""

    def __init__(self, sock, ver):
        self.s, self.ver, self.seq = sock, ver, 1

    def send(self, cmd, payload, key, hmac_key=None):
        raw = encode_payload(self.ver, cmd, payload, key)
        if self.ver == "3.5":
            frame = build_frame_6699(self.seq, cmd, raw, key, os.urandom(12))
        else:
            frame = build_frame(self.seq, cmd, raw, hmac_key)
        self.seq += 1
        self.s.sendall(frame)

    def recv(self, key, hmac_key=None):
        buf = recv_frame(self.s)
        if self.ver == "3.5":
            _, cmd, ret, payload, _ = parse_frame_6699(buf, key)
        else:
            _, cmd, ret, payload, _ = parse_frame(buf, hmac_key)
        return cmd, ret, payload


def probe(args):
    key = args.key.encode("latin1")
    if len(key) != 16:
        sys.exit("la clé locale doit faire 16 caractères")
    ver = args.version
    s = socket.create_connection((args.ip, PORT), timeout=5)
    link = Link(s, ver)
    hkey = None
    if ver in ("3.4", "3.5"):
        local_nonce = os.urandom(16)
        link.send(SESS_KEY_NEG_START, local_nonce, key, key)
        cmd, _, payload = link.recv(key, key)
        resp = unpad(aes_ecb(key, payload, decrypt=True)) if ver == "3.4" else payload
        remote_nonce = resp[:16]
        if resp[16:48] != hmac_sha256(key, local_nonce):
            print("attention : HMAC de la réponse de négociation incorrect (clé ?)")
        link.send(SESS_KEY_NEG_FINISH, hmac_sha256(key, remote_nonce), key, key)
        key = session_key(key, local_nonce, remote_nonce, ver)
        hkey = key
        print("clé de session négociée")
    cmd = DP_QUERY if ver == "3.3" else DP_QUERY_NEW
    link.send(cmd, query_json(ver, args.id, int(time.time())), key, hkey)
    rcmd, ret, payload = link.recv(key, hkey)
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

    # 3.5 : trames 6699 (AES-GCM), IV fixes pour des vecteurs reproductibles.
    skey35 = session_key(key, ln, rn, "3.5")
    iv_a, iv_b, iv_c, iv_d = (bytes(range(o, o + 12)) for o in (32, 48, 64, 80))
    neg35 = build_frame_6699(1, SESS_KEY_NEG_START, encode_payload("3.5", SESS_KEY_NEG_START, ln, key), key, iv_a)
    resp35 = build_frame_6699(7, SESS_KEY_NEG_RESP, struct.pack(">I", 0) + rn + hmac_sha256(key, ln), key, iv_b)
    q35 = build_frame_6699(2, DP_QUERY_NEW, encode_payload("3.5", DP_QUERY_NEW, query_json("3.5", dev, t), skey35),
                           skey35, iv_c)
    r35_json = b'{"dps":{"1":true,"19":24987,"20":2391}}'
    r35 = build_frame_6699(9, DP_QUERY_NEW, struct.pack(">I", 0) + encode_payload("3.5", STATUS, r35_json, skey35),
                           skey35, iv_d)
    assert parse_frame_6699(resp35, key)[3] == rn + hmac_sha256(key, ln)
    assert decode_payload("3.5", parse_frame_6699(r35, skey35)[3], skey35) == r35_json

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
    emit("tv_skey35", skey35)
    emit("tv_iv_a", iv_a)
    emit("tv_iv_c", iv_c)
    emit("tv_neg35", neg35)
    emit("tv_resp35", resp35)
    emit("tv_q35", q35)
    emit("tv_r35", r35)
    out.append(f"static const char tv_r35_json[] = {json.dumps(r35_json.decode())};")
    emit("tv_hmac_ln", hmac_sha256(key, ln))

    # Table des appels cryptographiques : le test C remplace AES/HMAC par une recherche dans cette table.
    out.append("")
    out.append("typedef struct { uint8_t op; const uint8_t *key; uint16_t in_len; const uint8_t *in; uint16_t out_len; const uint8_t *out; } tv_call_t;")
    for i, (op, k, din, dout) in enumerate(CALLS):
        emit(f"tv_k{i}", k)
        emit(f"tv_i{i}", din)
        emit(f"tv_o{i}", dout)
    ops = {"aes_enc": 0, "aes_dec": 1, "hmac": 2, "gcm_enc": 3, "gcm_dec": 4}
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
    p.add_argument("--version", choices=["3.3", "3.4", "3.5"], default="3.3")
    sub.add_parser("vectors")
    a = ap.parse_args()
    probe(a) if a.cmd == "probe" else vectors()


if __name__ == "__main__":
    main()
