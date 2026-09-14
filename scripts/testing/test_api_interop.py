"""Banc deux clients contre API, PostgreSQL et MinIO réels, entièrement jetables.

Le moteur C++ y affronte un **second client écrit dans ce banc** : un client
HTTP minimal et une réimplémentation du format d'archive (`save_format`). Ce
second client n'emprunte rien au produit — c'est ce qui rend la course de
compare-and-swap probante, puisque les deux concurrents ne partagent aucun code.

Ports loopback aléatoires, données Docker en tmpfs, secrets uniquement en env
ou stdin capturé. Ne lit aucune configuration utilisateur, n'utilise aucun
volume Compose, ne lance jamais les fixtures pytest qui vident une base
existante.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import operator
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import boto3
import httpx
import zstandard
from botocore.config import Config
from botocore.exceptions import BotoCoreError, ClientError
from save_format import ContentFile, create_archive, extract_archive

ROOT = Path(__file__).resolve().parents[2]


class CasConflict(Exception):
    """Le serveur a rangé notre contenu en branche : notre base n'était plus la tête."""

    def __init__(self, conflict_id: str, head: int, yours: int) -> None:
        super().__init__(f"conflit {conflict_id} : tête {head}, notre branche {yours}")
        self.conflict_id = conflict_id
        self.head = head
        self.yours = yours


class BenchApi:
    """Un client `/v0` minimal, écrit pour le banc.

    Il ne partage aucune ligne avec le moteur C++ : c'est ce qui donne son sens
    à la course de compare-and-swap plus bas. Volontairement direct — pas de
    relance, pas de file d'attente, pas d'état.
    """

    def __init__(self, url: str, token: str, client: httpx.Client) -> None:
        self.url = url.rstrip("/")
        self.token = token
        self.http = client
        self.device_id: str | None = None

    def _headers(self, idempotency_key: str | None = None) -> dict[str, str]:
        headers = {"Authorization": f"Bearer {self.token}"}
        if self.device_id:
            headers["X-Device-Id"] = self.device_id
        if idempotency_key:
            headers["Idempotency-Key"] = idempotency_key
        return headers

    def register_device(self, name: str, operating_system: str, version: str) -> str:
        response = self.http.post(
            f"{self.url}/v0/devices",
            headers=self._headers(f"register-{name}"),
            json={"name": name, "os": operating_system, "app_version": version},
        )
        response.raise_for_status()
        return response.json()["device_id"]

    def list_units(self) -> list[dict]:
        response = self.http.get(f"{self.url}/v0/units", headers=self._headers())
        response.raise_for_status()
        return response.json()["units"]

    def prepare_version(self, unit: str, *, idempotency_key: str, **metadata) -> dict:
        response = self.http.post(
            f"{self.url}/v0/units/{unit}/versions:prepare",
            headers=self._headers(idempotency_key),
            json=metadata,
        )
        response.raise_for_status()
        return response.json()

    def upload(self, url: str, path: Path) -> None:
        # L'URL présignée porte seule l'autorisation : aucun jeton d'API n'y va.
        response = self.http.put(url, content=path.read_bytes())
        response.raise_for_status()

    def confirm_version(self, unit: str, *, idempotency_key: str, **body) -> int:
        response = self.http.post(
            f"{self.url}/v0/units/{unit}/versions:confirm",
            headers=self._headers(idempotency_key),
            json=body,
        )
        if response.status_code == 409:
            payload = response.json()
            raise CasConflict(
                payload["conflict_id"],
                payload["head"]["number"],
                payload["yours"]["number"],
            )
        response.raise_for_status()
        return response.json()["version"]

    def download_version(self, unit: str, number: int) -> dict:
        response = self.http.get(
            f"{self.url}/v0/units/{unit}/versions/{number}/download",
            headers=self._headers(),
        )
        response.raise_for_status()
        return response.json()

    def download(self, url: str, path: Path) -> None:
        response = self.http.get(url)
        response.raise_for_status()
        path.write_bytes(response.content)


