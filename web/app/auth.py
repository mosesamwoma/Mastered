import base64
import binascii
import hashlib
import hmac
import json
import re
import secrets
import time
from http.cookies import CookieError, SimpleCookie

COOKIE_NAME = "mastered_session"
SCRYPT_N = 2 ** 14
SCRYPT_R = 8
SCRYPT_P = 1
SCRYPT_LENGTH = 32
SALT_BYTES = 16
MIN_PASSWORD_LENGTH = 10
MAX_PASSWORD_BYTES = 256
USERNAME = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
TOKEN_VERSION = "v1"


def encode(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def decode(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def derive(password, salt, n, r, p):
    return hashlib.scrypt(
        password.encode("utf-8"), salt=salt, n=n, r=r, p=p, dklen=SCRYPT_LENGTH, maxmem=256 * r * (n + p + 2)
    )


def hash_password(password):
    salt = secrets.token_bytes(SALT_BYTES)
    digest = derive(password, salt, SCRYPT_N, SCRYPT_R, SCRYPT_P)
    return "$".join(["scrypt", str(SCRYPT_N), str(SCRYPT_R), str(SCRYPT_P), encode(salt), encode(digest)])


def parse_hash(stored):
    parts = stored.split("$")
    if len(parts) != 6 or parts[0] != "scrypt":
        raise ValueError("not a scrypt hash")
    n, r, p = (int(value) for value in parts[1:4])
    if n < 2 or n & (n - 1) or r < 1 or p < 1 or n > 2 ** 20:
        raise ValueError("invalid scrypt parameters")
    salt = decode(parts[4])
    digest = decode(parts[5])
    if len(salt) < 8 or len(digest) != SCRYPT_LENGTH:
        raise ValueError("invalid scrypt data")
    return n, r, p, salt, digest


def verify_password(password, stored):
    try:
        n, r, p, salt, digest = parse_hash(stored)
    except (ValueError, binascii.Error):
        return False
    if len(password.encode("utf-8")) > MAX_PASSWORD_BYTES:
        return False
    return hmac.compare_digest(derive(password, salt, n, r, p), digest)


def parse_users(text):
    users = {}
    for entry in re.split(r"[,\s]+", text.strip()):
        if not entry:
            continue
        name, separator, stored = entry.partition(":")
        if not separator or not USERNAME.match(name):
            raise ValueError(f"invalid user entry {name[:20]!r}: use name:hash with letters, digits, . _ - in the name")
        try:
            parse_hash(stored)
        except (ValueError, binascii.Error):
            raise ValueError(f"the password hash for user {name!r} is not valid; create one with python -m app.passwd") from None
        if name in users:
            raise ValueError(f"user {name!r} is listed twice")
        users[name] = stored
    return users


def generation(stored):
    return hashlib.sha256(stored.encode("utf-8")).hexdigest()[:12]


class SessionSigner:
    def __init__(self, secret, lifetime_seconds):
        self.secret = secret
        self.lifetime = lifetime_seconds

    def sign(self, body):
        return encode(hmac.new(self.secret, f"{TOKEN_VERSION}.{body}".encode("ascii"), hashlib.sha256).digest())

    def issue(self, user, stored_hash, now=None):
        now = time.time() if now is None else now
        payload = json.dumps(
            {"u": user, "g": generation(stored_hash), "e": int(now + self.lifetime)}, separators=(",", ":")
        ).encode("utf-8")
        body = encode(payload)
        return f"{TOKEN_VERSION}.{body}.{self.sign(body)}"

    def read(self, token, users, now=None):
        now = time.time() if now is None else now
        parts = token.split(".") if token else []
        if len(parts) != 3 or parts[0] != TOKEN_VERSION:
            return None
        if not hmac.compare_digest(self.sign(parts[1]), parts[2]):
            return None
        try:
            payload = json.loads(decode(parts[1]))
            user = payload["u"]
            expires = float(payload["e"])
            marker = payload["g"]
        except (ValueError, KeyError, TypeError, binascii.Error):
            return None
        stored = users.get(user) if isinstance(user, str) else None
        if stored is None or expires <= now or marker != generation(stored):
            return None
        return user


def session_token(headers):
    for name, value in headers:
        if name == b"cookie":
            jar = SimpleCookie()
            try:
                jar.load(value.decode("latin-1"))
            except CookieError:
                return None
            morsel = jar.get(COOKIE_NAME)
            return morsel.value if morsel else None
    return None
