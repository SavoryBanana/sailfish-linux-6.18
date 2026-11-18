#!/usr/bin/env python3
"""
Build and v2-sign SailfishWifiOverlay.apk, a static RRO targeting
com.android.wifi.resources to enable config_wifiMainlineSupplicantEnabled
(so Android 17's Wi-Fi APEX uses its bundled wpa_supplicant_mainline).
"""
import datetime
import hashlib
import os
import struct
import sys
import zlib

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID


ANDROID_NS = "http://schemas.android.com/apk/res/android"
OVERLAY_PKG = "com.google.android.sailfish.wifi.overlay"
TARGET_PKG = "com.android.wifi.resources"
TARGET_NAME = "WifiCustomization"

# Verified against Android 17 framework-res.apk resources.arsc
ATTR_IDS = {
    "label": 0x01010001,
    "hasCode": 0x0101000C,
    "priority": 0x0101001C,
    "targetPackage": 0x01010021,
    "minSdkVersion": 0x0101020C,
    "versionCode": 0x0101021B,
    "versionName": 0x0101021C,
    "targetSdkVersion": 0x01010270,
    "targetName": 0x0101044D,
    "extractNativeLibs": 0x010104EA,
    "isStatic": 0x0101055A,
    "compileSdkVersion": 0x01010572,
    "compileSdkVersionCodename": 0x01010573,
}

TYPE_REFERENCE = 0x01
TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_BOOLEAN = 0x12


def _u8_len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    return bytes([0x80 | ((n >> 8) & 0x7F), n & 0xFF])


def _u16_len(n: int) -> bytes:
    if n < 0x8000:
        return struct.pack("<H", n)
    return struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)


def encode_string_pool(strings: list[str], utf8: bool = True) -> bytes:
    offsets = []
    blob = bytearray()
    for s in strings:
        offsets.append(len(blob))
        if utf8:
            raw = s.encode("utf-8")
            blob += _u8_len(len(s)) + _u8_len(len(raw)) + raw + b"\x00"
        else:
            raw = s.encode("utf-16le")
            blob += _u16_len(len(s)) + raw + b"\x00\x00"
    while len(blob) % 4 != 0:
        blob.append(0)

    hdr_size = 28
    strings_start = hdr_size + len(offsets) * 4
    total_size = strings_start + len(blob)
    flags = 0x100 if utf8 else 0
    hdr = struct.pack(
        "<HHIIIIII",
        0x0001,
        hdr_size,
        total_size,
        len(strings),
        0,
        flags,
        strings_start,
        0,
    )
    off_table = struct.pack(f"<{len(offsets)}I", *offsets)
    return hdr + off_table + bytes(blob)


def build_resources_arsc() -> bytes:
    gpool = encode_string_pool(["Sailfish Wi-Fi Overlay"], utf8=True)
    tpool = encode_string_pool(["bool", "string"], utf8=False)
    kpool = encode_string_pool(
        ["config_wifiMainlineSupplicantEnabled", "sailfish_wifi_overlay_title"],
        utf8=True,
    )

    def _type_spec(tid: int) -> bytes:
        return struct.pack("<HHIBBHII", 0x0202, 16, 20, tid, 0, 0, 1, 0)

    def _type_chunk(tid: int, key_idx: int, dtype: int, val: int) -> bytes:
        cfg = struct.pack("<I", 64) + b"\x00" * 60
        entries_start = 84 + 4
        entry = struct.pack("<HBBI", key_idx, 0x08, dtype, val)
        total = entries_start + len(entry)
        hdr = struct.pack(
            "<HHIBBHII",
            0x0201,
            84,
            total,
            tid,
            0x02,  # FLAG_OFFSET16
            0,
            1,
            entries_start,
        )
        offsets = struct.pack("<HH", 0, 0)
        return hdr + cfg + offsets + entry

    pkg_body = (
        tpool
        + kpool
        + _type_spec(1)
        + _type_chunk(1, 0, TYPE_INT_BOOLEAN, 0xFFFFFFFF)
        + _type_spec(2)
        + _type_chunk(2, 1, TYPE_STRING, 0)
    )
    name_utf16 = OVERLAY_PKG.encode("utf-16le").ljust(256, b"\x00")
    pkg_hdr_size = 288
    type_strings_off = pkg_hdr_size
    key_strings_off = pkg_hdr_size + len(tpool)
    pkg_hdr = (
        struct.pack("<HHI", 0x0200, pkg_hdr_size, pkg_hdr_size + len(pkg_body))
        + struct.pack("<I", 0x7F)
        + name_utf16
        + struct.pack("<IIIII", type_strings_off, 0, key_strings_off, 0, 0)
    )
    table_body = gpool + pkg_hdr + pkg_body
    table_hdr = struct.pack("<HHII", 0x0002, 12, 12 + len(table_body), 1)
    return table_hdr + table_body


