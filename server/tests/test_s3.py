"""S3 HEAD verification tests without reading archive contents."""

from __future__ import annotations

from types import SimpleNamespace
from urllib.parse import urlparse

import pytest
from app import s3
from app.api.errors import ApiError
from botocore.exceptions import ClientError


async def test_signer_uses_public_endpoint_without_network(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """AD-25 embeds the public host while all real calls stay on the ops client."""

    settings = SimpleNamespace(
        s3_endpoint="http://minio:9000",
        s3_signing_endpoint="http://192.168.1.20:9000",
        s3_region="auto",
        s3_access_key_id="retrosave",
        s3_secret_access_key="retrosave-dev",
        s3_force_path_style=True,
        s3_bucket="retrosave-poc",
        presign_expires_s=900,
    )
    monkeypatch.setattr(s3, "get_settings", lambda: settings)
    s3.get_s3_client.cache_clear()
    s3.get_s3_signer.cache_clear()
    try:
        assert s3.get_s3_client().meta.endpoint_url == "http://minio:9000"
        assert s3.get_s3_signer().meta.endpoint_url == "http://192.168.1.20:9000"
        put_url, _ = await s3.presign_put("u/blob.tar.zst")
        get_url, _ = await s3.presign_get("u/blob.tar.zst")
        assert urlparse(put_url).netloc == "192.168.1.20:9000"
        assert urlparse(get_url).netloc == "192.168.1.20:9000"
    finally:
        s3.get_s3_client.cache_clear()
        s3.get_s3_signer.cache_clear()


async def test_signer_is_never_used_for_s3_operations(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The signer exposes no network operation and production paths still work."""

    class Ops:
        def head_bucket(self, **_kwargs) -> None:
            return None

        def head_object(self, **_kwargs) -> dict[str, int]:
            return {"ContentLength": 80}

        def delete_object(self, **_kwargs) -> None:
            return None

    class Signer:
        def generate_presigned_url(self, operation: str, **_kwargs) -> str:
            return f"http://public.invalid/{operation}"

    monkeypatch.setattr(s3, "get_s3_client", lambda: Ops())
    monkeypatch.setattr(s3, "get_s3_signer", lambda: Signer())

    assert await s3.check_storage() is True
    assert await s3.head_object_size("u/blob.tar.zst") == 80
    await s3.delete_object("u/blob.tar.zst")
    assert (await s3.presign_put("u/blob.tar.zst"))[0].endswith("/put_object")
    assert (await s3.presign_get("u/blob.tar.zst"))[0].endswith("/get_object")


async def test_head_size_mismatch_is_checksum_error(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A confirm cannot accept a blob whose uploaded length changed."""

    async def wrong_size(_object_key: str) -> int:
        return 79

    monkeypatch.setattr(s3, "head_object_size", wrong_size)

    with pytest.raises(ApiError) as captured:
        await s3.verify_uploaded_object("u/blob.tar.zst", 80)

    assert captured.value.status_code == 422
    assert captured.value.code == "checksum_mismatch"
    assert captured.value.details == {
        "expected_archive_bytes": 80,
        "actual_archive_bytes": 79,
    }


async def test_missing_object_is_checksum_error(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An absent direct upload cannot create version metadata."""

    async def missing(_object_key: str) -> int:
        raise ClientError(
            {"Error": {"Code": "NoSuchKey", "Message": "missing"}},
            "HeadObject",
        )

    monkeypatch.setattr(s3, "head_object_size", missing)

    with pytest.raises(ApiError) as captured:
        await s3.verify_uploaded_object("u/missing.tar.zst", 80)

    assert captured.value.status_code == 422
    assert captured.value.code == "checksum_mismatch"


async def test_storage_failure_is_not_mislabeled_as_client_checksum(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """S3 authorization or service failures remain server-side failures."""

    failure = ClientError(
        {"Error": {"Code": "AccessDenied", "Message": "denied"}},
        "HeadObject",
    )

    async def denied(_object_key: str) -> int:
        raise failure

    monkeypatch.setattr(s3, "head_object_size", denied)

    with pytest.raises(ClientError) as captured:
        await s3.verify_uploaded_object("u/blob.tar.zst", 80)

    assert captured.value is failure
