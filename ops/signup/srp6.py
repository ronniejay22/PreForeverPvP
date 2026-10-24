"""SRP6 registration data exactly as TrinityCore computes it
(src/common/Cryptography/Authentication/SRP6.cpp):

    v = g ^ SHA1(salt || SHA1(USERNAME ":" PASSWORD)) mod N

with g = 7 and N stored little-endian, and every BigNumber <-> bytes
conversion little-endian (BigNumber.h). Username and password are uppercased
first, as AccountMgr::CreateAccount does.
"""
import hashlib
import secrets

G = 7
N = int("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7", 16)
SALT_LENGTH = 32
VERIFIER_LENGTH = 32


def calculate_verifier(username: str, password: str, salt: bytes) -> bytes:
    inner = hashlib.sha1(f"{username}:{password}".upper().encode("ascii")).digest()
    x = int.from_bytes(hashlib.sha1(salt + inner).digest(), "little")
    return pow(G, x, N).to_bytes(VERIFIER_LENGTH, "little")


def make_registration_data(username: str, password: str) -> tuple[bytes, bytes]:
    salt = secrets.token_bytes(SALT_LENGTH)
    return salt, calculate_verifier(username, password, salt)