def command(args, *, env=None, cwd=None):
    result = subprocess.run(
        args, env=env, cwd=cwd, capture_output=True, text=True, timeout=60, check=False
    )
    if result.returncode:
        # Une trace de bootstrap pourrait contenir un DSN : ne pas la reproduire.
        raise RuntimeError(f"Échec de l'outil de test : {Path(args[0]).name}")
    return result.stdout.strip()


@contextmanager
def disposable_services(*, environment: dict[str, str] | None = None):
    names = []
    process = None
    with tempfile.TemporaryDirectory(prefix="retrosave-api-test-") as directory:
        try:
            password = secrets.token_hex(24)
            env = dict(
                os.environ,
                POSTGRES_USER="test",
                POSTGRES_DB="test",
                POSTGRES_PASSWORD=password,
                MINIO_ROOT_USER="testuser",
                MINIO_ROOT_PASSWORD=password,
            )

            def container(image, port, mount, variables, *args):
                name = "retrosave-api-test-" + secrets.token_hex(6)
                names.append(name)
                command(
                    [
                        "docker",
                        "run",
                        "-d",
                        "--name",
                        name,
                        "-p",
                        f"127.0.0.1::{port}",
                        "--tmpfs",
                        mount,
                        *functools.reduce(operator.iadd, (["--env", key] for key in variables), []),
                        image,
                        *args,
                    ],
                    env=env,
                )
                address = command(["docker", "port", name, f"{port}/tcp"])
                return name, int(address.rsplit(":", 1)[1])

            pg, pg_port = container(
                "postgres:16-alpine",
                5432,
                "/var/lib/postgresql/data:rw,size=256m",
                ["POSTGRES_USER", "POSTGRES_DB", "POSTGRES_PASSWORD"],
            )
            _, s3_port = container(
                os.environ.get(
                    "RETROSAVE_TEST_S3_IMAGE", "quay.io/minio/minio:RELEASE.2025-09-07T16-13-09Z"
                ),
                9000,
                "/data:rw,size=512m",
                ["MINIO_ROOT_USER", "MINIO_ROOT_PASSWORD"],
                "server",
                "/data",
            )
            endpoint = f"http://127.0.0.1:{s3_port}"
            storage = boto3.client(
                "s3",
                endpoint_url=endpoint,
                aws_access_key_id="testuser",
                aws_secret_access_key=password,
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
            storage_ready = False
            storage_error = "none"
            while True:
                ready = (
                    subprocess.run(
                        ["docker", "exec", pg, "pg_isready", "-h", "127.0.0.1", "-U", "test"],
                        check=False,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    ).returncode
                    == 0
                )
                try:
                    if not storage_ready:
                        storage.create_bucket(Bucket="synthetic-tests")
                        storage_ready = True
                    if ready:
                        break
                except (BotoCoreError, ClientError) as error:
                    storage_error = (
                        error.response["Error"]["Code"]
                        if isinstance(error, ClientError)
                        else type(error).__name__
                    )
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"Services de test indisponibles : postgres={ready}, s3={storage_error}"
                    )
                time.sleep(0.2)
            env.update(
                DATABASE_URL=f"postgresql+asyncpg://test:{password}@127.0.0.1:{pg_port}/test",
                APP_ENV="dev",
                S3_ENDPOINT=endpoint,
                S3_PUBLIC_ENDPOINT=endpoint,
                S3_REGION="auto",
                S3_BUCKET="synthetic-tests",
                S3_ACCESS_KEY_ID="testuser",
                S3_SECRET_ACCESS_KEY=password,
                S3_FORCE_PATH_STYLE="true",
                LOG_LEVEL="ERROR",
                PYTHONPATH=str(ROOT / "server"),
                ACCOUNT_JWKS_URL="",
                ACCOUNT_JWT_SECRET="",
                ACCOUNT_ISSUER="",
            )
            if environment is not None:
                environment.update(env)
            command(
                [sys.executable, "-m", "alembic", "upgrade", "head"], env=env, cwd=ROOT / "server"
            )
            token = command(
                [sys.executable, "-m", "app.cli", "new-token", "--label", "Synthetic interop"],
                env=env,
                cwd=directory,
            )
            # Socket déjà réservé et transmis à Uvicorn : aucune course de port.
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                url = f"http://127.0.0.1:{listener.getsockname()[1]}"
                process = subprocess.Popen(
                    [
                        sys.executable,
                        "-m",
                        "uvicorn",
                        "app.main:app",
                        "--fd",
                        str(listener.fileno()),
                        "--no-access-log",
                        "--log-level",
                        "error",
                    ],
                    pass_fds=(listener.fileno(),),
                    cwd=directory,
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                deadline = time.monotonic() + 20
                with httpx.Client(timeout=1, trust_env=False) as http:
                    while True:
                        try:
                            health = http.get(url + "/healthz")
                            if (
                                health.status_code == 200
                                and health.json()["db"]
                                and health.json()["s3"]
                            ):
                                break
                        except httpx.HTTPError:
                            pass
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("API de test indisponible")
                        time.sleep(0.2)
                yield url, token, storage, Path(directory)
        finally:
            if process:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            for name in names:
                subprocess.run(
                    ["docker", "rm", "--force", name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )


@contextmanager
def lose_confirm_response(url):
    statuses = []

    class Proxy(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            with httpx.Client(timeout=5, trust_env=False) as http:
                response = http.post(
                    url + self.path,
                    content=body,
                    headers={
                        key: value
                        for key, value in self.headers.items()
                        if key.lower() not in {"host", "connection", "content-length"}
                    },
                )
                statuses.append(response.status_code)
            # Le serveur a répondu après commit. Le client ne reçoit aucun octet.
            self.close_connection = True

    server = ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", statuses
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp", type=Path, required=True)
    cpp = parser.parse_args().cpp.resolve(strict=True)
    with disposable_services() as (url, token, storage, directory):
        device = ""

        def run(mode, **values):
            config = {
                "url": url,
                "token": token,
                "device": device,
                "mode": mode,
                "key": secrets.token_hex(16),
            }
            config.update(values)
            completed = subprocess.run(
                [str(cpp)],
                input=json.dumps(config),
                capture_output=True,
                text=True,
                timeout=15,
                check=True,
            )
            result = json.loads(completed.stdout)
            assert result["staging_clean"], "staging non nettoyé"
            return result

        registered = run("register", key="register-cpp")
        assert registered["ok"]
        device = registered["id"]
        assert run("register", key="register-cpp")["id"] == device
        with httpx.Client(timeout=10, trust_env=False) as http:
            bench = BenchApi(url, token, http)
            bench.device_id = bench.register_device("Synthetic bench", "linux", "test")
            unit = run("create")["id"]
            assert len(bench.list_units()) == 1
            a = create_archive("dir", [ContentFile("save.bin", b"cpp branch" * 1000)])
            b = create_archive("dir", [ContentFile("save.bin", b"bench branch" * 1000)])

            def metadata(blob, base):
                return {
                    "base_version": base,
                    "content_sha256": blob.content_sha256,
                    "archive_sha256": blob.archive_sha256,
                    "size": blob.size_bytes,
                    "archive_bytes": blob.archive_bytes,
                }

            def confirmation(blob, base, object_key):
                return {
                    "base_version": base,
                    "object_key": object_key,
                    "content_sha256": blob.content_sha256,
                    "archive_sha256": blob.archive_sha256,
                }

            def cpp_upload(blob, target):
                path = directory / "synthetic.tar.zst"
                path.write_bytes(blob)
                result = run("upload", target=target, source=str(path))
                assert result["ok"], result

            prepared = run("prepare", id=unit, body=metadata(a, 0))
            pending = bench.prepare_version(unit, **metadata(b, 0), idempotency_key="prepare-bench")
            assert pending.get("upload"), pending
            cpp_upload(a.bytes, prepared["target"])
            b_path = directory / "bench.tar.zst"
            b_path.write_bytes(b.bytes)
            bench.upload(pending["upload"]["url"], b_path)
            barrier = threading.Barrier(2)

            def cpp_confirm():
                barrier.wait()
                return run("confirm", id=unit, body=confirmation(a, 0, prepared["object_key"]))

            def bench_confirm():
                barrier.wait()
                try:
                    version = bench.confirm_version(
                        unit,
                        **confirmation(b, 0, pending["upload"]["object_key"]),
                        env={"os": "linux"},
                        client_mtime=None,
                        idempotency_key="confirm-bench",
                    )
                    return {"status": 201, "version": version}
                except CasConflict as conflict:
                    return {
                        "status": 409,
                        "conflict": conflict.conflict_id,
                        "first": conflict.head,
                        "second": conflict.yours,
                    }

            with ThreadPoolExecutor(2) as pool:
                futures = [pool.submit(cpp_confirm), pool.submit(bench_confirm)]
                results = [future.result() for future in futures]
            assert sorted(result["status"] for result in results) == [201, 409], results
            conflict = next(result for result in results if result["status"] == 409)
            assert run("conflicts")["count"] == 1
            assert run("resolve", id=conflict["conflict"], body={"winner": 1})["head"] == 1
            history = run("history", id=unit)
            assert history["head"] == 1 and history["versions"] == [2, 1]
            for version in [1, 2]:
                assert run("pull", id=unit, body={"version": version})["content_valid"]
                target = bench.download_version(unit, version)
                downloaded = directory / f"download-{version}.tar.zst"
                bench.download(target["url"], downloaded)
                # Relu par une implémentation du format qui n'est pas celle du
                # produit : c'est ce qui prouve que le moteur écrit bien §5.3,
                # et pas seulement quelque chose qu'il sait relire lui-même.
                extract_archive(downloaded.read_bytes(), "dir", target["content_sha256"])
            print(
                "CAS réel : 201 + 409 ; deux branches récupérables après résolution, "
                "relues par un lecteur indépendant.",
                flush=True,
            )

            c = create_archive("dir", [ContentFile("save.bin", b"lost response" * 1000)])
            prepared = run("prepare", id=unit, body=metadata(c, 1))
            cpp_upload(c.bytes, prepared["target"])
            confirm_body = confirmation(c, 1, prepared["object_key"])
            with lose_confirm_response(url) as (proxy, statuses):
                lost = run("confirm", url=proxy, id=unit, body=confirm_body, key="lost-response")
                assert not lost["ok"] and lost["uncertain"] and statuses == [201]
            replay = run("confirm", id=unit, body=confirm_body, key="lost-response")
            assert replay["version"] == 3
            assert run("history", id=unit)["versions"] == [3, 2, 1]
            assert run("prepare", id=unit, body=metadata(c, 3))["duplicate"] == 3
            print(
                "Perte de réponse après commit : rejeu C++ même clé → v3 unique ; déduplication confirmée.",
                flush=True,
            )

            # Simulation d'un PUT encore autorisé qui remplace l'enveloppe du
            # même contenu. Taille et empreinte stockées deviennent périmées (Q10).
            raw_tar = zstandard.ZstdDecompressor().decompress(c.bytes, max_output_size=1024 * 1024)
            alternative = zstandard.ZstdCompressor(
                level=1, write_checksum=True, write_content_size=True
            ).compress(raw_tar)
            assert len(alternative) != c.archive_bytes
            assert hashlib.sha256(alternative).hexdigest() != c.archive_sha256
            cpp_upload(alternative, prepared["target"])
            valid = run("pull", id=unit, body={"version": 3})
            assert valid["content_valid"] and valid["archive_mismatch"] and valid["size_mismatch"]
            limited = run("pull", id=unit, body={"version": 3}, maximum_bytes=1)
            assert not limited["ok"] and limited["sinks"] == 0
            cpp_upload(b.bytes, prepared["target"])
            invalid = run("pull", id=unit, body={"version": 3})
            assert not invalid["content_valid"] and invalid["sinks"] == 0
            # Seul l'objet SYNTHÉTIQUE du conteneur de test a été altéré.
            storage.put_object(Bucket="synthetic-tests", Key=prepared["object_key"], Body=c.bytes)
            restored = run("restore", id=unit, body={"version": 1})
            assert restored["version"] == 4
            assert run("pull", id=unit, body={"version": 4})["content_valid"]
            print(
                "Q10 : enveloppe différente acceptée, contenu différent et dépassement refusés avant sink ; restore v4 vérifié.",
                flush=True,
            )
    print("Services jetables arrêtés ; volumes existants et sauvegardes utilisateur intacts.")


if __name__ == "__main__":
    main()
