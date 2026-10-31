import argparse
import getpass
import sys

from .auth import MIN_PASSWORD_LENGTH, USERNAME, hash_password


def read_password(from_stdin):
    if from_stdin:
        return sys.stdin.readline().rstrip("\r\n")
    first = getpass.getpass("Password: ")
    second = getpass.getpass("Repeat password: ")
    if first != second:
        raise SystemExit("The passwords do not match.")
    return first


def main():
    parser = argparse.ArgumentParser(prog="python -m app.passwd", description="Create an AUTH_USERS entry.")
    parser.add_argument("username")
    parser.add_argument("--stdin", action="store_true", help="read the password from standard input")
    arguments = parser.parse_args()
    if not USERNAME.match(arguments.username):
        raise SystemExit("The username may only contain letters, digits, dots, underscores and hyphens (1 to 32 characters).")
    password = read_password(arguments.stdin)
    if len(password) < MIN_PASSWORD_LENGTH:
        raise SystemExit(f"The password must be at least {MIN_PASSWORD_LENGTH} characters long.")
    print(f"{arguments.username}:{hash_password(password)}")


if __name__ == "__main__":
    main()
