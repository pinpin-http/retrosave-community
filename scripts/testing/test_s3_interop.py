"""Banc S3 réel C++/Python sur un MinIO Docker jetable, sans volume existant.

Le conteneur appartient exclusivement à ce script : nom aléatoire, port publié
sur loopback et données en tmpfs. Le finally le détruit même si un test échoue.
Les URL présignées passent au C++ sur stdin, jamais dans la ligne de commande.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import boto3
from botocore.config import Config
from botocore.exceptions import BotoCoreError, ClientError


def docker(*args: str, env: dict[str, str] | None = None) -> str:
    return subprocess.check_output(["docker", *args], env=env, text=True).strip()


def run(cpp: Path, **config: object) -> dict:
    completed = subprocess.run(
        [str(cpp)],
        input=json.dumps(config),
        text=True,
        capture_output=True,
        timeout=40,
        check=True,
    )
    result = json.loads(completed.stdout)
    assert result["staging_clean"], "un fichier temporaire n'a pas été nettoyé"
    return result


def check_failures(cpp: Path) -> None:
    """Serveur local volontairement défaillant, sans signature ni donnée réelle."""
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # Ne jamais imprimer une URL de requête.

        def do_GET(self):
            requests.append((self.path, dict(self.headers)))
            if self.path == "/redirect":
                self.send_response(307)
                self.send_header("Location", "/forbidden")
                self.end_headers()
            elif self.path == "/empty":
                self.send_response(204)
                self.end_headers()
            else:
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b"partial")
                self.wfile.flush()
                if self.path == "/slow":
                    time.sleep(0.4)
                # Fermeture avant Content-Length : vrai téléchargement tronqué.
                self.close_connection = True

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        for path, options, error in [
            ("/redirect", {}, "redirect"),
            ("/empty", {}, "http_error"),
            ("/truncated", {}, "network_error"),
            ("/slow", {"timeout_ms": 80}, "timeout"),
            ("/slow", {"cancel": True}, "cancelled"),
        ]:
            result = run(
                cpp,
                mode="download",
                url=f"http://127.0.0.1:{server.server_port}{path}",
                bytes=0 if path == "/empty" else 100,
                sha256=hashlib.sha256(b"").hexdigest(),
                **options,
            )
            assert result[error] and not result["staged"], result
        assert all(path != "/forbidden" for path, _ in requests)
        for _, headers in requests:
            assert not (
                {key.lower() for key in headers}
                & {"authorization", "cookie", "x-device-id", "idempotency-key"}
            )
        print(
            "Pannes HTTP : redirection, 204, coupure, timeout et annulation refusés ; staging nettoyé."
        )
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp", type=Path, required=True)
    args = parser.parse_args()
    cpp = args.cpp.resolve(strict=True)
    check_failures(cpp)
    name = "retrosave-s3-test-" + secrets.token_hex(5)
    access, secret = "test" + secrets.token_hex(8), secrets.token_hex(24)
    environment = dict(os.environ, MINIO_ROOT_USER=access, MINIO_ROOT_PASSWORD=secret)
    try:
        docker(
            "run",
            "--detach",
            "--name",
            name,
            "--publish",
            "127.0.0.1::9000",
            "--tmpfs",
            "/data:rw,size=512m",
            "--env",
            "MINIO_ROOT_USER",
            "--env",
            "MINIO_ROOT_PASSWORD",
            os.environ.get(
                "RETROSAVE_TEST_S3_IMAGE", "quay.io/minio/minio:RELEASE.2025-09-07T16-13-09Z"
            ),
            "server",
            "/data",
            env=environment,
        )
        port = docker("port", name, "9000/tcp").rsplit(":", 1)[1]
        s3 = boto3.client(
            "s3",
            endpoint_url=f"http://127.0.0.1:{port}",
            aws_access_key_id=access,
            aws_secret_access_key=secret,
            region_name="us-east-1",
            config=Config(
                signature_version="s3v4",
                s3={"addressing_style": "path"},
                connect_timeout=1,
                read_timeout=2,
                retries={"max_attempts": 0},
            ),
        )
        deadline = time.monotonic() + 30
        while True:
            try:
                s3.create_bucket(Bucket="synthetic-tests")
                break
            except (BotoCoreError, ClientError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("MinIO de test indisponible") from None
                time.sleep(0.25)

        def signed(method: str, key: str) -> str:
            params = {"Bucket": "synthetic-tests", "Key": key}
            if method == "put_object":
                params["ContentType"] = "application/zstd"
            return s3.generate_presigned_url(method, Params=params, ExpiresIn=120)

        with tempfile.TemporaryDirectory(prefix="retrosave-s3-fixtures-") as directory:
            # Octets synthétiques opaques : le transport ne connaît pas tar.
            # Les formats sont vérifiés par le banc d'archive séparé.
            source = Path(directory) / "synthetic.tar.zst"
            chunk = bytes(range(256)) * 256
            digest = hashlib.sha256()
            with source.open("wb") as stream:
                for _ in range(1024):  # 64 Mio, sans construire une allocation de 64 Mio
                    stream.write(chunk)
                    digest.update(chunk)
            size, sha = source.stat().st_size, digest.hexdigest()
            result = run(
                cpp, mode="upload", url=signed("put_object", "cpp"), source=str(source), bytes=size
            )
            assert result["ok"] and result["bytes"] == size, result
            # Python relit ce que Qt a réellement envoyé à MinIO.
            body = s3.get_object(Bucket="synthetic-tests", Key="cpp")["Body"]
            actual = hashlib.sha256()
            try:
                for block in body.iter_chunks(64 * 1024):
                    actual.update(block)
            finally:
                body.close()
            assert actual.hexdigest() == sha, "contenu C++ → S3 → Python divergent"

            # Sens inverse : Python envoie, C++ reçoit dans un staging vérifié.
            with source.open("rb") as stream:
                s3.put_object(
                    Bucket="synthetic-tests",
                    Key="python",
                    Body=stream,
                    ContentLength=size,
                    ContentType="application/zstd",
                )
            url = signed("get_object", "python")
            result = run(cpp, mode="download", url=url, bytes=size, sha256=sha)
            assert result["ok"] and result["staged"] and result["sha256"] == sha, result
            for wrong_size, wrong_hash in [(size, "0" * 64), (size - 1, sha), (size + 1, sha)]:
                result = run(cpp, mode="download", url=url, bytes=wrong_size, sha256=wrong_hash)
                assert not result["ok"] and result["integrity_error"] and not result["staged"], (
                    result
                )
            result = run(
                cpp,
                mode="download",
                url=signed("get_object", "missing"),
                bytes=0,
                sha256=hashlib.sha256(b"").hexdigest(),
            )
            assert result["http_error"] and result["status"] == 404, result
        print(
            "S3 réel : 64 Mio C++ → MinIO → Python et Python → MinIO → C++ ; "
            "empreinte/taille incorrectes et 404 refusées ; staging nettoyé."
        )
    finally:
        # Ce nom a été généré par CETTE exécution ; aucun volume utilisateur.
        subprocess.run(
            ["docker", "rm", "--force", name],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


if __name__ == "__main__":
    main()