def build_android_manifest_axml() -> bytes:
    # AXML rule: attribute names with android resIds must come first in string
    # pool, ordered monotonically matching RES_XML_RESOURCE_MAP_TYPE.
    sorted_attrs = sorted(ATTR_IDS.items(), key=lambda kv: kv[1])
    pool_strs = [k for k, _ in sorted_attrs]
    res_ids = [v for _, v in sorted_attrs]

    extra_strs = [
        "android",
        ANDROID_NS,
        "manifest",
        "uses-sdk",
        "overlay",
        "application",
        "package",
        "platformBuildVersionCode",
        "platformBuildVersionName",
        "1.0",
        "17",
        OVERLAY_PKG,
        TARGET_PKG,
        TARGET_NAME,
    ]
    pool_strs.extend(extra_strs)
    sidx = {s: i for i, s in enumerate(pool_strs)}

    spool = encode_string_pool(pool_strs, utf8=False)
    rmap_len = 8 + len(res_ids) * 4
    rmap = struct.pack("<HHI", 0x0180, 8, rmap_len) + struct.pack(
        f"<{len(res_ids)}I", *res_ids
    )

    chunks = bytearray(spool + rmap)
    chunks += struct.pack(
        "<HHIIIII",
        0x0100,  # START_NAMESPACE
        16,
        24,
        1,
        0xFFFFFFFF,
        sidx["android"],
        sidx[ANDROID_NS],
    )

    def start_tag(line: int, name: str, attrs: list[tuple[str | None, str, int, int]]) -> None:
        # Sort android-namespaced attributes by resId ascending, then non-ns attrs
        ordered = sorted(
            attrs,
            key=lambda a: (0, ATTR_IDS[a[1]]) if a[0] is not None else (1, sidx[a[1]]),
        )
        attr_bytes = bytearray()
        for ns, aname, dtype, raw_val in ordered:
            ns_i = sidx[ns] if ns is not None else -1
            raw_s_i = raw_val if dtype == TYPE_STRING else -1
            attr_bytes += struct.pack(
                "<iiiHBBI",
                ns_i,
                sidx[aname],
                raw_s_i,
                8,
                0,
                dtype,
                raw_val & 0xFFFFFFFF,
            )
        hdr = struct.pack(
            "<HHIIIiiHHHHHH",
            0x0102,
            16,
            36 + len(attr_bytes),
            line,
            0xFFFFFFFF,
            -1,
            sidx[name],
            0x0014,
            20,
            len(ordered),
            0,
            0,
            0,
        )
        chunks.extend(hdr + attr_bytes)

    def end_tag(line: int, name: str) -> None:
        chunks.extend(
            struct.pack("<HHIIIii", 0x0103, 16, 24, line, 0xFFFFFFFF, -1, sidx[name])
        )

    start_tag(
        2,
        "manifest",
        [
            (ANDROID_NS, "versionCode", TYPE_INT_DEC, 1),
            (ANDROID_NS, "versionName", TYPE_STRING, sidx["1.0"]),
            (ANDROID_NS, "compileSdkVersion", TYPE_INT_DEC, 37),
            (ANDROID_NS, "compileSdkVersionCodename", TYPE_STRING, sidx["17"]),
            (None, "package", TYPE_STRING, sidx[OVERLAY_PKG]),
            (None, "platformBuildVersionCode", TYPE_INT_DEC, 37),
            (None, "platformBuildVersionName", TYPE_INT_DEC, 17),
        ],
    )
    start_tag(
        5,
        "uses-sdk",
        [
            (ANDROID_NS, "minSdkVersion", TYPE_INT_DEC, 35),
            (ANDROID_NS, "targetSdkVersion", TYPE_INT_DEC, 37),
        ],
    )
    end_tag(5, "uses-sdk")
    start_tag(
        6,
        "overlay",
        [
            (ANDROID_NS, "priority", TYPE_INT_DEC, 100),
            (ANDROID_NS, "targetPackage", TYPE_STRING, sidx[TARGET_PKG]),
            (ANDROID_NS, "targetName", TYPE_STRING, sidx[TARGET_NAME]),
            (ANDROID_NS, "isStatic", TYPE_INT_BOOLEAN, 0xFFFFFFFF),
        ],
    )
    end_tag(6, "overlay")
    start_tag(
        10,
        "application",
        [
            (ANDROID_NS, "label", TYPE_REFERENCE, 0x7F020000),
            (ANDROID_NS, "hasCode", TYPE_INT_BOOLEAN, 0),
            (ANDROID_NS, "extractNativeLibs", TYPE_INT_BOOLEAN, 0),
        ],
    )
    end_tag(10, "application")
    end_tag(12, "manifest")

    chunks += struct.pack(
        "<HHIIIII",
        0x0101,  # END_NAMESPACE
        16,
        24,
        12,
        0xFFFFFFFF,
        sidx["android"],
        sidx[ANDROID_NS],
    )
    return struct.pack("<HHI", 0x0003, 8, 8 + len(chunks)) + bytes(chunks)


