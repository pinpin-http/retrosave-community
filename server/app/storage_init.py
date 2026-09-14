"""Create the private S3 bucket during self-hosted bootstrap."""

import boto3
from botocore.config import Config
from botocore.exceptions import ClientError

from app.config import get_settings


def main() -> None:
    settings = get_settings()
    client = boto3.client(
        "s3",
        endpoint_url=settings.s3_endpoint,
        region_name=settings.s3_region,
        aws_access_key_id=settings.s3_access_key_id,
        aws_secret_access_key=settings.s3_secret_access_key,
        config=Config(signature_version="s3v4", s3={"addressing_style": "path"}),
    )
    try:
        client.head_bucket(Bucket=settings.s3_bucket)
    except ClientError as error:
        if error.response["Error"]["Code"] not in {"404", "NoSuchBucket", "NotFound"}:
            raise
        client.create_bucket(Bucket=settings.s3_bucket)
    print("Private save bucket ready.")


if __name__ == "__main__":
    main()