def build_aligned_zip(entries: list[tuple[str, bytes, bool]]) -> tuple[bytes, bytes, bytes]:
    """Build ZIP sections: (entries_blob, cd_blob, eocd_blob) with 4B aligned stored entries."""
    entries_blob = bytearray()
    cd_blob = bytearray()

    for name, data, compress in entries:
        name_b = name.encode("utf-8")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if compress:
            comp = zlib.compressobj(9, zlib.DEFLATED, -15)
            payload = comp.compress(data) + comp.flush()
            method = 8
        else:
            payload = data
            method = 0

        local_off = len(entries_blob)
        extra = b""
        if method == 0:
            data_start = local_off + 30 + len(name_b)
            pad = (-data_start) % 4
            extra = b"\x00" * pad

        lfh = (
            struct.pack(
                "<IHHHHHIIIHH",
                0x04034B50,
                20,
                0,
                method,
                0,
                0x3A21,
                crc,
                len(payload),
                len(data),
                len(name_b),
                len(extra),
            )
            + name_b
            + extra
        )
        entries_blob += lfh + payload

        cdh = (
            struct.pack(
                "<IHHHHHHIIIHHHHHII",
                0x02014B50,
                20,
                20,
                0,
                method,
                0,
                0x3A21,
                crc,
                len(payload),
                len(data),
                len(name_b),
                0,
                0,
                0,
                0,
                0,
                local_off,
            )
            + name_b
        )
        cd_blob += cdh

    cd_off = len(entries_blob)
    eocd = struct.pack(
        "<IHHHHIIH",
        0x06054B50,
        0,
        0,
        len(entries),
        len(entries),
        len(cd_blob),
        cd_off,
        0,
    )
    return bytes(entries_blob), bytes(cd_blob), eocd


def load_or_create_key_and_cert(key_path: str) -> tuple[rsa.RSAPrivateKey, bytes]:
    if os.path.exists(key_path):
        with open(key_path, "rb") as f:
            priv = serialization.load_pem_private_key(f.read(), password=None)
    else:
        priv = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        with open(key_path, "wb") as f:
            f.write(
                priv.private_bytes(
                    serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption(),
                )
            )
    cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Sailfish Wi-Fi Overlay")]))
        .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Sailfish Wi-Fi Overlay")]))
        .public_key(priv.public_key())
        .serial_number(1)
        .not_valid_before(datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc))
        .not_valid_after(datetime.datetime(2050, 1, 1, tzinfo=datetime.timezone.utc))
        .sign(priv, hashes.SHA256())
    )
    return priv, cert.public_bytes(serialization.Encoding.DER)


def _lp32(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def compute_v2_digest(section: bytes) -> list[bytes]:
    chunk_sz = 1024 * 1024
    digs = []
    for off in range(0, max(1, len(section)), chunk_sz):
        chunk = section[off : off + chunk_sz] if section else b""
        h = hashlib.sha256(b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest()
        digs.append(h)
    return digs


def sign_apk_v2(entries_blob: bytes, cd_blob: bytes, eocd: bytes, key_path: str) -> bytes:
    priv, cert_der = load_or_create_key_and_cert(key_path)

    # Top-level SHA-256 content digest across ZIP entries, central directory, and pre-block EOCD
    chunk_digs = (
        compute_v2_digest(entries_blob)
        + compute_v2_digest(cd_blob)
        + compute_v2_digest(eocd)
    )
    root_digest = hashlib.sha256(
        b"\x5a" + struct.pack("<I", len(chunk_digs)) + b"".join(chunk_digs)
    ).digest()

    alg_id = 0x0103  # RSA-PKCS1-v1_5 with SHA-256
    digest_entry = _lp32(struct.pack("<I", alg_id) + _lp32(root_digest))
    digests_seq = _lp32(digest_entry)
    certs_seq = _lp32(_lp32(cert_der))
    attrs_seq = _lp32(b"")
    signed_data = digests_seq + certs_seq + attrs_seq

    sig = priv.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    sigs_seq = _lp32(_lp32(struct.pack("<I", alg_id) + _lp32(sig)))
    pub_der = priv.public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    signer = _lp32(signed_data) + sigs_seq + _lp32(pub_der)
    v2_val = _lp32(_lp32(signer))

    pair = struct.pack("<QI", 4 + len(v2_val), 0x7109871A) + v2_val
    # Pad signing block so total block size is a multiple of 4096 bytes? Optional,
    # AOSP requires total signing block = 8 + size_of_block + 8 (size_of_block appears twice)
    verity_pad_need = (4096 - ((8 + len(pair) + 24) % 4096)) % 4096
    if verity_pad_need >= 12:
        pad_id = 0x42726577  # Android Verity Padding Block
        pair += struct.pack("<QI", verity_pad_need - 8, pad_id) + b"\x00" * (verity_pad_need - 12)

    size_of_block = len(pair) + 8 + 16
    sig_block = struct.pack("<Q", size_of_block) + pair + struct.pack("<Q", size_of_block) + b"APK Sig Block 42"
    new_cd_off = len(entries_blob) + len(sig_block)
    new_eocd = eocd[:16] + struct.pack("<I", new_cd_off) + eocd[20:]
    return entries_blob + sig_block + cd_blob + new_eocd


def verify_apk(apk_path: str) -> None:
    import zipfile

    with zipfile.ZipFile(apk_path, "r") as z:
        assert z.testzip() is None
        info = z.getinfo("resources.arsc")
        assert info.compress_type == 0
        raw = open(apk_path, "rb").read()
        data_off = info.header_offset + 30 + len(info.filename) + len(info.extra)
        assert data_off % 4 == 0, f"resources.arsc unaligned: {data_off}"

        eocd_pos = raw.rfind(b"PK\x05\x06")
        cd_off = struct.unpack_from("<I", raw, eocd_pos + 16)[0]
        assert raw[cd_off - 16 : cd_off] == b"APK Sig Block 42"
        blk_sz = struct.unpack_from("<Q", raw, cd_off - 24)[0]
        blk_start = cd_off - 8 - blk_sz
        assert struct.unpack_from("<Q", raw, blk_start)[0] == blk_sz


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.apk>", file=sys.stderr)
        return 1
    out_apk = sys.argv[1]
    key_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "overlay_rsa2048.pem")

    arsc = build_resources_arsc()
    axml = build_android_manifest_axml()
    entries_blob, cd_blob, eocd = build_aligned_zip(
        [
            ("resources.arsc", arsc, False),
            ("AndroidManifest.xml", axml, True),
        ]
    )
    apk_bytes = sign_apk_v2(entries_blob, cd_blob, eocd, key_path)
    os.makedirs(os.path.dirname(os.path.abspath(out_apk)), exist_ok=True)
    with open(out_apk, "wb") as f:
        f.write(apk_bytes)
    verify_apk(out_apk)
    print(f"[+] Built & verified v2-signed {out_apk} ({len(apk_bytes)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
